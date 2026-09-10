#include "ui/SchemaTree.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/Theme.h"

namespace ds {
namespace {

constexpr int kNodeTypeRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;
constexpr int kTableRole = Qt::UserRole + 3;

enum NodeType { NodeTable, NodeColumn, NodeGroup, NodeIndex, NodeForeignKey };

QString humanBytes(int64_t bytes) {
  if (bytes < 0) return {};
  static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
  return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10 ? 1 : 0).arg(kUnits[u]);
}

}  // namespace

SchemaTree::SchemaTree(QWidget* parent) : QWidget(parent) {
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(QStringLiteral("Filter tables and columns…"));
  filter_->setClearButtonEnabled(true);

  tree_ = new QTreeWidget(this);
  tree_->setColumnCount(2);
  tree_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Detail")});
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree_->header()->setStretchLastSection(false);
  // Long table names elide rather than forcing a horizontal scrollbar.
  tree_->setTextElideMode(Qt::ElideMiddle);
  tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  tree_->setUniformRowHeights(true);
  // Alternating colours paint stripes across the empty area below the last
  // row, which reads as a list of phantom entries. Not worth it here.
  tree_->setAlternatingRowColors(false);
  tree_->setIndentation(14);
  tree_->setAnimated(true);
  tree_->setExpandsOnDoubleClick(false);   // double-click previews instead
  tree_->setIconSize(QSize(15, 15));
  tree_->header()->setStretchLastSection(false);
  // Long table names elide rather than forcing a horizontal scrollbar.
  tree_->setTextElideMode(Qt::ElideMiddle);
  tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);
  layout->addWidget(filter_);
  layout->addWidget(tree_, 1);

  connect(filter_, &QLineEdit::textChanged, this, &SchemaTree::applyFilter);
  connect(tree_, &QTreeWidget::itemActivated, this,
          &SchemaTree::onItemActivated);
  connect(tree_, &QWidget::customContextMenuRequested, this,
          &SchemaTree::showContextMenu);
}

void SchemaTree::setBusy(bool busy) {
  setEnabled(!busy);
}

