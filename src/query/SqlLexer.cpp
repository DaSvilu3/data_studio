#include "query/SqlLexer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

#include "core/Schema.h"

namespace ds {
namespace {

const std::vector<std::string>& keywordList() {
  static const std::vector<std::string> kKeywords = {
      "ADD", "ALL", "ALTER", "AND", "AS", "ASC", "BEGIN", "BETWEEN", "BY",
      "CASE", "CAST", "COLLATE", "COLUMN", "COMMIT", "CONSTRAINT", "CREATE",
      "CROSS", "DATABASE", "DEFAULT", "DELETE", "DESC", "DISTINCT", "DROP",
      "ELSE", "END", "EXCEPT", "EXISTS", "EXPLAIN", "FALSE", "FOREIGN", "FROM",
      "FULL", "GROUP", "HAVING", "IF", "IGNORE", "IN", "INDEX", "INNER",
      "INSERT", "INTERSECT", "INTO", "IS", "JOIN", "KEY", "LEFT", "LIKE",
      "LIMIT", "NATURAL", "NOT", "NULL", "OFFSET", "ON", "OR", "ORDER",
      "OUTER", "PRIMARY", "REFERENCES", "RENAME", "REPLACE", "RIGHT",
      "ROLLBACK", "SELECT", "SET", "TABLE", "THEN", "TRANSACTION", "TRUE",
      "TRUNCATE", "UNION", "UNIQUE", "UPDATE", "USING", "VALUES", "VIEW",
      "WHEN", "WHERE", "WITH",
  };
  return kKeywords;
}

const std::vector<std::string>& functionList() {
  static const std::vector<std::string> kFunctions = {
      "ABS", "AVG", "CEIL", "CHAR_LENGTH", "COALESCE", "CONCAT", "COUNT",
      "CURRENT_DATE", "CURRENT_TIMESTAMP", "DATE", "DATE_ADD", "DATE_FORMAT",
      "DATEDIFF", "DENSE_RANK", "FLOOR", "GREATEST", "GROUP_CONCAT", "IFNULL",
      "JSON_EXTRACT", "LAG", "LEAD", "LEAST", "LENGTH", "LOWER", "LTRIM",
      "MAX", "MIN", "NOW", "NULLIF", "RANK", "ROUND", "ROW_NUMBER", "RTRIM",
      "SUBSTR", "SUBSTRING", "SUM", "TRIM", "UPPER",
  };
  return kFunctions;
}

std::unordered_set<std::string> upperSet(const std::vector<std::string>& v) {
  return std::unordered_set<std::string>(v.begin(), v.end());
}

std::string toUpper(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

bool isIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;  // allow UTF-8 identifiers
}

bool isIdentChar(char c) {
  return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) ||
         c == '$';
}

}  // namespace

const std::vector<std::string>& sqlKeywords() { return keywordList(); }
const std::vector<std::string>& sqlFunctions() { return functionList(); }

bool isSqlKeyword(const std::string& word) {
  static const auto kSet = upperSet(keywordList());
  return kSet.count(toUpper(word)) > 0;
}

bool isSqlFunction(const std::string& word) {
  static const auto kSet = upperSet(functionList());
  return kSet.count(toUpper(word)) > 0;
}

std::string Token::unquoted() const {
  if (text.size() < 2) return text;
  const char f = text.front();
  const char b = text.back();
  if ((f == '`' && b == '`') || (f == '"' && b == '"') ||
      (f == '[' && b == ']')) {
    std::string inner = text.substr(1, text.size() - 2);
    // Collapse doubled quote characters back to one.
    std::string out;
    for (size_t i = 0; i < inner.size(); ++i) {
      out.push_back(inner[i]);
      if (i + 1 < inner.size() && inner[i] == f && inner[i + 1] == f) ++i;
    }
    return out;
  }
  return text;
}

bool Token::isKeyword(const char* kw) const {
  return type == TokenType::Keyword && iequals(text, kw);
}

