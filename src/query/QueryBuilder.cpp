#include "query/QueryBuilder.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace ds {
namespace {

std::string quoteIdent(const std::string& s, Dialect d) {
  const char q = d == Dialect::MySql ? '`' : '"';
  std::string out(1, q);
  for (char c : s) {
    if (c == q) out.push_back(q);
    out.push_back(c);
  }
  out.push_back(q);
  return out;
}

std::string sqlQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out.push_back('\'');
    if (c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

bool looksNumeric(const std::string& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i >= s.size()) return false;
  bool dot = false;
  for (; i < s.size(); ++i) {
    if (s[i] == '.') {
      if (dot) return false;
      dot = true;
    } else if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}

// Renders a filter's right-hand side. Numeric columns take bare literals;
// everything else is quoted. Values the user wrote as an expression (a
// function call, another column) are passed through untouched.
std::string renderValue(const Column* col, const std::string& raw) {
  std::string v = raw;
  // Trim.
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) {
    v.erase(v.begin());
  }
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) {
    v.pop_back();
  }
  if (v.empty()) return "''";

  // Already a literal or an expression -- leave it alone.
  if (v.front() == '\'' || v.front() == '(' ||
      v.find('(') != std::string::npos) {
    return v;
  }
  if (iequals(v, "NULL") || iequals(v, "TRUE") || iequals(v, "FALSE")) {
    return v;
  }

  const bool numericColumn =
      col && (col->kind == Column::Kind::Integer ||
              col->kind == Column::Kind::Real ||
              col->kind == Column::Kind::Boolean);
  if (numericColumn && looksNumeric(v)) return v;
  if (!col && looksNumeric(v)) return v;
  return sqlQuote(v);
}

std::string renderInList(const Column* col, const std::string& raw) {
  // Accept a comma-separated list and quote each element on its own.
  std::string out = "(";
  std::string item;
  bool first = true;
  auto flush = [&]() {
    if (item.empty()) return;
    if (!first) out += ", ";
    out += renderValue(col, item);
    first = false;
    item.clear();
  };
  for (char c : raw) {
    if (c == ',') flush();
    else item.push_back(c);
  }
  flush();
  out += ")";
  return out == "()" ? "(NULL)" : out;
}

}  // namespace

bool FilterItem::isUnary(const std::string& op) {
  return iequals(op, "IS NULL") || iequals(op, "IS NOT NULL");
}

const std::vector<std::string>& FilterItem::operators() {
  static const std::vector<std::string> kOps = {
      "=", "<>", ">", ">=", "<", "<=", "LIKE", "NOT LIKE",
      "IN", "NOT IN", "BETWEEN", "IS NULL", "IS NOT NULL"};
  return kOps;
}

std::string aliasFor(const std::string& table,
                     const std::vector<std::string>& taken) {
  // Initials of the underscore-separated parts: order_items -> oi.
  std::string base;
  bool atStart = true;
  for (char c : table) {
    if (c == '_' || c == '-') { atStart = true; continue; }
    if (atStart && std::isalpha(static_cast<unsigned char>(c))) {
      base.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      atStart = false;
    }
  }
  if (base.empty()) {
    base = table.substr(0, std::min<size_t>(2, table.size()));
  }

  auto isTaken = [&](const std::string& cand) {
    return std::any_of(taken.begin(), taken.end(), [&](const std::string& t) {
      return iequals(t, cand);
    });
  };

  if (!isTaken(base)) return base;
  for (int n = 2; n < 100; ++n) {
    const std::string cand = base + std::to_string(n);
    if (!isTaken(cand)) return cand;
  }
  return base;
}

