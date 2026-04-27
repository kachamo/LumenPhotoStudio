// ==============================================================================
// ui/NodeGraphWidget.cpp
// ==============================================================================
#include "NodeGraphWidget.h"

#include <QBrush>
#include <QFont>
#include <QFrame>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QWheelEvent>

namespace {

// Brand accent for selected nodes — same #CCFF00 used elsewhere in the UI.
const QColor kAccentColor(0xCC, 0xFF, 0x00);

// Visual constants.
constexpr qreal kNodeWidth   = 110.0;
constexpr qreal kNodeHeight  = 68.0;
constexpr qreal kNodeRadius  = 8.0;
constexpr qreal kNodeSpacing = 40.0;   // horizontal gap between adjacent nodes

// One pipeline node — a rounded rect with a centered label. Selectable
// via QGraphicsView's default selection machinery; selected state drives
// a brand-color border in paint(). No payload yet — V1 is visual only.
//
// Shape: a vertically-centered rounded rect at the item's local origin.
// Position is the top-left of the rect in scene coords.
class NodeItem : public QGraphicsRectItem
{
public:
    explicit NodeItem(const QString& label, QGraphicsItem* parent = nullptr)
        : QGraphicsRectItem(0, 0, kNodeWidth, kNodeHeight, parent)
    {
        setFlag(QGraphicsItem::ItemIsSelectable, true);
        // Movement disabled in V1 — connection lines are static
        // QGraphicsLineItems with fixed endpoints, so dragging a node
        // would leave the lines pointing at empty space. Future work
        // (real edges that re-route on node move) can re-enable this.
        setBrush(QBrush(QColor(0x16, 0x18, 0x1D)));
        setPen(QPen(QColor(0x2A, 0x2D, 0x35), 1.0));
        setAcceptHoverEvents(true);

        // Centered label as a child item so it moves with the node.
        m_label = new QGraphicsSimpleTextItem(label, this);
        QFont f = m_label->font();
        f.setPointSizeF(f.pointSizeF() * 1.05);
        f.setBold(true);
        m_label->setFont(f);
        m_label->setBrush(QColor(0xE7, 0xE9, 0xEE));

        const QRectF lr = m_label->boundingRect();
        m_label->setPos((kNodeWidth  - lr.width())  * 0.5,
                        (kNodeHeight - lr.height()) * 0.5);
    }

    // Custom paint: rounded rect (QGraphicsRectItem only paints sharp
    // corners) with selection-aware border color.
    void paint(QPainter* p,
               const QStyleOptionGraphicsItem* /*opt*/,
               QWidget* /*widget*/) override
    {
        p->setRenderHint(QPainter::Antialiasing, true);

        const bool selected = isSelected();
        const QColor border = selected ? kAccentColor : QColor(0x2A, 0x2D, 0x35);
        const qreal  width  = selected ? 2.0 : 1.0;

        if (selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(204, 255, 0, 36));
            p->drawRoundedRect(rect().adjusted(-4, -4, 4, 4), kNodeRadius + 4, kNodeRadius + 4);
        }

        p->setBrush(brush());
        p->setPen(QPen(border, width));
        p->drawRoundedRect(rect(), kNodeRadius, kNodeRadius);

        p->setPen(Qt::NoPen);
        p->setBrush(selected ? kAccentColor : QColor(0x65, 0x6B, 0x75));
        p->drawEllipse(QPointF(0.0, rect().height() * 0.5), 4.0, 4.0);
        p->drawEllipse(QPointF(rect().width(), rect().height() * 0.5), 4.0, 4.0);

        // QGraphicsSimpleTextItem (the label child) paints itself via
        // the scene's normal traversal — nothing to do here for it.
    }

private:
    QGraphicsSimpleTextItem* m_label = nullptr;
};

} // namespace

NodeGraphWidget::NodeGraphWidget(QWidget* parent)
    : QGraphicsView(parent)
{
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);

    // Visual style — dark canvas, no scroll bars (we provide pan + zoom
    // gestures instead, which feels more like a node editor and less
    // like a document viewer).
    setBackgroundBrush(QColor(0x0E, 0x0F, 0x12));
    setFrameShape(QFrame::NoFrame);
    setStyleSheet("QGraphicsView { background: #0E0F12; border: 0; }");
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::TextAntialiasing, true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    setDragMode(QGraphicsView::NoDrag);   // we implement panning manually

    buildDefaultGraph();
}

