#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "core/Schema.h"
#include "query/SqlLexer.h"

namespace ds {

// One table mentioned in FROM / JOIN / UPDATE / INSERT INTO, with its alias.
struct TableRef {
  std::string table;
  std::string alias;   // empty when the table was named without one
  size_t position = 0; // token offset where the name appeared

  // What the user would type to qualify a column from this ref.
  const std::string& handle() const { return alias.empty() ? table : alias; }
};

// Which clause the cursor sits in. Drives what we offer to complete.
enum class Clause {
  Unknown, Select, From, Join, On, Where, GroupBy, Having, OrderBy,
  Limit, Set, Insert, Values, Update, Delete, Create,
};

const char* clauseName(Clause c);

struct CursorContext {
  Clause clause = Clause::Unknown;
  std::vector<TableRef> tablesInScope;

  std::string prefix;      // the partial word under the cursor
  std::string qualifier;   // "u" in `u.na|` -- empty when unqualified
  size_t replaceStart = 0; // where an accepted completion should be written
  size_t replaceLength = 0;

  bool afterDot = false;
  // True when the statement is a bare `SELECT ... FROM` with nothing after
  // FROM yet -- we lead with tables rather than columns.
  bool expectsTable = false;
  bool expectsColumn = false;
};

enum class CompletionKind { Column, Table, Alias, Keyword, Function, JoinClause };

struct Completion {
  CompletionKind kind = CompletionKind::Keyword;
  std::string insertText;  // what actually goes into the document
  std::string label;       // shown in the popup
  std::string detail;      // right-hand grey text: type, table, FK target
  int score = 0;           // higher sorts first
};

// Reads the statement around `cursor` (a byte offset) and works out what the
// user is in the middle of typing.
CursorContext analyzeCursor(const std::string& sql, size_t cursor);

// Ranked completions for that position, drawn from the live schema.
std::vector<Completion> completeAt(const Schema& schema, const std::string& sql,
                                   size_t cursor);

// The statement containing `cursor`, as a [begin, end) byte range. Used to run
// just the statement under the caret.
std::pair<size_t, size_t> statementRangeAt(const std::string& sql, size_t cursor);

}  // namespace ds
