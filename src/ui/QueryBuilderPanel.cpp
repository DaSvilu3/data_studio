#include "ui/QueryBuilderPanel.h"

#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/SqlHighlighter.h"

namespace ds {
namespace {

constexpr int kTableRole = Qt::UserRole + 1;
constexpr int kColumnRole = Qt::UserRole + 2;

const QStringList& aggregates() {
  static const QStringList kAggregates = {
      QString(), QStringLiteral("COUNT"), QStringLiteral("SUM"),
      QStringLiteral("AVG"),  QStringLiteral("MIN"), QStringLiteral("MAX")};
  return kAggregates;
}

}  // namespace

QueryBuilderPanel::QueryBuilderPanel(QWidget* parent) : QWidget(parent) {
  // ---- tables ----
  allTables_ = new QListWidget(this);
  allTables_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  chosenTables_ = new QListWidget(this);
  // Order matters: the first table becomes the FROM, the rest are joined onto
  // it, so let the user drag to reorder.
  chosenTables_->setDragDropMode(QAbstractItemView::InternalMove);

  auto* addBtn = new QToolButton(this);
  addBtn->setText(QStringLiteral("→"));
  addBtn->setToolTip(QStringLiteral("Add to the query"));
  auto* removeBtn = new QToolButton(this);
  removeBtn->setText(QStringLiteral("←"));
  removeBtn->setToolTip(QStringLiteral("Remove from the query"));

  auto* tableButtons = new QVBoxLayout;
  tableButtons->addStretch(1);
  tableButtons->addWidget(addBtn);
  tableButtons->addWidget(removeBtn);
  tableButtons->addStretch(1);

  auto* tablesRow = new QHBoxLayout;
  tablesRow->addWidget(allTables_, 1);
  tablesRow->addLayout(tableButtons, 0);
  tablesRow->addWidget(chosenTables_, 1);

  auto* tablesBox = new QGroupBox(QStringLiteral("Tables"), this);
  tablesBox->setLayout(tablesRow);

  // ---- columns ----
  columns_ = new QTreeWidget(this);
  columns_->setColumnCount(3);
  columns_->setHeaderLabels({QStringLiteral("Column"),
                             QStringLiteral("Aggregate"),
                             QStringLiteral("Alias")});
  columns_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  columns_->setEditTriggers(QAbstractItemView::AllEditTriggers);

  auto* columnsBox = new QGroupBox(
      QStringLiteral("Columns  ·  tick to select, leave all clear for *"),
      this);
  auto* columnsLayout = new QVBoxLayout(columnsBox);
  columnsLayout->setContentsMargins(6, 6, 6, 6);
  columnsLayout->addWidget(columns_);

  // ---- filters ----
  filters_ = new QTableWidget(0, 4, this);
  filters_->setHorizontalHeaderLabels({QStringLiteral("Column"),
                                       QStringLiteral("Operator"),
                                       QStringLiteral("Value"),
                                       QStringLiteral("And/Or")});
  filters_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  filters_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  filters_->verticalHeader()->setVisible(false);

  auto* addFilter = new QPushButton(QStringLiteral("Add filter"), this);
  auto* removeFilter = new QPushButton(QStringLiteral("Remove"), this);

  auto* filterButtons = new QHBoxLayout;
  filterButtons->addWidget(addFilter);
  filterButtons->addWidget(removeFilter);
  filterButtons->addStretch(1);

  auto* filtersBox = new QGroupBox(QStringLiteral("Filters"), this);
  auto* filtersLayout = new QVBoxLayout(filtersBox);
  filtersLayout->setContentsMargins(6, 6, 6, 6);
  filtersLayout->addWidget(filters_);
  filtersLayout->addLayout(filterButtons);

  // ---- options ----
  distinct_ = new QCheckBox(QStringLiteral("DISTINCT"), this);
  limit_ = new QSpinBox(this);
  limit_->setRange(0, 1000000);
  limit_->setValue(200);
  limit_->setSpecialValueText(QStringLiteral("no limit"));
  limit_->setPrefix(QStringLiteral("LIMIT "));

  auto* optionsRow = new QHBoxLayout;
  optionsRow->addWidget(distinct_);
  optionsRow->addWidget(limit_);
  optionsRow->addStretch(1);

  // ---- preview ----
  preview_ = new QPlainTextEdit(this);
  preview_->setReadOnly(true);
  QFont mono(QStringLiteral("Menlo"));
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSize(12);
  preview_->setFont(mono);
  new SqlHighlighter(preview_->document());

  warnings_ = new QLabel(this);
  warnings_->setWordWrap(true);
  warnings_->setStyleSheet(QStringLiteral("color: #c98a20;"));

  auto* toEditor = new QPushButton(QStringLiteral("Send to editor"), this);
  auto* runNow = new QPushButton(QStringLiteral("Run"), this);
  runNow->setDefault(true);

  auto* previewButtons = new QHBoxLayout;
  previewButtons->addStretch(1);
  previewButtons->addWidget(toEditor);
  previewButtons->addWidget(runNow);

  auto* previewBox = new QGroupBox(QStringLiteral("Generated SQL"), this);
  auto* previewLayout = new QVBoxLayout(previewBox);
  previewLayout->setContentsMargins(6, 6, 6, 6);
  previewLayout->addWidget(preview_, 1);
  previewLayout->addWidget(warnings_);
  previewLayout->addLayout(previewButtons);

  // ---- assembly ----
  auto* left = new QWidget(this);
  auto* leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(0, 0, 0, 0);
  leftLayout->addWidget(tablesBox, 2);
  leftLayout->addWidget(columnsBox, 3);
  leftLayout->addWidget(filtersBox, 2);
  leftLayout->addLayout(optionsRow);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  splitter->addWidget(left);
  splitter->addWidget(previewBox);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addWidget(splitter);

  connect(addBtn, &QToolButton::clicked, this,
          &QueryBuilderPanel::addSelectedTable);
  connect(removeBtn, &QToolButton::clicked, this,
          &QueryBuilderPanel::removeChosenTable);
  connect(allTables_, &QListWidget::itemDoubleClicked, this,
          &QueryBuilderPanel::addSelectedTable);
  connect(addFilter, &QPushButton::clicked, this,
          &QueryBuilderPanel::addFilterRow);
  connect(removeFilter, &QPushButton::clicked, this,
          &QueryBuilderPanel::removeFilterRow);
  connect(distinct_, &QCheckBox::toggled, this,
          &QueryBuilderPanel::regenerate);
  connect(limit_, &QSpinBox::valueChanged, this,
          &QueryBuilderPanel::regenerate);
  connect(columns_, &QTreeWidget::itemChanged, this,
          &QueryBuilderPanel::regenerate);
  connect(filters_, &QTableWidget::cellChanged, this,
          &QueryBuilderPanel::regenerate);
  connect(chosenTables_->model(), &QAbstractItemModel::rowsMoved, this,
          &QueryBuilderPanel::regenerate);

  connect(toEditor, &QPushButton::clicked, this,
          [this] { emit sendToEditor(preview_->toPlainText()); });
  connect(runNow, &QPushButton::clicked, this,
          [this] { emit runRequested(preview_->toPlainText()); });
}

void QueryBuilderPanel::setSchema(const Schema& schema) {
  schema_ = schema;
  updating_ = true;

  // Keep the user's picks across a schema refresh where the table still exists.
  QStringList previouslyChosen;
  for (int i = 0; i < chosenTables_->count(); ++i) {
    previouslyChosen << chosenTables_->item(i)->text();
  }

  allTables_->clear();
  chosenTables_->clear();
  for (const auto& t : schema.tables) {
    allTables_->addItem(QString::fromStdString(t.name));
  }
  for (const QString& name : previouslyChosen) {
    if (schema.findTable(name.toStdString())) chosenTables_->addItem(name);
  }

  updating_ = false;
  rebuildColumnTree();
  regenerate();
}

void QueryBuilderPanel::addSelectedTable() {
  for (QListWidgetItem* item : allTables_->selectedItems()) {
    const QString name = item->text();
    bool present = false;
    for (int i = 0; i < chosenTables_->count(); ++i) {
      if (chosenTables_->item(i)->text() == name) { present = true; break; }
    }
    if (!present) chosenTables_->addItem(name);
  }
  rebuildColumnTree();
  regenerate();
}

void QueryBuilderPanel::removeChosenTable() {
  qDeleteAll(chosenTables_->selectedItems());
  rebuildColumnTree();
  regenerate();
}

void QueryBuilderPanel::rebuildColumnTree() {
  updating_ = true;

  // Remember what was ticked so rebuilding doesn't wipe the selection.
  QSet<QString> checked;
  QHash<QString, QString> aggregateFor;
  for (int i = 0; i < columns_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* group = columns_->topLevelItem(i);
    for (int j = 0; j < group->childCount(); ++j) {
      QTreeWidgetItem* c = group->child(j);
      const QString ref = c->data(0, kTableRole).toString() +
                          QLatin1Char('.') + c->data(0, kColumnRole).toString();
      if (c->checkState(0) == Qt::Checked) checked.insert(ref);
      if (auto* box = qobject_cast<QComboBox*>(columns_->itemWidget(c, 1))) {
        aggregateFor[ref] = box->currentText();
      }
    }
  }

  columns_->clear();
  for (int i = 0; i < chosenTables_->count(); ++i) {
    const QString tableName = chosenTables_->item(i)->text();
    const Table* t = schema_.findTable(tableName.toStdString());
    if (!t) continue;

    auto* group = new QTreeWidgetItem(columns_);
    group->setText(0, tableName);
    group->setFirstColumnSpanned(true);
    QFont bold = group->font(0);
    bold.setBold(true);
    group->setFont(0, bold);
    group->setExpanded(true);
    group->setFlags(group->flags() & ~Qt::ItemIsUserCheckable);

    for (const auto& col : t->columns) {
      auto* item = new QTreeWidgetItem(group);
      const QString colName = QString::fromStdString(col.name);
      const QString ref = tableName + QLatin1Char('.') + colName;
      item->setText(0, colName);
      item->setData(0, kTableRole, tableName);
      item->setData(0, kColumnRole, colName);
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                     Qt::ItemIsEditable);
      item->setCheckState(0, checked.contains(ref) ? Qt::Checked
                                                   : Qt::Unchecked);
      item->setToolTip(0, QString::fromStdString(col.type));

      auto* agg = new QComboBox(columns_);
      agg->addItems(aggregates());
      if (aggregateFor.contains(ref)) {
        agg->setCurrentText(aggregateFor[ref]);
      }
      connect(agg, &QComboBox::currentTextChanged, this,
              &QueryBuilderPanel::regenerate);
      columns_->setItemWidget(item, 1, agg);
    }
  }

