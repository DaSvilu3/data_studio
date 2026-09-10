#include "ui/DiagramView.h"

#include <QComboBox>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QQueue>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QShowEvent>
#include <QtMath>

#include <algorithm>

#include "query/JoinGraph.h"
#include "ui/Theme.h"

namespace ds {
namespace {

constexpr qreal kNodeWidth = 190;
constexpr qreal kHeaderHeight = 26;
constexpr qreal kRowHeight = 17;
constexpr int kMaxColumnsShown = 9;

}  // namespace

// One table, drawn as a header plus the columns that matter most.
class TableNode : public QGraphicsItem {
 public:
  TableNode(const Table& table, bool isFocus, int hops)
      : table_(table), isFocus_(isFocus), hops_(hops) {
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    setAcceptHoverEvents(true);
    setToolTip(QString::fromStdString(table.name));

    // Keys and relationships first: they are what the diagram is about.
    for (const auto& c : table.columns) {
      bool isFk = false;
      for (const auto& fk : table.foreignKeys) {
        for (const auto& fc : fk.fromColumns) {
          if (iequals(fc, c.name)) isFk = true;
        }
      }
      if (c.primaryKey) keyColumns_.append({QString::fromStdString(c.name),
                                            QStringLiteral("PK")});
      else if (isFk) keyColumns_.append({QString::fromStdString(c.name),
                                         QStringLiteral("FK")});
      else plainColumns_.append({QString::fromStdString(c.name), QString()});
    }
    rows_ = keyColumns_;
    for (const auto& c : plainColumns_) {
      if (rows_.size() >= kMaxColumnsShown) break;
      rows_.append(c);
    }
    hidden_ = static_cast<int>(table.columns.size()) - rows_.size();
  }

  QString tableName() const { return QString::fromStdString(table_.name); }

  QRectF boundingRect() const override {
    const qreal h = kHeaderHeight + rows_.size() * kRowHeight +
                    (hidden_ > 0 ? kRowHeight : 0) + 6;
    return QRectF(0, 0, kNodeWidth, h);
  }

  // Where an edge should meet this node, given where it is coming from.
  QPointF anchorTowards(const QPointF& target) const {
    const QRectF r = boundingRect().translated(pos());
    const QPointF c = r.center();
    QLineF line(c, target);
    // Intersect the centre-to-target line with the node's border.
    const QList<QLineF> edges = {
        QLineF(r.topLeft(), r.topRight()),
        QLineF(r.topRight(), r.bottomRight()),
        QLineF(r.bottomRight(), r.bottomLeft()),
        QLineF(r.bottomLeft(), r.topLeft())};
    for (const QLineF& e : edges) {
      QPointF hit;
      if (line.intersects(e, &hit) == QLineF::BoundedIntersection) return hit;
    }
    return c;
  }

 protected:
  void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
    const QRectF r = boundingRect();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Distance from the focus is encoded as opacity, so the eye lands on the
    // table you asked about.
    const qreal fade = hops_ == 0 ? 1.0 : (hops_ == 1 ? 0.94 : 0.78);
    p->setOpacity(fade);

    QPainterPath path;
    path.addRoundedRect(r, 8, 8);

    p->fillPath(path, theme::surface());
    QPen border(isFocus_ ? theme::accent() : theme::border());
    border.setWidthF(isFocus_ ? 2.0 : 1.0);
    if (isSelected()) {
      border.setColor(theme::accent());
      border.setWidthF(2.0);
    }
    p->setPen(border);
    p->drawPath(path);

    // Header
    QPainterPath header;
    header.addRoundedRect(QRectF(r.left(), r.top(), r.width(), kHeaderHeight),
                          8, 8);
    header.addRect(QRectF(r.left(), r.top() + kHeaderHeight - 8, r.width(), 8));
    p->fillPath(header, isFocus_ ? theme::accent() : theme::surfaceRaised());

    QFont titleFont = p->font();
    titleFont.setBold(true);
    titleFont.setPointSize(11);
    p->setFont(titleFont);
    p->setPen(isFocus_ ? QColor(Qt::white) : theme::textPrimary());
    p->drawText(QRectF(r.left() + 9, r.top(), r.width() - 18, kHeaderHeight),
                Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(tableName(), Qt::ElideMiddle,
                                            static_cast<int>(r.width() - 18)));

    QFont rowFont = p->font();
    rowFont.setBold(false);
    rowFont.setPointSize(10);
    p->setFont(rowFont);

