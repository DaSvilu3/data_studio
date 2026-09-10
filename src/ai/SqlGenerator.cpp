#include "ai/SqlGenerator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ds {
namespace {

// Strips ```json / ``` fences and any prose either side of them.
QString stripFences(const QString& text) {
  static const QRegularExpression kFence(
      QStringLiteral("```[a-zA-Z]*\\s*(.*?)```"),
      QRegularExpression::DotMatchesEverythingOption);
  const auto match = kFence.match(text);
  return match.hasMatch() ? match.captured(1).trimmed() : text.trimmed();
}

// Returns the outermost {...} span, ignoring braces inside string literals.
QString extractJsonObject(const QString& text) {
  int start = text.indexOf(QLatin1Char('{'));
  if (start < 0) return {};

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (int i = start; i < text.length(); ++i) {
    const QChar c = text.at(i);
    if (inString) {
      if (escaped) escaped = false;
      else if (c == QLatin1Char('\\')) escaped = true;
      else if (c == QLatin1Char('"')) inString = false;
      continue;
    }
    if (c == QLatin1Char('"')) inString = true;
    else if (c == QLatin1Char('{')) ++depth;
    else if (c == QLatin1Char('}') && --depth == 0) {
      return text.mid(start, i - start + 1);
    }
  }
  return {};
}

// Small models often put real newlines and tabs inside JSON string values,
// which is invalid JSON. Escape control characters that appear inside strings
// so the document parses.
QString repairControlChars(const QString& json) {
  QString out;
  out.reserve(json.size());
  bool inString = false;
  bool escaped = false;

  for (const QChar c : json) {
    if (inString) {
      if (escaped) {
        escaped = false;
        out.append(c);
        continue;
      }
      if (c == QLatin1Char('\\')) {
        escaped = true;
        out.append(c);
        continue;
      }
      if (c == QLatin1Char('"')) {
        inString = false;
        out.append(c);
        continue;
      }
      if (c == QLatin1Char('\n')) { out.append(QStringLiteral("\\n")); continue; }
      if (c == QLatin1Char('\r')) { out.append(QStringLiteral("\\r")); continue; }
      if (c == QLatin1Char('\t')) { out.append(QStringLiteral("\\t")); continue; }
      out.append(c);
      continue;
    }
    if (c == QLatin1Char('"')) inString = true;
    out.append(c);
  }
  return out;
}

// Last resort: find a bare SQL statement in the reply.
QString findBareSql(const QString& text) {
  static const QRegularExpression kSqlFence(
      QStringLiteral("```sql\\s*(.*?)```"),
      QRegularExpression::DotMatchesEverythingOption |
          QRegularExpression::CaseInsensitiveOption);
  const auto fenced = kSqlFence.match(text);
  if (fenced.hasMatch()) return fenced.captured(1).trimmed();

  // A statement keyword only starts a statement, so anchor to the start of a
  // line. Without that, prose like "I can't help with that" matches on WITH.
  static const QRegularExpression kStatement(
      QStringLiteral("(?:^|\\n)\\s*((?:WITH|SELECT|INSERT|UPDATE|DELETE|CREATE|"
                     "ALTER|DROP|TRUNCATE|REPLACE)\\b.*?)(?:;|$)"),
      QRegularExpression::DotMatchesEverythingOption |
          QRegularExpression::CaseInsensitiveOption);
  const auto bare = kStatement.match(text);
  if (bare.hasMatch()) {
    const QString candidate = bare.captured(1).trimmed();
    // Even anchored, an English sentence can open with one of these words.
    // Require something that actually reads as SQL before believing it.
    static const QRegularExpression kSqlish(
        QStringLiteral("\\b(FROM|INTO|TABLE|VALUES|SET)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kSimpleSelect(
        QStringLiteral("^SELECT\\s+[\\w*(]"),
        QRegularExpression::CaseInsensitiveOption);
    if (kSqlish.match(candidate).hasMatch() ||
        kSimpleSelect.match(candidate).hasMatch()) {
      return candidate;
    }
  }
  return {};
}

bool looksDestructive(const QString& sql) {
  static const QRegularExpression kWrite(
      QStringLiteral("^\\s*(INSERT|UPDATE|DELETE|DROP|TRUNCATE|ALTER|CREATE|"
                     "REPLACE|GRANT|REVOKE)\\b"),
      QRegularExpression::CaseInsensitiveOption);
  return kWrite.match(sql).hasMatch();
}

void fillFromJson(const QJsonObject& obj, NlSqlResult* out) {
  out->sql = obj.value(QStringLiteral("sql")).toString().trimmed();
  out->explanation = obj.value(QStringLiteral("explanation")).toString().trimmed();
  out->caveats = obj.value(QStringLiteral("caveats")).toString().trimmed();
  out->destructive = obj.value(QStringLiteral("destructive")).toBool();
  out->tablesUsed.clear();
  for (const QJsonValue& v :
       obj.value(QStringLiteral("tables_used")).toArray()) {
    out->tablesUsed << v.toString();
  }
}

}  // namespace

bool parseModelReply(const QString& raw, NlSqlResult* out) {
  if (!out || raw.trimmed().isEmpty()) return false;

  // 1. Straight parse -- what a model honouring the schema returns.
  QJsonParseError error{};
  QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
  if (doc.isObject()) {
    fillFromJson(doc.object(), out);
    if (!out->sql.isEmpty()) return true;
  }

  // 2. Unwrap markdown fences and surrounding prose.
  const QString unfenced = stripFences(raw);
  const QString candidate = extractJsonObject(unfenced);
  if (!candidate.isEmpty()) {
    doc = QJsonDocument::fromJson(candidate.toUtf8(), &error);
    if (!doc.isObject()) {
      // 3. Repair raw newlines inside string values and retry.
      doc = QJsonDocument::fromJson(repairControlChars(candidate).toUtf8(),
                                    &error);
    }
    if (doc.isObject()) {
      fillFromJson(doc.object(), out);
      if (!out->sql.isEmpty()) {
        // A model that ignored the schema may also have skipped this flag.
        if (!out->destructive) out->destructive = looksDestructive(out->sql);
        return true;
      }
    }
  }

  // 4. No usable JSON -- take a bare SQL statement if there is one.
  const QString sql = findBareSql(unfenced);
  if (sql.isEmpty()) return false;

  out->sql = sql;
  if (out->explanation.isEmpty()) {
    out->explanation =
        QStringLiteral("The model didn't return structured output, so only the "
                       "SQL could be recovered — check it carefully.");
  }
  out->destructive = looksDestructive(sql);
  return true;
}

}  // namespace ds
