#include "ui/SqlEditor.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QSet>

#include <cctype>
#include <QTextBlock>

#include "query/JoinGraph.h"
#include "query/QueryBuilder.h"
#include "ui/SqlHighlighter.h"
#include "ui/Theme.h"

namespace ds {
namespace {

constexpr int kCompletionRole = Qt::UserRole + 1;   // text to insert
constexpr int kKindRole = Qt::UserRole + 2;

class LineNumberArea : public QWidget {
 public:
  explicit LineNumberArea(SqlEditor* editor)
      : QWidget(editor), editor_(editor) {}

  QSize sizeHint() const override {
    return QSize(editor_->lineNumberAreaWidth(), 0);
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    editor_->lineNumberAreaPaintEvent(event);
  }

 private:
  SqlEditor* editor_;
};

QString kindLabel(CompletionKind k) {
  switch (k) {
    case CompletionKind::Column: return QStringLiteral("col");
    case CompletionKind::Table: return QStringLiteral("tbl");
    case CompletionKind::Alias: return QStringLiteral("as");
    case CompletionKind::Keyword: return QStringLiteral("kw");
    case CompletionKind::Function: return QStringLiteral("fn");
    case CompletionKind::JoinClause: return QStringLiteral("join");
  }
  return {};
}

QColor kindColour(CompletionKind k) {
  switch (k) {
    case CompletionKind::Column: return QColor("#57aaf7");
    case CompletionKind::Table: return QColor("#c77dbb");
    case CompletionKind::Alias: return QColor("#6aab73");
    case CompletionKind::JoinClause: return QColor("#e0a458");
    default: return QColor("#8c8c8c");
  }
}

}  // namespace

SqlEditor::SqlEditor(QWidget* parent) : QPlainTextEdit(parent) {
  const QFont mono = theme::monospaceFont(13);
  setFont(mono);
  setTabStopDistance(4 * QFontMetricsF(mono).horizontalAdvance(' '));
  setLineWrapMode(QPlainTextEdit::NoWrap);
  document()->setDocumentMargin(10);
  setPlaceholderText(
      QStringLiteral("Write SQL here.  ⌘↩ runs the statement under the "
                     "cursor.  ⌃Space completes from the live schema."));

  lineNumbers_ = new LineNumberArea(this);
  highlighter_ = new SqlHighlighter(document());

  completionModel_ = new QStandardItemModel(this);
  completer_ = new QCompleter(completionModel_, this);
  completer_->setWidget(this);
  completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
  completer_->setCaseSensitivity(Qt::CaseInsensitive);
  connect(completer_,
          QOverload<const QModelIndex&>::of(&QCompleter::activated), this,
          &SqlEditor::insertCompletion);

  settleTimer_.setSingleShot(true);
  settleTimer_.setInterval(400);
  connect(&settleTimer_, &QTimer::timeout, this, &SqlEditor::textSettled);

  connect(this, &QPlainTextEdit::blockCountChanged, this,
          &SqlEditor::updateLineNumberAreaWidth);
  connect(this, &QPlainTextEdit::updateRequest, this,
          &SqlEditor::updateLineNumberArea);
  connect(this, &QPlainTextEdit::cursorPositionChanged, this,
          &SqlEditor::highlightCurrentLine);
  connect(this, &QPlainTextEdit::textChanged, this, [this] {
    highlighter_->clearErrorRange();
    settleTimer_.start();
  });

  updateLineNumberAreaWidth();
  highlightCurrentLine();
}

void SqlEditor::setSchema(const Schema& schema) {
  schema_ = schema;
  highlighter_->setSchema(schema);
}

QString SqlEditor::currentStatement() const {
  const QTextCursor cursor = textCursor();
  if (cursor.hasSelection()) return cursor.selectedText().replace(
      QChar(0x2029), QLatin1Char('\n'));

  const std::string all = toPlainText().toStdString();
  size_t position = static_cast<size_t>(cursor.position());

  auto range = statementRangeAt(all, position);
  if (range.second <= range.first) {
    // The caret is past the last semicolon, on trailing whitespace. Running
    // nothing at all looks like the app ignored the keystroke, so fall back to
    // the statement that just ended.
    size_t back = std::min(position, all.size());
    while (back > 0 && std::isspace(static_cast<unsigned char>(all[back - 1]))) {
      --back;
    }
    if (back > 0 && all[back - 1] == ';') --back;
    if (back == 0) return {};
    range = statementRangeAt(all, back);
  }
  if (range.second <= range.first) return {};
  return QString::fromStdString(
      all.substr(range.first, range.second - range.first));
}

void SqlEditor::markError(int offset, int length) {
  highlighter_->setErrorRange(offset, length);
}

void SqlEditor::clearError() { highlighter_->clearErrorRange(); }

void SqlEditor::setSoftWrap(bool on) {
  setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

bool SqlEditor::softWrap() const {
  return lineWrapMode() == QPlainTextEdit::WidgetWidth;
}

QString SqlEditor::formatSql(const QString& sql) {
  const auto tokens = tokenize(sql.toStdString());
  if (tokens.empty()) return sql;

  // Clause keywords start a new line; JOIN variants and boolean connectives
  // get indented under the clause they belong to.
  static const QSet<QString> kClause = {
      QStringLiteral("SELECT"), QStringLiteral("FROM"),
      QStringLiteral("WHERE"),  QStringLiteral("HAVING"),
      QStringLiteral("LIMIT"),  QStringLiteral("UNION"),
      QStringLiteral("INSERT"), QStringLiteral("UPDATE"),
      QStringLiteral("DELETE"), QStringLiteral("VALUES"),
      QStringLiteral("SET")};
  static const QSet<QString> kIndented = {
      QStringLiteral("JOIN"), QStringLiteral("ON"), QStringLiteral("AND"),
      QStringLiteral("OR")};

  QString out;
  const std::string source = sql.toStdString();

  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token& t = tokens[i];
    const QString text = QString::fromStdString(t.text);
    const QString upper = text.toUpper();

    bool newline = false;
    QString indent;
    if (t.type == TokenType::Keyword) {
      if (kClause.contains(upper)) {
        newline = true;
      } else if (upper == QStringLiteral("GROUP") ||
                 upper == QStringLiteral("ORDER")) {
        newline = true;
      } else if (kIndented.contains(upper)) {
        newline = true;
        indent = QStringLiteral("  ");
      } else if (upper == QStringLiteral("LEFT") ||
                 upper == QStringLiteral("RIGHT") ||
                 upper == QStringLiteral("INNER") ||
                 upper == QStringLiteral("OUTER") ||
                 upper == QStringLiteral("CROSS")) {
        newline = true;
      }
    }

    // A JOIN right after LEFT/INNER/... belongs on the same line as it.
    if (newline && i > 0 && tokens[i - 1].type == TokenType::Keyword) {
      const QString prev = QString::fromStdString(tokens[i - 1].text).toUpper();
      if (upper == QStringLiteral("JOIN") &&
          (prev == QStringLiteral("LEFT") || prev == QStringLiteral("RIGHT") ||
           prev == QStringLiteral("INNER") || prev == QStringLiteral("OUTER") ||
           prev == QStringLiteral("CROSS"))) {
        newline = false;
      }
      if (upper == QStringLiteral("BY") &&
          (prev == QStringLiteral("GROUP") || prev == QStringLiteral("ORDER"))) {
        newline = false;
      }
    }

    if (newline && !out.isEmpty()) {
      out += QLatin1Char('\n');
      out += indent;
    } else if (!out.isEmpty()) {
      // Keep punctuation tight against what it follows.
      bool tight = t.type == TokenType::Punctuation &&
                   (text == QStringLiteral(",") ||
                    text == QStringLiteral(")") ||
                    text == QStringLiteral(".") ||
                    text == QStringLiteral(";"));
      // A call belongs to its name: SUM(x), not SUM (x). Keywords that take a
      // parenthesised list -- IN, VALUES -- keep their space by convention.
      if (!tight && text == QStringLiteral("(") && i > 0) {
        const Token& prev = tokens[i - 1];
        tight = prev.type == TokenType::Identifier ||
                (prev.type == TokenType::Keyword &&
                 isSqlFunction(prev.text));
      }
      const QString last = out.right(1);
      const bool afterOpen = last == QStringLiteral("(") ||
                             last == QStringLiteral(".");
      if (!tight && !afterOpen) out += QLatin1Char(' ');
    }
    out += text;
  }
  Q_UNUSED(source);
  return out;
}

void SqlEditor::formatCurrentStatement() {
  const std::string all = toPlainText().toStdString();
  const auto [begin, end] =
      statementRangeAt(all, static_cast<size_t>(textCursor().position()));
  if (end <= begin) return;

  const QString original =
      QString::fromStdString(all.substr(begin, end - begin));
  const QString formatted = formatSql(original);
  if (formatted == original) return;

  QTextCursor cursor = textCursor();
  cursor.beginEditBlock();
  cursor.setPosition(static_cast<int>(begin));
  cursor.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
  cursor.insertText(formatted);
  cursor.endEditBlock();
}

int SqlEditor::lineNumberAreaWidth() const {
  int digits = 1;
  int max = qMax(1, blockCount());
  while (max >= 10) { max /= 10; ++digits; }
  return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void SqlEditor::updateLineNumberAreaWidth() {
  setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void SqlEditor::updateLineNumberArea(const QRect& rect, int dy) {
  if (dy) {
    lineNumbers_->scroll(0, dy);
  } else {
    lineNumbers_->update(0, rect.y(), lineNumbers_->width(), rect.height());
  }
  if (rect.contains(viewport()->rect())) updateLineNumberAreaWidth();
}

void SqlEditor::resizeEvent(QResizeEvent* e) {
  QPlainTextEdit::resizeEvent(e);
  const QRect cr = contentsRect();
  lineNumbers_->setGeometry(
      QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void SqlEditor::focusInEvent(QFocusEvent* e) {
  completer_->setWidget(this);
  QPlainTextEdit::focusInEvent(e);
}

void SqlEditor::lineNumberAreaPaintEvent(QPaintEvent* event) {
  QPainter painter(lineNumbers_);
  const QColor bg = palette().color(QPalette::Base).darker(105);
  painter.fillRect(event->rect(), bg);

  QTextBlock block = firstVisibleBlock();
  int blockNumber = block.blockNumber();
  int top = static_cast<int>(
      blockBoundingGeometry(block).translated(contentOffset()).top());
  int bottom = top + static_cast<int>(blockBoundingRect(block).height());
  const int currentLine = textCursor().blockNumber();

  while (block.isValid() && top <= event->rect().bottom()) {
    if (block.isVisible() && bottom >= event->rect().top()) {
      painter.setPen(blockNumber == currentLine
                         ? palette().color(QPalette::Text)
                         : palette().color(QPalette::Mid));
      painter.drawText(0, top, lineNumbers_->width() - 7,
                       fontMetrics().height(), Qt::AlignRight,
                       QString::number(blockNumber + 1));
    }
    block = block.next();
    top = bottom;
    bottom = top + static_cast<int>(blockBoundingRect(block).height());
    ++blockNumber;
  }
}

void SqlEditor::highlightCurrentLine() {
  QList<QTextEdit::ExtraSelection> selections;
  if (!isReadOnly()) {
    QTextEdit::ExtraSelection sel;
    sel.format.setBackground(palette().color(QPalette::Base).darker(103));
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    sel.cursor = textCursor();
    sel.cursor.clearSelection();
    selections.append(sel);
  }
  setExtraSelections(selections);
}

void SqlEditor::appendJoinSuggestions(const CursorContext& ctx,
                                      std::vector<Completion>* out) const {
  if (ctx.tablesInScope.empty()) return;
  // Only worth offering right where a join would go.
  if (ctx.clause != Clause::From && ctx.clause != Clause::Join &&
      ctx.clause != Clause::Where) {
    return;
  }

  JoinGraph graph(schema_);
  std::vector<std::string> present;
  for (const auto& r : ctx.tablesInScope) present.push_back(r.table);

  for (const auto& ref : ctx.tablesInScope) {
    for (const auto& edge : graph.neighbors(ref.table)) {
      // Skip tables already joined in.
      bool already = false;
      for (const auto& p : present) {
        if (iequals(p, edge.toTable)) { already = true; break; }
      }
      if (already) continue;

      std::vector<std::string> taken;
      for (const auto& r : ctx.tablesInScope) taken.push_back(r.handle());
      const std::string alias = aliasFor(edge.toTable, taken);

      Completion c;
      c.kind = CompletionKind::JoinClause;
      c.label = "JOIN " + edge.toTable;
      c.insertText = std::string(edge.reversed ? "LEFT JOIN " : "JOIN ") +
                     edge.toTable + " " + alias + " ON " +
                     edge.onClause(ref.handle(), alias);
      c.detail = edge.reversed
                     ? "one-to-many via " + edge.constraintName
                     : "many-to-one via " + edge.constraintName;
      // Above tables, below exact-prefix columns.
      c.score = 950;
      out->push_back(std::move(c));
    }
  }
}

void SqlEditor::triggerCompletion(bool explicitRequest) {
  const std::string sql = toPlainText().toStdString();
  const size_t pos = static_cast<size_t>(textCursor().position());

  const CursorContext ctx = analyzeCursor(sql, pos);

  // Without an explicit request, don't pop up on an empty prefix -- it would
  // fire on every space.
  if (!explicitRequest && ctx.prefix.empty() && !ctx.afterDot) {
    hideCompleter();
    return;
  }

  std::vector<Completion> items = completeAt(schema_, sql, pos);
  if (!ctx.afterDot) appendJoinSuggestions(ctx, &items);

  // Join suggestions are appended after sorting, so re-sort.
  std::sort(items.begin(), items.end(),
            [](const Completion& a, const Completion& b) {
              return a.score > b.score;
            });
  if (items.size() > 60) items.resize(60);

  if (items.empty()) {
    hideCompleter();
    return;
  }

  replaceStart_ = static_cast<int>(ctx.replaceStart);
  replaceLength_ = static_cast<int>(ctx.replaceLength);

  completionModel_->clear();
  for (const auto& c : items) {
    auto* item = new QStandardItem(QString::fromStdString(c.label));
    item->setData(QString::fromStdString(c.insertText), kCompletionRole);
    item->setData(static_cast<int>(c.kind), kKindRole);
    item->setToolTip(QString::fromStdString(c.detail));
    // The kind badge and detail ride along in the display string; a delegate
    // would look nicer but this keeps the popup a plain list view.
    item->setText(QStringLiteral("%1   %2   %3")
                      .arg(kindLabel(c.kind), -5)
                      .arg(QString::fromStdString(c.label), -28)
                      .arg(QString::fromStdString(c.detail)));
    item->setForeground(kindColour(c.kind));
    completionModel_->appendRow(item);
  }

  QRect rect = cursorRect();
  rect.setWidth(completer_->popup()->sizeHintForColumn(0) +
                completer_->popup()->verticalScrollBar()->sizeHint().width() +
                20);
  completer_->popup()->setFont(font());
  completer_->complete(rect);
  completer_->popup()->setCurrentIndex(
      completer_->completionModel()->index(0, 0));
}

void SqlEditor::hideCompleter() {
  if (completer_->popup()->isVisible()) completer_->popup()->hide();
}

void SqlEditor::insertCompletion(const QModelIndex& index) {
  const QString text = index.data(kCompletionRole).toString();
  if (text.isEmpty()) return;

  QTextCursor cursor = textCursor();
  cursor.setPosition(replaceStart_);
  cursor.setPosition(replaceStart_ + replaceLength_, QTextCursor::KeepAnchor);
  cursor.insertText(text);
  setTextCursor(cursor);
}

void SqlEditor::keyPressEvent(QKeyEvent* e) {
  QAbstractItemView* popup = completer_->popup();
  if (popup->isVisible()) {
    switch (e->key()) {
      case Qt::Key_Enter:
      case Qt::Key_Return:
      case Qt::Key_Tab:
        // Let the popup consume these so they accept the highlighted row.
        insertCompletion(popup->currentIndex());
        popup->hide();
        e->accept();
        return;
      case Qt::Key_Escape:
        popup->hide();
        e->accept();
        return;
      case Qt::Key_Up:
      case Qt::Key_Down:
      case Qt::Key_PageUp:
      case Qt::Key_PageDown:
        e->ignore();
        return;
      default:
        break;
    }
  }

  const bool runChord =
      (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
      (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
  if (runChord) {
    emit runRequested();
    e->accept();
    return;
  }

  const bool completeChord =
      e->key() == Qt::Key_Space && (e->modifiers() & Qt::ControlModifier);
  if (completeChord) {
    triggerCompletion(true);
    e->accept();
    return;
  }

  QPlainTextEdit::keyPressEvent(e);

  // Re-offer completions as the user types a word, and immediately after a dot.
  const QString typed = e->text();
  if (typed.isEmpty()) {
    if (popup->isVisible()) hideCompleter();
    return;
  }
  const QChar c = typed.at(0);
  if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.')) {
    triggerCompletion(false);
  } else if (popup->isVisible()) {
    hideCompleter();
  }
}

}  // namespace ds
