#include "ui/Theme.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

namespace ds::theme {
namespace {

QColor pick(const char* light, const char* dark) {
  return QColor(isDark() ? dark : light);
}

// Draws into a device-pixel-ratio-aware pixmap so glyphs stay crisp on retina.
QPixmap blankPixmap(int size) {
  const qreal dpr = qApp ? qApp->devicePixelRatio() : 2.0;
  QPixmap pm(QSize(size, size) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  return pm;
}

void setupPainter(QPainter& p, const QColor& c) {
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(c);
  pen.setWidthF(1.6);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
}

}  // namespace

bool isDark() {
  if (!qApp) return false;
  return qApp->palette().color(QPalette::Window).lightness() < 128;
}

QColor accent()        { return pick("#2f6feb", "#4c8dff"); }
QColor accentMuted()   { return pick("#dce7fb", "#1d3557"); }
QColor surface()       { return pick("#ffffff", "#1e1f22"); }
QColor surfaceRaised() { return pick("#f5f6f8", "#2b2d31"); }
QColor border()        { return pick("#d8dbe0", "#3a3d42"); }
QColor textPrimary()   { return pick("#1c1e21", "#e3e5e8"); }
QColor textMuted()     { return pick("#6b7280", "#9aa0a8"); }
QColor danger()        { return pick("#c0392b", "#e5706a"); }
QColor warning()       { return pick("#a06800", "#e0a458"); }
QColor success()       { return pick("#1f7a45", "#5cc98a"); }
QColor info()          { return pick("#3a6ea5", "#6f9fd8"); }

QColor tableColour()    { return pick("#2f6feb", "#6aa9ff"); }
QColor viewColour()     { return pick("#7a3e9d", "#c77dbb"); }
QColor keyColour()      { return pick("#b8860b", "#e0b341"); }
QColor indexColour()    { return pick("#1f7a45", "#5cc98a"); }
QColor relationColour() { return pick("#a0522d", "#d59a6a"); }

QFont monospaceFont(int pointSize) {
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  // The system fixed font on macOS is Menlo, which is what we want anyway;
  // this just guarantees a real monospace face everywhere.
  f.setStyleHint(QFont::Monospace);
  f.setFixedPitch(true);
  f.setPointSize(pointSize);
  return f;
}

QIcon statusDot(const QColor& colour) {
  QPixmap pm = blankPixmap(10);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  p.setBrush(colour);
  p.drawEllipse(QRectF(1, 1, 8, 8));
  p.end();
  return QIcon(pm);
}

QIcon icon(const QString& name, const QColor& tint) {
  const int s = 16;
  const QColor c = tint.isValid() ? tint : textPrimary();
  QPixmap pm = blankPixmap(s);
  QPainter p(&pm);
  setupPainter(p, c);

  if (name == QLatin1String("run")) {
    // Filled triangle -- the primary action should read as solid.
    QPainterPath path;
    path.moveTo(4.5, 3);
    path.lineTo(13, 8);
    path.lineTo(4.5, 13);
    path.closeSubpath();
    p.fillPath(path, c);
  } else if (name == QLatin1String("runAll")) {
    QPainterPath path;
    path.moveTo(3, 3);
    path.lineTo(9, 8);
    path.lineTo(3, 13);
    path.closeSubpath();
    p.fillPath(path, c);
    p.drawLine(QPointF(12, 3), QPointF(12, 13));
  } else if (name == QLatin1String("explain")) {
    // A little bar chart: this is about reading a plan.
    p.drawLine(QPointF(3.5, 13), QPointF(3.5, 8));
    p.drawLine(QPointF(8, 13), QPointF(8, 3.5));
    p.drawLine(QPointF(12.5, 13), QPointF(12.5, 6));
  } else if (name == QLatin1String("cancel")) {
    p.drawEllipse(QRectF(2.5, 2.5, 11, 11));
    p.drawLine(QPointF(6, 6), QPointF(10, 10));
    p.drawLine(QPointF(10, 6), QPointF(6, 10));
  } else if (name == QLatin1String("refresh")) {
    p.drawArc(QRectF(3, 3, 10, 10), 60 * 16, 280 * 16);
    QPainterPath head;
    head.moveTo(12.8, 1.8);
    head.lineTo(13.4, 6.0);
    head.lineTo(9.4, 5.0);
    head.closeSubpath();
    p.fillPath(head, c);
  } else if (name == QLatin1String("database")) {
    p.drawEllipse(QRectF(2.5, 2, 11, 3.6));
    p.drawLine(QPointF(2.5, 3.8), QPointF(2.5, 12.2));
    p.drawLine(QPointF(13.5, 3.8), QPointF(13.5, 12.2));
    p.drawArc(QRectF(2.5, 10.4, 11, 3.6), 180 * 16, 180 * 16);
    p.drawArc(QRectF(2.5, 6.2, 11, 3.6), 180 * 16, 180 * 16);
  } else if (name == QLatin1String("table")) {
    p.drawRoundedRect(QRectF(2.5, 3, 11, 10), 2, 2);
    p.drawLine(QPointF(2.5, 6.5), QPointF(13.5, 6.5));
    p.drawLine(QPointF(8, 6.5), QPointF(8, 13));
  } else if (name == QLatin1String("view")) {
    // An eye -- a view is something you look through.
    QPainterPath path;
    path.moveTo(2, 8);
    path.quadTo(8, 2.5, 14, 8);
    path.quadTo(8, 13.5, 2, 8);
    p.drawPath(path);
    p.drawEllipse(QRectF(6.4, 6.4, 3.2, 3.2));
  } else if (name == QLatin1String("column")) {
    p.drawLine(QPointF(5, 3.5), QPointF(5, 12.5));
    p.drawLine(QPointF(8, 3.5), QPointF(8, 12.5));
    p.drawLine(QPointF(11, 3.5), QPointF(11, 12.5));
  } else if (name == QLatin1String("primaryKey")) {
    p.drawEllipse(QRectF(2.5, 5.5, 6, 6));
    p.drawLine(QPointF(8, 8.5), QPointF(13.5, 8.5));
    p.drawLine(QPointF(11, 8.5), QPointF(11, 11));
    p.drawLine(QPointF(13, 8.5), QPointF(13, 10.5));
  } else if (name == QLatin1String("foreignKey")) {
    // Two links: a relationship between tables.
    p.drawRoundedRect(QRectF(1.8, 6, 6, 4), 2, 2);
    p.drawRoundedRect(QRectF(8.2, 6, 6, 4), 2, 2);
    p.drawLine(QPointF(7, 8), QPointF(9, 8));
  } else if (name == QLatin1String("index")) {
    p.drawLine(QPointF(3, 4.5), QPointF(13, 4.5));
    p.drawLine(QPointF(3, 8), QPointF(10, 8));
    p.drawLine(QPointF(3, 11.5), QPointF(12, 11.5));
  } else if (name == QLatin1String("sparkle")) {
    QPainterPath path;
    path.moveTo(8, 2);
    path.quadTo(8.8, 7.2, 14, 8);
    path.quadTo(8.8, 8.8, 8, 14);
    path.quadTo(7.2, 8.8, 2, 8);
    path.quadTo(7.2, 7.2, 8, 2);
    p.fillPath(path, c);
  } else if (name == QLatin1String("plug")) {
    p.drawLine(QPointF(6, 2.5), QPointF(6, 6));
    p.drawLine(QPointF(10, 2.5), QPointF(10, 6));
    p.drawRoundedRect(QRectF(3.5, 6, 9, 5), 2, 2);
    p.drawLine(QPointF(8, 11), QPointF(8, 14));
  } else if (name == QLatin1String("error") || name == QLatin1String("warning") ||
             name == QLatin1String("info")) {
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    if (name == QLatin1String("warning")) {
      QPainterPath tri;
      tri.moveTo(8, 2.5);
      tri.lineTo(14.5, 13.5);
      tri.lineTo(1.5, 13.5);
      tri.closeSubpath();
      p.fillPath(tri, c);
    } else {
      p.drawEllipse(QRectF(2.5, 2.5, 11, 11));
    }
  }

  p.end();
  return QIcon(pm);
}

QString appStyleSheet() {
  const QString bg = surface().name();
  const QString raised = surfaceRaised().name();
  const QString line = border().name();
  const QString text = textPrimary().name();
  const QString muted = textMuted().name();
  const QString acc = accent().name();
  const QString accSoft = accentMuted().name();

  return QStringLiteral(R"(
QToolBar {
  background: %2;
  border: none;
  border-bottom: 1px solid %3;
  padding: 5px 8px;
  spacing: 3px;
}
QToolButton {
  color: %4;
  padding: 5px 10px;
  border-radius: 6px;
  border: 1px solid transparent;
}
QToolButton:hover:!disabled { background: %6; }
QToolButton:pressed { background: %3; }
QToolButton:disabled { color: %5; }
QToolButton#primaryAction {
  background: %7;
  color: white;
  font-weight: 600;
}
QToolButton#primaryAction:hover:!disabled { background: %7; }
QToolButton#primaryAction:disabled { background: %3; color: %5; }

QTabWidget::pane { border: none; border-top: 1px solid %3; }
QTabBar::tab {
  background: transparent;
  color: %5;
  padding: 6px 14px;
  margin-right: 2px;
  border: none;
  border-bottom: 2px solid transparent;
}
QTabBar::tab:hover { color: %4; }
QTabBar::tab:selected {
  color: %4;
  border-bottom: 2px solid %7;
  font-weight: 600;
}

QLineEdit, QSpinBox, QComboBox {
  background: %1;
  border: 1px solid %3;
  border-radius: 6px;
  padding: 4px 8px;
  color: %4;
  selection-background-color: %7;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid %7; }

QTreeWidget, QTableView, QListWidget, QPlainTextEdit, QTextEdit {
  background: %1;
  border: none;
  color: %4;
  selection-background-color: %6;
  selection-color: %4;
  outline: none;
}
QHeaderView::section {
  background: %2;
  color: %5;
  padding: 5px 8px;
  border: none;
  border-right: 1px solid %3;
  border-bottom: 1px solid %3;
  font-weight: 600;
}
QTreeWidget::item, QListWidget::item { padding: 3px 2px; }
QTreeWidget::item:selected, QListWidget::item:selected,
QTableView::item:selected { background: %6; color: %4; }

QSplitter::handle { background: %3; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QStatusBar { background: %2; border-top: 1px solid %3; color: %5; }
QStatusBar::item { border: none; }

QGroupBox {
  border: 1px solid %3;
  border-radius: 8px;
  margin-top: 10px;
  padding-top: 8px;
  color: %5;
  font-weight: 600;
}
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }

QPushButton {
  background: %2;
  border: 1px solid %3;
  border-radius: 6px;
  padding: 5px 12px;
  color: %4;
}
QPushButton:hover:!disabled { background: %6; }
QPushButton:disabled { color: %5; }
QPushButton:default {
  background: %7;
  border-color: %7;
  color: white;
  font-weight: 600;
}
QScrollBar:vertical { background: transparent; width: 11px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0; }
QScrollBar::handle {
  background: %3;
  border-radius: 5px;
  min-height: 28px;
  min-width: 28px;
}
QScrollBar::handle:hover { background: %5; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QToolTip {
  background: %2;
  color: %4;
  border: 1px solid %3;
  padding: 4px 6px;
}
)")
      .arg(bg, raised, line, text, muted, accSoft, acc);
}

}  // namespace ds::theme
