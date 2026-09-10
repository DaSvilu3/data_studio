#include "ui/ResultsView.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>

#include "ui/ResultsModel.h"

namespace ds {

ResultsView::ResultsView(QWidget* parent) : QWidget(parent) {
  model_ = new ResultsModel(this);
  proxy_ = new QSortFilterProxyModel(this);
  proxy_->setSourceModel(model_);
  proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  // -1 filters across every column, which is what a bare search box implies.
  proxy_->setFilterKeyColumn(-1);
  proxy_->setSortRole(ResultsModel::SortRole);

  table_ = new QTableView(this);
  table_->setModel(proxy_);
  table_->setSortingEnabled(true);
  table_->setSelectionBehavior(QAbstractItemView::SelectItems);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setAlternatingRowColors(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setDefaultSectionSize(
      table_->fontMetrics().height() + 8);

  QFont mono(QStringLiteral("Menlo"));
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSize(12);
  table_->setFont(mono);

  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(QStringLiteral("Filter rows…"));
  filter_->setClearButtonEnabled(true);
  connect(filter_, &QLineEdit::textChanged, this, [this](const QString& t) {
    proxy_->setFilterFixedString(t);
    emit statusMessage(QStringLiteral("%1 of %2 rows match")
                           .arg(proxy_->rowCount())
                           .arg(model_->rowCount()));
  });

  status_ = new QLabel(this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  inspector_ = new QTextEdit(this);
  inspector_->setReadOnly(true);
  inspector_->setFont(mono);
  inspector_->setPlaceholderText(
      QStringLiteral("Select a cell to see its full value."));

  splitter_ = new QSplitter(Qt::Vertical, this);
  splitter_->addWidget(table_);
  splitter_->addWidget(inspector_);
  splitter_->setStretchFactor(0, 5);
  splitter_->setStretchFactor(1, 1);
  splitter_->setSizes({500, 90});

  auto* top = new QHBoxLayout;
  top->setContentsMargins(6, 4, 6, 4);
  top->addWidget(status_, 1);
  top->addWidget(filter_, 0);
  filter_->setFixedWidth(220);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addLayout(top);
  layout->addWidget(splitter_, 1);

  connect(table_, &QWidget::customContextMenuRequested, this,
          &ResultsView::showContextMenu);
  connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &ResultsView::updateInspector);

  auto* copyAction = new QAction(this);
  copyAction->setShortcut(QKeySequence::Copy);
  copyAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  connect(copyAction, &QAction::triggered, this, &ResultsView::copySelection);
  addAction(copyAction);
  table_->addAction(copyAction);
}

void ResultsView::setResult(const ResultSet& result) {
  // Drop any column sort the user left behind. Otherwise the proxy re-sorts
  // every new result by whatever the header indicator happens to point at,
  // silently overriding the ORDER BY in the statement they just ran.
  table_->setSortingEnabled(false);
  table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
  proxy_->sort(-1);

  model_->setResult(result);
  filter_->clear();
  table_->setSortingEnabled(true);

  // Size to content, but cap the width so one wide text column doesn't push
  // everything else off screen.
  table_->resizeColumnsToContents();
  for (int c = 0; c < model_->columnCount(); ++c) {
    if (table_->columnWidth(c) > 380) table_->setColumnWidth(c, 380);
    if (table_->columnWidth(c) < 60) table_->setColumnWidth(c, 60);
  }

  QString text;
  if (result.isSelect()) {
    text = QStringLiteral("%1 row%2 · %3 column%4 · %5 ms")
               .arg(result.rows.size())
               .arg(result.rows.size() == 1 ? "" : "s")
               .arg(result.columns.size())
               .arg(result.columns.size() == 1 ? "" : "s")
               .arg(result.elapsedMs, 0, 'f', 1);
    if (result.truncated) {
      text += QStringLiteral("  ⚠ truncated at the row limit — raise it or "
                             "add a LIMIT to see the rest");
    }
  } else {
    text = QStringLiteral("%1 row%2 affected · %3 ms")
               .arg(result.rowsAffected)
               .arg(result.rowsAffected == 1 ? "" : "s")
               .arg(result.elapsedMs, 0, 'f', 1);
    if (result.lastInsertId > 0) {
      text += QStringLiteral(" · last insert id %1").arg(result.lastInsertId);
    }
  }
  status_->setText(text);
  status_->setStyleSheet(QString());
  inspector_->clear();
}

void ResultsView::setError(const QString& message, const QString& statement) {
  model_->setResult(ResultSet{});
  status_->setText(message);
  status_->setStyleSheet(QStringLiteral("color:#d05050;"));
  inspector_->setPlainText(statement.trimmed());
}

void ResultsView::clear() {
  model_->setResult(ResultSet{});
  status_->clear();
  inspector_->clear();
}

void ResultsView::updateInspector() {
  const QModelIndex idx = table_->currentIndex();
  if (!idx.isValid()) {
    inspector_->clear();
    return;
  }
  const QModelIndex src = proxy_->mapToSource(idx);
  const ResultSet& rs = model_->result();
  if (src.row() >= static_cast<int>(rs.rows.size())) return;
  if (src.column() >= static_cast<int>(rs.rows[src.row()].size())) return;

  const Value& v = rs.rows[src.row()][src.column()];
  const QString name = QString::fromStdString(rs.columns[src.column()].name);
  const QString type = QString::fromStdString(rs.columns[src.column()].typeName);
  inspector_->setPlainText(
      QStringLiteral("%1  (%2)\n\n%3")
          .arg(name, type, QString::fromStdString(toDisplayString(v))));
}

QString ResultsView::sourceTableName() const {
  const ResultSet& rs = model_->result();
  // Only meaningful when every column came from the same table.
  QString name;
  for (const auto& c : rs.columns) {
    if (c.tableName.empty()) continue;
    const QString t = QString::fromStdString(c.tableName);
    if (name.isEmpty()) name = t;
    else if (name != t) return {};
  }
  return name;
}

void ResultsView::copySelection() {
  const auto sel = table_->selectionModel()->selectedIndexes();
  QModelIndexList mapped;
  for (const auto& i : sel) mapped << proxy_->mapToSource(i);
  const QString text = model_->copyAsText(mapped);
  if (!text.isEmpty()) {
    QApplication::clipboard()->setText(text);
    emit statusMessage(QStringLiteral("Copied %1 cells").arg(mapped.size()));
  }
}

void ResultsView::copySelectionAsCsv() {
  const auto sel = table_->selectionModel()->selectedIndexes();
  QModelIndexList mapped;
  for (const auto& i : sel) mapped << proxy_->mapToSource(i);
  QApplication::clipboard()->setText(model_->copyAsCsv(mapped));
  emit statusMessage(QStringLiteral("Copied as CSV"));
}

void ResultsView::copySelectionAsInsert() {
  const auto sel = table_->selectionModel()->selectedIndexes();
  QModelIndexList mapped;
  for (const auto& i : sel) mapped << proxy_->mapToSource(i);
  QApplication::clipboard()->setText(
      model_->copyAsInsert(mapped, sourceTableName(), dialect_));
  emit statusMessage(QStringLiteral("Copied as INSERT statements"));
}

void ResultsView::showContextMenu(const QPoint& pos) {
  if (model_->rowCount() == 0) return;
  QMenu menu(this);
  menu.addAction(QStringLiteral("Copy"), this, &ResultsView::copySelection);
  menu.addAction(QStringLiteral("Copy as CSV"), this,
                 &ResultsView::copySelectionAsCsv);
  menu.addAction(QStringLiteral("Copy as INSERT…"), this,
                 &ResultsView::copySelectionAsInsert);
  menu.addSeparator();
  menu.addAction(QStringLiteral("Select all"), table_,
                 &QAbstractItemView::selectAll);
  menu.exec(table_->viewport()->mapToGlobal(pos));
}

}  // namespace ds