// Lay out the pipeline nodes in a horizontal row, connected by lines. The graph is
// re-fittable; the view's resize anchor recenters nicely. Node positions
// are scene coords, lines are scene-anchored connections from each
// node's right edge to the next node's left edge.
void NodeGraphWidget::buildDefaultGraph()
{
    const QStringList labels = {
        QStringLiteral("Input"),
        QStringLiteral("Lens"),
        QStringLiteral("Transform"),
        QStringLiteral("Tone"),
        QStringLiteral("Color"),
        QStringLiteral("Curves"),
        QStringLiteral("Grading"),
        QStringLiteral("Details"),
        QStringLiteral("Effects"),
        QStringLiteral("Output"),
    };

    QList<NodeItem*> nodes;
    nodes.reserve(labels.size());

    qreal x = 0.0;
    const qreal y = 0.0;
    for (const QString& lbl : labels) {
        auto* node = new NodeItem(lbl);
        node->setPos(x, y);
        m_scene->addItem(node);
        nodes.append(node);
        x += kNodeWidth + kNodeSpacing;
    }

    // Connection lines — straight horizontal segments between adjacent
    // nodes' midpoints. Drawn behind nodes so they don't overlap text.
    QPen linePen(QColor(0x4B, 0x52, 0x5F, 210), 1.4);
    linePen.setCapStyle(Qt::RoundCap);
    for (int i = 0; i + 1 < nodes.size(); ++i) {
        const QPointF lhs = nodes[i]->pos()
            + QPointF(kNodeWidth, kNodeHeight * 0.5);
        const QPointF rhs = nodes[i + 1]->pos()
            + QPointF(0.0, kNodeHeight * 0.5);
        auto* line = new QGraphicsLineItem(QLineF(lhs, rhs));
        line->setPen(linePen);
        line->setZValue(-1.0);   // below nodes
        m_scene->addItem(line);

        auto* dot = new QGraphicsEllipseItem(-3.0, -3.0, 6.0, 6.0);
        dot->setPos(rhs);
        dot->setBrush(QBrush((i % 2 == 0) ? kAccentColor : QColor(0x65, 0x6B, 0x75)));
        dot->setPen(Qt::NoPen);
        dot->setZValue(0.0);
        m_scene->addItem(dot);
    }

    // Set scene rect to a generous padding around the laid-out nodes.
    // Pan + zoom can extend beyond this; QGraphicsView allows free
    // navigation regardless of scene rect, but having a defined rect
    // makes initial fitInView reasonable.
    const qreal totalWidth = (kNodeWidth + kNodeSpacing) * labels.size()
                              - kNodeSpacing;
    const qreal padding = 80.0;
    m_scene->setSceneRect(-padding, -padding,
                           totalWidth + padding * 2,
                           kNodeHeight + padding * 2);
}

void NodeGraphWidget::mousePressEvent(QMouseEvent* event)
{
    // Middle-button pan. Same convention as PreviewWidget — we don't
    // claim left-click pan because that would prevent node selection.
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPanPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void NodeGraphWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        const QPoint delta = event->pos() - m_lastPanPos;
        m_lastPanPos = event->pos();
        // Translate the scrollbars manually since they're hidden.
        // horizontalScrollBar()/verticalScrollBar() drive the view's
        // viewport offset; subtracting moves the canvas in the drag
        // direction.
        horizontalScrollBar()->setValue(
            horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(
            verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void NodeGraphWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_panning && event->button() == Qt::MiddleButton) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

// Ctrl + wheel: zoom anchored at cursor (AnchorUnderMouse handles this
// for us via QGraphicsView's transformation anchor). Plain wheel falls
// through to the base class.
void NodeGraphWidget::wheelEvent(QWheelEvent* event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    const double step = 1.15;
    const double factor = (event->angleDelta().y() > 0) ? step : 1.0 / step;
    // Bound the cumulative scale so users can't zoom into a single
    // pixel or out to nothing.
    const double current = transform().m11();   // uniform scale
    const double target  = current * factor;
    if (target < 0.1 || target > 8.0) {
        event->accept();
        return;
    }
    scale(factor, factor);
    event->accept();
}
