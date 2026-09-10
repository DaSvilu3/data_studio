#pragma once
#include <QWidget>

#include "core/Schema.h"
#include "core/Value.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;

namespace ds {

// Browsable tree of tables → columns / indexes / foreign keys, with a filter
// box. Double-clicking a node inserts its name into the editor.
class SchemaTree : public QWidget {
  Q_OBJECT
 public:
  explicit SchemaTree(QWidget* parent = nullptr);

  void setSchema(const Schema& schema);
  void setBusy(bool busy);

 signals:
  void insertText(const QString& text);
  void previewTableRequested(const QString& table);
  void refreshRequested();

 private slots:
  void applyFilter(const QString& text);
  void onItemActivated(QTreeWidgetItem* item, int column);
  void showContextMenu(const QPoint& pos);

 private:
  QTreeWidget* tree_ = nullptr;
  QLineEdit* filter_ = nullptr;
  Schema schema_;
};

}  // namespace ds
