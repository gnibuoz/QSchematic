#include <QApplication>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QCursor>
#include <QtMath>
#include "node.h"
#include "label.h"
#include "itemfactory.h"
#include "../commands/commandnoderesize.h"
#include "../commands/commandnoderotate.h"
#include "../utils.h"
#include "../scene.h"

const QColor COLOR_HIGHLIGHTED = QColor(Qt::blue);
const QColor COLOR_BODY_FILL   = QColor(Qt::green);
const QColor COLOR_BODY_BORDER = QColor(Qt::black);
const qreal PEN_WIDTH          = 1.5;

namespace QSchematic {

// NOTE: Temporary to load design files saved before the change to QS::Node as
// primary base item
auto get_item_maybe_node(const Gpds::Container& container) -> Gpds::Container* {
    auto a = container.getValue<Gpds::Container*>("item");
    if (auto b = a->getValue<Gpds::Container*>("node")) {
        return b;
    }
    else {
        return a;
    }
}

}

using namespace QSchematic;

const int DEFAULT_WIDTH     = 160;
const int DEFAULT_HEIGHT    = 240;

Node::Node(int type, QGraphicsItem* parent) :
    Item(type, parent),
    _interactionMode(None),
    _size(DEFAULT_WIDTH, DEFAULT_HEIGHT),
    _allowMouseResize(true),
    _allowMouseRotate(true),
    _connectorsMovable(false),
    _connectorsSnapToGrid(true),
    _connectorsSnapPolicy(Connector::NodeSizerectOutline)
{
}

Gpds::Container Node::toContainer() const
{
    // Connectors configuration
    Gpds::Container connectorsConfigurationContainer;
    connectorsConfigurationContainer.addValue("movable", connectorsMovable());
    connectorsConfigurationContainer.addValue("snap_policy", connectorsSnapPolicy());
    connectorsConfigurationContainer.addValue("snap_to_grid", connectorsSnapToGrid());

    // Connectors
    Gpds::Container connectorsContainer;
    for (const auto& connector : connectors()) {
        if ( _specialConnectors.contains( connector ) ) {
            continue;
        }

        connectorsContainer.addValue("connector", connector->toContainer());
    }

    // Root
    Gpds::Container root;
    addItemTypeIdToContainer(root);
    root.addValue("item", Item::toContainer());
    root.addValue("width", size().width());
    root.addValue("height", size().height());
    root.addValue("allow_mouse_resize", allowMouseResize());
    root.addValue("allow_mouse_rotate", allowMouseRotate());
    root.addValue("connectors_configuration", connectorsConfigurationContainer);
    root.addValue("connectors", connectorsContainer);

    return root;
}

void Node::fromContainer(const Gpds::Container& container)
{
    // Root
    Item::fromContainer( *container.getValue<Gpds::Container*>( "item" ) );
    setSize( container.getValue<double>( "width" ), container.getValue<double>( "height" ) );
    setAllowMouseResize( container.getValue<bool>( "allow_mouse_resize", true ) );
    setAllowMouseRotate( container.getValue<bool>( "allow_mouse_rotate", true ) );

    // Connectors configuration
    const Gpds::Container* connectorsConfigurationContainer = container.getValue<Gpds::Container*>( "connectors_configuration" );
    if (connectorsConfigurationContainer) {
        setConnectorsMovable( connectorsConfigurationContainer->getValue<bool>( "movable" ) );
        setConnectorsSnapPolicy( static_cast<Connector::SnapPolicy>( connectorsConfigurationContainer->getValue<int>( "snap_policy" ) ) );
        setConnectorsSnapToGrid( connectorsConfigurationContainer->getValue<bool>( "snap_to_grid" ) );
    }

    // Connectors
    const Gpds::Container* connectorsContainer = container.getValue<Gpds::Container*>( "connectors" );
    if (connectorsContainer) {
        clearConnectors();
        for (const Gpds::Container* connectorContainer : connectorsContainer->getValues<Gpds::Container*>( "connector" ) ) {
            auto connector = std::dynamic_pointer_cast<Connector>(ItemFactory::instance().fromContainer(*connectorContainer));
            if (!connector) {
                continue;
            }
            connector->fromContainer(*connectorContainer);
            addConnector(connector);
        }
    }
}

