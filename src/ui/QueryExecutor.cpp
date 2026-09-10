#include "ui/QueryExecutor.h"

#include <QMetaObject>

namespace ds {

QueryWorker::QueryWorker(ConnectionConfig cfg) : cfg_(std::move(cfg)) {}

QueryWorker::~QueryWorker() = default;

void QueryWorker::requestCancel() {
  // Deliberately touches the driver from the caller's thread -- both drivers
  // document cancel() as the one cross-thread-safe entry point.
  if (driver_) driver_->cancel();
}

void QueryWorker::openConnection() {
  try {
    driver_ = makeDriver(cfg_);
    driver_->connect(cfg_);
    QStringList dbs;
    for (const auto& d : driver_->databases()) {
      dbs << QString::fromStdString(d);
    }
    emit connected(QString::fromStdString(driver_->serverVersion()), dbs);
  } catch (const DbError& e) {
    driver_.reset();
    emit connectionFailed(QString::fromUtf8(e.what()));
  } catch (const std::exception& e) {
    driver_.reset();
    emit connectionFailed(QString::fromUtf8(e.what()));
  }
}

void QueryWorker::closeConnection() {
  if (driver_) driver_->disconnect();
  driver_.reset();
  emit finished();
}

void QueryWorker::runScript(const QString& sql, int maxRows) {
  if (!driver_) {
    emit failed(QStringLiteral("Not connected."), sql);
    return;
  }
  emit busyChanged(true);

  const auto statements = splitStatements(sql.toStdString());
  const int total = static_cast<int>(statements.size());
  for (int i = 0; i < total; ++i) {
    try {
      ResultSet rs = driver_->execute(statements[i], maxRows);
      emit resultReady(rs, i, total);
    } catch (const DbError& e) {
      emit failed(QString::fromUtf8(e.what()),
                  QString::fromStdString(statements[i]));
      break;  // a failed statement usually invalidates the ones after it
    } catch (const std::exception& e) {
      emit failed(QString::fromUtf8(e.what()),
                  QString::fromStdString(statements[i]));
      break;
    }
  }
  emit busyChanged(false);
}

void QueryWorker::runExplain(const QString& sql) {
  if (!driver_) return;
  emit busyChanged(true);
  try {
    const auto stmts = splitStatements(sql.toStdString());
    if (!stmts.empty()) {
      emit explainReady(driver_->explain(stmts.front()),
                        QString::fromStdString(stmts.front()));
    }
  } catch (const std::exception& e) {
    emit failed(QString::fromUtf8(e.what()), sql);
  }
  emit busyChanged(false);
}

void QueryWorker::refreshSchema() {
  if (!driver_) return;
  emit busyChanged(true);
  try {
    emit schemaReady(driver_->introspect());
  } catch (const std::exception& e) {
    emit failed(QString::fromUtf8(e.what()), QStringLiteral("<introspection>"));
  }
  emit busyChanged(false);
}

void QueryWorker::profileColumn(const QString& table, const QString& column) {
  if (!driver_) return;
  emit busyChanged(true);
  try {
    Schema schema = driver_->introspect();
    const Table* t = schema.findTable(table.toStdString());
    const Column* c = t ? t->findColumn(column.toStdString()) : nullptr;
    if (!t || !c) {
      emit failed(QStringLiteral("No such column: %1.%2").arg(table, column),
                  QString());
      emit busyChanged(false);
      return;
    }

    ProfileQueries queries(driver_->dialect(), *t, *c);
    const ResultSet counts = driver_->execute(queries.counts(), 0);
    // MIN/MAX over a blob or JSON column is meaningless, so it's skipped
    // rather than failing the whole profile.
    ResultSet range;
    if (queries.supportsRange()) {
      try {
        range = driver_->execute(queries.range(), 0);
      } catch (const DbError&) {
      }
    }
    ResultSet top;
    try {
      top = driver_->execute(queries.topValues(8), 0);
    } catch (const DbError&) {
    }

    emit profileReady(assembleProfile(*t, *c, counts, range, top));
  } catch (const std::exception& e) {
    emit failed(QString::fromUtf8(e.what()),
                QStringLiteral("<profiling %1.%2>").arg(table, column));
  }
  emit busyChanged(false);
}

void QueryWorker::switchDatabase(const QString& name) {
  if (!driver_) return;
  // Re-introspecting a large schema is not instant. Without this the GUI looks
  // idle while the connection has already moved, so the sidebar still lists the
  // previous database's tables and clicking one runs it against the new one.
  emit busyChanged(true);
  try {
    driver_->useDatabase(name.toStdString());
    emit schemaReady(driver_->introspect());
  } catch (const std::exception& e) {
    emit failed(QString::fromUtf8(e.what()), QStringLiteral("USE ") + name);
  }
  emit busyChanged(false);
}

void QueryWorker::locateTable(const QString& table) {
  if (!driver_ || driver_->dialect() != Dialect::MySql) return;
  try {
    // Only used to explain a failure, so a single cheap lookup is fine.
    std::string escaped;
    for (char c : table.toStdString()) {
      if (c == '\'' || c == '\\') escaped.push_back('\\');
      escaped.push_back(c);
    }
    const ResultSet rs = driver_->execute(
        "SELECT table_schema FROM information_schema.tables "
        "WHERE table_name = '" + escaped + "' ORDER BY table_schema", 0);

    QStringList found;
    for (const auto& row : rs.rows) {
      if (!row.empty()) found << QString::fromStdString(toDisplayString(row[0]));
    }
    if (!found.isEmpty()) emit tableLocated(table, found);
  } catch (const std::exception&) {
    // Best effort: this only ever improves an error message.
  }
}

// ---------------------------------------------------------------------------

ConnectionSession::ConnectionSession(const ConnectionConfig& cfg,
                                     QObject* parent)
    : QObject(parent), cfg_(cfg) {
  static const int kOnce = []() {
    qRegisterMetaType<ds::ResultSet>("ds::ResultSet");
    qRegisterMetaType<ds::Schema>("ds::Schema");
    qRegisterMetaType<ds::ColumnProfile>("ds::ColumnProfile");
    return 0;
  }();
  Q_UNUSED(kOnce);

  worker_ = new QueryWorker(cfg_);
  worker_->moveToThread(&thread_);
  connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);

