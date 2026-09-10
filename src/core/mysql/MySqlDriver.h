#pragma once
#include <atomic>
#include <mutex>
#include <string>

#include "core/Driver.h"

typedef struct MYSQL MYSQL;

namespace ds {

class MySqlDriver final : public Driver {
 public:
  MySqlDriver() = default;
  ~MySqlDriver() override;

  MySqlDriver(const MySqlDriver&) = delete;
  MySqlDriver& operator=(const MySqlDriver&) = delete;

  Dialect dialect() const override { return Dialect::MySql; }
  void connect(const ConnectionConfig& cfg) override;
  void disconnect() override;
  bool isConnected() const override { return conn_ != nullptr; }
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
  std::string escape(const std::string& s) const;

  MYSQL* conn_ = nullptr;
  ConnectionConfig cfg_;
  std::string activeDb_;
  unsigned long threadId_ = 0;
  // Guards threadId_/cfg_, which cancel() reads from another thread.
  mutable std::mutex cancelMutex_;
};

}  // namespace ds