void SchemaTree::setSchema(const Schema& schema) {
  schema_ = schema;
  tree_->clear();

  for (const auto& t : schema.tables) {
    auto* tableItem = new QTreeWidgetItem(tree_);
    tableItem->setText(0, QString::fromStdString(t.name));
    tableItem->setIcon(0, theme::icon(t.isView ? QStringLiteral("view")
                                               : QStringLiteral("table"),
                                      t.isView ? theme::viewColour()
                                               : theme::tableColour()));
    tableItem->setData(0, kNodeTypeRole, NodeTable);
    tableItem->setData(0, kNameRole, QString::fromStdString(t.name));
    tableItem->setData(0, kTableRole, QString::fromStdString(t.name));

    QStringList detail;
    detail << (t.isView ? QStringLiteral("view") : QStringLiteral("table"));
    if (t.estimatedRows >= 0) {
      detail << QStringLiteral("~%L1 rows").arg(t.estimatedRows);
    }
    if (t.sizeBytes >= 0) detail << humanBytes(t.sizeBytes);
    tableItem->setText(1, detail.join(QStringLiteral(" · ")));
    tableItem->setForeground(1, theme::textMuted());
    if (!t.comment.empty()) {
      tableItem->setToolTip(0, QString::fromStdString(t.comment));
    }

    QFont bold = tableItem->font(0);
    bold.setBold(true);
    tableItem->setFont(0, bold);

    for (const auto& c : t.columns) {
      auto* col = new QTreeWidgetItem(tableItem);
      col->setText(0, QString::fromStdString(c.name));
      // A key or a relationship is worth spotting at a glance; everything else
      // gets the neutral column glyph.
      bool isFk = false;
      for (const auto& fk : t.foreignKeys) {
        for (const auto& fc : fk.fromColumns) {
          if (iequals(fc, c.name)) isFk = true;
        }
      }
      if (c.primaryKey) {
        col->setIcon(0, theme::icon(QStringLiteral("primaryKey"),
                                    theme::keyColour()));
      } else if (isFk) {
        col->setIcon(0, theme::icon(QStringLiteral("foreignKey"),
                                    theme::relationColour()));
      } else {
        col->setIcon(0, theme::icon(QStringLiteral("column"),
                                    theme::textMuted()));
      }
      if (!c.nullable) {
        QFont f = col->font(0);
        f.setBold(true);   // required columns carry more weight
        col->setFont(0, f);
      }
      col->setData(0, kNodeTypeRole, NodeColumn);
      col->setData(0, kNameRole, QString::fromStdString(c.name));
      col->setData(0, kTableRole, QString::fromStdString(t.name));

      QStringList bits;
      bits << QString::fromStdString(c.type);
      if (!c.nullable) bits << QStringLiteral("NOT NULL");
      if (c.autoIncrement) bits << QStringLiteral("AUTO");
      if (c.defaultValue) {
        bits << QStringLiteral("default %1")
                    .arg(QString::fromStdString(*c.defaultValue));
      }
      col->setText(1, bits.join(QStringLiteral(" · ")));
      col->setForeground(1, theme::textMuted());
      if (!c.comment.empty()) {
        col->setToolTip(0, QString::fromStdString(c.comment));
      }
    }

    if (!t.foreignKeys.empty()) {
      auto* group = new QTreeWidgetItem(tableItem);
      group->setText(0, QStringLiteral("Foreign keys"));
      group->setIcon(0, theme::icon(QStringLiteral("foreignKey"),
                                    theme::relationColour()));
      group->setForeground(0, theme::textMuted());
      group->setData(0, kNodeTypeRole, NodeGroup);
      for (const auto& fk : t.foreignKeys) {
        auto* item = new QTreeWidgetItem(group);
        QStringList from, to;
        for (const auto& c : fk.fromColumns) {
          from << QString::fromStdString(c);
        }
        for (const auto& c : fk.toColumns) to << QString::fromStdString(c);
        item->setText(0, from.join(QStringLiteral(", ")));
        item->setText(1, QStringLiteral("→ %1 (%2)")
                             .arg(QString::fromStdString(fk.toTable),
                                  to.join(QStringLiteral(", "))));
        item->setData(0, kNodeTypeRole, NodeForeignKey);
        item->setData(0, kTableRole, QString::fromStdString(fk.toTable));
      }
    }

    if (!t.indexes.empty()) {
      auto* group = new QTreeWidgetItem(tableItem);
      group->setText(0, QStringLiteral("Indexes"));
      group->setIcon(0, theme::icon(QStringLiteral("index"),
                                    theme::indexColour()));
      group->setForeground(0, theme::textMuted());
      group->setData(0, kNodeTypeRole, NodeGroup);
      for (const auto& idx : t.indexes) {
        auto* item = new QTreeWidgetItem(group);
        item->setText(0, QString::fromStdString(idx.name));
        item->setIcon(0, theme::icon(QStringLiteral("index"),
                                     theme::indexColour()));
        QStringList cols;
        for (const auto& c : idx.columns) cols << QString::fromStdString(c);
        QString detail = cols.join(QStringLiteral(", "));
        if (idx.primary) detail += QStringLiteral("  · PRIMARY");
        else if (idx.unique) detail += QStringLiteral("  · UNIQUE");
        item->setText(1, detail);
        item->setData(0, kNodeTypeRole, NodeIndex);
      }
    }
  }

  if (schema.tables.empty()) {
    auto* empty = new QTreeWidgetItem(tree_);
    empty->setText(0, QStringLiteral("No tables"));
    empty->setText(1, QStringLiteral("connect to a database to browse it"));
    empty->setForeground(0, theme::textMuted());
    empty->setForeground(1, theme::textMuted());
    empty->setFlags(Qt::NoItemFlags);
  }

  applyFilter(filter_->text());
}

