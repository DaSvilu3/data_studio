#include "query/SqlContext.h"

#include <algorithm>
#include <cctype>

namespace ds {
namespace {

// Fuzzy subsequence match: "usnm" matches "user_name". Returns a score, or -1
// when the candidate doesn't match at all.
int fuzzyScore(const std::string& needle, const std::string& hay) {
  if (needle.empty()) return 1;
  if (needle.size() > hay.size()) return -1;

  size_t hi = 0;
  int score = 0;
  int streak = 0;
  bool prefixMatch = true;

  for (size_t ni = 0; ni < needle.size(); ++ni) {
    const char nc =
        static_cast<char>(std::tolower(static_cast<unsigned char>(needle[ni])));
    bool found = false;
    while (hi < hay.size()) {
      const char hc = static_cast<char>(
          std::tolower(static_cast<unsigned char>(hay[hi])));
      const bool atBoundary =
          hi == 0 || hay[hi - 1] == '_' || hay[hi - 1] == '.';
      ++hi;
      if (hc == nc) {
        score += 10;
        if (atBoundary) score += 15;   // word-start hits are what people mean
        streak = streak ? streak + 1 : 1;
        score += streak * 3;
        found = true;
        break;
      }
      streak = 0;
      if (ni == 0) prefixMatch = false;
      score -= 1;
    }
    if (!found) return -1;
  }
  if (prefixMatch && hi >= needle.size()) score += 25;
  // Prefer the shorter of two equally good matches.
  score -= static_cast<int>(hay.size()) / 4;
  return score;
}

bool isClauseBoundary(const Token& t) {
  static const char* kBoundaries[] = {
      "SELECT", "FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT",
      "JOIN", "ON", "SET", "VALUES", "INSERT", "UPDATE", "DELETE",
      "UNION", "INTO", "CREATE"};
  for (const char* b : kBoundaries) {
    if (t.isKeyword(b)) return true;
  }
  return false;
}

Clause clauseFor(const std::vector<Token>& toks, size_t idx) {
  // Walk backwards to the nearest clause keyword, skipping over anything
  // inside parentheses so a subquery's FROM doesn't leak into the outer scope.
  int depth = 0;
  for (size_t i = idx; i-- > 0;) {
    const Token& t = toks[i];
    if (t.type == TokenType::Punctuation) {
      if (t.text == ")") { ++depth; continue; }
      if (t.text == "(") {
        if (depth == 0) return Clause::Unknown;  // inside a call/subquery head
        --depth;
        continue;
      }
      if (t.text == ";" && depth == 0) return Clause::Unknown;
    }
    if (depth != 0 || t.type != TokenType::Keyword) continue;

    if (t.isKeyword("SELECT")) return Clause::Select;
    if (t.isKeyword("FROM")) return Clause::From;
    if (t.isKeyword("JOIN")) return Clause::Join;
    if (t.isKeyword("ON") || t.isKeyword("USING")) return Clause::On;
    if (t.isKeyword("WHERE")) return Clause::Where;
    if (t.isKeyword("HAVING")) return Clause::Having;
    if (t.isKeyword("SET")) return Clause::Set;
    if (t.isKeyword("VALUES")) return Clause::Values;
    if (t.isKeyword("LIMIT") || t.isKeyword("OFFSET")) return Clause::Limit;
    if (t.isKeyword("INTO")) return Clause::Insert;
    if (t.isKeyword("UPDATE")) return Clause::Update;
    if (t.isKeyword("DELETE")) return Clause::Delete;
    if (t.isKeyword("CREATE")) return Clause::Create;
    if (t.isKeyword("BY")) {
      // Disambiguate GROUP BY from ORDER BY by looking one token back.
      if (i > 0 && toks[i - 1].isKeyword("GROUP")) return Clause::GroupBy;
      if (i > 0 && toks[i - 1].isKeyword("ORDER")) return Clause::OrderBy;
    }
  }
  return Clause::Unknown;
}

// Reads a possibly schema-qualified, possibly quoted table name starting at
// `i`, advancing `i` past it.
std::string readTableName(const std::vector<Token>& toks, size_t& i) {
  if (i >= toks.size()) return {};
  const Token& first = toks[i];
  if (first.type != TokenType::Identifier &&
      first.type != TokenType::QuotedIdentifier) {
    return {};
  }
  std::string name = first.unquoted();
  ++i;
  // schema.table -- keep only the table part; the schema is implied by the
  // active connection.
  if (i + 1 < toks.size() && toks[i].type == TokenType::Punctuation &&
      toks[i].text == "." &&
      (toks[i + 1].type == TokenType::Identifier ||
       toks[i + 1].type == TokenType::QuotedIdentifier)) {
    name = toks[i + 1].unquoted();
    i += 2;
  }
  return name;
}

std::vector<TableRef> collectTableRefs(const std::vector<Token>& toks) {
  std::vector<TableRef> refs;

  for (size_t i = 0; i < toks.size(); ++i) {
    const Token& t = toks[i];
    const bool introducesTable =
        t.isKeyword("FROM") || t.isKeyword("JOIN") || t.isKeyword("UPDATE") ||
        t.isKeyword("INTO");
    if (!introducesTable) continue;

    // FROM and JOIN can list several comma-separated tables.
    size_t j = i + 1;
    while (j < toks.size()) {
      if (toks[j].type == TokenType::Punctuation && toks[j].text == "(") {
        // A derived table -- skip to its closing paren; we can't know its
        // columns without executing it.
        int depth = 1;
        ++j;
        while (j < toks.size() && depth > 0) {
          if (toks[j].text == "(") ++depth;
          if (toks[j].text == ")") --depth;
          ++j;
        }
      } else {
        TableRef ref;
        ref.position = j;
        ref.table = readTableName(toks, j);
        if (ref.table.empty()) break;

        if (j < toks.size() && toks[j].isKeyword("AS")) ++j;
        // A bare identifier that isn't a keyword is the alias.
        if (j < toks.size() && (toks[j].type == TokenType::Identifier ||
                                toks[j].type == TokenType::QuotedIdentifier)) {
          ref.alias = toks[j].unquoted();
          ++j;
        }
        refs.push_back(std::move(ref));
      }

      if (j < toks.size() && toks[j].type == TokenType::Punctuation &&
          toks[j].text == ",") {
        ++j;
        continue;
      }
      break;
    }
  }

  // De-duplicate on handle so `FROM a JOIN a` doesn't double up.
  std::vector<TableRef> unique;
  for (auto& r : refs) {
    bool seen = false;
    for (const auto& u : unique) {
      if (iequals(u.handle(), r.handle())) { seen = true; break; }
    }
    if (!seen) unique.push_back(std::move(r));
  }
  return unique;
}

std::string columnTypeDetail(const Column& c) {
  std::string d = c.type;
  if (c.primaryKey) d += "  PK";
  if (!c.nullable) d += "  NOT NULL";
  if (c.autoIncrement) d += "  AUTO";
  return d;
}

}  // namespace

const char* clauseName(Clause c) {
  switch (c) {
    case Clause::Select: return "SELECT";
    case Clause::From: return "FROM";
    case Clause::Join: return "JOIN";
    case Clause::On: return "ON";
    case Clause::Where: return "WHERE";
    case Clause::GroupBy: return "GROUP BY";
    case Clause::Having: return "HAVING";
    case Clause::OrderBy: return "ORDER BY";
    case Clause::Limit: return "LIMIT";
    case Clause::Set: return "SET";
    case Clause::Insert: return "INSERT";
    case Clause::Values: return "VALUES";
    case Clause::Update: return "UPDATE";
    case Clause::Delete: return "DELETE";
    case Clause::Create: return "CREATE";
    case Clause::Unknown: return "";
  }
  return "";
}

std::pair<size_t, size_t> statementRangeAt(const std::string& sql,
                                           size_t cursor) {
  const auto toks = tokenize(sql);
  size_t begin = 0;
  size_t end = sql.size();

  for (const auto& t : toks) {
    if (t.type != TokenType::Punctuation || t.text != ";") continue;
    const size_t semi = t.start;
    if (semi < cursor) {
      begin = semi + 1;
    } else {
      end = semi;
      break;
    }
  }
  // Trim leading whitespace so the reported range starts at real SQL.
  while (begin < end && std::isspace(static_cast<unsigned char>(sql[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(sql[end - 1]))) {
    --end;
  }
  return {begin, end};
}

CursorContext analyzeCursor(const std::string& sql, size_t cursor) {
  CursorContext ctx;
  cursor = std::min(cursor, sql.size());

  auto [stmtBegin, stmtEnd] = statementRangeAt(sql, cursor);
  const std::string stmt = sql.substr(stmtBegin, stmtEnd - stmtBegin);
  const size_t local = cursor > stmtBegin ? cursor - stmtBegin : 0;

  const auto toks = tokenize(stmt);
  ctx.tablesInScope = collectTableRefs(toks);

  // Find the token the cursor is in or immediately after.
  size_t idx = toks.size();
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].start < local && local <= toks[i].start + toks[i].length) {
      idx = i;
      break;
    }
    if (toks[i].start >= local) {
      idx = i;
      break;
    }
  }

