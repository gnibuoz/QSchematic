#include <algorithm>
#include <QPainter>
#include <QGraphicsSceneMouseEvent>
#include <QMessageBox>
#include <QXmlStreamWriter>
#include <QUndoStack>
#include <QPixmap>
#include <QMimeData>
#include <QtMath>
#include "scene.h"
#include "commands/commanditemmove.h"
#include "commands/commanditemadd.h"
#include "items/itemfactory.h"
#include "items/item.h"
#include "items/itemmimedata.h"
#include "items/node.h"
#include "items/wire.h"
#include "items/wirenet.h"

using namespace QSchematic;

Scene::Scene(QObject* parent) :
    QGraphicsScene(parent),
    _mode(NormalMode),
    _newWireSegment(false),
    _invertWirePosture(true)
{
    // Undo stack
    _undoStack = new QUndoStack;
    connect(_undoStack, &QUndoStack::cleanChanged, [this](bool isClean) {
        emit isDirtyChanged(!isClean);
    });

    // Stuff
    connect(this, &QGraphicsScene::sceneRectChanged, [this]{
        renderCachedBackground();
    });

    // Prepare the background
    renderCachedBackground();
}

Gpds::Container Scene::toContainer() const
{
    // Scene
    Gpds::Container scene;
    {
        // Rect
        Gpds::Container r;
        const QRect& rect = sceneRect().toRect();
        r.addValue("x", rect.x());
        r.addValue("y", rect.y());
        r.addValue("width", rect.width());
        r.addValue("height", rect.height());
        scene.addValue("rect", r);
    }

    // Nodes
    Gpds::Container nodesList;
    for (const auto& node : nodes()) {
        nodesList.addValue("node", node->toContainer());
    }

    // Nets
    Gpds::Container netsList;
    for (const auto& net : nets()) {
        netsList.addValue("net", net->toContainer());
    }

    // Root
    Gpds::Container c;
    c.addValue("scene", scene);
    c.addValue("nodes", nodesList);
    c.addValue("nets", netsList);

    return c;
}

void Scene::fromContainer(const Gpds::Container& container)
{
    // Scene
    {
        const Gpds::Container* sceneContainer = container.getValue<Gpds::Container*>("scene");
        Q_ASSERT( sceneContainer );

        // Rect
        const Gpds::Container* rectContainer = sceneContainer->getValue<Gpds::Container*>("rect");
        if ( rectContainer ) {
            QRect rect;
            rect.setX( rectContainer->getValue<int>("x") );
            rect.setY( rectContainer->getValue<int>("y") );
            rect.setWidth( rectContainer->getValue<int>("width") );
            rect.setHeight( rectContainer->getValue<int>("height") );

            setSceneRect( rect );
        }
    }

    // Nodes
    const Gpds::Container* nodesContainer = container.getValue<Gpds::Container*>("nodes");
    if ( nodesContainer ) {
        for (const auto& nodeContainer : nodesContainer->getValues<Gpds::Container*>("node")) {
            Q_ASSERT(nodeContainer);

            std::unique_ptr<Item> node = ItemFactory::instance().fromContainer(*nodeContainer);
            if (!node) {
                qCritical("Scene::fromContainer(): Couldn't restore node. Skipping.");
                continue;
            }
            node->fromContainer(*nodeContainer);
            addItem(std::move(node));
        }
    }

    // Nets
    const Gpds::Container* netsContainer = container.getValue<Gpds::Container*>("nets");
    if ( netsContainer ) {
        Q_ASSERT( netsContainer );

        for (const Gpds::Container* netContainer : netsContainer->getValues<Gpds::Container*>("net")) {
            Q_ASSERT( netContainer );

            auto net = std::make_shared<WireNet>();
            net->fromContainer( *netContainer );

            for (auto& wire : net->wires()) {
                addItem(wire);
            }

            addWireNet(net);
        }
    }

    // Clear the undo history
    _undoStack->clear();
}

void Scene::setSettings(const Settings& settings)
{
    // Update settings of all items
    for (auto& item : items()) {
        item->setSettings(settings);
    }

    // Store new settings
    _settings = settings;

    // Redraw
    renderCachedBackground();
    update();
}

