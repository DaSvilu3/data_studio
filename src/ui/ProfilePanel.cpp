#include "ui/ProfilePanel.h"

#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/Theme.h"

namespace ds {
namespace {

QString formatCount(int64_t n) {
  return QLocale().toString(static_cast<qlonglong>(n));
}

// A proportion bar drawn with block characters -- no custom delegate needed,
// and it copies as text.
QString proportionBar(double share, int width = 14) {
  const int filled = qBound(0, static_cast<int>(qRound(share * width)), width);
  return QString(filled, QChar(0x2588)) + QString(width - filled, QChar(0x2591));
}

}  // namespace

ProfilePanel::ProfilePanel(QWidget* parent) : QWidget(parent) {
  placeholder_ = new QLabel(this);
  placeholder_->setAlignment(Qt::AlignCenter);
  placeholder_->setWordWrap(true);
  placeholder_->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::textMuted().name()));

  heading_ = new QLabel(this);
  heading_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  stats_ = new QLabel(this);
  stats_->setWordWrap(true);
  stats_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  insights_ = new QLabel(this);
  insights_->setWordWrap(true);
  insights_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  values_ = new QTreeWidget(this);
  values_->setColumnCount(4);
  values_->setHeaderLabels({QStringLiteral("Value"), QStringLiteral("Rows"),
                            QStringLiteral("Share"), QString()});
  values_->setRootIsDecorated(false);
  values_->setUniformRowHeights(true);
  values_->setAlternatingRowColors(false);
  values_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  values_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  values_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  values_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  values_->setContextMenuPolicy(Qt::CustomContextMenu);

  // A frequent value is usually the filter you were about to write.
  connect(values_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) {
            QTreeWidgetItem* item = values_->itemAt(pos);
            if (!item) return;
            const QString predicate = item->data(0, Qt::UserRole).toString();
            if (predicate.isEmpty()) return;
            QMenu menu(this);
            menu.addAction(QStringLiteral("Insert as a WHERE condition"), this,
                           [this, predicate] { emit insertText(predicate); });
            menu.exec(values_->viewport()->mapToGlobal(pos));
          });
  connect(values_, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem* item, int) {
            const QString predicate = item->data(0, Qt::UserRole).toString();
            if (!predicate.isEmpty()) emit insertText(predicate);
          });

  auto* content = new QWidget(this);
  auto* inner = new QVBoxLayout(content);
  inner->setContentsMargins(12, 10, 12, 10);
  inner->setSpacing(8);
  inner->addWidget(heading_);
  inner->addWidget(stats_);
  inner->addWidget(insights_);
  inner->addWidget(values_, 1);

  stack_ = new QStackedWidget(this);
  stack_->addWidget(placeholder_);
  stack_->addWidget(content);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(stack_);

  clear();
}

void ProfilePanel::clear() {
  placeholder_->setText(QStringLiteral(
      "Select a column in the sidebar and choose “Profile column”.\n\n"
      "Shows how many rows are null, how many distinct values there are, the "
      "range, and the most common values — the things that decide whether an "
      "index is worth having."));
  stack_->setCurrentWidget(placeholder_);
}

void ProfilePanel::showPending(const QString& table, const QString& column) {
  placeholder_->setText(
      QStringLiteral("Profiling %1.%2…").arg(table, column));
  stack_->setCurrentWidget(placeholder_);
}

void ProfilePanel::showProfile(const ColumnProfile& p) {
  heading_->setText(
      QStringLiteral("<span style='font-size:15px; font-weight:600;'>%1.%2</span>"
                     "&nbsp;&nbsp;<span style='color:%3;'>%4</span>")
          .arg(QString::fromStdString(p.table).toHtmlEscaped(),
               QString::fromStdString(p.column).toHtmlEscaped(),
               theme::textMuted().name(),
               QString::fromStdString(p.declaredType).toHtmlEscaped()));

  QStringList bits;
  bits << QStringLiteral("<b>%1</b> rows").arg(formatCount(p.totalRows));
  bits << QStringLiteral("<b>%1</b> distinct (%2%)")
              .arg(formatCount(p.distinctCount))
              .arg(p.selectivity() * 100, 0, 'f', 1);
  bits << QStringLiteral("<b>%1</b> null (%2%)")
              .arg(formatCount(p.nullCount))
              .arg(p.nullShare() * 100, 0, 'f', 1);
  if (p.hasRange) {
    bits << QStringLiteral("range <b>%1</b> → <b>%2</b>")
                .arg(QString::fromStdString(p.minValue).left(28).toHtmlEscaped(),
                     QString::fromStdString(p.maxValue).left(28).toHtmlEscaped());
  }
  if (p.hasMean) {
    bits << QStringLiteral("mean <b>%1</b>").arg(p.meanValue, 0, 'f', 2);
  }
  stats_->setText(bits.join(QStringLiteral("&nbsp; · &nbsp;")));

  const Table* table = schema_.findTable(p.table);
  QStringList notes;
  if (table) {
    for (const auto& note : profileInsights(p, *table)) {
      notes << QStringLiteral("• ") + QString::fromStdString(note).toHtmlEscaped();
    }
  }
  if (notes.isEmpty()) {
    insights_->clear();
    insights_->setVisible(false);
  } else {
    insights_->setVisible(true);
    insights_->setText(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(theme::warning().name(), notes.join(QStringLiteral("<br>"))));
  }

  values_->clear();
  const QFont mono = theme::monospaceFont(11);
  for (const auto& v : p.topValues) {
    auto* item = new QTreeWidgetItem(values_);
    const QString value = QString::fromStdString(v.value);
    item->setText(0, value.left(120));
    item->setFont(0, mono);
    item->setText(1, formatCount(v.count));
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(2, QStringLiteral("%1%").arg(v.share * 100, 0, 'f', 1));
    item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(3, proportionBar(v.share));
    item->setForeground(3, theme::accent());
    item->setFont(3, mono);
    item->setToolTip(0, value);

    // Double-click or right-click turns the value into a predicate.
    const bool numeric = v.value.find_first_not_of("-0123456789.") ==
                         std::string::npos && !v.value.empty();
    const QString literal =
        numeric ? value
                : QLatin1Char('\'') + QString(value).replace(
                                          QLatin1Char('\''),
                                          QStringLiteral("''")) +
                      QLatin1Char('\'');
    item->setData(0, Qt::UserRole,
                  QStringLiteral("%1 = %2")
                      .arg(QString::fromStdString(p.column), literal));
  }
  if (p.topValues.empty()) {
    auto* item = new QTreeWidgetItem(values_);
    item->setText(0, QStringLiteral("no non-null values"));
    item->setForeground(0, theme::textMuted());
  }

  stack_->setCurrentIndex(1);
}

}  // namespace ds
