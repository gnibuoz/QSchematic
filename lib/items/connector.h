#pragma once

#include "item.h"

namespace QSchematic {

    class Connector : public Item
    {
        Q_OBJECT
        Q_DISABLE_COPY(Connector)

    public:
        enum SnapPolicy {
            Anywhere,
            NodeSizerect,
            NodeSizerectOutline,
            NodeShape
        };
        Q_ENUM(SnapPolicy)

        Connector(QGraphicsItem* parent = nullptr);
        virtual ~Connector() override = default;

        void setSnapPolicy(SnapPolicy policy);
        SnapPolicy snapPolicy() const;
        void setText(const QString& text);
        QString text() const;
        virtual void update() override;

        QPoint connectionPoint() const;
        virtual QRectF boundingRect() const override;
        virtual QVariant itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value) override;
        virtual void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

    private:
        QPointF snapPointToRect(QPointF point, const QRectF& rect) const;
        QPointF snapPointToRectOutline(const QPointF& point, const QRectF& rect) const;
        QPointF snapPointToPath(const QPointF& point, const QPainterPath& path) const;
        void calculateSymbolRect();
        void calculateTextRect();

        SnapPolicy _snapPolicy;
        QString _text;
        QRectF _symbolRect;
        QRectF _textRect;
        Direction _textDiretion;
    };

}