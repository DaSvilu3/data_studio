#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/Schema.h"
#include "core/Value.h"

namespace ds {

struct ConnectionConfig {
  std::string id;      // stable uuid, assigned by the connection store
  std::string name;    // user-facing label
  Dialect dialect = Dialect::Sqlite;

  // SQLite
  std::string filePath;
  bool readOnly = false;

  // MySQL
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user;
  std::string password;   // never persisted to disk in plaintext
  std::string database;
  bool useSsl = true;
};

// A live connection to one database. Not thread-safe: each instance is owned by
// exactly one worker thread. cancel() is the sole exception -- it may be called
// from another thread while execute() is in flight.
class Driver {
 public:
  virtual ~Driver() = default;

  virtual Dialect dialect() const = 0;
  virtual void connect(const ConnectionConfig& cfg) = 0;
  virtual void disconnect() = 0;
  virtual bool isConnected() const = 0;

  // Server/engine version string, for display.
  virtual std::string serverVersion() const = 0;

  // Runs one statement. Caps SELECT output at `maxRows` (<=0 means unlimited)
  // and sets ResultSet::truncated when the cap bites. Throws DbError.
  virtual ResultSet execute(const std::string& sql, int maxRows) = 0;

  // Best-effort interrupt of an in-flight execute(). Safe from any thread.
  virtual void cancel() = 0;

  // Reads tables, columns, indexes and foreign keys for the active database.
  virtual Schema introspect() = 0;

  // Databases/schemas visible on this connection (SQLite: attached databases).
  virtual std::vector<std::string> databases() = 0;

  // Switch the active database without reconnecting. No-op on SQLite.
  virtual void useDatabase(const std::string& name) = 0;

  // Query plan for `sql`, in whatever shape the engine reports it.
  virtual ResultSet explain(const std::string& sql) = 0;

  virtual std::string quoteIdentifier(const std::string& ident) const = 0;
};

using DriverPtr = std::unique_ptr<Driver>;

// Builds an unconnected driver for the config's dialect. Throws DbError if the
// dialect wasn't compiled in.
DriverPtr makeDriver(const ConnectionConfig& cfg);

// Splits a script into individual statements, respecting string literals,
// bracket/backtick quoting and both comment styles.
std::vector<std::string> splitStatements(const std::string& script);

}  // namespace ds