std::shared_ptr<Item> Node::deepCopy() const
{
    auto clone = std::make_shared<Node>(type(), parentItem());
    copyAttributes(*(clone.get()));

    return clone;
}

void Node::copyAttributes(Node& dest) const
{
    // Base class
    Item::copyAttributes(dest);

    // Connectors
    dest.clearConnectors();
    for (const auto& connector : _connectors) {
        if ( _specialConnectors.contains( connector ) ) {
            continue;
        }

        auto connectorClone = std::dynamic_pointer_cast<Connector>(connector->deepCopy());
        connectorClone->setParentItem(&dest);
        dest._connectors << connectorClone;
    }

    // Attributes
    dest._interactionMode = _interactionMode;
    dest._interactionLastMousePosWithGridMove = _interactionLastMousePosWithGridMove;
    dest._interactionResizeHandle = _interactionResizeHandle;
    dest._size = _size;
    dest._allowMouseResize = _allowMouseResize;
    dest._allowMouseRotate = _allowMouseRotate;
    dest._connectorsMovable = _connectorsMovable;
    dest._connectorsSnapPolicy = _connectorsSnapPolicy;
    dest._connectorsSnapToGrid = _connectorsSnapToGrid;
    dest._specialConnectors = _specialConnectors;
}

Node::Mode Node::mode() const
{
    return _interactionMode;
}

void Node::setSize(const QSizeF& size)
{
    // short circuit when no effective change at all times as a manner of policy
    if (size == _size) {
        return;
    }

    // Boundary checks
    if (size.width() < 1 or size.height() < 1) {
        return;
    }

    QSizeF oldSize = _size;

    prepareGeometryChange();

    _size = size;

    // Move connectors
    for (const auto& connector: connectors()) {
        if (qFuzzyCompare(connector->posX(), oldSize.width()) or
            connector->posX() > size.width())
        {
            connector->setX(size.width());
        }
        if (qFuzzyCompare(connector->posY(), oldSize.height()) or
            connector->posY() > size.height())
        {
            connector->setY(size.height());
        }
    }

    setTransformOriginPoint(sizeRect().center());

    sizeChangedEvent();
    emit sizeChanged();
}

void Node::setSize(qreal width, qreal height)
{
    return setSize(QSizeF(width, height));
}

void Node::setWidth(qreal width)
{
    setSize( width, size().height() );
}

void Node::setHeight(qreal height)
{
    setSize( size().width(), height );
}

QSizeF Node::size() const
{
    return _size;
}

QRectF Node::sizeRect() const
{
    return QRectF(0, 0, _size.width(), _size.height());
}

qreal Node::width() const
{
    return _size.width();
}

qreal Node::height() const
{
    return _size.height();
}

void Node::setAllowMouseResize(bool enabled)
{
    _allowMouseResize = enabled;
}

void Node::setAllowMouseRotate(bool enabled)
{
    _allowMouseRotate = enabled;
}

bool Node::allowMouseResize() const
{
    return _allowMouseResize;
}

bool Node::allowMouseRotate() const
{
    return _allowMouseRotate;
}

void Node::addSpecialConnector(const std::shared_ptr<Connector>& connector)
{
    _specialConnectors.push_back( connector );

    addConnector( connector );
}