    qreal y = r.top() + kHeaderHeight + 2;
    for (const auto& [name, badge] : rows_) {
      const bool key = !badge.isEmpty();
      p->setPen(key ? (badge == QStringLiteral("PK") ? theme::keyColour()
                                                     : theme::relationColour())
                    : theme::textMuted());
      p->drawText(QRectF(r.left() + 9, y, r.width() - 46, kRowHeight),
                  Qt::AlignVCenter | Qt::AlignLeft,
                  p->fontMetrics().elidedText(name, Qt::ElideRight,
                                              static_cast<int>(r.width() - 46)));
      if (key) {
        p->drawText(QRectF(r.right() - 34, y, 26, kRowHeight),
                    Qt::AlignVCenter | Qt::AlignRight, badge);
      }
      y += kRowHeight;
    }

    if (hidden_ > 0) {
      p->setPen(theme::textMuted());
      QFont small = p->font();
      small.setItalic(true);
      small.setPointSize(9);
      p->setFont(small);
      p->drawText(QRectF(r.left() + 9, y, r.width() - 18, kRowHeight),
                  Qt::AlignVCenter | Qt::AlignLeft,
                  QStringLiteral("+%1 more").arg(hidden_));
    }
  }

 private:
  Table table_;
  bool isFocus_;
  int hops_;
  QList<QPair<QString, QString>> keyColumns_, plainColumns_, rows_;
  int hidden_ = 0;
};

namespace {

// Scroll-wheel zoom, which is what everyone expects from a canvas.
class DiagramCanvas : public QGraphicsView {
 public:
  using QGraphicsView::QGraphicsView;

 protected:
  void wheelEvent(QWheelEvent* event) override {
    if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
      const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
      const qreal next = transform().m11() * factor;
      if (next > 0.15 && next < 4.0) scale(factor, factor);
      event->accept();
      return;
    }
    QGraphicsView::wheelEvent(event);
  }
};

}  // namespace

DiagramView::DiagramView(QWidget* parent) : QWidget(parent) {
  tablePicker_ = new QComboBox(this);
  tablePicker_->setEditable(true);
  tablePicker_->setInsertPolicy(QComboBox::NoInsert);
  tablePicker_->setMinimumWidth(220);
  tablePicker_->setToolTip(QStringLiteral("Table to centre the diagram on"));

  depthPicker_ = new QSpinBox(this);
  depthPicker_->setRange(1, 3);
  depthPicker_->setValue(1);
  depthPicker_->setPrefix(QStringLiteral("depth "));
  depthPicker_->setToolTip(
      QStringLiteral("How many foreign-key hops out from the centre to draw"));

  summary_ = new QLabel(this);
  summary_->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::textMuted().name()));

  scene_ = new QGraphicsScene(this);
  auto* canvas = new DiagramCanvas(scene_, this);
  canvas->setRenderHint(QPainter::Antialiasing, true);
  canvas->setDragMode(QGraphicsView::ScrollHandDrag);
  canvas->setBackgroundBrush(theme::surfaceRaised());
  canvas->setFrameShape(QFrame::NoFrame);
  view_ = canvas;

  auto* controls = new QHBoxLayout;
  controls->setContentsMargins(8, 6, 8, 6);
  controls->addWidget(new QLabel(QStringLiteral("Centre on"), this));
  controls->addWidget(tablePicker_);
  controls->addWidget(depthPicker_);
  controls->addWidget(summary_, 1);
  auto* hint = new QLabel(QStringLiteral("⌘-scroll to zoom · drag to pan"), this);
  hint->setStyleSheet(
      QStringLiteral("color: %1; font-size: 11px;").arg(theme::textMuted().name()));
  controls->addWidget(hint);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addLayout(controls);
  layout->addWidget(view_, 1);

  connect(tablePicker_, &QComboBox::currentTextChanged, this,
          [this](const QString& t) {
            if (!t.isEmpty() && t != focus_) focusTable(t);
          });
  connect(depthPicker_, &QSpinBox::valueChanged, this,
          &DiagramView::relayout);
}

