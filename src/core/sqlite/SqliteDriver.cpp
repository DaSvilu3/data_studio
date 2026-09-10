#include "core/sqlite/SqliteDriver.h"

#include <sqlite3.h>

#include <chrono>
#include <cstring>

namespace ds {
namespace {

Value columnValue(sqlite3_stmt* st, int i) {
  switch (sqlite3_column_type(st, i)) {
    case SQLITE_NULL:
      return std::monostate{};
    case SQLITE_INTEGER:
      return static_cast<int64_t>(sqlite3_column_int64(st, i));
    case SQLITE_FLOAT:
      return sqlite3_column_double(st, i);
    case SQLITE_BLOB: {
      const auto* p = static_cast<const unsigned char*>(sqlite3_column_blob(st, i));
      int n = sqlite3_column_bytes(st, i);
      return Blob(p, p + n);
    }
    default: {
      const auto* p = sqlite3_column_text(st, i);
      int n = sqlite3_column_bytes(st, i);
      return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
    }
  }
}

std::string safeStr(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

SqliteDriver::~SqliteDriver() { disconnect(); }

void SqliteDriver::fail(const std::string& what) const {
  int code = db_ ? sqlite3_extended_errcode(db_) : SQLITE_ERROR;
  std::string msg = db_ ? safeStr(sqlite3_errmsg(db_)) : "no connection";
  throw DbError(what + ": " + msg, code);
}

void SqliteDriver::connect(const ConnectionConfig& cfg) {
  disconnect();
  if (cfg.filePath.empty()) {
    throw DbError("No SQLite database file was given.");
  }

  int flags = cfg.readOnly ? SQLITE_OPEN_READONLY
                           : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  flags |= SQLITE_OPEN_URI;

  int rc = sqlite3_open_v2(cfg.filePath.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = db_ ? safeStr(sqlite3_errmsg(db_)) : sqlite3_errstr(rc);
    sqlite3_close(db_);
    db_ = nullptr;
    throw DbError("Could not open " + cfg.filePath + ": " + msg, rc);
  }

  sqlite3_busy_timeout(db_, 5000);
  // WAL keeps readers from blocking on a concurrent writer, which matters when
  // the user has the same file open in another tool.
  if (!cfg.readOnly) {
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
  }
  sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
}

void SqliteDriver::disconnect() {
  if (db_) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

std::string SqliteDriver::serverVersion() const {
  return std::string("SQLite ") + sqlite3_libversion();
}

void SqliteDriver::cancel() {
  cancelRequested_.store(true);
  if (db_) sqlite3_interrupt(db_);
}

ResultSet SqliteDriver::execute(const std::string& sql, int maxRows) {
  if (!db_) throw DbError("Not connected.");
  cancelRequested_.store(false);
  return runInternal(sql, maxRows);
}

ResultSet SqliteDriver::runInternal(const std::string& sql, int maxRows) {
  const auto start = std::chrono::steady_clock::now();

  sqlite3_stmt* st = nullptr;
  const char* tail = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, &tail) != SQLITE_OK) {
    fail("Syntax error");
  }
  if (!st) {
    // Statement was entirely whitespace or a comment.
    ResultSet empty;
    empty.statement = sql;
    return empty;
  }

  ResultSet rs;
  rs.statement = sql;

  const int ncol = sqlite3_column_count(st);
  rs.columns.reserve(static_cast<size_t>(ncol));
  for (int i = 0; i < ncol; ++i) {
    ColumnMeta m;
    m.name = safeStr(sqlite3_column_name(st, i));
    m.typeName = safeStr(sqlite3_column_decltype(st, i));
    if (m.typeName.empty()) m.typeName = "any";
#ifdef SQLITE_ENABLE_COLUMN_METADATA
    m.tableName = safeStr(sqlite3_column_table_name(st, i));
#endif
    rs.columns.push_back(std::move(m));
  }

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (maxRows > 0 && static_cast<int>(rs.rows.size()) >= maxRows) {
      rs.truncated = true;
      break;
    }
    Row row;
    row.reserve(static_cast<size_t>(ncol));
    for (int i = 0; i < ncol; ++i) row.push_back(columnValue(st, i));
    rs.rows.push_back(std::move(row));
  }

  if (rc != SQLITE_DONE && rc != SQLITE_ROW && !rs.truncated) {
    sqlite3_finalize(st);
    if (rc == SQLITE_INTERRUPT) throw DbError("Query cancelled.", rc);
    fail("Query failed");
  }
  sqlite3_finalize(st);

  if (ncol == 0) {
    rs.rowsAffected = sqlite3_changes64(db_);
    rs.lastInsertId = sqlite3_last_insert_rowid(db_);
  }

