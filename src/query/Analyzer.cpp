#include "query/Analyzer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "query/JoinGraph.h"
#include "query/SqlContext.h"
#include "query/SqlLexer.h"

namespace ds {
namespace {

// Row counts below this aren't worth an index; a full scan of a small table is
// faster than the indirection.
constexpr int64_t kSmallTableRows = 500;
// Above this, a full scan is worth complaining about even without a plan.
constexpr int64_t kLargeTableRows = 50000;

bool isComparisonOp(const std::string& t) {
  static const std::set<std::string> kOps = {"=",  "<", ">", "<=",
                                             ">=", "<>", "!="};
  return kOps.count(t) > 0;
}

std::string lower(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return o;
}

// Maps an alias back to the real table, using the refs the context analyzer
// already collected.
const Table* resolve(const Schema& schema, const std::vector<TableRef>& refs,
                     const std::string& qualifier) {
  if (qualifier.empty()) {
    // Unqualified: only unambiguous when exactly one table is in play.
    if (refs.size() == 1) return schema.findTable(refs[0].table);
    return nullptr;
  }
  for (const auto& r : refs) {
    if (iequals(r.handle(), qualifier)) return schema.findTable(r.table);
  }
  return schema.findTable(qualifier);
}

bool anyIndexLeadsWith(const Table& t, const std::string& column) {
  for (const auto& idx : t.indexes) {
    if (!idx.columns.empty() && iequals(idx.columns.front(), column)) {
      return true;
    }
  }
  // A primary key is always indexed even when the driver didn't list it.
  const Column* c = t.findColumn(column);
  return c && c->primaryKey;
}

std::string createIndexSql(const Table& t, const std::string& column,
                           Dialect dialect) {
  const char q = dialect == Dialect::MySql ? '`' : '"';
  auto quote = [&](const std::string& s) {
    return std::string(1, q) + s + std::string(1, q);
  };
  return "CREATE INDEX idx_" + t.name + "_" + column + " ON " + quote(t.name) +
         " (" + quote(column) + ");";
}

}  // namespace

const char* severityName(Severity s) {
  switch (s) {
    case Severity::Info: return "Info";
    case Severity::Warning: return "Warning";
    case Severity::Error: return "Error";
  }
  return "";
}

std::vector<PredicateRef> extractPredicates(const std::string& sql) {
  std::vector<PredicateRef> out;
  const auto toks = tokenize(sql);

  // Only look inside clauses where an index would actually be consulted.
  bool inFilter = false;
  for (size_t i = 0; i < toks.size(); ++i) {
    const Token& t = toks[i];

    if (t.type == TokenType::Keyword) {
      if (t.isKeyword("WHERE") || t.isKeyword("ON") || t.isKeyword("HAVING")) {
        inFilter = true;
      } else if (t.isKeyword("SELECT") || t.isKeyword("FROM") ||
                 t.isKeyword("GROUP") || t.isKeyword("LIMIT") ||
                 t.isKeyword("VALUES") || t.isKeyword("SET")) {
        inFilter = false;
      }
      // ORDER BY benefits from an index too, so keep scanning there.
      if (t.isKeyword("ORDER")) inFilter = true;
    }
    if (!inFilter) continue;
    if (t.type != TokenType::Identifier &&
        t.type != TokenType::QuotedIdentifier) {
      continue;
    }

    PredicateRef ref;
    ref.offset = t.start;
    ref.length = t.length;
    size_t j = i;

    // FUNC(column ...) -- the wrapping is what matters, not the function.
    bool wrapped = false;
    if (i >= 2 && toks[i - 1].type == TokenType::Punctuation &&
        toks[i - 1].text == "(" &&
        (toks[i - 2].type == TokenType::Identifier ||
         toks[i - 2].type == TokenType::Keyword)) {
      wrapped = true;
    }

    std::string qualifier;
    std::string column = t.unquoted();
    if (j + 2 < toks.size() && toks[j + 1].type == TokenType::Punctuation &&
        toks[j + 1].text == "." &&
        (toks[j + 2].type == TokenType::Identifier ||
         toks[j + 2].type == TokenType::QuotedIdentifier)) {
      qualifier = column;
      column = toks[j + 2].unquoted();
      ref.length = toks[j + 2].start + toks[j + 2].length - t.start;
      j += 2;
    }

    // What follows decides whether this is really a predicate.
    size_t k = j + 1;
    if (wrapped) {
      // Skip past the closing paren of the wrapping call.
      int depth = 1;
      while (k < toks.size() && depth > 0) {
        if (toks[k].text == "(") ++depth;
        if (toks[k].text == ")") --depth;
        ++k;
      }
    }
    if (k >= toks.size()) continue;

    const Token& next = toks[k];
    if (isComparisonOp(next.text)) {
      ref.op = next.text;
    } else if (next.isKeyword("LIKE")) {
      ref.op = "LIKE";
      if (k + 1 < toks.size() && toks[k + 1].type == TokenType::String) {
        const std::string& lit = toks[k + 1].text;
        ref.leadingWildcard = lit.size() > 1 && (lit[1] == '%' || lit[1] == '_');
      }
    } else if (next.isKeyword("IN")) {
      ref.op = "IN";
    } else if (next.isKeyword("BETWEEN")) {
      ref.op = "BETWEEN";
    } else if (next.isKeyword("IS")) {
      ref.op = "IS";
    } else if (next.isKeyword("ASC") || next.isKeyword("DESC") ||
               (next.type == TokenType::Punctuation && next.text == ",")) {
      ref.op = "ORDER";
    } else {
      continue;
    }

    ref.qualifier = std::move(qualifier);
    ref.column = std::move(column);
    ref.wrappedInFunction = wrapped;
    out.push_back(std::move(ref));
    i = j;
  }
  return out;
}

std::vector<Diagnostic> analyzeStatic(const Schema& schema,
                                      const std::string& sql, Dialect dialect) {
  std::vector<Diagnostic> diags;
  const auto toks = tokenize(sql);
  if (toks.empty()) return diags;

  const CursorContext ctx = analyzeCursor(sql, sql.size());
  const std::vector<TableRef>& refs = ctx.tablesInScope;

  auto has = [&](const char* kw) {
    return std::any_of(toks.begin(), toks.end(),
                       [&](const Token& t) { return t.isKeyword(kw); });
  };

  const bool isSelect = toks[0].isKeyword("SELECT") || toks[0].isKeyword("WITH");
  const bool isUpdate = toks[0].isKeyword("UPDATE");
  const bool isDelete = toks[0].isKeyword("DELETE");

  // ---- unknown tables ----------------------------------------------------
  for (const auto& r : refs) {
    if (schema.findTable(r.table)) continue;
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "unknown-table";
    d.title = "No table named '" + r.table + "'";
    d.detail = "This name isn't in the current schema. Check the spelling, or "
               "refresh the schema if it was created outside this session.";
    if (r.position < toks.size()) {
      d.offset = toks[r.position].start;
      d.length = toks[r.position].length;
    }
    diags.push_back(std::move(d));
  }

  // ---- unknown columns ---------------------------------------------------
  for (const auto& p : extractPredicates(sql)) {
    const Table* t = resolve(schema, refs, p.qualifier);
    if (!t) continue;
    if (t->findColumn(p.column)) continue;
    // An unqualified name might belong to any table in scope.
    if (p.qualifier.empty()) {
      bool foundSomewhere = false;
      for (const auto& r : refs) {
        const Table* rt = schema.findTable(r.table);
        if (rt && rt->findColumn(p.column)) { foundSomewhere = true; break; }
      }
      if (foundSomewhere) continue;
    }
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "unknown-column";
    d.title = "'" + t->name + "' has no column '" + p.column + "'";
    d.detail = "The statement filters on a column that doesn't exist.";
    d.offset = p.offset;
    d.length = p.length;
    diags.push_back(std::move(d));
  }

  // ---- destructive statement with no WHERE -------------------------------
  if ((isUpdate || isDelete) && !has("WHERE")) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "unbounded-write";
    d.title = std::string(isUpdate ? "UPDATE" : "DELETE") +
              " with no WHERE clause";
    d.detail = "This affects every row in the table.";
    if (!refs.empty()) {
      const Table* t = schema.findTable(refs[0].table);
      if (t && t->estimatedRows > 0) {
        d.detail = "This affects all ~" + std::to_string(t->estimatedRows) +
                   " rows in " + t->name + ".";
      }
    }
    d.suggestion = "Add a WHERE clause, or run it inside a transaction you can "
                   "roll back.";
    diags.push_back(std::move(d));
  }