void DiagramView::setSchema(const Schema& schema) {
  schema_ = schema;

  const QString previous = focus_;
  tablePicker_->blockSignals(true);
  tablePicker_->clear();
  for (const auto& t : schema.tables) {
    tablePicker_->addItem(QString::fromStdString(t.name));
  }
  tablePicker_->blockSignals(false);

  if (schema.tables.empty()) {
    focus_.clear();
    buildScene();
    return;
  }

  // Keep the current focus across a refresh; otherwise start at the table with
  // the most relationships, which is usually the heart of the schema.
  if (!previous.isEmpty() && schema.findTable(previous.toStdString())) {
    focusTable(previous);
    return;
  }

  QString best;
  size_t bestScore = 0;
  for (const auto& t : schema.tables) {
    const size_t score =
        t.foreignKeys.size() +
        schema.incomingForeignKeys(t.name).size();
    if (score >= bestScore) {
      bestScore = score;
      best = QString::fromStdString(t.name);
    }
  }
  focusTable(best);
}

QList<QStringList> DiagramView::neighbourhood(const QString& root,
                                              int depth) const {
  // Past roughly this many neighbours the picture stops being readable, so we
  // show the most connected ones and say how many were left out.
  constexpr int kMaxPerRing = 24;

  QList<QStringList> rings;
  if (!schema_.findTable(root.toStdString())) return rings;

  JoinGraph graph(schema_);
  QSet<QString> seen{root};
  QStringList current{root};
  rings << current;

  for (int hop = 0; hop < depth; ++hop) {
    QStringList next;
    for (const QString& name : current) {
      for (const auto& edge : graph.neighbors(name.toStdString())) {
        const QString other = QString::fromStdString(edge.toTable);
        if (seen.contains(other)) continue;
        seen.insert(other);
        next << other;
      }
    }
    if (next.isEmpty()) break;
    if (next.size() > kMaxPerRing) {
      // Keep the tables with the most relationships of their own -- those are
      // the ones worth seeing.
      std::sort(next.begin(), next.end(),
                [this](const QString& a, const QString& b) {
                  auto weight = [this](const QString& n) {
                    const Table* t = schema_.findTable(n.toStdString());
                    if (!t) return size_t{0};
                    return t->foreignKeys.size() +
                           schema_.incomingForeignKeys(t->name).size();
                  };
                  return weight(a) > weight(b);
                });
      next = next.mid(0, kMaxPerRing);
    }
    rings << next;
    current = next;
  }
  return rings;
}

void DiagramView::focusTable(const QString& table) {
  if (table.isEmpty()) return;
  focus_ = table;
  if (tablePicker_->currentText() != table) {
    tablePicker_->blockSignals(true);
    tablePicker_->setCurrentText(table);
    tablePicker_->blockSignals(false);
  }
  buildScene();
}

void DiagramView::relayout() { buildScene(); }

