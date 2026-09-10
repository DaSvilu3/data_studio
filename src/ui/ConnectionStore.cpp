#include "ui/ConnectionStore.h"

#include <QProcess>
#include <QSettings>
#include <QUuid>

namespace ds {
namespace {

constexpr const char* kService = "DataStudio";

QString settingsKeyFor(int index) {
  return QStringLiteral("connections/%1/").arg(index);
}

}  // namespace

#ifdef Q_OS_MACOS

// The `security` CLI is the supported way to reach the Keychain without
// linking the Security framework and dealing with its C API.
bool keychainStore(const QString& account, const QString& secret) {
  QProcess p;
  p.start(QStringLiteral("/usr/bin/security"),
          {QStringLiteral("add-generic-password"), QStringLiteral("-U"),
           QStringLiteral("-a"), account, QStringLiteral("-s"),
           QString::fromLatin1(kService), QStringLiteral("-w"), secret});
  p.waitForFinished(5000);
  return p.exitCode() == 0;
}

bool keychainFetch(const QString& account, QString* secret) {
  QProcess p;
  p.start(QStringLiteral("/usr/bin/security"),
          {QStringLiteral("find-generic-password"), QStringLiteral("-a"),
           account, QStringLiteral("-s"), QString::fromLatin1(kService),
           QStringLiteral("-w")});
  p.waitForFinished(5000);
  if (p.exitCode() != 0) return false;
  *secret = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
  return true;
}

void keychainErase(const QString& account) {
  QProcess p;
  p.start(QStringLiteral("/usr/bin/security"),
          {QStringLiteral("delete-generic-password"), QStringLiteral("-a"),
           account, QStringLiteral("-s"), QString::fromLatin1(kService)});
  p.waitForFinished(5000);
}

#else

bool keychainStore(const QString&, const QString&) { return false; }
bool keychainFetch(const QString&, QString*) { return false; }
void keychainErase(const QString&) {}

#endif

ConnectionStore::ConnectionStore(QObject* parent) : QObject(parent) { load(); }

void ConnectionStore::load() {
  items_.clear();
  QSettings settings(QStringLiteral("DataStudio"),
                     QStringLiteral("DataStudio"));
  const int count = settings.value(QStringLiteral("connections/count"), 0).toInt();

  for (int i = 0; i < count; ++i) {
    const QString k = settingsKeyFor(i);
    ConnectionConfig c;
    c.id = settings.value(k + "id").toString().toStdString();
    c.name = settings.value(k + "name").toString().toStdString();
    c.dialect = settings.value(k + "dialect").toInt() == 1 ? Dialect::MySql
                                                           : Dialect::Sqlite;
    c.filePath = settings.value(k + "filePath").toString().toStdString();
    c.readOnly = settings.value(k + "readOnly", false).toBool();
    c.host = settings.value(k + "host", "127.0.0.1").toString().toStdString();
    c.port = settings.value(k + "port", 3306).toInt();
    c.user = settings.value(k + "user").toString().toStdString();
    c.database = settings.value(k + "database").toString().toStdString();
    c.useSsl = settings.value(k + "useSsl", true).toBool();
    if (c.id.empty()) continue;
    items_.append(std::move(c));
  }
  emit changed();
}

void ConnectionStore::persist() const {
  QSettings settings(QStringLiteral("DataStudio"),
                     QStringLiteral("DataStudio"));
  settings.remove(QStringLiteral("connections"));
  settings.setValue(QStringLiteral("connections/count"), items_.size());

  for (int i = 0; i < items_.size(); ++i) {
    const ConnectionConfig& c = items_[i];
    const QString k = settingsKeyFor(i);
    settings.setValue(k + "id", QString::fromStdString(c.id));
    settings.setValue(k + "name", QString::fromStdString(c.name));
    settings.setValue(k + "dialect", c.dialect == Dialect::MySql ? 1 : 0);
    settings.setValue(k + "filePath", QString::fromStdString(c.filePath));
    settings.setValue(k + "readOnly", c.readOnly);
    settings.setValue(k + "host", QString::fromStdString(c.host));
    settings.setValue(k + "port", c.port);
    settings.setValue(k + "user", QString::fromStdString(c.user));
    settings.setValue(k + "database", QString::fromStdString(c.database));
    settings.setValue(k + "useSsl", c.useSsl);
    // c.password is deliberately never written here.
  }
  settings.sync();
}

QString ConnectionStore::save(ConnectionConfig cfg, bool savePassword) {
  if (cfg.id.empty()) {
    cfg.id = QUuid::createUuid()
                 .toString(QUuid::WithoutBraces)
                 .toStdString();
  }
  const QString id = QString::fromStdString(cfg.id);

  if (savePassword && !cfg.password.empty()) {
    keychainStore(id, QString::fromStdString(cfg.password));
  } else if (!savePassword) {
    keychainErase(id);
  }

  bool replaced = false;
  for (auto& existing : items_) {
    if (existing.id == cfg.id) {
      // The in-memory copy keeps the password for this session; the stored
      // copy never has one.
      existing = cfg;
      existing.password.clear();
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    ConnectionConfig stored = cfg;
    stored.password.clear();
    items_.append(std::move(stored));
  }

  persist();
  emit changed();
  return id;
}

void ConnectionStore::remove(const QString& id) {
  const std::string target = id.toStdString();
  for (int i = 0; i < items_.size(); ++i) {
    if (items_[i].id == target) {
      items_.remove(i);
      break;
    }
  }
  keychainErase(id);
  persist();
  emit changed();
}

ConnectionConfig ConnectionStore::byId(const QString& id) const {
  const std::string target = id.toStdString();
  for (const auto& c : items_) {
    if (c.id == target) return c;
  }
  return {};
}

bool ConnectionStore::loadPassword(ConnectionConfig* cfg) const {
  if (!cfg || cfg->id.empty()) return false;
  QString secret;
  if (!keychainFetch(QString::fromStdString(cfg->id), &secret)) return false;
  cfg->password = secret.toStdString();
  return true;
}

}  // namespace ds