void Scene::setWireFactory(const std::function<std::unique_ptr<Wire>()>& factory)
{
    _wireFactory = factory;
}

void Scene::setMode(int mode)
{
    // Dont do anything unnecessary
    if (mode == _mode) {
        return;
    }

    // Check what the previous mode was
    switch (_mode) {

    // Discard current wire/bus
    case WireMode:
        _newWire.reset();
        break;

    default:
        break;

    }

    // Store the new mode
    _mode = mode;

    // Update the UI
    update();

    // Let the world know
    emit modeChanged(_mode);
}

int Scene::mode() const
{
    return _mode;
}

void Scene::toggleWirePosture()
{
    _invertWirePosture = !_invertWirePosture;
}

bool Scene::isDirty() const
{
    Q_ASSERT(_undoStack);

    return !_undoStack->isClean();
}

void Scene::clearIsDirty()
{
    Q_ASSERT(_undoStack);

    _undoStack->setClean();
}

void Scene::clear()
{
    // Remove from scene
    // Do not use QGraphicsScene::clear() as that would also delete the items. However,
    // we still need them as we manage them via smart pointers (eg. in commands)
    while (!_items.isEmpty()) {
        removeItem(_items.first());
    }
    Q_ASSERT(_items.isEmpty());

    // Nets
    _nets.clear();
    Q_ASSERT(_nets.isEmpty());

    // Selected items
    _selectedItems.clear();
    Q_ASSERT(_selectedItems.isEmpty());

    // Undo stack
    _undoStack->clear();
    clearIsDirty();

    // Update
    update();
}

bool Scene::addItem(const std::shared_ptr<Item>& item)
{
    // Sanity check
    if (!item) {
        return false;
    }

    // Setup item
    setupNewItem(*(item.get()));

    // Add to scene
    QGraphicsScene::addItem(item.get());

    // Store the shared pointer to keep the item alive for the QGraphicsScene
    _items << item;

    // Let the world know
    emit itemAdded(item);

    return true;
}

bool Scene::removeItem(const std::shared_ptr<Item>& item)
{
    // Sanity check
    if (!item) {
        return false;
    }

    // Remove from scene (if necessary)
    if (item->QGraphicsItem::scene()) {
        QGraphicsScene::removeItem(item.get());
    }

    // Remove shared pointer from local list to reduce instance count
    _items.removeAll(item);

    // Let the world know
    emit itemRemoved(item);

    return true;
}

QList<std::shared_ptr<Item>> Scene::items() const
{
    return _items;
}

QList<std::shared_ptr<Item>> Scene::items(int itemType) const
{
    QList<std::shared_ptr<Item>> items;

    for (auto& item : _items) {
        if (item->type() != itemType) {
            continue;
        }

        items << item;
    }

    return items;
}

QVector<std::shared_ptr<Item>> Scene::selectedItems() const
{
    // Retrieve items from QGraphicsScene
    const auto& rawItems = QGraphicsScene::selectedItems();

    // Retrieve corresponding smart pointers
    QVector<std::shared_ptr<Item>> items(rawItems.count());
    int i = 0;
    for (auto& item : _items) {
        if (rawItems.contains(item.get())) {
            items[i++] = item;
        }
    }

    return items;
}

QList<std::shared_ptr<Node>> Scene::nodes() const
{
    QList<std::shared_ptr<Node>> nodes;

    for (auto& item : _items) {
        auto node = std::dynamic_pointer_cast<Node>(item);
        if (!node) {
            continue;
        }

        nodes << node;
    }

    return nodes;
}