QMap<RectanglePoint, QRectF> Node::resizeHandles() const
{
    QMap<RectanglePoint, QRectF> map;
    const int& resizeHandleSize = _settings.resizeHandleSize;

    const QRectF& r = sizeRect();

    // Corners
    map.insert(RectanglePointBottomRight, QRectF(r.bottomRight()+QPointF(1,1)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize)));
    map.insert(RectanglePointBottomLeft, QRectF(r.bottomLeft()+QPointF(1,1)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize)));
    map.insert(RectanglePointTopRight, QRectF(r.topRight()+QPointF(1,1)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize)));
    map.insert(RectanglePointTopLeft, QRectF(r.topLeft()+QPointF(1,1)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize)));

    // Sides
    if (r.topRight().x() - r.topLeft().x() > 7*resizeHandleSize) {
        map.insert(RectanglePointTop, QRectF(Utils::centerPoint(r.topRight(), r.topLeft())+QPointF(1,1)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize)));
        map.insert(RectanglePointBottom, QRectF(Utils::centerPoint(r.bottomRight(), r.bottomLeft())+QPointF(1,1)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize)));
    }
    if (r.bottomLeft().y() - r.topLeft().y() > 7*resizeHandleSize) {
        map.insert(RectanglePointRight, QRectF(Utils::centerPoint(r.topRight(), r.bottomRight())+QPointF(1,0)-QPoint(resizeHandleSize, resizeHandleSize), QSize(2*resizeHandleSize, 2*resizeHandleSize)));
        map.insert(RectanglePointLeft, QRectF(Utils::centerPoint(r.bottomLeft(), r.topLeft())+QPointF(1,0)-QPoint(resizeHandleSize, resizeHandleSize), QSize(2*resizeHandleSize, 2*resizeHandleSize)));
    }

    return map;
}

QRectF Node::rotationHandle() const
{
    const QRectF& r = sizeRect();
    const int& resizeHandleSize = _settings.resizeHandleSize;
    return QRectF(Utils::centerPoint(r.topRight(), r.topLeft())+QPointF(1,-resizeHandleSize*3)-QPointF(resizeHandleSize, resizeHandleSize), QSizeF(2*resizeHandleSize, 2*resizeHandleSize));
}

bool Node::addConnector(const std::shared_ptr<Connector>& connector)
{
    if (!connector) {
        return false;
    }

    connector->setParentItem(this);
    connector->setMovable(_connectorsMovable);
    connector->setSnapPolicy(_connectorsSnapPolicy);
    connector->setSnapToGrid(_connectorsSnapToGrid);

    _connectors << connector;

    return true;
}

bool Node::removeConnector(const std::shared_ptr<Connector>& connector)
{
    if (!connector or !_connectors.contains(connector)) {
        return false;
    }

    connector->setParentItem(nullptr);

    _connectors.removeAll(connector);
    _specialConnectors.removeAll(connector);

    return true;
}

void Node::clearConnectors()
{
    // Remove from scene
    auto s = scene();
    if (s) {
        for (auto connector : _connectors) {
            s->removeItem(connector);
        }
    }

    // Clear the local list
    _connectors.clear();
}

QList<std::shared_ptr<Connector>> Node::connectors() const
{
    return _connectors;
}

QList<QPointF> Node::connectionPointsRelative() const
{
    QList<QPointF> list;

    for (const auto& connector : _connectors) {
        QPointF pos = connector->pos();
        // Rotate the position around to the node's origin
        {
            QPointF d = transformOriginPoint() - pos;
            qreal angle = rotation() * M_PI / 180;
            QPointF rotated;
            rotated.setX(qCos(angle) * d.rx() - qSin(angle) * d.ry());
            rotated.setY(qSin(angle) * d.rx() + qCos(angle) * d.ry());
            pos = transformOriginPoint() - rotated;
        }
        list << connector->connectionPoint() + pos;
    }

    return list;
}

QList<QPointF> Node::connectionPointsAbsolute() const
{
    QList<QPointF> list(connectionPointsRelative());

    for (QPointF& point : list) {
        point += pos();
    }

    return list;
}

void Node::setConnectorsMovable(bool enabled)
{
    // Update connectors
    for (auto connector : _connectors) {
        connector->setMovable(enabled);
    }

    // Update local
    _connectorsMovable = enabled;
}

bool Node::connectorsMovable() const
{
    return _connectorsMovable;
}

void Node::setConnectorsSnapPolicy(Connector::SnapPolicy policy)
{
    // Update connectors
    for (auto connector : _connectors) {
        connector->setSnapPolicy(policy);
    }

    // Update local
    _connectorsSnapPolicy = policy;
}

Connector::SnapPolicy Node::connectorsSnapPolicy() const
{
    return _connectorsSnapPolicy;
}

void Node::setConnectorsSnapToGrid(bool enabled)
{
    // Update connectors
    for (auto connector : _connectors) {
        connector->setSnapToGrid(enabled);
    }

    // Update local
    _connectorsSnapToGrid = enabled;
}

