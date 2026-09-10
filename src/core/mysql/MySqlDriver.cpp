#include "core/mysql/MySqlDriver.h"

#include <mysql.h>

#include <chrono>
#include <cstring>

namespace ds {
namespace {

bool isBinaryType(enum_field_types t, unsigned int flags) {
  if (!(flags & BINARY_FLAG)) return false;
  switch (t) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY:
      return true;
    default:
      return false;
  }
}

bool isIntegerType(enum_field_types t) {
  switch (t) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

bool isFloatType(enum_field_types t) {
  return t == MYSQL_TYPE_FLOAT || t == MYSQL_TYPE_DOUBLE;
}

const char* typeName(enum_field_types t) {
  switch (t) {
    case MYSQL_TYPE_TINY: return "tinyint";
    case MYSQL_TYPE_SHORT: return "smallint";
    case MYSQL_TYPE_LONG: return "int";
    case MYSQL_TYPE_INT24: return "mediumint";
    case MYSQL_TYPE_LONGLONG: return "bigint";
    case MYSQL_TYPE_FLOAT: return "float";
    case MYSQL_TYPE_DOUBLE: return "double";
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DECIMAL: return "decimal";
    case MYSQL_TYPE_DATE: return "date";
    case MYSQL_TYPE_TIME: return "time";
    case MYSQL_TYPE_DATETIME: return "datetime";
    case MYSQL_TYPE_TIMESTAMP: return "timestamp";
    case MYSQL_TYPE_YEAR: return "year";
    case MYSQL_TYPE_JSON: return "json";
    case MYSQL_TYPE_BIT: return "bit";
    case MYSQL_TYPE_ENUM: return "enum";
    case MYSQL_TYPE_SET: return "set";
    case MYSQL_TYPE_BLOB: return "blob";
    case MYSQL_TYPE_TINY_BLOB: return "tinyblob";
    case MYSQL_TYPE_MEDIUM_BLOB: return "mediumblob";
    case MYSQL_TYPE_LONG_BLOB: return "longblob";
    case MYSQL_TYPE_VAR_STRING: return "varchar";
    case MYSQL_TYPE_STRING: return "char";
    default: return "text";
  }
}

// Casts the opaque handle back to the client library's connection type.
MYSQL* handleOf(void* conn) { return static_cast<MYSQL*>(conn); }

}  // namespace

MySqlDriver::~MySqlDriver() { disconnect(); }

void MySqlDriver::fail(const std::string& what) const {
  if (!conn_) throw DbError(what + ": not connected.");
  throw DbError(what + ": " + mysql_error(handleOf(conn_)),
                static_cast<int>(mysql_errno(handleOf(conn_))),
                mysql_sqlstate(handleOf(conn_)) ? mysql_sqlstate(handleOf(conn_)) : "");
}

void MySqlDriver::connect(const ConnectionConfig& cfg) {
  disconnect();

  conn_ = mysql_init(nullptr);
  if (!conn_) throw DbError("mysql_init failed (out of memory).");

  unsigned int timeout = 10;
  mysql_options(handleOf(conn_), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(handleOf(conn_), MYSQL_SET_CHARSET_NAME, "utf8mb4");

  // Prefer TLS but don't refuse a server that can't offer it -- requiring it
  // outright breaks against plenty of local/dev servers. The two client
  // libraries spell this differently.
#ifdef MARIADB_PACKAGE_VERSION
  // Connector/C negotiates TLS on its own when the server offers it, so
  // "prefer" is simply the default; only the opt-out needs saying.
  my_bool enforceSsl = 0;
  if (!cfg.useSsl) mysql_options(handleOf(conn_), MYSQL_OPT_SSL_ENFORCE, &enforceSsl);
#else
  unsigned int sslMode = cfg.useSsl ? SSL_MODE_PREFERRED : SSL_MODE_DISABLED;
  mysql_options(handleOf(conn_), MYSQL_OPT_SSL_MODE, &sslMode);
#endif

  // CLIENT_MULTI_STATEMENTS is deliberately *not* set: statements are split
  // and executed one at a time so each gets its own timing and result tab.
  MYSQL* ok = mysql_real_connect(
      handleOf(conn_), cfg.host.c_str(), cfg.user.c_str(),
      cfg.password.empty() ? nullptr : cfg.password.c_str(),
      cfg.database.empty() ? nullptr : cfg.database.c_str(),
      static_cast<unsigned int>(cfg.port), nullptr, 0);

  if (!ok) {
    std::string msg = mysql_error(handleOf(conn_));
    unsigned int code = mysql_errno(handleOf(conn_));
    mysql_close(handleOf(conn_));
    conn_ = nullptr;
    throw DbError("Could not connect to " + cfg.host + ":" +
                      std::to_string(cfg.port) + " -- " + msg,
                  static_cast<int>(code));
  }

  std::lock_guard<std::mutex> lock(cancelMutex_);
  cfg_ = cfg;
  activeDb_ = cfg.database;
  threadId_ = mysql_thread_id(handleOf(conn_));
}

void MySqlDriver::disconnect() {
  if (conn_) {
    mysql_close(handleOf(conn_));
    conn_ = nullptr;
  }
  std::lock_guard<std::mutex> lock(cancelMutex_);
  threadId_ = 0;
}

std::string MySqlDriver::serverVersion() const {
  if (!conn_) return "MySQL (disconnected)";
  // MariaDB reports itself in the version string, so don't hard-code "MySQL".
  const std::string version = mysql_get_server_info(handleOf(conn_));
  const bool isMaria = version.find("Maria") != std::string::npos;
  return (isMaria ? "" : "MySQL ") + version;
}

void MySqlDriver::cancel() {
  // The in-flight query owns the connection, so the kill has to travel over a second,
  // short-lived connection. This is how the MySQL CLI does Ctrl-C too.
  ConnectionConfig cfg;
  unsigned long tid = 0;
  {
    std::lock_guard<std::mutex> lock(cancelMutex_);
    if (threadId_ == 0) return;
    cfg = cfg_;
    tid = threadId_;
  }

  MYSQL* killer = mysql_init(nullptr);
  if (!killer) return;
  unsigned int timeout = 5;
  mysql_options(killer, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  if (mysql_real_connect(killer, cfg.host.c_str(), cfg.user.c_str(),
                         cfg.password.empty() ? nullptr : cfg.password.c_str(),
                         nullptr, static_cast<unsigned int>(cfg.port), nullptr,
                         0)) {
    const std::string kill = "KILL QUERY " + std::to_string(tid);
    mysql_real_query(killer, kill.c_str(),
                     static_cast<unsigned long>(kill.size()));
  }
  mysql_close(killer);
}

std::string MySqlDriver::escape(const std::string& s) const {
  std::string out(s.size() * 2 + 1, '\0');
  unsigned long n =
      conn_ ? mysql_real_escape_string(handleOf(conn_), out.data(), s.c_str(),
                                       static_cast<unsigned long>(s.size()))
            : 0;
  out.resize(n);
  return out;
}

ResultSet MySqlDriver::execute(const std::string& sql, int maxRows) {
  if (!conn_) throw DbError("Not connected.");
  const auto start = std::chrono::steady_clock::now();

  if (mysql_real_query(handleOf(conn_), sql.c_str(),
                       static_cast<unsigned long>(sql.size())) != 0) {
    fail("Query failed");
  }

  ResultSet rs;
  rs.statement = sql;

  // use_result streams rows instead of buffering the whole set client-side,
  // so a stray `SELECT * FROM huge_table` doesn't blow up memory before the
  // row cap gets a chance to apply.
  MYSQL_RES* res = mysql_use_result(handleOf(conn_));
  if (!res) {
    if (mysql_field_count(handleOf(conn_)) != 0) fail("Could not read result");
    rs.rowsAffected = static_cast<int64_t>(mysql_affected_rows(handleOf(conn_)));
    rs.lastInsertId = static_cast<int64_t>(mysql_insert_id(handleOf(conn_)));
    rs.elapsedMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start).count();
    return rs;
  }

  const unsigned int ncol = mysql_num_fields(res);
  MYSQL_FIELD* fields = mysql_fetch_fields(res);
  rs.columns.reserve(ncol);
  for (unsigned int i = 0; i < ncol; ++i) {
    ColumnMeta m;
    m.name = fields[i].name ? fields[i].name : "";
    m.typeName = typeName(fields[i].type);
    m.tableName = fields[i].org_table ? fields[i].org_table : "";
    rs.columns.push_back(std::move(m));
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    if (maxRows > 0 && static_cast<int>(rs.rows.size()) >= maxRows) {
      rs.truncated = true;
      break;
    }
    unsigned long* lengths = mysql_fetch_lengths(res);
    Row out;
    out.reserve(ncol);
    for (unsigned int i = 0; i < ncol; ++i) {
      if (!row[i]) {
        out.emplace_back(std::monostate{});
        continue;
      }
      const size_t len = lengths ? lengths[i] : std::strlen(row[i]);
      const auto type = fields[i].type;
      if (isBinaryType(type, fields[i].flags)) {
        const auto* p = reinterpret_cast<const unsigned char*>(row[i]);
        out.emplace_back(Blob(p, p + len));
      } else if (isIntegerType(type)) {
        try {
          out.emplace_back(static_cast<int64_t>(
              std::stoll(std::string(row[i], len))));
        } catch (...) {
          out.emplace_back(std::string(row[i], len));
        }
      } else if (isFloatType(type)) {
        try {
          out.emplace_back(std::stod(std::string(row[i], len)));
        } catch (...) {
          out.emplace_back(std::string(row[i], len));
        }
      } else {
        // DECIMAL stays textual on purpose -- converting it to double would
        // silently lose the precision the column type exists to guarantee.
        out.emplace_back(std::string(row[i], len));
      }
    }
    rs.rows.push_back(std::move(out));
  }

  const bool streamError = mysql_errno(handleOf(conn_)) != 0;
  std::string streamMsg = streamError ? mysql_error(handleOf(conn_)) : "";
  unsigned int streamCode = mysql_errno(handleOf(conn_));

  if (rs.truncated) {
    // Drain the rest so the connection is usable again; the server-side query
    // keeps running otherwise.
    while (mysql_fetch_row(res) != nullptr) {}
  }
  mysql_free_result(res);

  if (streamError) {
    throw DbError(streamCode == 1317 ? "Query cancelled."
                                     : "Query failed: " + streamMsg,
                  static_cast<int>(streamCode));
  }

  rs.elapsedMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start).count();
  return rs;
}

ResultSet MySqlDriver::explain(const std::string& sql) {
  return execute("EXPLAIN " + sql, 0);
}

std::vector<std::string> MySqlDriver::databases() {
  std::vector<std::string> out;
  ResultSet rs = execute(
      "SELECT schema_name FROM information_schema.schemata "
      "WHERE schema_name NOT IN "
      "('information_schema','performance_schema','mysql','sys') "
      "ORDER BY schema_name",
      0);
  for (const auto& r : rs.rows) {
    if (!r.empty()) out.push_back(toDisplayString(r[0]));
  }
  return out;
}

void MySqlDriver::useDatabase(const std::string& name) {
  if (!conn_) throw DbError("Not connected.");
  if (mysql_select_db(handleOf(conn_), name.c_str()) != 0) {
    fail("Could not switch to database '" + name + "'");
  }
  std::lock_guard<std::mutex> lock(cancelMutex_);
  activeDb_ = name;
  cfg_.database = name;
}

std::string MySqlDriver::quoteIdentifier(const std::string& ident) const {
  std::string out = "`";
  for (char c : ident) {
    if (c == '`') out.push_back('`');
    out.push_back(c);
  }
  out.push_back('`');
  return out;
}

Schema MySqlDriver::introspect() {
  if (!conn_) throw DbError("Not connected.");

  std::string db = activeDb_;
  if (db.empty()) {
    ResultSet cur = execute("SELECT DATABASE()", 0);
    if (!cur.rows.empty() && !cur.rows[0].empty() && !isNull(cur.rows[0][0])) {
      db = toDisplayString(cur.rows[0][0]);
    }
  }
  if (db.empty()) {
    throw DbError("No database selected -- pick one to browse its schema.");
  }

  Schema schema;
  schema.name = db;
  const std::string dbLit = "'" + escape(db) + "'";

  // One round trip per metadata kind rather than per table: on a schema with a
  // few hundred tables the per-table version takes seconds.
  ResultSet tables = execute(
      "SELECT table_name, table_type, table_rows, "
      "       COALESCE(data_length + index_length, -1), "
      "       COALESCE(table_comment, '') "
      "FROM information_schema.tables WHERE table_schema = " + dbLit +
          " ORDER BY table_name",
      0);

  for (const auto& r : tables.rows) {
    Table t;
    t.schema = db;
    t.name = toDisplayString(r[0]);
    t.isView = toDisplayString(r[1]) == "VIEW";
    if (!isNull(r[2])) t.estimatedRows = toInt(r[2], -1);
    if (!isNull(r[3])) t.sizeBytes = toInt(r[3], -1);
    t.comment = toDisplayString(r[4]);
    schema.tables.push_back(std::move(t));
  }

  auto tableByName = [&](const std::string& n) -> Table* {
    for (auto& t : schema.tables) {
      if (t.name == n) return &t;
    }
    return nullptr;
  };

  ResultSet cols = execute(
      "SELECT table_name, column_name, column_type, is_nullable, column_key, "
      "       extra, column_default, ordinal_position, "
      "       COALESCE(column_comment, '') "
      "FROM information_schema.columns WHERE table_schema = " + dbLit +
          " ORDER BY table_name, ordinal_position",
      0);

  for (const auto& r : cols.rows) {
    Table* t = tableByName(toDisplayString(r[0]));
    if (!t) continue;
    Column c;
    c.name = toDisplayString(r[1]);
    c.type = toDisplayString(r[2]);
    c.nullable = toDisplayString(r[3]) == "YES";
    c.primaryKey = toDisplayString(r[4]) == "PRI";
    c.autoIncrement =
        toDisplayString(r[5]).find("auto_increment") != std::string::npos;
    if (!isNull(r[6])) c.defaultValue = toDisplayString(r[6]);
    c.ordinal = static_cast<int>(toInt(r[7], 1)) - 1;
    c.comment = toDisplayString(r[8]);
    c.kind = classifyType(c.type);
    t->columns.push_back(std::move(c));
  }

  ResultSet idx = execute(
      "SELECT table_name, index_name, non_unique, seq_in_index, column_name "
      "FROM information_schema.statistics WHERE table_schema = " + dbLit +
          " ORDER BY table_name, index_name, seq_in_index",
      0);

  for (const auto& r : idx.rows) {
    Table* t = tableByName(toDisplayString(r[0]));
    if (!t || isNull(r[4])) continue;
    const std::string name = toDisplayString(r[1]);
    Index* target = nullptr;
    for (auto& i : t->indexes) {
      if (i.name == name) { target = &i; break; }
    }
    if (!target) {
      Index i;
      i.name = name;
      i.unique = toDisplayString(r[2]) == "0";
      i.primary = name == "PRIMARY";
      t->indexes.push_back(std::move(i));
      target = &t->indexes.back();
    }
    target->columns.push_back(toDisplayString(r[4]));
  }

  ResultSet fks = execute(
      "SELECT k.constraint_name, k.table_name, k.column_name, "
      "       k.referenced_table_name, k.referenced_column_name "
      "FROM information_schema.key_column_usage k "
      "WHERE k.table_schema = " + dbLit +
          " AND k.referenced_table_name IS NOT NULL "
          "ORDER BY k.table_name, k.constraint_name, k.ordinal_position",
      0);

  for (const auto& r : fks.rows) {
    Table* t = tableByName(toDisplayString(r[1]));
    if (!t) continue;
    const std::string name = toDisplayString(r[0]);
    ForeignKey* target = nullptr;
    for (auto& f : t->foreignKeys) {
      if (f.name == name) { target = &f; break; }
    }
    if (!target) {
      ForeignKey f;
      f.name = name;
      f.fromTable = t->name;
      f.toTable = toDisplayString(r[3]);
      t->foreignKeys.push_back(std::move(f));
      target = &t->foreignKeys.back();
    }
    target->fromColumns.push_back(toDisplayString(r[2]));
    target->toColumns.push_back(toDisplayString(r[4]));
  }

  return schema;
}

}  // namespace ds