  // ---- SELECT * ----------------------------------------------------------
  if (isSelect) {
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
      if (!toks[i].isKeyword("SELECT")) continue;
      const Token& next = toks[i + 1];
      const bool star = next.type == TokenType::Operator && next.text == "*";
      if (!star) continue;

      size_t wideColumns = 0;
      std::string widest;
      for (const auto& r : refs) {
        const Table* t = schema.findTable(r.table);
        if (t && t->columns.size() > wideColumns) {
          wideColumns = t->columns.size();
          widest = t->name;
        }
      }
      Diagnostic d;
      d.severity = wideColumns > 15 ? Severity::Warning : Severity::Info;
      d.code = "select-star";
      d.title = "SELECT * pulls every column";
      d.detail = wideColumns > 0
                     ? widest + " has " + std::to_string(wideColumns) +
                           " columns. Naming just the ones you need cuts "
                           "transfer and lets covering indexes work."
                     : "Naming the columns you need cuts transfer and lets "
                       "covering indexes work.";
      d.offset = next.start;
      d.length = next.length;
      diags.push_back(std::move(d));
      break;
    }
  }

  // ---- predicates that can't use an index --------------------------------
  for (const auto& p : extractPredicates(sql)) {
    const Table* t = resolve(schema, refs, p.qualifier);
    if (!t) continue;
    const Column* col = t->findColumn(p.column);
    if (!col) continue;

    const std::string where = p.qualifier.empty()
                                  ? t->name + "." + p.column
                                  : p.qualifier + "." + p.column;

    if (p.wrappedInFunction) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "non-sargable-function";
      d.title = "Function call on " + where + " blocks index use";
      d.detail = "Wrapping a column in a function forces a full scan, because "
                 "the index holds the raw values. Rewrite the predicate so the "
                 "column stands alone -- compare against a computed range "
                 "instead.";
      d.offset = p.offset;
      d.length = p.length;
      diags.push_back(std::move(d));
      continue;
    }

    if (p.leadingWildcard) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "leading-wildcard";
      d.title = "LIKE '%...' on " + where + " can't use an index";
      d.detail = "A leading wildcard means the engine has to test every row. "
                 "A trailing-only wildcard ('foo%') uses an index; for genuine "
                 "substring search you want a full-text index instead.";
      d.offset = p.offset;
      d.length = p.length;
      diags.push_back(std::move(d));
      continue;
    }

    const bool smallTable =
        t->estimatedRows >= 0 && t->estimatedRows < kSmallTableRows;
    if (p.op != "ORDER" && p.op != "IS" && !smallTable &&
        !anyIndexLeadsWith(*t, col->name)) {
      Diagnostic d;
      d.severity = t->estimatedRows > kLargeTableRows ? Severity::Warning
                                                      : Severity::Info;
      d.code = "missing-index";
      d.title = "No index on " + t->name + "." + col->name;
      d.detail = "This predicate has to be evaluated row by row" +
                 (t->estimatedRows > 0
                      ? " across ~" + std::to_string(t->estimatedRows) + " rows."
                      : std::string("."));
      d.suggestion = createIndexSql(*t, col->name, dialect);
      d.offset = p.offset;
      d.length = p.length;
      diags.push_back(std::move(d));
    }
  }

  // ---- comma joins with nothing linking them -----------------------------
  if (refs.size() > 1 && !has("JOIN") && !has("WHERE")) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "cross-join";
    d.title = "Cartesian product across " + std::to_string(refs.size()) +
              " tables";
    d.detail = "The tables are listed together with nothing relating them, so "
               "every row is paired with every other row.";

    JoinGraph graph(schema);
    std::vector<std::string> names;
    for (const auto& r : refs) names.push_back(r.table);
    auto path = graph.connect(names, nullptr);
    if (!path.empty()) {
      std::string sug = "Relate them with:";
      std::map<std::string, std::string> handle;
      for (const auto& r : refs) handle[r.table] = r.handle();
      for (const auto& e : path) {
        const std::string a =
            handle.count(e.fromTable) ? handle[e.fromTable] : e.fromTable;
        const std::string b =
            handle.count(e.toTable) ? handle[e.toTable] : e.toTable;
        sug += "\n  " + e.onClause(a, b);
      }
      d.suggestion = sug;
    }
    diags.push_back(std::move(d));
  }

  // ---- ORDER BY with no bound --------------------------------------------
  if (isSelect && has("ORDER") && !has("LIMIT")) {
    int64_t biggest = -1;
    std::string name;
    for (const auto& r : refs) {
      const Table* t = schema.findTable(r.table);
      if (t && t->estimatedRows > biggest) {
        biggest = t->estimatedRows;
        name = t->name;
      }
    }
    if (biggest > kLargeTableRows) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "unbounded-sort";
      d.title = "ORDER BY with no LIMIT over ~" + std::to_string(biggest) +
                " rows";
      d.detail = "The whole result has to be materialised and sorted before "
                 "the first row comes back. Add a LIMIT if you only need the "
                 "top of the list.";
      diags.push_back(std::move(d));
    }
  }

  // ---- joins onto unindexed columns --------------------------------------
  for (size_t i = 0; i < toks.size(); ++i) {
    if (!toks[i].isKeyword("ON")) continue;
    // Look at both sides of the equality that follows.
    for (size_t j = i + 1; j + 4 < toks.size() && j < i + 12; ++j) {
      if (toks[j].text != "=") continue;
      auto check = [&](size_t identIdx) {
        if (identIdx + 2 >= toks.size()) return;
        if (toks[identIdx + 1].text != ".") return;
        const std::string qual = toks[identIdx].unquoted();
        const std::string colName = toks[identIdx + 2].unquoted();
        const Table* t = resolve(schema, refs, qual);
        if (!t || t->estimatedRows < kSmallTableRows) return;
        const Column* c = t->findColumn(colName);
        if (!c || anyIndexLeadsWith(*t, c->name)) return;
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "unindexed-join";
        d.title = "Join column " + t->name + "." + c->name + " is not indexed";
        d.detail = "Each row from the other side triggers a scan to find its "
                   "match. This is usually the single biggest win available on "
                   "a slow join.";
        d.suggestion = createIndexSql(*t, c->name, dialect);
        d.offset = toks[identIdx].start;
        d.length = toks[identIdx + 2].start + toks[identIdx + 2].length -
                   toks[identIdx].start;
        diags.push_back(std::move(d));
      };
      if (j >= 3) check(j - 3);
      check(j + 1);
      break;
    }
  }

  // ---- text/number comparison mismatch -----------------------------------
  for (size_t i = 0; i + 2 < toks.size(); ++i) {
    if (!isComparisonOp(toks[i + 1].text)) continue;
    if (toks[i].type != TokenType::Identifier) continue;
    const Table* t = resolve(schema, refs, "");
    std::string colName = toks[i].unquoted();
    if (i >= 2 && toks[i - 1].text == ".") {
      t = resolve(schema, refs, toks[i - 2].unquoted());
    }
    if (!t) continue;
    const Column* c = t->findColumn(colName);
    if (!c) continue;

    const Token& rhs = toks[i + 2];
    const bool numericColumn = c->kind == Column::Kind::Integer ||
                               c->kind == Column::Kind::Real;
    if (numericColumn && rhs.type == TokenType::String) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "type-mismatch";
      d.title = "Comparing numeric " + t->name + "." + c->name +
                " against a string literal";
      d.detail = "MySQL coerces the column to match, which discards the index "
                 "for this predicate and can match rows you didn't expect. "
                 "Drop the quotes.";
      d.offset = rhs.start;
      d.length = rhs.length;
      diags.push_back(std::move(d));
    } else if (c->kind == Column::Kind::Text &&
               rhs.type == TokenType::Number) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "type-mismatch";
      d.title = "Comparing text " + t->name + "." + c->name +
                " against a bare number";
      d.detail = "The column is coerced to a number, so the index goes unused "
                 "and non-numeric values compare equal to 0.";
      d.offset = rhs.start;
      d.length = rhs.length;
      diags.push_back(std::move(d));
    }
  }

  return diags;
}