  rs.elapsedMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  return rs;
}

ResultSet SqliteDriver::explain(const std::string& sql) {
  return execute("EXPLAIN QUERY PLAN " + sql, 0);
}

std::vector<std::string> SqliteDriver::databases() {
  std::vector<std::string> out;
  ResultSet rs = execute("PRAGMA database_list", 0);
  for (const auto& r : rs.rows) {
    if (r.size() > 1) out.push_back(toDisplayString(r[1]));
  }
  if (out.empty()) out.push_back("main");
  return out;
}

void SqliteDriver::useDatabase(const std::string&) {
  // SQLite has no notion of switching databases on a live handle; attached
  // databases are addressed by prefix instead.
}

std::string SqliteDriver::quoteIdentifier(const std::string& ident) const {
  std::string out = "\"";
  for (char c : ident) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

Schema SqliteDriver::introspect() {
  if (!db_) throw DbError("Not connected.");

  Schema schema;
  schema.name = "main";

  ResultSet tables = execute(
      "SELECT name, type FROM sqlite_master "
      "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
      "ORDER BY name",
      0);

  for (const auto& row : tables.rows) {
    Table t;
    t.name = toDisplayString(row[0]);
    t.isView = toDisplayString(row[1]) == "view";

    const std::string quoted = quoteIdentifier(t.name);

    // Columns. table_xinfo also reports hidden/generated columns.
    ResultSet cols = execute("PRAGMA table_xinfo(" + quoted + ")", 0);
    for (const auto& c : cols.rows) {
      if (c.size() < 6) continue;
      Column col;
      col.ordinal = static_cast<int>(toInt(c[0]));
      col.name = toDisplayString(c[1]);
      col.type = toDisplayString(c[2]);
      col.nullable = toDisplayString(c[3]) == "0";
      if (!isNull(c[4])) col.defaultValue = toDisplayString(c[4]);
      col.primaryKey = toDisplayString(c[5]) != "0";
      col.kind = classifyType(col.type);
      t.columns.push_back(std::move(col));
    }

    // A single-column INTEGER PRIMARY KEY is the implicit rowid alias, which
    // SQLite auto-assigns whether or not AUTOINCREMENT was declared.
    auto pks = t.primaryKeyColumns();
    if (pks.size() == 1) {
      if (Column* c = const_cast<Column*>(t.findColumn(pks[0]))) {
        if (c->kind == Column::Kind::Integer) c->autoIncrement = true;
      }
    }

    if (!t.isView) {
      ResultSet idx = execute("PRAGMA index_list(" + quoted + ")", 0);
      for (const auto& ir : idx.rows) {
        if (ir.size() < 4) continue;
        Index index;
        index.name = toDisplayString(ir[1]);
        index.unique = toDisplayString(ir[2]) == "1";
        index.primary = toDisplayString(ir[3]) == "pk";
        ResultSet info = execute(
            "PRAGMA index_info(" + quoteIdentifier(index.name) + ")", 0);
        for (const auto& c : info.rows) {
          if (c.size() >= 3 && !isNull(c[2])) {
            index.columns.push_back(toDisplayString(c[2]));
          }
        }
        if (!index.columns.empty()) t.indexes.push_back(std::move(index));
      }

      ResultSet fks = execute("PRAGMA foreign_key_list(" + quoted + ")", 0);
      // Rows for a composite key share an `id`; group them back together.
      for (const auto& fr : fks.rows) {
        if (fr.size() < 5) continue;
        const int64_t id = toInt(fr[0]);
        const std::string toTable = toDisplayString(fr[2]);
        const std::string fromCol = toDisplayString(fr[3]);
        const std::string toCol =
            isNull(fr[4]) ? std::string{} : toDisplayString(fr[4]);

        ForeignKey* target = nullptr;
        for (auto& existing : t.foreignKeys) {
          if (existing.name == t.name + "_fk_" + std::to_string(id)) {
            target = &existing;
            break;
          }
        }
        if (!target) {
          ForeignKey fk;
          fk.name = t.name + "_fk_" + std::to_string(id);
          fk.fromTable = t.name;
          fk.toTable = toTable;
          t.foreignKeys.push_back(std::move(fk));
          target = &t.foreignKeys.back();
        }
        target->fromColumns.push_back(fromCol);
        // A null `to` column means the FK targets the parent's primary key.
        target->toColumns.push_back(toCol);
      }
    }

    schema.tables.push_back(std::move(t));
  }

  // Resolve implicit FK targets now that every table is known, and pull row
  // estimates. COUNT(*) is exact and cheap enough for SQLite-sized data.
  for (auto& t : schema.tables) {
    for (auto& fk : t.foreignKeys) {
      for (size_t i = 0; i < fk.toColumns.size(); ++i) {
        if (!fk.toColumns[i].empty()) continue;
        if (const Table* parent = schema.findTable(fk.toTable)) {
          auto pks = parent->primaryKeyColumns();
          fk.toColumns[i] = i < pks.size() ? pks[i] : std::string{};
        }
      }
    }
    if (!t.isView) {
      try {
        ResultSet c = execute(
            "SELECT COUNT(*) FROM " + quoteIdentifier(t.name), 0);
        if (!c.rows.empty() && !c.rows[0].empty()) {
          t.estimatedRows = toInt(c.rows[0][0], -1);
        }
      } catch (const DbError&) {
        // A broken view or missing module shouldn't sink the whole refresh.
      }
    }
  }

  return schema;
}

}  // namespace ds
