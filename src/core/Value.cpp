#include "core/Value.h"

#include <cstdio>
#include <sstream>

namespace ds {

bool isNull(const Value& v) {
  return std::holds_alternative<std::monostate>(v);
}

std::string toDisplayString(const Value& v) {
  struct Visitor {
    std::string operator()(std::monostate) const { return "NULL"; }
    std::string operator()(int64_t i) const { return std::to_string(i); }
    std::string operator()(double d) const {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.10g", d);
      return buf;
    }
    std::string operator()(const std::string& s) const { return s; }
    std::string operator()(const Blob& b) const {
      return "<" + std::to_string(b.size()) + " bytes>";
    }
  };
  return std::visit(Visitor{}, v);
}

namespace {
std::string quoteString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') out.push_back('\'');
    if (c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}
}  // namespace

std::string toSqlLiteral(const Value& v) {
  struct Visitor {
    std::string operator()(std::monostate) const { return "NULL"; }
    std::string operator()(int64_t i) const { return std::to_string(i); }
    std::string operator()(double d) const {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", d);
      return buf;
    }
    std::string operator()(const std::string& s) const { return quoteString(s); }
    std::string operator()(const Blob& b) const {
      static const char* kHex = "0123456789ABCDEF";
      std::string out = "X'";
      for (unsigned char c : b) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
      }
      out.push_back('\'');
      return out;
    }
  };
  return std::visit(Visitor{}, v);
}

const char* dialectName(Dialect d) {
  switch (d) {
    case Dialect::Sqlite: return "SQLite";
    case Dialect::MySql:  return "MySQL";
  }
  return "Unknown";
}

}  // namespace ds
