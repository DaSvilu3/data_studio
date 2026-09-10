#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/Schema.h"
#include "core/Value.h"
#include "query/JoinGraph.h"

namespace ds {

struct SelectItem {
  std::string table;      // table name as selected in the canvas
  std::string column;     // "*" selects everything from that table
  std::string aggregate;  // COUNT / SUM / AVG / MIN / MAX; empty for none
  std::string alias;
};

struct FilterItem {
  std::string table;
  std::string column;
  std::string op = "=";   // =, <>, >, >=, <, <=, LIKE, IN, IS NULL, BETWEEN...
  std::string value;      // raw user text; quoted per the column's type
  std::string value2;     // upper bound for BETWEEN
  bool orWithPrevious = false;

  // Operators that take no right-hand operand.
  static bool isUnary(const std::string& op);
  static const std::vector<std::string>& operators();
};

struct SortItem {
  std::string table;
  std::string column;
  bool descending = false;
};

// The visual builder's document. Turning this into SQL is pure -- no database
// access -- so the preview can update on every keystroke.
struct QuerySpec {
  std::vector<std::string> tables;   // in the order the user added them
  std::vector<SelectItem> select;    // empty means SELECT *
  std::vector<FilterItem> filters;
  std::vector<SortItem> sort;
  std::vector<std::pair<std::string, std::string>> groupBy;  // table, column
  bool distinct = false;
  int limit = 0;                     // 0 means no LIMIT
  int offset = 0;
};

struct BuildResult {
  std::string sql;
  // Tables the user picked that have no FK path to the rest; they would have
  // produced a cross join, so they're reported instead of silently joined.
  std::vector<std::string> unjoinedTables;
  std::vector<std::string> warnings;
};

// Generates SQL for `spec`, using the FK graph to work out the joins and the
// schema to quote values according to each column's type.
BuildResult buildQuery(const Schema& schema, const QuerySpec& spec,
                       Dialect dialect);

// Short, stable alias for a table name: "order_items" -> "oi". Disambiguated
// against `taken`.
std::string aliasFor(const std::string& table,
                     const std::vector<std::string>& taken);

}  // namespace ds
