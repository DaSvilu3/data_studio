#include "ai/ClaudeSqlGenerator.h"

#include "ai/SchemaPrompt.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>

#include "ui/ConnectionStore.h"

namespace ds {
namespace {

constexpr const char* kEndpoint = "https://api.anthropic.com/v1/messages";
constexpr const char* kApiVersion = "2023-06-01";
constexpr const char* kKeychainAccount = "anthropic-api-key";


QString systemPrompt(Dialect dialect) {
  return QStringLiteral(
             "You translate questions into %1 SQL for a database tool.\n\n"
             "Rules:\n"
             "- Use only the tables and columns given in the schema. Never "
             "invent names.\n"
             "- Join through the declared foreign keys shown in the schema.\n"
             "- Write %1 dialect specifically.\n"
             "- Add a LIMIT (default 200) to any query that could return many "
             "rows, unless the question asks for an aggregate or an exact "
             "count.\n"
             "- Prefer explicit column lists over SELECT *.\n"
             "- If the question is ambiguous, choose the most reasonable "
             "reading and say what you assumed in `caveats`.\n"
             "- If the question cannot be answered from this schema, return an "
             "empty `sql` and explain why in `explanation`.\n"
             "- Set `destructive` to true for anything that writes: INSERT, "
             "UPDATE, DELETE, DROP, TRUNCATE, ALTER.")
      .arg(QString::fromLatin1(dialectName(dialect)));
}

QJsonObject responseSchema() {
  QJsonObject str;
  str["type"] = "string";
  QJsonObject boolean;
  boolean["type"] = "boolean";
  QJsonObject strArray;
  strArray["type"] = "array";
  strArray["items"] = str;

  QJsonObject props;
  props["sql"] = str;
  props["explanation"] = str;
  props["tables_used"] = strArray;
  props["caveats"] = str;
  props["destructive"] = boolean;

  QJsonObject schema;
  schema["type"] = "object";
  schema["properties"] = props;
  schema["required"] = QJsonArray{"sql", "explanation", "tables_used",
                                  "caveats", "destructive"};
  // Structured outputs requires this to be exactly false.
  schema["additionalProperties"] = false;
  return schema;
}

}  // namespace

ClaudeSqlGenerator::ClaudeSqlGenerator(AiSettings settings, QObject* parent)
    : SqlGenerator(parent), settings_(std::move(settings)) {
  network_ = new QNetworkAccessManager(this);
}

QString ClaudeSqlGenerator::label() const { return settings_.claudeModel; }

QString ClaudeSqlGenerator::apiKey() {
  const QString fromEnv =
      QProcessEnvironment::systemEnvironment().value(
          QStringLiteral("ANTHROPIC_API_KEY"));
  if (!fromEnv.isEmpty()) return fromEnv;

  QString stored;
  if (keychainFetch(QString::fromLatin1(kKeychainAccount), &stored)) {
    return stored;
  }
  return {};
}

bool ClaudeSqlGenerator::hasApiKey() { return !apiKey().isEmpty(); }

void ClaudeSqlGenerator::setApiKey(const QString& key) {
  if (key.isEmpty()) {
    keychainErase(QString::fromLatin1(kKeychainAccount));
  } else {
    keychainStore(QString::fromLatin1(kKeychainAccount), key);
  }
}

void ClaudeSqlGenerator::ask(const QString& question, const Schema& schema,
                             Dialect dialect) {
  cancel();

  const QString key = apiKey();
  if (key.isEmpty()) {
    emit failed(QStringLiteral(
        "No API key. Set ANTHROPIC_API_KEY, or add one under "
        "Query ▸ Set API key…"));
    return;
  }
  if (schema.tables.empty()) {
    emit failed(QStringLiteral(
        "The schema is empty — connect to a database first."));
    return;
  }

  QJsonObject userMessage;
  userMessage["role"] = "user";
  userMessage["content"] =
      QStringLiteral("Schema:\n\n%1\n\nQuestion: %2")
          .arg(describeSchema(schema, dialect, question,
                              settings_.maxTablesInPrompt),
               question);

  QJsonObject format;
  format["type"] = "json_schema";
  format["schema"] = responseSchema();

  QJsonObject outputConfig;
  outputConfig["format"] = format;

  QJsonObject body;
  body["model"] = settings_.claudeModel;
  body["max_tokens"] = 8000;
  body["system"] = systemPrompt(dialect);
  body["messages"] = QJsonArray{userMessage};
  body["output_config"] = outputConfig;

  QNetworkRequest request{QUrl(QString::fromLatin1(kEndpoint))};
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  request.setRawHeader("x-api-key", key.toUtf8());
  request.setRawHeader("anthropic-version", kApiVersion);

  reply_ = network_->post(request, QJsonDocument(body).toJson(
                                       QJsonDocument::Compact));
  connect(reply_, &QNetworkReply::finished, this, &ClaudeSqlGenerator::handleReply);
}

void ClaudeSqlGenerator::cancel() {
  if (!reply_) return;
  QNetworkReply* dying = reply_;
  reply_ = nullptr;
  disconnect(dying, nullptr, this, nullptr);
  dying->abort();
  dying->deleteLater();
}

void ClaudeSqlGenerator::handleReply() {
  QNetworkReply* reply = reply_;
  if (!reply) return;
  reply_ = nullptr;
  reply->deleteLater();

  const QByteArray payload = reply->readAll();
  const QJsonDocument doc = QJsonDocument::fromJson(payload);

  if (reply->error() != QNetworkReply::NoError) {
    // The API puts a useful message in the body even on a 4xx.
    QString detail = doc.object()
                         .value(QStringLiteral("error"))
                         .toObject()
                         .value(QStringLiteral("message"))
                         .toString();
    if (detail.isEmpty()) detail = reply->errorString();
    emit failed(detail);
    return;
  }

  const QJsonObject root = doc.object();

  // A policy decline comes back as HTTP 200 with no usable content.
  if (root.value(QStringLiteral("stop_reason")).toString() ==
      QStringLiteral("refusal")) {
    emit failed(QStringLiteral(
        "The model declined this request. Try rephrasing the question."));
    return;
  }
  if (root.value(QStringLiteral("stop_reason")).toString() ==
      QStringLiteral("max_tokens")) {
    emit failed(QStringLiteral(
        "The answer was cut off before it finished. Try a narrower question."));
    return;
  }

  // Thinking is on by default, so the text block is not necessarily first.
  QString text;
  for (const QJsonValue& block : root.value(QStringLiteral("content")).toArray()) {
    const QJsonObject obj = block.toObject();
    if (obj.value(QStringLiteral("type")).toString() ==
        QStringLiteral("text")) {
      text += obj.value(QStringLiteral("text")).toString();
    }
  }
  if (text.isEmpty()) {
    emit failed(QStringLiteral("The model returned an empty response."));
    return;
  }

  NlSqlResult result;
  result.providerLabel = label();
  if (!parseModelReply(text, &result)) {
    emit failed(QStringLiteral("Could not parse the model's response."));
    return;
  }
  if (result.sql.isEmpty()) {
    emit failed(result.explanation.isEmpty()
                    ? QStringLiteral("No query could be written for that.")
                    : result.explanation);
    return;
  }
  emit succeeded(result);
}

}  // namespace ds
