#include "ai/SchemaPrompt.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace ds {
namespace {

// Splits an identifier or a question into lowercase word stems, so that
// "order_items", "OrderItems" and "orders" all share the stem "order".
QStringList wordsOf(const QString& text) {
  static const QRegularExpression kSplit(
      QStringLiteral("[^A-Za-z0-9]+|(?<=[a-z0-9])(?=[A-Z])"));
  QStringList out;
  for (const QString& raw : text.split(kSplit, Qt::SkipEmptyParts)) {
    QString w = raw.toLower();
    if (w.length() < 3) continue;
    // Crude singularisation -- enough to match "customers" against "customer".
    if (w.endsWith(QStringLiteral("ies")) && w.length() > 4) {
      w = w.left(w.length() - 3) + QLatin1Char('y');
    } else if (w.endsWith(QLatin1Char('s')) && !w.endsWith(QStringLiteral("ss"))) {
      w.chop(1);
    }
    out << w;
  }
  return out;
}

int relevanceScore(const Table& table, const QSet<QString>& questionWords) {
  if (questionWords.isEmpty()) return 0;
  int score = 0;

  for (const QString& w : wordsOf(QString::fromStdString(table.name))) {
    if (questionWords.contains(w)) score += 40;   // the table itself was named
  }
  for (const auto& c : table.columns) {
    for (const QString& w : wordsOf(QString::fromStdString(c.name))) {
      if (questionWords.contains(w)) score += 8;
    }
  }
  // A table nobody references and nobody named is the least likely to matter.
  score += static_cast<int>(table.foreignKeys.size());
  return score;
}

}  // namespace

QStringList selectRelevantTables(const Schema& schema, const QString& question,
                                 int maxTables) {
  QStringList all;
  for (const auto& t : schema.tables) all << QString::fromStdString(t.name);
  if (maxTables <= 0 || all.size() <= maxTables) return all;

  QSet<QString> questionWords;
  for (const QString& w : wordsOf(question)) questionWords.insert(w);

  std::vector<std::pair<int, QString>> ranked;
  ranked.reserve(schema.tables.size());
  for (const auto& t : schema.tables) {
    ranked.emplace_back(relevanceScore(t, questionWords),
                        QString::fromStdString(t.name));
  }
  // Stable by score, then by name, so the prompt is deterministic.
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const auto& a, const auto& b) {
                     if (a.first != b.first) return a.first > b.first;
                     return a.second < b.second;
                   });

  QStringList chosen;
  QSet<QString> taken;
  auto take = [&](const QString& name) {
    if (taken.contains(name) || chosen.size() >= maxTables) return;
    taken.insert(name);
    chosen << name;
  };

  // Seed with the best-scoring half, so there is room left for the neighbours
  // that make those tables joinable.
  const int seedCount = std::max(1, maxTables / 2);
  for (int i = 0; i < ranked.size() && chosen.size() < seedCount; ++i) {
    take(ranked[i].second);
  }

  // One hop out along foreign keys, in both directions.
  const QStringList seeds = chosen;
  for (const QString& name : seeds) {
    const Table* t = schema.findTable(name.toStdString());
    if (!t) continue;
    for (const auto& fk : t->foreignKeys) {
      take(QString::fromStdString(fk.toTable));
    }
    for (const auto* fk : schema.incomingForeignKeys(name.toStdString())) {
      take(QString::fromStdString(fk->fromTable));
    }
  }

  // Fill any remaining budget by score.
  for (const auto& [score, name] : ranked) {
    if (chosen.size() >= maxTables) break;
    take(name);
  }

  // Emit in schema order rather than score order -- easier for a model to read.
  QStringList ordered;
  for (const auto& t : schema.tables) {
    const QString name = QString::fromStdString(t.name);
    if (taken.contains(name)) ordered << name;
  }
  return ordered;
}

QString describeSchema(const Schema& schema, Dialect dialect,
                       const QString& question, int maxTables) {
  const QStringList keep = selectRelevantTables(schema, question, maxTables);
  QSet<QString> keepSet(keep.begin(), keep.end());

  QStringList lines;
  lines << QStringLiteral("-- %1 database \"%2\"")
               .arg(QString::fromLatin1(dialectName(dialect)),
                    QString::fromStdString(schema.name));

  for (const auto& t : schema.tables) {
    const QString name = QString::fromStdString(t.name);
    if (!keepSet.contains(name)) continue;

    QString header = QStringLiteral("CREATE %1 %2 (")
                         .arg(t.isView ? QStringLiteral("VIEW")
                                       : QStringLiteral("TABLE"),
                              name);
    if (t.estimatedRows >= 0) {
      header += QStringLiteral("  -- ~%1 rows").arg(t.estimatedRows);
    }
    lines << header;

    QStringList parts;
    for (const auto& c : t.columns) {
      QString line = QStringLiteral("  %1 %2")
                         .arg(QString::fromStdString(c.name),
                              QString::fromStdString(c.type));
      if (!c.nullable) line += QStringLiteral(" NOT NULL");
      if (c.primaryKey) line += QStringLiteral(" PRIMARY KEY");
      if (!c.comment.empty()) {
        line += QStringLiteral("  -- %1").arg(QString::fromStdString(c.comment));
      }
      parts << line;
    }
    for (const auto& fk : t.foreignKeys) {
      // Drop a relationship whose other end was pruned away.
      if (!keepSet.contains(QString::fromStdString(fk.toTable))) continue;
      QStringList from, to;
      for (const auto& c : fk.fromColumns) from << QString::fromStdString(c);
      for (const auto& c : fk.toColumns) to << QString::fromStdString(c);
      parts << QStringLiteral("  FOREIGN KEY (%1) REFERENCES %2(%3)")
                   .arg(from.join(QStringLiteral(", ")),
                        QString::fromStdString(fk.toTable),
                        to.join(QStringLiteral(", ")));
    }
    lines << parts.join(QStringLiteral(",\n")) << QStringLiteral(");");

    for (const auto& idx : t.indexes) {
      if (idx.primary) continue;
      QStringList cols;
      for (const auto& c : idx.columns) cols << QString::fromStdString(c);
      lines << QStringLiteral("CREATE %1INDEX %2 ON %3 (%4);")
                   .arg(idx.unique ? QStringLiteral("UNIQUE ") : QString(),
                        QString::fromStdString(idx.name), name,
                        cols.join(QStringLiteral(", ")));
    }
    lines << QString();
  }

  const int omitted = static_cast<int>(schema.tables.size()) - keep.size();
  if (omitted > 0) {
    lines << QStringLiteral("-- (%1 unrelated table(s) omitted to fit the "
                            "context window)")
                 .arg(omitted);
  }
  return lines.join(QLatin1Char('\n'));
}

}  // namespace ds