bool Node::connectorsSnapToGrid() const
{
    return _connectorsSnapToGrid;
}

void Node::alignConnectorLabels() const
{
    for (auto connector : _connectors) {
        Q_ASSERT(connector);
        connector->alignLabel();
    }
}

auto Node::sizeChangedEvent() -> void
{
    // default implementation is noop
}

void Node::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    event->accept();

    // Let the base class handle selection and so on
    Item::mousePressEvent(event);

    // Presume no mode
    _interactionMode = None;

    // Check if clicked on a resize handle
    if (isSelected() && _allowMouseResize) {
        auto handles = resizeHandles();
        auto it = handles.constBegin();
        while (it != handles.constEnd()) {
            if (it.value().contains(event->pos().toPoint())) {
                _interactionMode = Resize;
                _interactionLastMousePosWithGridMove = event->scenePos();
                _interactionResizeHandle = it.key();
                break;
            }
            it++;
        }
    }

    // Rotation
    if (isSelected() && _allowMouseRotate) {
        if (rotationHandle().contains(event->pos().toPoint())) {
            _interactionMode = Rotate;
        }
    }

    if (_interactionMode != None) {
        interactionBeginEvent(event);
    }
}

void Node::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    if (_interactionMode != None) {
        interactionEndEvent(event);
    }

    event->accept();

    Item::mouseReleaseEvent(event);

    _interactionMode = None;
}

void Node::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    Q_ASSERT( scene() );

    event->accept();

    QPointF newMousePos( event->scenePos() );

    switch (_interactionMode) {
    case None:
    {
        Item::mouseMoveEvent(event);

        break;
    }

    case Resize:
    {
        // Sanity check
        if (!_allowMouseResize) {
            qFatal("Node::mouseMoveEvent(): _mode is 'Resize' although _allowMouseResize is false");
            break;
        }

        // Left mouse button to move
        if (event->buttons() & Qt::LeftButton) {

            if ( canSnapToGrid() ) {
                newMousePos = _settings.snapToGrid( newMousePos );
            }

            // Calculate mouse movement in grid units
            QPointF d( newMousePos - _interactionLastMousePosWithGridMove );

            // Rotate mouse movement
            {
                qreal angle = 2*M_PI - rotation() * M_PI / 180;
                qreal x = qCos(angle) * d.rx() - qSin(angle) * d.ry();
                qreal y = qSin(angle) * d.rx() + qCos(angle) * d.ry();
                d = QPointF(x, y);
            }

            qreal dx = d.x();
            qreal dy = d.y();

            // Don't do anything if there's nothing to do
            if (qFuzzyIsNull(dx) and qFuzzyIsNull(dy)) {
                break;
            }

            // Track this
            _interactionLastMousePosWithGridMove = newMousePos;

            // Perform resizing
            qreal newX = posX();
            qreal newY = posY();
            qreal newWidth = _size.width();
            qreal newHeight = _size.height();
            switch (_interactionResizeHandle) {
            case RectanglePointTopLeft:
                newX += dx;
                newY += dy;
                newWidth -= dx;
                newHeight -= dy;
                break;

            case RectanglePointTop:
                newY += dy;
                newHeight -= dy;
                break;

            case RectanglePointTopRight:
                newY += dy;
                newWidth += dx;
                newHeight -= dy;
                break;

            case RectanglePointRight:
                newWidth += dx;
                break;

            case RectanglePointBottomRight:
                newWidth += dx;
                newHeight += dy;
                break;

            case RectanglePointBottom:
                newHeight += dy;
                break;

            case RectanglePointBottomLeft:
                newX += dx;
                newWidth -= dx;
                newHeight += dy;
                break;

            case RectanglePointLeft:
                newX += dx;
                newWidth -= dx;
                break;
            }

            // Snap to grid (if supposed to)
            QPointF newPos( newX, newY );
            QSizeF newSize( newWidth, newHeight );
            if ( canSnapToGrid() ) {
                newSize = _settings.snapToGrid( newSize );
            }

            // Minimum size
            if (newSize.height() < 1) {
                newSize.setHeight(1);
                if (not qFuzzyCompare(newPos.ry(), pos().ry())) {
                    newPos.setY(posY() + _size.height() - 1);
                }
            }
            if (newSize.width() < 1) {
                newSize.setWidth(1);
                if (not qFuzzyCompare(newPos.rx(), pos().rx())) {
                    newPos.setX(posX() + _size.width() - 1);
                }
            }

            // Correct origin
            auto newOrigin = QPointF(newSize.width()/2, newSize.height()/2)+newPos - pos();
            auto angle = rotation() * M_PI / 180;
            auto offset = newOrigin - transformOriginPoint();
            auto newOriginRotated = QPointF(qCos(angle) * offset.rx() - qSin(angle) * offset.ry(), qSin(angle) * offset.rx() + qCos(angle) * offset.ry());
            auto correction = newOriginRotated - offset;
            newPos += correction;

            // Apply
            interactionChangeResizeEvent(newPos, newSize);
        }

        break;
    }
    case Rotate:
    {
        // Sanity check
        if (!_allowMouseRotate) {
            qFatal("Node::mouseMoveEvent(): _mode is 'Rotate' although _allowMouseRotate is false");
            break;
        }

        auto center = sizeRect().center() + pos();
        auto delta = center - newMousePos;
        auto angle = fmod(qAtan2(delta.ry(), delta.rx())*180/M_PI + 270, 360);
        if (QApplication::keyboardModifiers() == Qt::ShiftModifier) {
            angle = qRound(angle/15)*15;
        }
        interactionChangeRotateEvent(angle);
    }
    }
}

