#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace ds {

using Blob = std::vector<unsigned char>;

// A single cell. monostate == SQL NULL.
using Value = std::variant<std::monostate, int64_t, double, std::string, Blob>;

using Row = std::vector<Value>;

bool isNull(const Value& v);

// Human-readable rendering for grids/CSV. Blobs render as "<N bytes>".
std::string toDisplayString(const Value& v);

// SQL literal rendering, properly escaped for the given dialect.
std::string toSqlLiteral(const Value& v);

enum class Dialect { Sqlite, MySql };

const char* dialectName(Dialect d);

// Error carrying whatever the underlying driver told us.
class DbError : public std::runtime_error {
 public:
  DbError(std::string message, int code = 0, std::string sqlState = {})
      : std::runtime_error(message),
        code_(code),
        sqlState_(std::move(sqlState)) {}

  int code() const { return code_; }
  const std::string& sqlState() const { return sqlState_; }

 private:
  int code_;
  std::string sqlState_;
};

struct ColumnMeta {
  std::string name;
  std::string typeName;   // driver-reported type, best effort
  std::string tableName;  // origin table when the driver can tell us
};

struct ResultSet {
  std::vector<ColumnMeta> columns;
  std::vector<Row> rows;
  int64_t rowsAffected = -1;   // for DML; -1 when not applicable
  int64_t lastInsertId = -1;
  bool truncated = false;      // hit the row cap
  double elapsedMs = 0.0;
  std::string statement;       // the SQL that produced this

  bool isSelect() const { return !columns.empty(); }
};

}  // namespace ds
