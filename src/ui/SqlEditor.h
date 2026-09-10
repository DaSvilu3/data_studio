#pragma once
#include <QPlainTextEdit>
#include <QTimer>

#include "core/Schema.h"
#include "core/Value.h"
#include "query/SqlContext.h"

class QCompleter;
class QStandardItemModel;
class QListView;

namespace ds {

class SqlHighlighter;

// SQL editor with line numbers, current-line highlight, schema-aware
// completion and FK-derived JOIN suggestions.
class SqlEditor : public QPlainTextEdit {
  Q_OBJECT
 public:
  explicit SqlEditor(QWidget* parent = nullptr);

  void setSchema(const Schema& schema);
  void setDialect(Dialect d) { dialect_ = d; }

  // The statement under the caret, or the selection when there is one.
  QString currentStatement() const;
  void markError(int offsetInDocument, int length);
  void clearError();

  void lineNumberAreaPaintEvent(QPaintEvent* event);
  int lineNumberAreaWidth() const;

 signals:
  void runRequested();
  void textSettled();   // fires ~400ms after typing stops, for live analysis

 protected:
  void keyPressEvent(QKeyEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void focusInEvent(QFocusEvent* e) override;

 private slots:
  void updateLineNumberAreaWidth();
  void updateLineNumberArea(const QRect& rect, int dy);
  void highlightCurrentLine();
  void insertCompletion(const QModelIndex& index);

 private:
  void triggerCompletion(bool explicitRequest);
  void hideCompleter();
  // Offers `JOIN other ON a.x = b.y` for every table reachable by a foreign
  // key from what's already in the FROM clause.
  void appendJoinSuggestions(const CursorContext& ctx,
                             std::vector<Completion>* out) const;

  QWidget* lineNumbers_ = nullptr;
  SqlHighlighter* highlighter_ = nullptr;
  QCompleter* completer_ = nullptr;
  QStandardItemModel* completionModel_ = nullptr;
  QTimer settleTimer_;
  Schema schema_;
  Dialect dialect_ = Dialect::Sqlite;
  // Where an accepted completion should be written, captured when the popup
  // was built -- the cursor may have moved by the time it's accepted.
  int replaceStart_ = 0;
  int replaceLength_ = 0;
};

}  // namespace ds
