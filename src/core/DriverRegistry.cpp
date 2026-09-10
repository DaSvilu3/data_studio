#include "core/Driver.h"

#include <cctype>
#include "core/sqlite/SqliteDriver.h"

#if DS_HAVE_MYSQL
#include "core/mysql/MySqlDriver.h"
#endif

namespace ds {

DriverPtr makeDriver(const ConnectionConfig& cfg) {
  switch (cfg.dialect) {
    case Dialect::Sqlite:
      return std::make_unique<SqliteDriver>();
    case Dialect::MySql:
#if DS_HAVE_MYSQL
      return std::make_unique<MySqlDriver>();
#else
      throw DbError("This build has no MySQL support (libmysqlclient was "
                    "missing at configure time).");
#endif
  }
  throw DbError("Unknown database dialect.");
}

std::vector<std::string> splitStatements(const std::string& script) {
  std::vector<std::string> out;
  std::string cur;
  enum class State { Normal, Single, Double, Backtick, Bracket, LineComment, BlockComment };
  State st = State::Normal;

  for (size_t i = 0; i < script.size(); ++i) {
    char c = script[i];
    char next = i + 1 < script.size() ? script[i + 1] : '\0';

    switch (st) {
      case State::Normal:
        if (c == '-' && next == '-') { st = State::LineComment; cur += c; break; }
        if (c == '#') { st = State::LineComment; cur += c; break; }
        if (c == '/' && next == '*') { st = State::BlockComment; cur += c; break; }
        if (c == '\'') { st = State::Single; cur += c; break; }
        if (c == '"') { st = State::Double; cur += c; break; }
        if (c == '`') { st = State::Backtick; cur += c; break; }
        if (c == '[') { st = State::Bracket; cur += c; break; }
        if (c == ';') {
          out.push_back(cur);
          cur.clear();
          break;
        }
        cur += c;
        break;

      case State::Single:
        cur += c;
        if (c == '\\' && next != '\0') { cur += next; ++i; break; }
        if (c == '\'') {
          if (next == '\'') { cur += next; ++i; break; }  // escaped quote
          st = State::Normal;
        }
        break;

      case State::Double:
        cur += c;
        if (c == '\\' && next != '\0') { cur += next; ++i; break; }
        if (c == '"') st = State::Normal;
        break;

      case State::Backtick:
        cur += c;
        if (c == '`') st = State::Normal;
        break;

      case State::Bracket:
        cur += c;
        if (c == ']') st = State::Normal;
        break;

      case State::LineComment:
        cur += c;
        if (c == '\n') st = State::Normal;
        break;

      case State::BlockComment:
        cur += c;
        if (c == '*' && next == '/') { cur += next; ++i; st = State::Normal; }
        break;
    }
  }
  out.push_back(cur);

  // Drop anything that is only whitespace/comments.
  std::vector<std::string> kept;
  for (auto& s : out) {
    bool meaningful = false;
    for (char c : s) {
      if (!std::isspace(static_cast<unsigned char>(c))) { meaningful = true; break; }
    }
    if (meaningful) kept.push_back(std::move(s));
  }
  return kept;
}

}  // namespace ds
