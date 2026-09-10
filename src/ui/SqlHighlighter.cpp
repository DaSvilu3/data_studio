#include "ui/SqlHighlighter.h"

#include <QPalette>
#include <QApplication>

#include "query/SqlLexer.h"

namespace ds {

SqlHighlighter::SqlHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {
  const bool dark =
      QApplication::palette().color(QPalette::Window).lightness() < 128;

  auto colour = [&](const char* lightHex, const char* darkHex) {
    return QColor(dark ? darkHex : lightHex);
  };

  keyword_.setForeground(colour("#0033b3", "#cf8e6d"));
  keyword_.setFontWeight(QFont::DemiBold);
  function_.setForeground(colour("#7a3e9d", "#c77dbb"));
  string_.setForeground(colour("#067d17", "#6aab73"));
  number_.setForeground(colour("#1750eb", "#2aacb8"));
  comment_.setForeground(colour("#8c8c8c", "#7a7e85"));
  comment_.setFontItalic(true);
  identifier_.setForeground(colour("#000000", "#bcbec4"));
  knownName_.setForeground(colour("#1d5a8a", "#57aaf7"));
  parameter_.setForeground(colour("#9c5d00", "#d5b778"));
  operatorFmt_.setForeground(colour("#444444", "#a9b7c6"));

  errorFmt_.setUnderlineStyle(QTextCharFormat::WaveUnderline);
  errorFmt_.setUnderlineColor(QColor("#e05252"));
}

void SqlHighlighter::setSchema(const Schema& schema) {
  knownNames_.clear();
  for (const auto& t : schema.tables) {
    knownNames_.insert(QString::fromStdString(t.name).toLower());
    for (const auto& c : t.columns) {
      knownNames_.insert(QString::fromStdString(c.name).toLower());
    }
  }
  rehighlight();
}

void SqlHighlighter::setErrorRange(int start, int length) {
  errorStart_ = start;
  errorLength_ = length;
  rehighlight();
}

void SqlHighlighter::clearErrorRange() {
  if (errorStart_ < 0) return;
  errorStart_ = -1;
  errorLength_ = 0;
  rehighlight();
}

void SqlHighlighter::highlightBlock(const QString& text) {
  // Block comments can span lines, so carry the state across blocks.
  const int kInBlockComment = 1;
  int state = previousBlockState() == kInBlockComment ? kInBlockComment : 0;

  QString working = text;
  int offset = 0;
  if (state == kInBlockComment) {
    const int end = text.indexOf(QStringLiteral("*/"));
    if (end < 0) {
      setFormat(0, text.length(), comment_);
      setCurrentBlockState(kInBlockComment);
      return;
    }
    setFormat(0, end + 2, comment_);
    offset = end + 2;
    working = text.mid(offset);
    state = 0;
  }

  const auto tokens = tokenize(working.toStdString(), /*keepTrivia=*/true);
  for (const auto& t : tokens) {
    const int start = offset + static_cast<int>(t.start);
    const int len = static_cast<int>(t.length);
    if (len <= 0) continue;

    switch (t.type) {
      case TokenType::Keyword:
        setFormat(start, len, keyword_);
        break;
      case TokenType::String:
        setFormat(start, len, string_);
        break;
      case TokenType::Number:
        setFormat(start, len, number_);
        break;
      case TokenType::Comment:
        setFormat(start, len, comment_);
        // An unterminated /* runs into the next block.
        if (t.text.rfind("/*", 0) == 0 &&
            t.text.find("*/") == std::string::npos) {
          state = kInBlockComment;
        }
        break;
      case TokenType::Parameter:
        setFormat(start, len, parameter_);
        break;
      case TokenType::Operator:
        setFormat(start, len, operatorFmt_);
        break;
      case TokenType::Identifier:
      case TokenType::QuotedIdentifier: {
        const QString name =
            QString::fromStdString(t.unquoted()).toLower();
        if (isSqlFunction(t.text)) {
          setFormat(start, len, function_);
        } else if (knownNames_.contains(name)) {
          setFormat(start, len, knownName_);
        } else {
          setFormat(start, len, identifier_);
        }
        break;
      }
      default:
        break;
    }
  }

  setCurrentBlockState(state);

  // Overlay the squiggle for a reported error, if it falls in this block.
  if (errorStart_ >= 0 && errorLength_ > 0) {
    const int blockStart = currentBlock().position();
    const int localStart = errorStart_ - blockStart;
    if (localStart < text.length() && localStart + errorLength_ > 0) {
      const int from = qMax(0, localStart);
      const int to = qMin(text.length(), localStart + errorLength_);
      if (to > from) {
        QTextCharFormat merged = format(from);
        merged.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        merged.setUnderlineColor(QColor("#e05252"));
        setFormat(from, to - from, merged);
      }
    }
  }
}

}  // namespace ds