void DiagramView::buildScene() {
  scene_->clear();
  nodes_.clear();

  auto showMessage = [this](const QString& text) {
    auto* item = scene_->addText(text);
    item->setDefaultTextColor(theme::textMuted());
    scene_->setSceneRect(item->boundingRect().adjusted(-40, -40, 40, 40));
    view_->resetTransform();
    view_->centerOn(item);
    summary_->clear();
  };

  if (schema_.tables.empty()) {
    showMessage(QStringLiteral("Connect to a database to see its shape."));
    return;
  }
  if (focus_.isEmpty() || !schema_.findTable(focus_.toStdString())) {
    showMessage(QStringLiteral("Choose a table to centre the diagram on."));
    return;
  }

  const QList<QStringList> rings =
      neighbourhood(focus_, depthPicker_->value());
  if (rings.isEmpty()) return;

  // A table nothing references and that references nothing has no diagram to
  // draw -- say so rather than rendering a lone box in a void.
  if (rings.size() == 1) {
    showMessage(QStringLiteral(
                    "“%1” has no foreign keys in either direction.\n\n"
                    "Nothing in this schema declares a relationship to it.")
                    .arg(focus_));
    return;
  }

  // Radial layout: the focus at the centre, each hop on its own ring. Simple,
  // deterministic and far more readable than force-directed for this shape.
  int total = 0;
  for (const auto& ring : rings) total += ring.size();

  for (int r = 0; r < rings.size(); ++r) {
    const QStringList& ring = rings[r];
    // The ring has to be big enough for its nodes to sit side by side without
    // overlapping: circumference >= count * (width + gap).
    const qreal needed = ring.size() * (kNodeWidth + 44.0) / (2 * M_PI);
    const qreal radius = r == 0 ? 0 : qMax(240.0 * r, needed);

    for (int i = 0; i < ring.size(); ++i) {
      const Table* t = schema_.findTable(ring[i].toStdString());
      if (!t) continue;

      auto* node = new TableNode(*t, r == 0, r);
      // Start each ring at 12 o'clock and space evenly.
      const qreal angle =
          ring.size() == 1 ? -M_PI / 2
                           : (2 * M_PI * i) / ring.size() - M_PI / 2;
      const QPointF centre(radius * qCos(angle), radius * qSin(angle));
      node->setPos(centre - QPointF(kNodeWidth / 2,
                                    node->boundingRect().height() / 2));
      node->setZValue(10 - r);
      scene_->addItem(node);
      nodes_.insert(ring[i], node);
    }
  }

  // Edges, drawn under the nodes.
  JoinGraph graph(schema_);
  QSet<QString> drawn;
  for (auto it = nodes_.constBegin(); it != nodes_.constEnd(); ++it) {
    for (const auto& edge : graph.neighbors(it.key().toStdString())) {
      const QString other = QString::fromStdString(edge.toTable);
      TableNode* target = nodes_.value(other, nullptr);
      if (!target) continue;

      // One line per pair, whichever direction we meet it from.
      const QString key = it.key() < other ? it.key() + "|" + other
                                           : other + "|" + it.key();
      if (drawn.contains(key)) continue;
      drawn.insert(key);

      TableNode* source = it.value();
      const QPointF a = source->anchorTowards(
          target->pos() + QPointF(kNodeWidth / 2,
                                  target->boundingRect().height() / 2));
      const QPointF b = target->anchorTowards(
          source->pos() + QPointF(kNodeWidth / 2,
                                  source->boundingRect().height() / 2));

      QPen pen(theme::relationColour());
      pen.setWidthF(1.4);
      auto* line = scene_->addLine(QLineF(a, b), pen);
      line->setZValue(-1);
      line->setOpacity(0.55);
      line->setToolTip(
          QStringLiteral("%1 → %2\n%3")
              .arg(QString::fromStdString(edge.fromTable),
                   QString::fromStdString(edge.toTable),
                   QString::fromStdString(
                       edge.onClause(edge.fromTable, edge.toTable))));

      // A dot on the "many" end reads as a crow's foot without the clutter.
      const QPointF manyEnd = edge.reversed ? b : a;
      auto* dot = scene_->addEllipse(QRectF(manyEnd.x() - 3.5,
                                            manyEnd.y() - 3.5, 7, 7),
                                     QPen(Qt::NoPen),
                                     QBrush(theme::relationColour()));
      dot->setZValue(-1);
      dot->setOpacity(0.75);
    }
  }

  scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60));
  fitted_ = false;
  fitToScene();

  // Count the whole neighbourhood, not just what fitted on the rings.
  JoinGraph full(schema_);
  QSet<QString> reachable;
  QStringList frontier{focus_};
  reachable.insert(focus_);
  for (int hop = 0; hop < depthPicker_->value(); ++hop) {
    QStringList next;
    for (const QString& n : frontier) {
      for (const auto& e : full.neighbors(n.toStdString())) {
        const QString other = QString::fromStdString(e.toTable);
        if (!reachable.contains(other)) {
          reachable.insert(other);
          next << other;
        }
      }
    }
    frontier = next;
  }

  const int related = reachable.size() - 1;
  const int shown = total - 1;
  QString text = QStringLiteral("%1 related table%2 within %3 hop%4")
                     .arg(related)
                     .arg(related == 1 ? "" : "s")
                     .arg(depthPicker_->value())
                     .arg(depthPicker_->value() == 1 ? "" : "s");
  if (shown < related) {
    text += QStringLiteral("  ·  showing the %1 most connected").arg(shown);
  }
  summary_->setText(text);
}

void DiagramView::fitToScene() {
  if (!scene_ || scene_->items().isEmpty()) return;
  // Before the tab is first shown the viewport has no size and fitInView
  // produces a degenerate transform, which is why this is retried on show.
  if (view_->viewport()->width() < 50 || view_->viewport()->height() < 50) {
    return;
  }
  view_->resetTransform();
  view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
  // Never magnify past 1:1 -- a two-table neighbourhood would fill the screen.
  if (view_->transform().m11() > 1.0) view_->resetTransform();
  fitted_ = true;
}

void DiagramView::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (!fitted_) fitToScene();
}

void DiagramView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (!fitted_) fitToScene();
}

}  // namespace ds