BuildResult buildQuery(const Schema& schema, const QuerySpec& spec,
                       Dialect dialect) {
  BuildResult result;
  if (spec.tables.empty()) return result;

  JoinGraph graph(schema);
  std::vector<JoinEdge> joins = graph.connect(spec.tables,
                                              &result.unjoinedTables);

  // Assign one alias per table, in join order.
  std::map<std::string, std::string> alias;
  std::vector<std::string> taken;
  auto assign = [&](const std::string& table) {
    if (alias.count(table)) return;
    const std::string a = aliasFor(table, taken);
    taken.push_back(a);
    alias[table] = a;
  };
  assign(spec.tables.front());
  for (const auto& j : joins) {
    assign(j.fromTable);
    assign(j.toTable);
  }
  for (const auto& t : spec.tables) assign(t);

  auto handleOf = [&](const std::string& table) -> std::string {
    auto it = alias.find(table);
    return it == alias.end() ? table : it->second;
  };
  auto ref = [&](const std::string& table, const std::string& column) {
    if (column == "*") return handleOf(table) + ".*";
    return handleOf(table) + "." + quoteIdent(column, dialect);
  };
  auto columnOf = [&](const std::string& table,
                      const std::string& col) -> const Column* {
    const Table* t = schema.findTable(table);
    return t ? t->findColumn(col) : nullptr;
  };

  std::string sql = "SELECT ";
  if (spec.distinct) sql += "DISTINCT ";

  if (spec.select.empty()) {
    sql += "*";
  } else {
    bool first = true;
    for (const auto& item : spec.select) {
      if (!first) sql += ",\n       ";
      first = false;
      std::string expr = ref(item.table, item.column);
      if (!item.aggregate.empty()) expr = item.aggregate + "(" + expr + ")";
      if (!item.alias.empty()) {
        expr += " AS " + quoteIdent(item.alias, dialect);
      }
      sql += expr;
    }
  }

  const std::string& root = spec.tables.front();
  sql += "\nFROM " + quoteIdent(root, dialect) + " AS " + handleOf(root);

  for (const auto& j : joins) {
    // A reversed hop walks parent -> child, which can duplicate parent rows.
    // LEFT JOIN keeps parents that have no children, which is nearly always
    // the intent when someone drags a child table onto the canvas.
    const char* kind = j.reversed ? "LEFT JOIN" : "JOIN";
    sql += std::string("\n") + kind + " " + quoteIdent(j.toTable, dialect) +
           " AS " + handleOf(j.toTable) + "\n  ON " +
           j.onClause(handleOf(j.fromTable), handleOf(j.toTable));
    if (j.reversed) {
      result.warnings.push_back(
          "Joining " + j.toTable + " one-to-many from " + j.fromTable +
          " can repeat " + j.fromTable + " rows -- aggregate or DISTINCT if "
          "that isn't what you want.");
    }
  }

  for (const auto& t : result.unjoinedTables) {
    result.warnings.push_back(
        t + " has no foreign-key path to the other tables, so it was left "
            "out rather than cross-joined.");
  }

  if (!spec.filters.empty()) {
    sql += "\nWHERE ";
    bool first = true;
    for (const auto& f : spec.filters) {
      if (!first) sql += f.orWithPrevious ? "\n   OR " : "\n  AND ";
      first = false;

      const Column* col = columnOf(f.table, f.column);
      const std::string lhs = ref(f.table, f.column);

      if (FilterItem::isUnary(f.op)) {
        sql += lhs + " " + f.op;
      } else if (iequals(f.op, "IN") || iequals(f.op, "NOT IN")) {
        sql += lhs + " " + f.op + " " + renderInList(col, f.value);
      } else if (iequals(f.op, "BETWEEN")) {
        sql += lhs + " BETWEEN " + renderValue(col, f.value) + " AND " +
               renderValue(col, f.value2);
      } else {
        sql += lhs + " " + f.op + " " + renderValue(col, f.value);
      }
    }
  }

  if (!spec.groupBy.empty()) {
    sql += "\nGROUP BY ";
    bool first = true;
    for (const auto& [table, col] : spec.groupBy) {
      if (!first) sql += ", ";
      first = false;
      sql += ref(table, col);
    }
  }

  if (!spec.sort.empty()) {
    sql += "\nORDER BY ";
    bool first = true;
    for (const auto& s : spec.sort) {
      if (!first) sql += ", ";
      first = false;
      sql += ref(s.table, s.column);
      if (s.descending) sql += " DESC";
    }
  }

  if (spec.limit > 0) {
    sql += "\nLIMIT " + std::to_string(spec.limit);
    if (spec.offset > 0) sql += " OFFSET " + std::to_string(spec.offset);
  }

  // An aggregate mixed with plain columns needs those columns grouped; MySQL
  // in ONLY_FULL_GROUP_BY mode rejects the query outright.
  const bool hasAggregate =
      std::any_of(spec.select.begin(), spec.select.end(),
                  [](const SelectItem& s) { return !s.aggregate.empty(); });
  const bool hasBare =
      std::any_of(spec.select.begin(), spec.select.end(),
                  [](const SelectItem& s) { return s.aggregate.empty(); });
  if (hasAggregate && hasBare && spec.groupBy.empty()) {
    result.warnings.push_back(
        "Aggregates are mixed with plain columns but nothing is grouped -- "
        "add a GROUP BY or MySQL will reject this.");
  }

  result.sql = std::move(sql);
  return result;
}

}  // namespace ds
