#pragma once
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QSet>

#include "core/Schema.h"

namespace ds {

// Colours SQL by token class, and additionally marks identifiers that match a
// real table or column in the connected schema -- so a typo stands out before
// the query is ever run.
class SqlHighlighter : public QSyntaxHighlighter {
  Q_OBJECT
 public:
  explicit SqlHighlighter(QTextDocument* doc);

  void setSchema(const Schema& schema);
  void setErrorRange(int start, int length);
  void clearErrorRange();

 protected:
  void highlightBlock(const QString& text) override;

 private:
  QTextCharFormat keyword_, function_, string_, number_, comment_,
      identifier_, knownName_, parameter_, operatorFmt_, errorFmt_;
  QSet<QString> knownNames_;   // lower-cased table and column names
  int errorStart_ = -1;
  int errorLength_ = 0;
};

}  // namespace ds