std::vector<Token> tokenize(const std::string& sql, bool keepTrivia) {
  std::vector<Token> out;
  const size_t n = sql.size();
  size_t i = 0;

  auto push = [&](TokenType t, size_t start, size_t end) {
    if (!keepTrivia &&
        (t == TokenType::Whitespace || t == TokenType::Comment)) {
      return;
    }
    Token tok;
    tok.type = t;
    tok.start = start;
    tok.length = end - start;
    tok.text = sql.substr(start, tok.length);
    out.push_back(std::move(tok));
  };

  while (i < n) {
    const size_t start = i;
    const char c = sql[i];
    const char next = i + 1 < n ? sql[i + 1] : '\0';

    if (std::isspace(static_cast<unsigned char>(c))) {
      while (i < n && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
      push(TokenType::Whitespace, start, i);
      continue;
    }

    if ((c == '-' && next == '-') || c == '#') {
      while (i < n && sql[i] != '\n') ++i;
      push(TokenType::Comment, start, i);
      continue;
    }

    if (c == '/' && next == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
      i = std::min(n, i + 2);
      push(TokenType::Comment, start, i);
      continue;
    }

    if (c == '\'') {
      ++i;
      while (i < n) {
        if (sql[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') { i += 2; continue; }
          ++i;
          break;
        }
        ++i;
      }
      push(TokenType::String, start, i);
      continue;
    }

    // A double-quoted run is a string in MySQL's default mode but an
    // identifier in SQLite. Identifier is the more useful reading for
    // completion, and highlighting treats both the same way.
    if (c == '"' || c == '`' || c == '[') {
      const char closer = c == '[' ? ']' : c;
      ++i;
      while (i < n) {
        if (sql[i] == '\\' && closer != ']' && i + 1 < n) { i += 2; continue; }
        if (sql[i] == closer) {
          if (i + 1 < n && sql[i + 1] == closer && closer != ']') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      push(TokenType::QuotedIdentifier, start, i);
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && std::isdigit(static_cast<unsigned char>(next)))) {
      bool seenExp = false;
      while (i < n) {
        const char d = sql[i];
        if (std::isdigit(static_cast<unsigned char>(d)) || d == '.' ||
            d == 'x' || d == 'X') {
          ++i;
        } else if ((d == 'e' || d == 'E') && !seenExp) {
          seenExp = true;
          ++i;
          if (i < n && (sql[i] == '+' || sql[i] == '-')) ++i;
        } else if (std::isxdigit(static_cast<unsigned char>(d)) &&
                   start + 1 < n && (sql[start + 1] == 'x' || sql[start + 1] == 'X')) {
          ++i;
        } else {
          break;
        }
      }
      push(TokenType::Number, start, i);
      continue;
    }

    if (c == '?' || ((c == ':' || c == '@' || c == '$') && isIdentStart(next))) {
      ++i;
      while (i < n && isIdentChar(sql[i])) ++i;
      push(TokenType::Parameter, start, i);
      continue;
    }

    if (isIdentStart(c)) {
      while (i < n && isIdentChar(sql[i])) ++i;
      const std::string word = sql.substr(start, i - start);
      push(isSqlKeyword(word) ? TokenType::Keyword : TokenType::Identifier,
           start, i);
      continue;
    }

    static const char* kTwoCharOps[] = {"<=", ">=", "<>", "!=", "||", "&&",
                                        "<<", ">>", ":=", "->"};
    bool matched = false;
    for (const char* op : kTwoCharOps) {
      if (c == op[0] && next == op[1]) {
        i += 2;
        // MySQL's JSON unquote operator is three characters.
        if (i < n && sql[i] == '>' && op[0] == '-') ++i;
        push(TokenType::Operator, start, i);
        matched = true;
        break;
      }
    }
    if (matched) continue;

    if (std::strchr("+-*/%=<>!~&|^", c) != nullptr) {
      ++i;
      push(TokenType::Operator, start, i);
      continue;
    }

    if (std::strchr("(),;.", c) != nullptr) {
      ++i;
      push(TokenType::Punctuation, start, i);
      continue;
    }

    ++i;
    push(TokenType::Unknown, start, i);
  }

  return out;
}

}  // namespace ds
