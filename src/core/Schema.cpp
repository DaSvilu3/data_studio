#include "core/Schema.h"

#include <algorithm>
#include <cctype>

namespace ds {

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool Index::coversPrefix(const std::vector<std::string>& cols) const {
  if (cols.empty() || cols.size() > columns.size()) return false;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (!iequals(columns[i], cols[i])) return false;
  }
  return true;
}

const Column* Table::findColumn(const std::string& n) const {
  for (const auto& c : columns) {
    if (iequals(c.name, n)) return &c;
  }
  return nullptr;
}

std::vector<std::string> Table::primaryKeyColumns() const {
  std::vector<std::string> out;
  for (const auto& c : columns) {
    if (c.primaryKey) out.push_back(c.name);
  }
  return out;
}

std::string Table::qualifiedName() const {
  return schema.empty() ? name : schema + "." + name;
}

const Table* Schema::findTable(const std::string& n) const {
  // Accept either "table" or "schema.table".
  auto dot = n.rfind('.');
  std::string bare = dot == std::string::npos ? n : n.substr(dot + 1);
  std::string sch = dot == std::string::npos ? std::string{} : n.substr(0, dot);
  for (const auto& t : tables) {
    if (!iequals(t.name, bare)) continue;
    if (!sch.empty() && !iequals(t.schema, sch)) continue;
    return &t;
  }
  return nullptr;
}

std::vector<const ForeignKey*> Schema::incomingForeignKeys(
    const std::string& table) const {
  std::vector<const ForeignKey*> out;
  for (const auto& t : tables) {
    for (const auto& fk : t.foreignKeys) {
      if (iequals(fk.toTable, table)) out.push_back(&fk);
    }
  }
  return out;
}

std::vector<std::string> Schema::tableNames() const {
  std::vector<std::string> out;
  out.reserve(tables.size());
  for (const auto& t : tables) out.push_back(t.name);
  return out;
}

Column::Kind classifyType(const std::string& declaredType) {
  std::string t;
  for (char c : declaredType) {
    if (c == '(') break;
    t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  auto has = [&](const char* needle) {
    return t.find(needle) != std::string::npos;
  };

  if (has("bool")) return Column::Kind::Boolean;
  if (has("tinyint")) return Column::Kind::Integer;
  if (has("int") || has("serial")) return Column::Kind::Integer;
  if (has("json")) return Column::Kind::Json;
  if (has("datetime") || has("timestamp")) return Column::Kind::DateTime;
  if (has("date") || has("year")) return Column::Kind::Date;
  if (has("time")) return Column::Kind::DateTime;
  if (has("blob") || has("binary") || has("bytea")) return Column::Kind::Blob;
  if (has("char") || has("text") || has("enum") || has("set") || has("clob")) {
    return Column::Kind::Text;
  }
  if (has("real") || has("float") || has("double") || has("decimal") ||
      has("numeric")) {
    return Column::Kind::Real;
  }
  return Column::Kind::Unknown;
}

}  // namespace ds