bool Scene::addWire(const std::shared_ptr<Wire>& wire)
{
    // Sanity check
    if (!wire) {
        return false;
    }

    // Check if any point of the wire lies on any line segment of all existing line segments.
    // If yes, add to that net. Otherwise, create a new one
    for (auto& net : _nets) {
        for (const Line& line : net->lineSegments()) {
            for (const QPointF& point : wire->pointsRelative()) {
                if (line.containsPoint(point.toPoint(), 0)) {
                    net->addWire(wire);
                    return true;
                }
            }
        }
    }

    // Check if any line segment of the wire lies on any point of all existing wires.
    // If yes, add to that net. Otherwise, create a new one
    for (auto& net : _nets) {
        for (const auto& otherWire : net->wires()) {
            for (const WirePoint& otherPoint : otherWire->wirePointsRelative()) {
                for (const Line& line : wire->lineSegments()) {
                    if (line.containsPoint(otherPoint.toPoint())) {
                        net->addWire(wire);
                        return true;
                    }
                }
            }
        }
    }

    // No point of the new wire lies on an existing line segment - create a new wire net
    auto newNet = std::make_unique<WireNet>();
    newNet->addWire(wire);
    addWireNet(std::move(newNet));

    // Add wire to scene
    // Wires createde by mouse interactions are already added to the scene in the Scene::mouseXxxEvent() calls. Prevent
    // adding an already added item to the scene
    if (wire->QGraphicsItem::scene() != this) {
        if (!addItem(wire)) {
            return false;
        }
    }

    return true;
}

bool Scene::removeWire(const std::shared_ptr<Wire>& wire)
{
    // Remove the wire from the scene
    removeItem(wire);

    // Remove the wire from the list
    QList<std::shared_ptr<WireNet>> netsToDelete;
    for (auto& net : _nets) {
        if (net->contains(wire)) {
            net->removeWire(wire);
        }

        if (net->wires().count() < 1) {
            netsToDelete.append(net);
        }
    }

    // Delete the net if this was the nets last wire
    for (auto& net : netsToDelete) {
        _nets.removeAll(net);
    }

    return true;
}

QList<std::shared_ptr<Wire>> Scene::wires() const
{
    QList<std::shared_ptr<Wire>> list;

    for (const auto& wireNet : _nets) {
        list.append(wireNet->wires());
    }

    return list;
}

QList<std::shared_ptr<WireNet>> Scene::nets() const
{
    return _nets;
}

QList<std::shared_ptr<WireNet>> Scene::nets(const std::shared_ptr<WireNet>& wireNet) const
{
    QList<std::shared_ptr<WireNet>> list;

    for (auto& net : _nets) {
        if (!net) {
            continue;
        }

        if (net->name().isEmpty()) {
            continue;
        }

        if (QString::compare(net->name(), wireNet->name(), Qt::CaseInsensitive) == 0) {
            list.append(net);
        }
    }

    return list;
}

std::shared_ptr<WireNet> Scene::net(const std::shared_ptr<Wire>& wire) const
{
    for (auto& net : _nets) {
        for (const auto& w : net->wires()) {
            if (w == wire) {
                return net;
            }
        }
    }

    return nullptr;
}

QList<std::shared_ptr<WireNet>> Scene::netsAt(const QPoint& point)
{
    QList<std::shared_ptr<WireNet>> list;

    for (auto& net : _nets) {
        for (const Line& line : net->lineSegments()) {
            if (line.containsPoint(point) && !list.contains(net)) {
                list.append(net);
            }
        }
    }

    return list;
}

void Scene::undo()
{
    _undoStack->undo();
}

void Scene::redo()
{
    _undoStack->redo();
}

QUndoStack* Scene::undoStack() const
{
    return _undoStack;
}

void Scene::itemMoved(const Item& item, const QVector2D& movedBy)
{
    // Nothing to do if the item didn't move at all
    if (movedBy.isNull()) {
        return;
    }

    // If this is a Node class, move wires with it
    const Node* node = dynamic_cast<const Node*>(&item);
    if (node) {
        // Create a list of all wires there were connected to the SchematicObject
        auto wiresConnectedToMovingObjects = wiresConnectedTo(*node, movedBy*(-1));

        // Update wire positions
        for (auto& wire : wiresConnectedToMovingObjects) {
            for (const QPointF& connectionPoint : node->connectionPointsAbsolute()) {
                wireMovePoint(connectionPoint, *wire, movedBy);
            }
        }

        // Clean up the wires
        for (const auto& wire : wiresConnectedToMovingObjects) {
            auto wireNet = net(wire);
            if (!wireNet) {
                continue;
            }

            wireNet->simplify();
        }
    }
}