  // Is the cursor sitting inside a word we should replace?
  bool insideWord = false;
  if (idx < toks.size()) {
    const Token& t = toks[idx];
    const bool wordy = t.type == TokenType::Identifier ||
                       t.type == TokenType::Keyword ||
                       t.type == TokenType::QuotedIdentifier;
    if (wordy && t.start < local && local <= t.start + t.length) {
      insideWord = true;
      ctx.prefix = stmt.substr(t.start, local - t.start);
      ctx.replaceStart = stmtBegin + t.start;
      ctx.replaceLength = local - t.start;
    }
  }
  if (!insideWord) {
    ctx.replaceStart = cursor;
    ctx.replaceLength = 0;
  }

  // The token index that clause detection should look back from.
  const size_t clauseIdx = insideWord ? idx : idx;
  ctx.clause = clauseFor(toks, clauseIdx);

  // Qualified reference: `alias.pre|`
  const size_t wordTok = insideWord ? idx : idx;
  size_t dotAt = wordTok;
  if (insideWord && wordTok >= 1) dotAt = wordTok - 1;
  else if (!insideWord && wordTok >= 1) dotAt = wordTok - 1;

  if (dotAt < toks.size() && toks[dotAt].type == TokenType::Punctuation &&
      toks[dotAt].text == "." && dotAt >= 1) {
    const Token& q = toks[dotAt - 1];
    if (q.type == TokenType::Identifier ||
        q.type == TokenType::QuotedIdentifier) {
      ctx.qualifier = q.unquoted();
      ctx.afterDot = true;
      if (!insideWord) {
        ctx.replaceStart = stmtBegin + toks[dotAt].start + 1;
        ctx.replaceLength = 0;
      }
    }
  }