void Node::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
{
    editStatusChange(true);
}

void Node::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    Item::hoverEnterEvent(event);
}

void Node::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    Item::hoverLeaveEvent(event);

    unsetCursor();
}

void Node::hoverMoveEvent(QGraphicsSceneHoverEvent* event)
{
    Item::hoverMoveEvent(event);

    // Set the cursor
    {
        setCursor(Qt::ArrowCursor);

        // If selected, we should adjust the cursor for the resize handles
        if (isSelected() and _allowMouseResize) {
            auto handles = resizeHandles();
            auto it = handles.constBegin();
            while (it != handles.constEnd()) {
                if (it.value().contains(event->pos().toPoint())) {
                    switch (it.key()) {
                    case RectanglePointTopLeft:
                    case RectanglePointBottomRight:
                        setCursor(Qt::SizeFDiagCursor);
                        break;

                    case RectanglePointBottom:
                    case RectanglePointTop:
                        setCursor(Qt::SizeVerCursor);
                        break;

                    case RectanglePointBottomLeft:
                    case RectanglePointTopRight:
                        setCursor(Qt::SizeBDiagCursor);
                        break;

                    case RectanglePointRight:
                    case RectanglePointLeft:
                        setCursor(Qt::SizeHorCursor);
                        break;
                    }
                    break;
                }
                it++;
            }
        }
        if (isSelected() and _allowMouseRotate) {
            if (rotationHandle().contains(event->pos().toPoint())) {
                setCursor(Qt::SizeAllCursor);
            }
        }
    }
}

void Node::interactionBeginEvent(QGraphicsSceneMouseEvent* event) {
    Q_UNUSED(event)
}

void Node::interactionChangeResizeEvent(QPointF newPos, QSizeF newSize) {
    scene()->undoStack()->push(new QSchematic::CommandNodeResize(this, newPos, newSize));
}

void Node::interactionChangeRotateEvent(qreal newAngle) {
    scene()->undoStack()->push(new QSchematic::CommandNodeRotate(this, newAngle));
}

void Node::interactionEndEvent(QGraphicsSceneMouseEvent* event) {
    Q_UNUSED(event)
}

QRectF Node::boundingRect() const
{
    // Body rect
    QRectF rect = QRectF(QPoint(0, 0), _size);
    qreal adj = 0.0;

    // Add half the pen width
    adj = qMax(adj, PEN_WIDTH / 2.0);

    // Add resize handles
    if (isSelected() and _allowMouseResize) {
        adj = qMax(adj, static_cast<qreal>(_settings.resizeHandleSize));
    }

    // Add highlight rect
    if (isHighlighted()) {
        adj = qMax(adj, static_cast<qreal>(_settings.highlightRectPadding));
    }

    // adjustment should be done before union with other rects, otherwise the
    // relative increase is added to outliers too
    rect = rect.adjusted(-adj, -adj, adj, adj);

    // Rotate handle
    if (isSelected() and _allowMouseRotate) {
        rect = rect.united(rotationHandle());
    }

    return rect;
}

