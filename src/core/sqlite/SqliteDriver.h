#pragma once
#include <atomic>
#include <string>

#include "core/Driver.h"

struct sqlite3;

namespace ds {

class SqliteDriver final : public Driver {
 public:
  SqliteDriver() = default;
  ~SqliteDriver() override;

  SqliteDriver(const SqliteDriver&) = delete;
  SqliteDriver& operator=(const SqliteDriver&) = delete;

  Dialect dialect() const override { return Dialect::Sqlite; }
  void connect(const ConnectionConfig& cfg) override;
  void disconnect() override;
  bool isConnected() const override { return db_ != nullptr; }
  std::string serverVersion() const override;

  ResultSet execute(const std::string& sql, int maxRows) override;
  void cancel() override;

  Schema introspect() override;
  std::vector<std::string> databases() override;
  void useDatabase(const std::string& name) override;
  ResultSet explain(const std::string& sql) override;
  std::string quoteIdentifier(const std::string& ident) const override;

 private:
  [[noreturn]] void fail(const std::string& what) const;
  ResultSet runInternal(const std::string& sql, int maxRows);

  sqlite3* db_ = nullptr;
  std::atomic<bool> cancelRequested_{false};
};

}  // namespace ds