void Scene::itemRotated(const Item& item, const qreal rotation)
{
    QList<std::shared_ptr<Wire>> wiresConnectedToRotatingObjects;
    // If this is a Node class, move wires with it
    const Node* node = dynamic_cast<const Node*>(&item);
    if (node) {
        // Move all the wires attached to the node
        for (auto& wire : wires()) {
            for (const WirePoint& wirePoint : wire->wirePointsAbsolute()) {
                for (const QPointF& connectionPoint : node->connectionPointsAbsolute()) {
                    // Calculate the point's previous position
                    QPointF pos = connectionPoint;
                    {
                        QPointF d = node->transformOriginPoint() + node->pos() - pos;
                        qreal angle = -rotation * M_PI / 180;
                        QPointF rotated;
                        rotated.setX(qCos(angle) * d.rx() - qSin(angle) * d.ry());
                        rotated.setY(qSin(angle) * d.rx() + qCos(angle) * d.ry());
                        pos = node->transformOriginPoint() + node->pos() - rotated;
                    }
                    if (QVector2D(wirePoint.toPointF() - pos).length() < 0.001f) {
                        QVector2D movedBy = QVector2D(connectionPoint - pos);
                        wireMovePoint(connectionPoint, *wire, movedBy);
                        wiresConnectedToRotatingObjects << wire;
                        break;
                    }
                }
            }
        }

        // Clean up the wires
        for (const auto& wire : wiresConnectedToRotatingObjects) {
            auto wireNet = net(wire);
            if (!wireNet) {
                continue;
            }

            wireNet->simplify();
        }
    }
}

void Scene::itemHighlightChanged(const Item& item, bool isHighlighted)
{
    // Retrieve the corresponding smart pointer
    auto sharedPointer = sharedItemPointer(item);
    if (not sharedPointer) {
        return;
    }

    // Let the world know
    emit itemHighlightChanged(sharedPointer, isHighlighted);
}

void Scene::wireNetHighlightChanged(bool highlighted)
{
    auto rawPointer = qobject_cast<WireNet*>(sender());
    if (!rawPointer) {
        return;
    }
    std::shared_ptr<WireNet> wireNet;
    for (auto& wn : _nets) {
        if (wn.get() == rawPointer) {
            wireNet = wn;
            break;
        }
    }
    if (!wireNet) {
        return;
    }

    // Highlight all wire nets that are part of this net
    for (auto& otherWireNet : nets(wireNet)) {
        if (otherWireNet == wireNet) {
            continue;
        }

        otherWireNet->blockSignals(true);
        otherWireNet->setHighlighted(highlighted);
        otherWireNet->blockSignals(false);
    }
}

void Scene::wirePointMoved(Wire& rawWire, WirePoint& point)
{
    Q_UNUSED(point)

    // Retrieve corresponding shared ptr
    std::shared_ptr<Wire> wire;
    for (auto& item : _items) {
        std::shared_ptr<Wire> wireItem = std::dynamic_pointer_cast<Wire>(item);
        if (!wireItem) {
            continue;
        }

        if (wireItem.get() == &rawWire) {
            wire = wireItem;
            break;
        }
    }

    // Remove the Wire from the current WireNet if it is part of a WireNet
    auto it = _nets.begin();
    while (it != _nets.end()) {
        // Alias the Net
        auto& net = *it;

        // Remove the Wire from the Net
        if (net->contains(wire)) {
            net->removeWire(wire);
            net->setHighlighted(false);

            // Remove the WireNet if it has no more Wires
            if (net->wires().isEmpty()) {
                it = _nets.erase(it);
            }

            // A Wire can only be part of one WireNet - therefore, we're done
            break;

        } else {

            it++;

        }
    }

    // Add the wire
    addWire(wire);
}