// probably easiest way of fixing "area selection" bug (note rect-selection and clicks etc. use same collision-compare source). Should go into #QSchematic::Node
auto Node::shape() const -> QPainterPath
{
    auto path = QPainterPath();

    // selection rubber-bounds shouldn't match meta-decorations like rotation handles etc.
    // NOTE: we assume user-interaction is rubber-selection if QSchematic::Node::mode() is `None`
    if (scene()->isVisualUserInteractionInProgress() and _interactionMode == None) {
        path.addRect(sizeRect());
    }
    else {
        path.addRect(boundingRect());
    }

    return path;
}

void Node::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    // Draw the bounding rect if debug mode is enabled
    if (_settings.debug) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(Qt::red));
        painter->drawRect(boundingRect());
    }

    // Highlight rectangle
    if (isHighlighted()) {
        // Highlight pen
        QPen highlightPen;
        highlightPen.setStyle(Qt::NoPen);

        // Highlight brush
        QBrush highlightBrush;
        highlightBrush.setStyle(Qt::SolidPattern);
        highlightBrush.setColor(COLOR_HIGHLIGHTED);

        // Highlight rectangle
        painter->setPen(highlightPen);
        painter->setBrush(highlightBrush);
        painter->setOpacity(0.5);
        int adj = _settings.highlightRectPadding;
        painter->drawRoundedRect(sizeRect().adjusted(-adj, -adj, adj, adj), _settings.gridSize/2, _settings.gridSize/2);
    }

    painter->setOpacity(1.0);

    // Body pen
    QPen bodyPen;
    bodyPen.setWidthF(PEN_WIDTH);
    bodyPen.setStyle(Qt::SolidLine);
    bodyPen.setColor(COLOR_BODY_BORDER);

    // Body brush
    QBrush bodyBrush;
    bodyBrush.setStyle(Qt::SolidPattern);
    bodyBrush.setColor(COLOR_BODY_FILL);

    // Draw the component body
    painter->setPen(bodyPen);
    painter->setBrush(bodyBrush);
    painter->drawRoundedRect(sizeRect(), _settings.gridSize/2, _settings.gridSize/2);

    // Resize handles
    if (isSelected() and allowMouseResize()) {
        paintResizeHandles(*painter);
    }

    // Rotate handle
    if (isSelected() and allowMouseRotate()) {
        paintRotateHandle(*painter);
    }
}

void Node::update()
{
    // The item class sets the origin to the center of the bounding box
    // but in this case we want to rotate around the center of the body
    setTransformOriginPoint(sizeRect().center());
    QGraphicsObject::update();
}

// It's more common with rectangular items than not, so let's offer it as a
// default implementation
auto Node::pointsOfInterest() const -> std::vector<QPointF>
{
    const auto rel = [
        w = size().width(),
        h = size().height()
    ](qreal x, qreal y) -> QPointF {
        return { x * w, y * h };
    };

    return {
        rel(0.5, 0.5),
        rel(0, 0),
        rel(0.5, 0),
        rel(1, 0),
        rel(1, 0.5),
        rel(1, 1),
        rel(0.5, 1),
        rel(0, 1),
        rel(0, 0.5),
    };
}

void Node::setHighlightPointOfInterest( const std::optional<QPointF>& point )
{
    _highlightPointOfInterest = point;
}

auto Node::highlightPointOfInterest() -> const std::optional<QPointF>
{
    return _highlightPointOfInterest;
}

