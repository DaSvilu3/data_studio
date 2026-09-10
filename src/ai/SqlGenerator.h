#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "core/Schema.h"
#include "core/Value.h"

namespace ds {

struct NlSqlResult {
  QString sql;
  QString explanation;
  QStringList tablesUsed;
  QString caveats;          // assumptions made about an ambiguous question
  bool destructive = false;
  QString providerLabel;    // e.g. "llama-3.2-3b-instruct (local)"
};

// Base for anything that turns a question into SQL. Implementations only ever
// send schema metadata -- names, types, keys, indexes -- never row data.
class SqlGenerator : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;
  ~SqlGenerator() override = default;

  virtual void ask(const QString& question, const Schema& schema,
                   Dialect dialect) = 0;
  virtual void cancel() = 0;
  virtual QString label() const = 0;

 signals:
  void succeeded(const ds::NlSqlResult& result);
  void failed(const QString& message);
};

// Pulls a result object out of whatever the model actually returned. Small
// local models routinely wrap JSON in markdown fences, add prose around it, or
// emit newlines inside string values -- all of which this recovers from.
// Returns false only when no SQL could be found at all.
bool parseModelReply(const QString& raw, NlSqlResult* out);

}  // namespace ds

Q_DECLARE_METATYPE(ds::NlSqlResult)