  ctx.expectsTable = ctx.clause == Clause::From ||
                     ctx.clause == Clause::Join ||
                     ctx.clause == Clause::Update ||
                     ctx.clause == Clause::Insert;
  ctx.expectsColumn = !ctx.expectsTable && ctx.clause != Clause::Unknown &&
                      ctx.clause != Clause::Limit &&
                      ctx.clause != Clause::Values;
  return ctx;
}

std::vector<Completion> completeAt(const Schema& schema, const std::string& sql,
                                   size_t cursor) {
  const CursorContext ctx = analyzeCursor(sql, cursor);
  std::vector<Completion> out;

  auto add = [&](CompletionKind kind, std::string insert, std::string label,
                 std::string detail, int base) {
    const int fuzzy = fuzzyScore(ctx.prefix, label);
    if (fuzzy < 0) return;
    Completion c;
    c.kind = kind;
    c.insertText = std::move(insert);
    c.label = std::move(label);
    c.detail = std::move(detail);
    c.score = base + fuzzy;
    out.push_back(std::move(c));
  };

  // ---- qualified: alias.<column> -----------------------------------------
  if (ctx.afterDot && !ctx.qualifier.empty()) {
    std::string tableName = ctx.qualifier;
    for (const auto& ref : ctx.tablesInScope) {
      if (iequals(ref.handle(), ctx.qualifier)) {
        tableName = ref.table;
        break;
      }
    }
    if (const Table* t = schema.findTable(tableName)) {
      for (const auto& c : t->columns) {
        add(CompletionKind::Column, c.name, c.name, columnTypeDetail(c), 1000);
      }
      add(CompletionKind::Column, "*", "*", "all columns of " + t->name, 900);
    }
    std::sort(out.begin(), out.end(),
              [](const Completion& a, const Completion& b) {
                return a.score > b.score;
              });
    return out;
  }

  // ---- tables ------------------------------------------------------------
  if (ctx.expectsTable || ctx.clause == Clause::Unknown) {
    for (const auto& t : schema.tables) {
      std::string detail = t.isView ? "view" : "table";
      if (t.estimatedRows >= 0) {
        detail += ", ~" + std::to_string(t.estimatedRows) + " rows";
      }
      detail += ", " + std::to_string(t.columns.size()) + " cols";
      add(CompletionKind::Table, t.name, t.name, detail, 900);
    }
  }

  // ---- columns from every table already in scope -------------------------
  if (ctx.expectsColumn || ctx.clause == Clause::Unknown) {
    const bool needsQualifier = ctx.tablesInScope.size() > 1;
    for (const auto& ref : ctx.tablesInScope) {
      const Table* t = schema.findTable(ref.table);
      if (!t) continue;
      for (const auto& c : t->columns) {
        // With several tables in play, insert the qualified form so the query
        // stays unambiguous.
        const std::string insert =
            needsQualifier ? ref.handle() + "." + c.name : c.name;
        add(CompletionKind::Column, insert, c.name,
            columnTypeDetail(c) + "  ·  " + ref.handle(), 1000);
      }
    }
    // Aliases themselves are worth completing.
    for (const auto& ref : ctx.tablesInScope) {
      if (ref.alias.empty()) continue;
      add(CompletionKind::Alias, ref.alias, ref.alias, "alias of " + ref.table,
          700);
    }
  }

  // ---- keywords and functions -------------------------------------------
  if (!ctx.afterDot) {
    for (const auto& k : sqlKeywords()) {
      add(CompletionKind::Keyword, k, k, "keyword", 300);
    }
    for (const auto& f : sqlFunctions()) {
      add(CompletionKind::Function, f + "(", f, "function", 400);
    }
  }

  std::sort(out.begin(), out.end(),
            [](const Completion& a, const Completion& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.label < b.label;
            });
  if (out.size() > 200) out.resize(200);
  return out;
}

}  // namespace ds