void SchemaTree::applyFilter(const QString& text) {
  const QString needle = text.trimmed();
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* table = tree_->topLevelItem(i);
    if (needle.isEmpty()) {
      table->setHidden(false);
      table->setExpanded(false);
      for (int j = 0; j < table->childCount(); ++j) {
        table->child(j)->setHidden(false);
      }
      continue;
    }

    const bool tableMatches =
        table->text(0).contains(needle, Qt::CaseInsensitive);
    bool anyChildMatches = false;
    for (int j = 0; j < table->childCount(); ++j) {
      QTreeWidgetItem* child = table->child(j);
      const bool m = child->text(0).contains(needle, Qt::CaseInsensitive);
      // A matching table shows all its columns; otherwise only the matches.
      child->setHidden(!tableMatches && !m);
      anyChildMatches = anyChildMatches || m;
    }
    table->setHidden(!tableMatches && !anyChildMatches);
    // Expand so the matching column is actually visible.
    table->setExpanded(anyChildMatches && !tableMatches);
  }
}

void SchemaTree::onItemActivated(QTreeWidgetItem* item, int) {
  if (!item) return;
  const int type = item->data(0, kNodeTypeRole).toInt();
  const QString name = item->data(0, kNameRole).toString();

  if (type == NodeTable) {
    emit previewTableRequested(name);
  } else if (type == NodeColumn) {
    emit insertText(name);
  }
}

void SchemaTree::showContextMenu(const QPoint& pos) {
  QTreeWidgetItem* item = tree_->itemAt(pos);
  QMenu menu(this);

  if (item) {
    const int type = item->data(0, kNodeTypeRole).toInt();
    const QString name = item->data(0, kNameRole).toString();
    const QString table = item->data(0, kTableRole).toString();

    if (type == NodeTable) {
      menu.addAction(QStringLiteral("Preview 200 rows"), this,
                     [this, table] { emit previewTableRequested(table); });
      menu.addAction(QStringLiteral("Show in relationship diagram"), this,
                     [this, table] { emit showInDiagramRequested(table); });
      menu.addAction(QStringLiteral("Insert name"), this,
                     [this, name] { emit insertText(name); });
      menu.addSeparator();
      menu.addAction(QStringLiteral("Insert SELECT with all columns"), this,
                     [this, table] {
                       const Table* t = schema_.findTable(table.toStdString());
                       if (!t) return;
                       QStringList cols;
                       for (const auto& c : t->columns) {
                         cols << QString::fromStdString(c.name);
                       }
                       emit insertText(
                           QStringLiteral("SELECT %1\nFROM %2\nLIMIT 100")
                               .arg(cols.join(QStringLiteral(",\n       ")),
                                    table));
                     });
    } else if (type == NodeColumn) {
      menu.addAction(QStringLiteral("Profile column"), this,
                     [this, table, name] {
                       emit profileColumnRequested(table, name);
                     });
      menu.addSeparator();
      menu.addAction(QStringLiteral("Insert name"), this,
                     [this, name] { emit insertText(name); });
      menu.addAction(QStringLiteral("Insert qualified name"), this,
                     [this, table, name] {
                       emit insertText(table + QLatin1Char('.') + name);
                     });
    }
    if (!name.isEmpty()) {
      menu.addAction(QStringLiteral("Copy name"), this, [name] {
        QApplication::clipboard()->setText(name);
      });
    }
    menu.addSeparator();
  }

  menu.addAction(QStringLiteral("Refresh schema"), this,
                 [this] { emit refreshRequested(); });
  menu.addAction(QStringLiteral("Expand all"), tree_,
                 &QTreeWidget::expandAll);
  menu.addAction(QStringLiteral("Collapse all"), tree_,
                 &QTreeWidget::collapseAll);
  menu.exec(tree_->viewport()->mapToGlobal(pos));
}

}  // namespace ds