void Scene::wireMovePoint(const QPointF& point, Wire& wire, const QVector2D& movedBy) const
{
    // If there are only two points (one line segment) and we are supposed to preserve
    // straight angles, we need to insert two additional points if we are not moving in
    // the direction of the line.
    if (wire.pointsRelative().count() == 2 && _settings.preserveStraightAngles) {
        const Line& line = wire.lineSegments().first();

        // Only do this if we're not moving in the direction of the line. Because in that case
        // this is unnecessary as we're just moving one of the two points.
        if ((line.isHorizontal() && !qFuzzyIsNull(movedBy.y())) || (line.isVertical() && !qFuzzyIsNull(movedBy.x()))) {
            qreal lineLength = line.lenght();
            QPointF p;

            // The line is horizontal
            if (line.isHorizontal()) {
                QPointF leftPoint = line.p1();
                if (line.p2().x() < line.p1().x()) {
                    leftPoint = line.p2();
                }

                p.rx() = leftPoint.x() + static_cast<int>(lineLength/2);
                p.ry() = leftPoint.y();

            // The line is vertical
            } else {
                QPointF upperPoint = line.p1();
                if (line.p2().x() < line.p1().x()) {
                    upperPoint = line.p2();
                }

                p.rx() = upperPoint.x();
                p.ry() = upperPoint.y() + static_cast<int>(lineLength/2);
            }

            // Insert twice as these two points will form the new additional vertical or
            // horizontal line segment that is required to preserver straight angles.
            wire.insertPoint(1, p);
            wire.insertPoint(1, p);
        }
    }

    // Move the points
    for (int i = 0; i < wire.pointsRelative().count(); i++) {
        QPointF currPoint = wire.pointsRelative().at(i);
        if (qFuzzyCompare(QVector2D(currPoint), QVector2D(point) - movedBy)) {

            // Preserve straight angles (if supposed to)
            if (_settings.preserveStraightAngles) {

                // Move previous point
                if (i >= 1) {
                    QPointF prevPoint = wire.pointsRelative().at(i-1);
                    Line line(prevPoint, currPoint);

                    // Make sure that two wire points never collide
                    if (wire.pointsRelative().count() > 3 and i >= 2 and Line(currPoint+movedBy.toPointF(), prevPoint).lenght() <= 2) {
                        wire.moveLineSegmentBy(i-2, movedBy);
                    }

                    // The line is horizontal
                    if (line.isHorizontal()) {
                        wire.movePointBy(i-1, QVector2D(0, movedBy.y()));
                    }

                    // The line is vertical
                    else if (line.isVertical()) {
                        wire.movePointBy(i-1, QVector2D(movedBy.x(), 0));
                    }
                }

                // Move next point
                if (i < wire.pointsRelative().count()-1) {
                    QPointF nextPoint = wire.pointsRelative().at(i+1);
                    Line line(currPoint, nextPoint);

                    // Make sure that two wire points never collide
                    if (wire.pointsRelative().count() > 3 and Line(currPoint+movedBy.toPointF(), nextPoint).lenght() <= 2) {
                        wire.moveLineSegmentBy(i+1, movedBy);
                    }

                    // The line is horizontal
                    if (line.isHorizontal()) {
                        wire.movePointBy(i+1, QVector2D(0, movedBy.y()));
                    }

                    // The line is vertical
                    else if (line.isVertical()) {
                        wire.movePointBy(i+1, QVector2D(movedBy.x(), 0));
                    }
                }
            }

            // Move the actual point itself
            wire.movePointBy(i, movedBy);

            break;
        }
    }
}

QList<std::shared_ptr<Wire>> Scene::wiresConnectedTo(const Node& node, const QVector2D& offset) const
{
    QList<std::shared_ptr<Wire>> list;

    for (auto& wire : wires()) {
        for (const WirePoint& wirePoint : wire->wirePointsAbsolute()) {
            for (const QPointF& connectionPoint : node.connectionPointsAbsolute()) {
                if (QVector2D(wirePoint.toPointF() - (connectionPoint + offset.toPointF())).length() < 0.001f) {
                    list.append(wire);
                    break;
                }
            }
        }
    }

    return list;
}

void Scene::addWireNet(const std::shared_ptr<WireNet>& wireNet)
{
    // Sanity check
    if (!wireNet) {
        return;
    }

    // Setup
    connect(wireNet.get(), &WireNet::pointMoved, this, &Scene::wirePointMoved);
    connect(wireNet.get(), &WireNet::highlightChanged, this, &Scene::wireNetHighlightChanged);

    // Keep track of stuff
    _nets.append(wireNet);
}

