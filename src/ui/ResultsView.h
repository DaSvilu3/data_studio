#pragma once
#include <QWidget>

#include "core/Value.h"

class QTableView;
class QLabel;
class QTextEdit;
class QSortFilterProxyModel;
class QLineEdit;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace ds {

class ResultsModel;

// Grid plus status line, a filter box and a cell inspector for values too big
// to read in a row.
class ResultsView : public QWidget {
  Q_OBJECT
 public:
  explicit ResultsView(QWidget* parent = nullptr);

  void setResult(const ResultSet& result);
  void setError(const QString& message, const QString& statement);
  // Appends a follow-up line to the error already on screen.
  void addErrorHint(const QString& hint);
  void clear();
  void setDialect(Dialect d) { dialect_ = d; }

 signals:
  void statusMessage(const QString& text);

 private slots:
  void copySelection();
  void copySelectionAsCsv();
  void copySelectionAsInsert();
  void showContextMenu(const QPoint& pos);
  void updateInspector();

 private:
  QString sourceTableName() const;

  QTableView* table_ = nullptr;
  ResultsModel* model_ = nullptr;
  QSortFilterProxyModel* proxy_ = nullptr;
  QLineEdit* filter_ = nullptr;
  QLabel* status_ = nullptr;
  QTextEdit* inspector_ = nullptr;
  QSplitter* splitter_ = nullptr;
  // Swaps between the grid and a placeholder, so an empty pane explains itself
  // instead of showing a void.
  QStackedWidget* stack_ = nullptr;
  QLabel* placeholder_ = nullptr;
  QToolButton* inspectorToggle_ = nullptr;
  Dialect dialect_ = Dialect::Sqlite;

  void showPlaceholder(const QString& text);
};

}  // namespace ds