  // Filter rows reference columns, so their pickers need the new list.
  for (int r = 0; r < filters_->rowCount(); ++r) {
    if (auto* box = qobject_cast<QComboBox*>(filters_->cellWidget(r, 0))) {
      const QString current = box->currentText();
      box->clear();
      box->addItems(availableColumnRefs());
      box->setCurrentText(current);
    }
  }

  updating_ = false;
}

QStringList QueryBuilderPanel::availableColumnRefs() const {
  QStringList refs;
  for (int i = 0; i < chosenTables_->count(); ++i) {
    const QString tableName = chosenTables_->item(i)->text();
    const Table* t = schema_.findTable(tableName.toStdString());
    if (!t) continue;
    for (const auto& col : t->columns) {
      refs << tableName + QLatin1Char('.') + QString::fromStdString(col.name);
    }
  }
  return refs;
}

void QueryBuilderPanel::addFilterRow() {
  updating_ = true;
  const int row = filters_->rowCount();
  filters_->insertRow(row);

  auto* column = new QComboBox(filters_);
  column->addItems(availableColumnRefs());
  connect(column, &QComboBox::currentTextChanged, this,
          &QueryBuilderPanel::regenerate);
  filters_->setCellWidget(row, 0, column);

  auto* op = new QComboBox(filters_);
  for (const auto& o : FilterItem::operators()) {
    op->addItem(QString::fromStdString(o));
  }
  connect(op, &QComboBox::currentTextChanged, this,
          &QueryBuilderPanel::regenerate);
  filters_->setCellWidget(row, 1, op);

  filters_->setItem(row, 2, new QTableWidgetItem());

  auto* conj = new QComboBox(filters_);
  conj->addItems({QStringLiteral("AND"), QStringLiteral("OR")});
  conj->setEnabled(row > 0);   // the first filter has nothing to join to
  connect(conj, &QComboBox::currentTextChanged, this,
          &QueryBuilderPanel::regenerate);
  filters_->setCellWidget(row, 3, conj);

  updating_ = false;
  regenerate();
}