QList<Item*> Scene::itemsAt(const QPointF& scenePos, Qt::SortOrder order) const
{
    QList<Item*> list;

    for (auto& graphicsItem : QGraphicsScene::items(scenePos, Qt::IntersectsItemShape, order)) {
        Item* item = qgraphicsitem_cast<Item*>(graphicsItem);
        if (item) {
            list << item;
        }
    }

    return list;
}

std::shared_ptr<Item> Scene::sharedItemPointer(const Item& item) const
{
    for (const auto& sharedPointer : _items) {
        if (sharedPointer.get() == &item) {
            return sharedPointer;
        }
    }

    return nullptr;
}

void Scene::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    event->accept();

    switch (_mode) {
    case NormalMode:
    {
        // Reset stuff
        _newWire.reset();

        // Handle selections
        QGraphicsScene::mousePressEvent(event);

        // Store the initial position of all the selected items
        _initialItemPositions.clear();
        for (auto& item: selectedItems()) {
            if (item) {
                _initialItemPositions.insert(item, item->pos());
            }
        }

        // Store the initial cursor position
        _initialCursorPosition = event->scenePos();

        break;
    }

    case WireMode:
    {

        // Left mouse button
        if (event->button() == Qt::LeftButton) {

            // Start a new wire if there isn't already one. Else continue the current one.
            if (!_newWire) {
                if (_wireFactory) {
                    _newWire.reset(_wireFactory().release());
                } else {
                    _newWire = std::make_shared<Wire>();
                }
                _undoStack->push(new CommandItemAdd(this, _newWire));
            }

            // Snap to grid
            const QPointF& snappedPos = _settings.snapToGrid(event->scenePos());
            _newWire->appendPoint(snappedPos);
            _newWireSegment = true;
        }

        break;
    }
    }

    _lastMousePos = event->scenePos();
}

void Scene::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    event->accept();

    switch (_mode) {
    case NormalMode:
    {
        QGraphicsScene::mouseReleaseEvent(event);
        // Move if none of the items is being resized or rotated
        bool moving = true;
        for (auto item : selectedItems()) {
            Node* node = qgraphicsitem_cast<Node*>(item.get());
            if (node && node->mode() != Node::None) {
                moving = false;
                break;
            }
        }

        // Reset the position for every selected item and
        // apply the translation through the undostack
        if (moving) {
            for (auto& i: selectedItems()) {
                Item* item = qgraphicsitem_cast<Item*>(i.get());
                // Move the item if it is movable and it was previously registered by the mousePressEvent
                if (item and item->isMovable() and _initialItemPositions.contains(i)) {
                    QVector2D moveBy(item->pos() - _initialItemPositions.value(i));
                    if (!moveBy.isNull()) {
                        // Move the item to its initial position
                        item->setPos(_initialItemPositions.value(i));
                        // Apply the translation
                        _undoStack->push(new CommandItemMove(QVector<std::shared_ptr<Item>>() << i, moveBy));
                    }
                }
            }
        }
        break;
    }

    case WireMode:
    {
        // Right mouse button: Abort wire mode
        if (event->button() == Qt::RightButton) {

            // Change the mode back to NormalMode if nothing below cursor
            if (QGraphicsScene::items(event->scenePos()).isEmpty()) {
                setMode(NormalMode);
            }

            // Show the context menu stuff
            QGraphicsScene::mouseReleaseEvent(event);
        }

        break;
    }
    }

    _lastMousePos = event->lastScenePos();
}

