#include "ui/AnalyzerPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ds {
namespace {

constexpr int kSuggestionRole = Qt::UserRole + 1;
constexpr int kOffsetRole = Qt::UserRole + 2;
constexpr int kLengthRole = Qt::UserRole + 3;

QColor severityColour(Severity s) {
  switch (s) {
    case Severity::Error: return QColor("#d05050");
    case Severity::Warning: return QColor("#c98a20");
    case Severity::Info: return QColor("#6f8fb0");
  }
  return {};
}

QString severityIcon(Severity s) {
  switch (s) {
    case Severity::Error: return QStringLiteral("●");
    case Severity::Warning: return QStringLiteral("▲");
    case Severity::Info: return QStringLiteral("○");
  }
  return {};
}

}  // namespace

AnalyzerPanel::AnalyzerPanel(QWidget* parent) : QWidget(parent) {
  summary_ = new QLabel(this);
  summary_->setWordWrap(true);

  explainButton_ = new QPushButton(QStringLiteral("Run EXPLAIN"), this);
  explainButton_->setToolTip(
      QStringLiteral("Ask the engine for its query plan and check it for "
                     "scans, filesorts and unused indexes."));
  connect(explainButton_, &QPushButton::clicked, this,
          &AnalyzerPanel::explainRequested);

  tree_ = new QTreeWidget(this);
  tree_->setColumnCount(2);
  tree_->setHeaderLabels({QStringLiteral("Finding"), QStringLiteral("Fix")});
  tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
  tree_->header()->setStretchLastSection(true);
  tree_->setColumnWidth(0, 380);
  tree_->setWordWrap(true);
  tree_->setAlternatingRowColors(false);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* top = new QHBoxLayout;
  top->setContentsMargins(6, 4, 6, 4);
  top->addWidget(summary_, 1);
  top->addWidget(explainButton_, 0);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addLayout(top);
  layout->addWidget(tree_, 1);

  connect(tree_, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem* item, int) {
            if (!item) return;
            const QString fix = item->data(0, kSuggestionRole).toString();
            const int offset = item->data(0, kOffsetRole).toInt();
            const int length = item->data(0, kLengthRole).toInt();
            if (length > 0) emit revealRange(offset, length);
            // Only offer to insert a fix that is actually runnable SQL.
            if (fix.trimmed().startsWith(QStringLiteral("CREATE"),
                                         Qt::CaseInsensitive)) {
              emit applySuggestion(fix);
            }
          });

  connect(tree_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) {
            QTreeWidgetItem* item = tree_->itemAt(pos);
            if (!item) return;
            const QString fix = item->data(0, kSuggestionRole).toString();
            if (fix.isEmpty()) return;
            QMenu menu(this);
            if (fix.trimmed().startsWith(QStringLiteral("CREATE"),
                                         Qt::CaseInsensitive)) {
              menu.addAction(QStringLiteral("Insert this statement"), this,
                             [this, fix] { emit applySuggestion(fix); });
            }
            menu.addAction(QStringLiteral("Copy fix"), this, [fix] {
              QApplication::clipboard()->setText(fix);
            });
            menu.exec(tree_->viewport()->mapToGlobal(pos));
          });
}

void AnalyzerPanel::analyze(const QString& sql) {
  sql_ = sql;
  staticDiags_.clear();
  if (!sql.trimmed().isEmpty()) {
    staticDiags_ = analyzeStatic(schema_, sql.toStdString(), dialect_);
  }
  // The old plan describes a statement that may no longer be there.
  clearPlan();
}

void AnalyzerPanel::setPlan(const ResultSet& plan, const QString& sql) {
  planDiags_ = analyzePlan(schema_, plan, sql.toStdString(), dialect_);
  havePlan_ = true;
  rebuild();
}

void AnalyzerPanel::clearPlan() {
  planDiags_.clear();
  havePlan_ = false;
  rebuild();
}

void AnalyzerPanel::rebuild() {
  tree_->clear();

  int errors = 0, warnings = 0, infos = 0;
  errors_ = 0;
  warnings_ = 0;
  auto addSection = [&](const QString& title,
                        const std::vector<Diagnostic>& diags) {
    if (diags.empty()) return;
    auto* section = new QTreeWidgetItem(tree_);
    section->setText(0, title);
    QFont bold = section->font(0);
    bold.setBold(true);
    section->setFont(0, bold);
    section->setFirstColumnSpanned(true);
    section->setExpanded(true);

    for (const auto& d : diags) {
      switch (d.severity) {
        case Severity::Error: ++errors; break;
        case Severity::Warning: ++warnings; break;
        case Severity::Info: ++infos; break;
      }
      auto* item = new QTreeWidgetItem(section);
      item->setText(0, QStringLiteral("%1  %2")
                           .arg(severityIcon(d.severity),
                                QString::fromStdString(d.title)));
      item->setForeground(0, severityColour(d.severity));
      item->setText(1, QString::fromStdString(d.suggestion));
      item->setToolTip(0, QString::fromStdString(d.detail));
      item->setToolTip(1, QString::fromStdString(d.suggestion));
      item->setData(0, kSuggestionRole, QString::fromStdString(d.suggestion));
      item->setData(0, kOffsetRole, static_cast<int>(d.offset));
      item->setData(0, kLengthRole, static_cast<int>(d.length));

      // The explanation goes in a child row so the list stays scannable.
      if (!d.detail.empty()) {
        auto* detail = new QTreeWidgetItem(item);
        detail->setText(0, QString::fromStdString(d.detail));
        detail->setFirstColumnSpanned(true);
        detail->setForeground(0, QColor(140, 140, 140));
      }
    }
  };

  addSection(QStringLiteral("Schema checks"), staticDiags_);
  addSection(QStringLiteral("Query plan"), planDiags_);

  errors_ = errors;
  warnings_ = warnings;
  const int total = errors + warnings + infos;
  if (sql_.trimmed().isEmpty()) {
    summary_->setText(QStringLiteral("Nothing to analyze yet."));
    summary_->setStyleSheet(QStringLiteral("color: gray;"));
  } else if (total == 0) {
    summary_->setText(havePlan_
                          ? QStringLiteral("Nothing to flag, plan included.")
                          : QStringLiteral("Nothing to flag."));
    summary_->setStyleSheet(QStringLiteral("color: #2d8a4e;"));
  } else {
    QStringList parts;
    if (errors) parts << QStringLiteral("%1 error%2").arg(errors).arg(errors == 1 ? "" : "s");
    if (warnings) parts << QStringLiteral("%1 warning%2").arg(warnings).arg(warnings == 1 ? "" : "s");
    if (infos) parts << QStringLiteral("%1 note%2").arg(infos).arg(infos == 1 ? "" : "s");
    summary_->setText(parts.join(QStringLiteral(", ")) +
                      (havePlan_ ? QString()
                                 : QStringLiteral("  ·  run EXPLAIN for more")));
    summary_->setStyleSheet(errors ? QStringLiteral("color: #d05050;")
                                   : QStringLiteral("color: #c98a20;"));
  }

  tree_->expandAll();
}

}  // namespace ds