void QueryBuilderPanel::removeFilterRow() {
  const int row = filters_->currentRow();
  if (row >= 0) {
    filters_->removeRow(row);
    regenerate();
  }
}

std::pair<QString, QString> QueryBuilderPanel::splitRef(const QString& ref) {
  const int dot = ref.lastIndexOf(QLatin1Char('.'));
  if (dot < 0) return {QString(), ref};
  return {ref.left(dot), ref.mid(dot + 1)};
}

QuerySpec QueryBuilderPanel::collectSpec() const {
  QuerySpec spec;
  for (int i = 0; i < chosenTables_->count(); ++i) {
    spec.tables.push_back(chosenTables_->item(i)->text().toStdString());
  }

  for (int i = 0; i < columns_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* group = columns_->topLevelItem(i);
    for (int j = 0; j < group->childCount(); ++j) {
      QTreeWidgetItem* c = group->child(j);
      if (c->checkState(0) != Qt::Checked) continue;

      SelectItem item;
      item.table = c->data(0, kTableRole).toString().toStdString();
      item.column = c->data(0, kColumnRole).toString().toStdString();
      if (auto* box = qobject_cast<QComboBox*>(columns_->itemWidget(c, 1))) {
        item.aggregate = box->currentText().toStdString();
      }
      item.alias = c->text(2).toStdString();
      spec.select.push_back(std::move(item));
    }
  }

  // Plain columns alongside an aggregate have to be grouped; do it for the
  // user rather than emitting SQL the server will reject.
  const bool anyAggregate =
      std::any_of(spec.select.begin(), spec.select.end(),
                  [](const SelectItem& s) { return !s.aggregate.empty(); });
  if (anyAggregate) {
    for (const auto& s : spec.select) {
      if (s.aggregate.empty()) spec.groupBy.emplace_back(s.table, s.column);
    }
  }

  for (int r = 0; r < filters_->rowCount(); ++r) {
    auto* colBox = qobject_cast<QComboBox*>(filters_->cellWidget(r, 0));
    auto* opBox = qobject_cast<QComboBox*>(filters_->cellWidget(r, 1));
    auto* conjBox = qobject_cast<QComboBox*>(filters_->cellWidget(r, 3));
    if (!colBox || !opBox) continue;
    if (colBox->currentText().isEmpty()) continue;

    auto [table, column] = splitRef(colBox->currentText());
    FilterItem f;
    f.table = table.toStdString();
    f.column = column.toStdString();
    f.op = opBox->currentText().toStdString();
    if (QTableWidgetItem* cell = filters_->item(r, 2)) {
      f.value = cell->text().toStdString();
    }
    f.orWithPrevious =
        conjBox && r > 0 && conjBox->currentText() == QStringLiteral("OR");
    spec.filters.push_back(std::move(f));
  }

  spec.distinct = distinct_->isChecked();
  spec.limit = limit_->value();
  return spec;
}

void QueryBuilderPanel::regenerate() {
  if (updating_) return;

  const QuerySpec spec = collectSpec();
  if (spec.tables.empty()) {
    preview_->setPlainText(
        QStringLiteral("-- Add a table to get started."));
    warnings_->clear();
    return;
  }

  const BuildResult result = buildQuery(schema_, spec, dialect_);
  preview_->setPlainText(QString::fromStdString(result.sql));

  QStringList warnings;
  for (const auto& w : result.warnings) {
    warnings << QString::fromStdString(w);
  }
  warnings_->setText(warnings.join(QLatin1Char('\n')));
}

}  // namespace ds