void Scene::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    event->accept();

    // Retrieve the new mouse position
    QPointF newMousePos = event->scenePos();
    QVector2D movedBy(event->scenePos() - event->lastScenePos());

    switch (_mode) {

    case NormalMode:
    {
        // Let the base class handle the basic stuff
        // Note that we DO NOT want this in WireMode to prevent highlighting of the wires
        // during placing a new wire.
        QGraphicsScene::mouseMoveEvent(event);

        // Move, resize or rotate if supposed to
        if (event->buttons() & Qt::LeftButton) {
            // Figure out if we're moving a node
            bool movingNode = false;
            for (auto item : selectedItems()) {
                Node* node = qgraphicsitem_cast<Node*>(item.get());
                if (node && node->mode() == Node::None) {
                    movingNode = true;
                    break;
                }
            }

            // Move all selected items
            if (movingNode) {
                for (auto& i : selectedItems()) {
                    Item* item = qgraphicsitem_cast<Item*>(i.get());
                    if (item and item->isMovable()) {
                        // Calculate by how much the item was moved
                        QPointF moveBy = _initialItemPositions.value(i) + newMousePos - _initialCursorPosition - item->pos();
                        // Apply the custom scene snapping
                        moveBy = itemsMoveSnap(i, QVector2D(moveBy)).toPointF();
                        // Move the item
                        item->setPos(item->pos() + moveBy);
                    }
                }
            }
        }

        break;
    }

    case WireMode:
    {
        // Make sure that there's a wire
        if (!_newWire) {
            break;
        }

        // Transform mouse coordinates to grid positions (snapped to nearest grid point)
        const QPointF& snappedPos = _settings.snapToGrid(event->scenePos());

        // Add a new wire segment. Only allow straight angles (if supposed to)
        if (_settings.routeStraightAngles) {
            if (_newWireSegment) {
                // Remove the last point if there was a previous segment
                if (_newWire->pointsRelative().count() > 1) {
                    _newWire->removeLastPoint();
                }

                // Create the intermediate point that creates the straight angle
                WirePoint prevNode(_newWire->pointsRelative().at(_newWire->pointsRelative().count()-1));
                QPointF corner(prevNode.x(), snappedPos.y());
                if (_invertWirePosture) {
                    corner.setX(snappedPos.x());
                    corner.setY(prevNode.y());
                }

                // Add the two new points
                _newWire->appendPoint(corner);
                _newWire->appendPoint(snappedPos);

                _newWireSegment = false;
            } else {
                // Create the intermediate point that creates the straight angle
                WirePoint p1(_newWire->pointsRelative().at(_newWire->pointsRelative().count()-3));
                QPointF p2(p1.x(), snappedPos.y());
                QPointF p3(snappedPos);
                if (_invertWirePosture) {
                    p2.setX(p3.x());
                    p2.setY(p1.y());
                }

                // Modify the actual wire
                _newWire->movePointTo(_newWire->pointsRelative().count()-2, p2);
                _newWire->movePointTo(_newWire->pointsRelative().count()-1, p3);
            }
        } else {
            // Don't care about angles and stuff. Fuck geometry, right?
            if (_newWire->pointsRelative().count() > 1) {
                _newWire->movePointTo(_newWire->pointsRelative().count()-1, snappedPos);
            } else {
                _newWire->appendPoint(snappedPos);
            }
        }

        break;
    }

    }

    // Save the last mouse position
    _lastMousePos = newMousePos;
}

void Scene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
    event->accept();

    switch (_mode) {
    case NormalMode:
    {
        QGraphicsScene::mouseDoubleClickEvent(event);
        return;
    }

    case WireMode:
    {

        // Only do something if there's a wire
        if (_newWire && _newWire->pointsRelative().count() > 1) {
            bool wireIsFloating = true;

            // Get rid of the last point as mouseDoubleClickEvent() is following mousePressEvent()
            _newWire->removeLastPoint();

            // Check whether the wire was connected to a connector
            for (const QPointF& connectionPoint : connectionPoints()) {
                if (connectionPoint == _newWire->pointsRelative().last()) {
                    wireIsFloating = false;
                    break;
                }
            }

            // Check wether the wire was connected to another wire
            for (const auto& wire : wires()) {
                if (wire->pointIsOnWire(_newWire->pointsRelative().last())) {
                    wireIsFloating = false;
                    break;
                }
            }

            // Notify the user if the wire ended up on a non-valid thingy
            if (wireIsFloating) {
                QMessageBox msgBox;
                msgBox.setWindowTitle("Wire mode");
                msgBox.setIcon(QMessageBox::Information);
                msgBox.setText("A wire must end on either:\n"
                               "  + A node connector\n"
                               "  + A wire\n");
                msgBox.exec();

                _newWire->removeLastPoint();

                return;
            }

            // Finish the current wire
            _newWire->setAcceptHoverEvents(true);
            _newWire->setFlag(QGraphicsItem::ItemIsSelectable, true);
            _newWire->simplify();
            _newWire.reset();

            return;
        }

        return;
    }

    }
}