void Node::paintPointOfInterest( QPainter& painter )
{
    // Don't do anything if there's no point of interest
    if ( not _highlightPointOfInterest.has_value() ) {
        return;
    }

    // Determine rect
    const auto handleSizing = _settings.resizeHandleSize;
    const QPointF& p = _highlightPointOfInterest.value();
    QRectF rect( p+QPointF( -handleSizing, -handleSizing ), p + QPointF( handleSizing, handleSizing ) );

    // Pen
    QPen pen;
    pen.setCosmetic( true );
    pen.setStyle( Qt::SolidLine );
    pen.setColor( QStringLiteral( "#6ebf75" ) );
    pen.setWidth( 4 );

    // Brush
    QBrush brush;
    brush.setStyle( Qt::NoBrush );

    // Render
    painter.setPen( pen );
    painter.setBrush( brush );
    painter.drawEllipse( rect );
}

auto Node::tempSelectabilityHackPropagationPass(bool flag) -> void
{
    if ( auto parent = dynamic_cast<Node*>(this->parentItem()) ) {
        parent->tempSelectabilityHackPropagationPass(flag);
    }
}


bool Node::canSnapToGrid() const
{
    // Only snap when the rotation is a multiple of 90
    return Item::snapToGrid() and fmod(rotation(), 90) == 0;
}

QVariant Node::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
    switch (change)
    {
    case QGraphicsItem::ItemPositionChange:
    {
        QPointF newPos = value.toPointF();
        if (canSnapToGrid()) {
            // If it is rotated 90 or 270 degrees and the difference between
            // the height and width is odd then the position needs to be
            // offset by half a grid unit vertically and horizontally.
            if ((qFuzzyCompare(qAbs(rotation()), 90) or qFuzzyCompare(qAbs(rotation()), 270)) and
                (fmod(_size.width()/_settings.gridSize - _size.height()/_settings.gridSize, 2) != 0))
            {
                newPos.setX(qCeil(newPos.rx()/_settings.gridSize)*_settings.gridSize);
                newPos.setY(qCeil(newPos.ry()/_settings.gridSize)*_settings.gridSize);
                newPos -= QPointF(_settings.gridSize/2, _settings.gridSize/2);
            } else {
                newPos = _settings.snapToGrid(newPos);
            }
        }
        return newPos;
    }

    case ItemSelectedHasChanged:
        if (isSelected() == false) {
            editStatusChange(false);
        }
        break;

    default:
        return Item::itemChange(change, value);
    }
}

void Node::editStatusChange(bool enabled)
{
    Q_UNUSED(enabled);
    // default is noop
}

void Node::paintResizeHandles(QPainter& painter)
{
    for (const QRectF& rect : resizeHandles()) {
        // Handle pen
        QPen handlePen;
        handlePen.setStyle(Qt::NoPen);
        painter.setPen(handlePen);

        // Handle Brush
        QBrush handleBrush;
        handleBrush.setStyle(Qt::SolidPattern);
        painter.setBrush(handleBrush);

        // Draw the outer handle
        handleBrush.setColor("#3fa9f5");
        painter.setBrush(handleBrush);
        painter.drawRect(rect.adjusted(-handlePen.width(), -handlePen.width(), handlePen.width()/2, handlePen.width()/2));

        // Draw the inner handle
        int adj = _settings.resizeHandleSize/2;
        handleBrush.setColor(Qt::white);
        painter.setBrush(handleBrush);
        painter.drawRect(rect.adjusted(-handlePen.width()+adj, -handlePen.width()+adj, (handlePen.width()/2)-adj, (handlePen.width()/2)-adj));
    }
}

void Node::paintRotateHandle(QPainter& painter)
{
    auto rect = rotationHandle();

    // Handle pen
    QPen handlePen;
    handlePen.setStyle(Qt::NoPen);
    painter.setPen(handlePen);

    // Handle Brush
    QBrush handleBrush;
    handleBrush.setStyle(Qt::SolidPattern);
    painter.setBrush(handleBrush);

    // Draw the outer handle
    handleBrush.setColor("#3fa9f5");
    painter.setBrush(handleBrush);
    painter.drawEllipse(rect.adjusted(-handlePen.width(), -handlePen.width(), handlePen.width()/2, handlePen.width()/2));

    // Draw the inner handle
    int adj = _settings.resizeHandleSize/2;
    handleBrush.setColor(Qt::white);
    painter.setBrush(handleBrush);
    painter.drawEllipse(rect.adjusted(-handlePen.width()+adj, -handlePen.width()+adj, (handlePen.width()/2)-adj, (handlePen.width()/2)-adj));
}
