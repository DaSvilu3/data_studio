#pragma once
#include <QObject>
#include <QString>
#include <QThread>
#include <memory>

#include "core/Driver.h"
#include "query/Profile.h"

Q_DECLARE_METATYPE(ds::ResultSet)
Q_DECLARE_METATYPE(ds::Schema)
Q_DECLARE_METATYPE(ds::ConnectionConfig)
Q_DECLARE_METATYPE(ds::ColumnProfile)

namespace ds {

// Runs driver calls on a worker thread. Every slot here executes off the GUI
// thread; results come back as queued signals.
class QueryWorker : public QObject {
  Q_OBJECT
 public:
  explicit QueryWorker(ConnectionConfig cfg);
  ~QueryWorker() override;

  // Safe to call from the GUI thread while a query is running -- this is the
  // one method that doesn't go through the event loop.
  void requestCancel();

 public slots:
  void openConnection();
  void closeConnection();
  void runScript(const QString& sql, int maxRows);
  void runExplain(const QString& sql);
  void refreshSchema();
  void switchDatabase(const QString& name);
  void profileColumn(const QString& table, const QString& column);
  // Which databases on this server contain a table of this name.
  void locateTable(const QString& table);

 signals:
  void connected(const QString& serverVersion, const QStringList& databases);
  void connectionFailed(const QString& message);
  void resultReady(const ds::ResultSet& result, int index, int total);
  void explainReady(const ds::ResultSet& plan, const QString& sql);
  void schemaReady(const ds::Schema& schema);
  void profileReady(const ds::ColumnProfile& profile);
  void tableLocated(const QString& table, const QStringList& databases);
  void failed(const QString& message, const QString& statement);
  void finished();
  void busyChanged(bool busy);

 private:
  ConnectionConfig cfg_;
  DriverPtr driver_;
};

// GUI-thread handle for one connection: owns the thread and forwards calls.
class ConnectionSession : public QObject {
  Q_OBJECT
 public:
  explicit ConnectionSession(const ConnectionConfig& cfg,
                             QObject* parent = nullptr);
  ~ConnectionSession() override;

  const ConnectionConfig& config() const { return cfg_; }
  const Schema& schema() const { return schema_; }
  bool isBusy() const { return busy_; }
  bool isOpen() const { return open_; }
  QString serverVersion() const { return serverVersion_; }
  QStringList databases() const { return databases_; }

  void open();
  void execute(const QString& sql, int maxRows);
  void explain(const QString& sql);
  void refreshSchema();
  void useDatabase(const QString& name);
  void profileColumn(const QString& table, const QString& column);
  void locateTable(const QString& table);
  void cancel();

 signals:
  void opened();
  void openFailed(const QString& message);
  void resultReady(const ds::ResultSet& result, int index, int total);
  void explainReady(const ds::ResultSet& plan, const QString& sql);
  void schemaChanged();
  void profileReady(const ds::ColumnProfile& profile);
  void tableLocated(const QString& table, const QStringList& databases);
  void failed(const QString& message, const QString& statement);
  void busyChanged(bool busy);

 private:
  ConnectionConfig cfg_;
  Schema schema_;
  QThread thread_;
  QueryWorker* worker_ = nullptr;
  QString serverVersion_;
  QStringList databases_;
  bool busy_ = false;
  bool open_ = false;
};

}  // namespace ds