void Scene::dragEnterEvent(QGraphicsSceneDragDropEvent* event)
{
    // Create a list of mime formats we can handle
    QStringList mimeFormatsWeCanHandle {
        MIME_TYPE_NODE,
    };

    // Check whether we can handle this drag/drop
    for (const QString& format : mimeFormatsWeCanHandle) {
        if (event->mimeData()->hasFormat(format)) {
            clearSelection();
            event->acceptProposedAction();
            return;
        }
    }

    event->ignore();
}

void Scene::dragMoveEvent(QGraphicsSceneDragDropEvent* event)
{
    event->acceptProposedAction();
}

void Scene::dragLeaveEvent(QGraphicsSceneDragDropEvent* event)
{
    event->acceptProposedAction();
}

void Scene::dropEvent(QGraphicsSceneDragDropEvent* event)
{
    event->accept();

    // Check the mime data
    const QMimeData* mimeData = event->mimeData();
    if (!mimeData) {
        return;
    }

    // Nodes
    if (mimeData->hasFormat(MIME_TYPE_NODE)) {
        // Get the ItemMimeData
        const ItemMimeData* mimeData = qobject_cast<const ItemMimeData*>(event->mimeData());
        if (!mimeData) {
            return;
        }

        // Get the Item
        auto item = mimeData->item();
        if (!item) {
            return;
        }

        // Add to the scene
        item->setPos(event->scenePos());
        _undoStack->push(new CommandItemAdd(this, std::move(item)));
    }
}

void Scene::drawBackground(QPainter* painter, const QRectF& rect)
{
    const QPointF& pixmapTopleft = rect.topLeft() - sceneRect().topLeft();

    painter->drawPixmap(rect, _backgroundPixmap, QRectF(pixmapTopleft.x(), pixmapTopleft.y(), rect.width(), rect.height()));
}

QVector2D Scene::itemsMoveSnap(const std::shared_ptr<Item>& items, const QVector2D& moveBy) const
{
    Q_UNUSED(items);

    return moveBy;
}

void Scene::renderCachedBackground()
{
    // Create the pixmap
    QRect rect = sceneRect().toRect();
    if (rect.isNull() or !rect.isValid()) {
        return;
    }
    QPixmap pixmap(rect.width(), rect.height());

    // Grid pen
    QPen gridPen;
    gridPen.setStyle(Qt::SolidLine);
    gridPen.setColor(Qt::gray);
    gridPen.setCapStyle(Qt::RoundCap);
    gridPen.setWidth(_settings.gridPointSize);

    // Grid brush
    QBrush gridBrush;
    gridBrush.setStyle(Qt::NoBrush);

    // Create a painter
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, _settings.antialiasing);

    // Draw background
    pixmap.fill(Qt::white);

    // Draw the grid if supposed to
    if (_settings.showGrid and (_settings.gridSize > 0)) {
        qreal left = int(rect.left()) - (int(rect.left()) % _settings.gridSize);
        qreal top = int(rect.top()) - (int(rect.top()) % _settings.gridSize);

        // Create a list of points
        QVector<QPointF> points;
        for (qreal x = left; x < rect.right(); x += _settings.gridSize) {
            for (qreal y = top; y < rect.bottom(); y += _settings.gridSize) {
                points.append(QPointF(x,y));
            }
        }

        // Draw the actual grid points
        painter.setPen(gridPen);
        painter.setBrush(gridBrush);
        painter.drawPoints(points.data(), points.size());
    }

    // Mark the origin if supposed to
    if (_settings.debug) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QBrush(Qt::red));
        painter.drawEllipse(-6, -6, 12, 12);
    }

    painter.end();

    // Update
    _backgroundPixmap = pixmap;
    update();
}

void Scene::setupNewItem(Item& item)
{
    // Set settings
    item.setSettings(_settings);

    // Connections
    connect(&item, &Item::moved, this, &Scene::itemMoved);
    connect(&item, &Item::rotated, this, &Scene::itemRotated);
}

QList<QPointF> Scene::connectionPoints() const
{
    QList<QPointF> list;

    for (const auto& node : nodes()) {
        list << node->connectionPointsAbsolute();
    }

    return list;
}
