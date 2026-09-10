#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ds {

struct Column {
  std::string name;
  std::string type;           // declared type, e.g. "VARCHAR(64)"
  bool nullable = true;
  bool primaryKey = false;
  bool autoIncrement = false;
  std::optional<std::string> defaultValue;
  std::string comment;
  int ordinal = 0;

  // Broad category derived from `type`; drives operator suggestions in the
  // query builder and type-mismatch warnings in the analyzer.
  enum class Kind { Unknown, Integer, Real, Text, Blob, Boolean, Date, DateTime, Json };
  Kind kind = Kind::Unknown;
};

struct Index {
  std::string name;
  bool unique = false;
  bool primary = false;
  std::vector<std::string> columns;

  // True if `cols` is a prefix of this index's column list -- the test that
  // decides whether an index can actually serve a predicate.
  bool coversPrefix(const std::vector<std::string>& cols) const;
};

struct ForeignKey {
  std::string name;
  std::string fromTable;
  std::vector<std::string> fromColumns;
  std::string toTable;
  std::vector<std::string> toColumns;
};

struct Table {
  std::string name;
  std::string schema;         // database name for MySQL; empty for SQLite main
  bool isView = false;
  std::string comment;
  std::vector<Column> columns;
  std::vector<Index> indexes;
  std::vector<ForeignKey> foreignKeys;
  int64_t estimatedRows = -1;
  int64_t sizeBytes = -1;

  const Column* findColumn(const std::string& name) const;
  std::vector<std::string> primaryKeyColumns() const;
  // Qualified name as it should appear in SQL for this table's schema.
  std::string qualifiedName() const;
};

struct Schema {
  std::string name;
  std::vector<Table> tables;

  const Table* findTable(const std::string& name) const;
  // Every FK pointing *at* the given table.
  std::vector<const ForeignKey*> incomingForeignKeys(const std::string& table) const;
  std::vector<std::string> tableNames() const;
};

// Map a driver-reported type string onto a Column::Kind.
Column::Kind classifyType(const std::string& declaredType);

// Case-insensitive compare, used throughout for identifier matching.
bool iequals(const std::string& a, const std::string& b);

}  // namespace ds
