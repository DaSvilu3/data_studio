#pragma once
#include <QString>

#include "core/Schema.h"
#include "core/Value.h"

namespace ds {

// Renders the schema as compact DDL for a model prompt. Only names, types,
// keys and indexes -- never row data.
//
// `question` and `maxTables` exist for small local models: an 8k-context 3B
// model cannot be handed three hundred tables, so when the schema exceeds
// `maxTables` the tables are scored for relevance against the question and the
// rest are dropped. Foreign-key neighbours of the chosen tables are kept too,
// otherwise the model is asked to join through relationships it cannot see.
// Pass maxTables <= 0 to disable pruning entirely.
QString describeSchema(const Schema& schema, Dialect dialect,
                       const QString& question = {}, int maxTables = 0);

// The tables `describeSchema` would keep, in the order it would emit them.
// Exposed for the settings dialog's preview and for tests.
QStringList selectRelevantTables(const Schema& schema, const QString& question,
                                 int maxTables);

}  // namespace ds
