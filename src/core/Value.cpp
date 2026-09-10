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

int64_t toInt(const Value& v, int64_t fallback) {
  struct Visitor {
    int64_t fallback;
    int64_t operator()(std::monostate) const { return fallback; }
    int64_t operator()(int64_t i) const { return i; }
    int64_t operator()(double d) const { return static_cast<int64_t>(d); }
    int64_t operator()(const std::string& s) const {
      try {
        // stoll stops at the first non-digit, which handles "12.5" too.
        return std::stoll(s);
      } catch (...) {
        return fallback;
      }
    }
    int64_t operator()(const Blob&) const { return fallback; }
  };
  return std::visit(Visitor{fallback}, v);
}

double toReal(const Value& v, double fallback) {
  struct Visitor {
    double fallback;
    double operator()(std::monostate) const { return fallback; }
    double operator()(int64_t i) const { return static_cast<double>(i); }
    double operator()(double d) const { return d; }
    double operator()(const std::string& s) const {
      try {
        return std::stod(s);
      } catch (...) {
        return fallback;
      }
    }
    double operator()(const Blob&) const { return fallback; }
  };
  return std::visit(Visitor{fallback}, v);
}

const char* dialectName(Dialect d) {
  switch (d) {
    case Dialect::Sqlite: return "SQLite";
    case Dialect::MySql:  return "MySQL";
  }
  return "Unknown";
}

}  // namespace ds
