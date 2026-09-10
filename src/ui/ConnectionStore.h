#pragma once
#include <QObject>
#include <QVector>

#include "core/Driver.h"

namespace ds {

// Persists saved connections. Everything but the password goes in QSettings;
// passwords go to the macOS Keychain so nothing sensitive is written to a
// plain-text preferences file.
class ConnectionStore : public QObject {
  Q_OBJECT
 public:
  explicit ConnectionStore(QObject* parent = nullptr);

  const QVector<ConnectionConfig>& connections() const { return items_; }

  // Adds or replaces by id. Assigns an id when the config has none.
  QString save(ConnectionConfig cfg, bool savePassword);
  void remove(const QString& id);
  ConnectionConfig byId(const QString& id) const;

  // Fills in the password from the keychain. Returns false when nothing was
  // stored (the caller should prompt).
  bool loadPassword(ConnectionConfig* cfg) const;

  void load();
  void persist() const;

 signals:
  void changed();

 private:
  QVector<ConnectionConfig> items_;
};

// Keychain access. No-ops returning false on platforms without one.
bool keychainStore(const QString& account, const QString& secret);
bool keychainFetch(const QString& account, QString* secret);
void keychainErase(const QString& account);

}  // namespace ds