std::vector<Diagnostic> analyzePlan(const Schema& schema, const ResultSet& plan,
                                    const std::string& sql, Dialect dialect) {
  std::vector<Diagnostic> diags;
  (void)sql;

  auto columnIndex = [&](const char* name) -> int {
    for (size_t i = 0; i < plan.columns.size(); ++i) {
      if (iequals(plan.columns[i].name, name)) return static_cast<int>(i);
    }
    return -1;
  };
  auto cell = [&](const Row& r, int idx) -> std::string {
    if (idx < 0 || idx >= static_cast<int>(r.size())) return {};
    return isNull(r[idx]) ? std::string{} : toDisplayString(r[idx]);
  };

  if (dialect == Dialect::Sqlite) {
    // EXPLAIN QUERY PLAN gives one prose line per node: "SCAN users" or
    // "SEARCH users USING INDEX idx_email (email=?)".
    const int detailCol = columnIndex("detail");
    for (const auto& r : plan.rows) {
      std::string detail = cell(r, detailCol >= 0 ? detailCol
                                                  : static_cast<int>(r.size()) - 1);
      const std::string low = lower(detail);
      if (low.rfind("scan", 0) == 0) {
        // "SCAN table" -- pull the table name out of the second word.
        std::string table;
        size_t start = detail.find(' ');
        if (start != std::string::npos) {
          size_t end = detail.find(' ', start + 1);
          table = detail.substr(start + 1, end == std::string::npos
                                               ? std::string::npos
                                               : end - start - 1);
        }
        const Table* t = schema.findTable(table);
        if (t && t->estimatedRows >= 0 && t->estimatedRows < kSmallTableRows) {
          continue;  // scanning a tiny table is the right plan
        }
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "plan-full-scan";
        d.title = "Full scan of " + (table.empty() ? "a table" : table);
        d.detail = "The planner found no usable index, so every row is read." +
                   (t && t->estimatedRows > 0
                        ? " That's ~" + std::to_string(t->estimatedRows) +
                              " rows."
                        : std::string());
        diags.push_back(std::move(d));
      }
      if (low.find("temp b-tree") != std::string::npos) {
        Diagnostic d;
        d.severity = Severity::Warning;
        d.code = "plan-temp-btree";
        d.title = "Sorting through a temporary B-tree";
        d.detail = detail +
                   " -- an index matching the ORDER BY / GROUP BY column order "
                   "would let the rows come out already sorted.";
        diags.push_back(std::move(d));
      }
    }
    return diags;
  }

  // MySQL EXPLAIN.
  const int tableCol = columnIndex("table");
  const int typeCol = columnIndex("type");
  const int keyCol = columnIndex("key");
  const int possibleCol = columnIndex("possible_keys");
  const int rowsCol = columnIndex("rows");
  const int filteredCol = columnIndex("filtered");
  const int extraCol = columnIndex("Extra");

  for (const auto& r : plan.rows) {
    const std::string table = cell(r, tableCol);
    const std::string type = cell(r, typeCol);
    const std::string key = cell(r, keyCol);
    const std::string possible = cell(r, possibleCol);
    const std::string extra = cell(r, extraCol);
    const std::string rowsStr = cell(r, rowsCol);

    int64_t rows = 0;
    try { rows = std::stoll(rowsStr); } catch (...) { rows = 0; }

    if (iequals(type, "ALL")) {
      Diagnostic d;
      d.severity = rows > kSmallTableRows ? Severity::Warning : Severity::Info;
      d.code = "plan-full-scan";
      d.title = "Full table scan on " + (table.empty() ? "?" : table);
      d.detail = "access type ALL over ~" + rowsStr + " rows.";
      if (!possible.empty() && key.empty()) {
        d.detail += " Indexes " + possible +
                    " were candidates but the optimiser rejected them -- often "
                    "a type mismatch or a function wrapping the column.";
      } else if (possible.empty()) {
        d.detail += " No index even applies to this predicate.";
        const Table* t = schema.findTable(table);
        if (t) {
          auto preds = extractPredicates(sql);
          for (const auto& p : preds) {
            if (t->findColumn(p.column) && !anyIndexLeadsWith(*t, p.column)) {
              d.suggestion = createIndexSql(*t, p.column, dialect);
              break;
            }
          }
        }
      }
      diags.push_back(std::move(d));
    }

    if (iequals(type, "index")) {
      Diagnostic d;
      d.severity = Severity::Info;
      d.code = "plan-index-scan";
      d.title = "Full index scan on " + table;
      d.detail = "The whole index is read rather than seeked into. Cheaper "
                 "than a table scan, but still linear in table size.";
      diags.push_back(std::move(d));
    }

    if (extra.find("Using filesort") != std::string::npos) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "plan-filesort";
      d.title = "Sorting " + table + " with a filesort";
      d.detail = "Results are buffered and sorted after retrieval. An index "
                 "whose leading columns match the ORDER BY would remove this "
                 "step entirely.";
      diags.push_back(std::move(d));
    }

    if (extra.find("Using temporary") != std::string::npos) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "plan-temp-table";
      d.title = "Materialising a temporary table";
      d.detail = "Usually a GROUP BY or DISTINCT whose columns don't line up "
                 "with any index. Large temporaries spill to disk.";
      diags.push_back(std::move(d));
    }

    if (extra.find("Using join buffer") != std::string::npos) {
      Diagnostic d;
      d.severity = Severity::Warning;
      d.code = "plan-join-buffer";
      d.title = "Block nested-loop join on " + table;
      d.detail = "No index is available on the join column, so rows are "
                 "buffered and matched in blocks. Indexing the join column is "
                 "the fix.";
      diags.push_back(std::move(d));
    }

    // A low `filtered` percentage means the engine read far more rows than it
    // returned -- the predicate isn't selective where it's being applied.
    const std::string filtered = cell(r, filteredCol);
    if (!filtered.empty() && rows > kSmallTableRows) {
      double pct = 100.0;
      try { pct = std::stod(filtered); } catch (...) {}
      if (pct < 10.0) {
        Diagnostic d;
        d.severity = Severity::Info;
        d.code = "plan-low-selectivity";
        d.title = "Only ~" + filtered + "% of rows read from " + table +
                  " survive the filter";
        d.detail = "About " + rowsStr +
                   " rows are examined to produce a small fraction of that. An "
                   "index covering the filtering column would let the engine "
                   "skip the rest.";
        diags.push_back(std::move(d));
      }
    }
  }

  return diags;
}

}  // namespace ds
