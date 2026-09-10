#pragma once
#include <QWidget>

#include "core/Schema.h"
#include "query/QueryBuilder.h"

class QListWidget;
class QTreeWidget;
class QTableWidget;
class QPlainTextEdit;
class QCheckBox;
class QSpinBox;
class QLabel;
class QComboBox;

namespace ds {

// Point-and-click query construction. Tables are picked from a list, joins are
// derived from foreign keys, and the SQL preview updates on every change.
class QueryBuilderPanel : public QWidget {
  Q_OBJECT
 public:
  explicit QueryBuilderPanel(QWidget* parent = nullptr);

  void setSchema(const Schema& schema);
  void setDialect(Dialect d) { dialect_ = d; regenerate(); }

 signals:
  void sendToEditor(const QString& sql);
  void runRequested(const QString& sql);

 private slots:
  void addSelectedTable();
  void removeChosenTable();
  void addFilterRow();
  void removeFilterRow();
  void regenerate();

 private:
  void rebuildColumnTree();
  QuerySpec collectSpec() const;
  // "table.column" split back out for the spec.
  static std::pair<QString, QString> splitRef(const QString& ref);
  QStringList availableColumnRefs() const;

  QListWidget* allTables_ = nullptr;
  QListWidget* chosenTables_ = nullptr;
  QTreeWidget* columns_ = nullptr;      // checkable, with an aggregate column
  QTableWidget* filters_ = nullptr;
  QListWidget* sort_ = nullptr;
  QCheckBox* distinct_ = nullptr;
  QSpinBox* limit_ = nullptr;
  QPlainTextEdit* preview_ = nullptr;
  QLabel* warnings_ = nullptr;

  Schema schema_;
  Dialect dialect_ = Dialect::Sqlite;
  bool updating_ = false;
};

}  // namespace ds