  connect(worker_, &QueryWorker::connected, this,
          [this](const QString& version, const QStringList& dbs) {
            serverVersion_ = version;
            databases_ = dbs;
            open_ = true;
            emit opened();
            refreshSchema();
          });
  connect(worker_, &QueryWorker::connectionFailed, this,
          [this](const QString& msg) {
            open_ = false;
            emit openFailed(msg);
          });
  connect(worker_, &QueryWorker::resultReady, this,
          &ConnectionSession::resultReady);
  connect(worker_, &QueryWorker::explainReady, this,
          &ConnectionSession::explainReady);
  connect(worker_, &QueryWorker::schemaReady, this, [this](const Schema& s) {
    schema_ = s;
    emit schemaChanged();
  });
  connect(worker_, &QueryWorker::profileReady, this,
          &ConnectionSession::profileReady);
  connect(worker_, &QueryWorker::tableLocated, this,
          &ConnectionSession::tableLocated);
  connect(worker_, &QueryWorker::failed, this, &ConnectionSession::failed);
  connect(worker_, &QueryWorker::busyChanged, this, [this](bool busy) {
    busy_ = busy;
    emit busyChanged(busy);
  });

  thread_.start();
}

ConnectionSession::~ConnectionSession() {
  if (worker_) worker_->requestCancel();
  QMetaObject::invokeMethod(worker_, "closeConnection", Qt::QueuedConnection);
  thread_.quit();
  // Long enough for a cancelled query to unwind; the thread is abandoned
  // rather than hanging the shutdown if the server never responds.
  if (!thread_.wait(5000)) {
    thread_.terminate();
    thread_.wait(1000);
  }
}

void ConnectionSession::open() {
  QMetaObject::invokeMethod(worker_, "openConnection", Qt::QueuedConnection);
}

void ConnectionSession::execute(const QString& sql, int maxRows) {
  QMetaObject::invokeMethod(worker_, "runScript", Qt::QueuedConnection,
                            Q_ARG(QString, sql), Q_ARG(int, maxRows));
}

void ConnectionSession::explain(const QString& sql) {
  QMetaObject::invokeMethod(worker_, "runExplain", Qt::QueuedConnection,
                            Q_ARG(QString, sql));
}

void ConnectionSession::refreshSchema() {
  QMetaObject::invokeMethod(worker_, "refreshSchema", Qt::QueuedConnection);
}

void ConnectionSession::useDatabase(const QString& name) {
  cfg_.database = name.toStdString();
  QMetaObject::invokeMethod(worker_, "switchDatabase", Qt::QueuedConnection,
                            Q_ARG(QString, name));
}

void ConnectionSession::profileColumn(const QString& table,
                                      const QString& column) {
  QMetaObject::invokeMethod(worker_, "profileColumn", Qt::QueuedConnection,
                            Q_ARG(QString, table), Q_ARG(QString, column));
}

void ConnectionSession::locateTable(const QString& table) {
  QMetaObject::invokeMethod(worker_, "locateTable", Qt::QueuedConnection,
                            Q_ARG(QString, table));
}

void ConnectionSession::cancel() {
  if (worker_) worker_->requestCancel();
}

}  // namespace ds
