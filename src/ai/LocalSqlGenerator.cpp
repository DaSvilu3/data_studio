#include "ai/LocalSqlGenerator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "ai/SchemaPrompt.h"

namespace ds {
namespace {

// Small models follow short, concrete rules far better than prose. This is
// deliberately terser and more directive than the prompt sent to Claude.
QString systemPrompt(Dialect dialect) {
  const QString name = QString::fromLatin1(dialectName(dialect));
  return QStringLiteral(
             "You write %1 SQL from a question about a database.\n"
             "\n"
             "Rules:\n"
             "1. Use ONLY the tables and columns in the schema. Never invent a "
             "name.\n"
             "2. Join tables using the FOREIGN KEY lines in the schema.\n"
             "3. Add LIMIT 200 unless the question asks for a count or a "
             "total.\n"
             "4. List columns explicitly. Do not write SELECT *.\n"
             "5. Set \"destructive\" to true for INSERT, UPDATE, DELETE, DROP, "
             "TRUNCATE or ALTER.\n"
             "6. If the schema cannot answer the question, set \"sql\" to \"\" "
             "and say why in \"explanation\".\n"
             "\n"
             "Reply with one JSON object and nothing else. No markdown, no "
             "code fences, no text before or after:\n"
             "{\"sql\": \"...\", \"explanation\": \"...\", "
             "\"tables_used\": [\"...\"], \"caveats\": \"\", "
             "\"destructive\": false}")
      .arg(name);
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
  schema["additionalProperties"] = false;
  return schema;
}

}  // namespace

LocalSqlGenerator::LocalSqlGenerator(AiSettings settings, QObject* parent)
    : SqlGenerator(parent), settings_(std::move(settings)) {
  network_ = new QNetworkAccessManager(this);
}

QString LocalSqlGenerator::label() const {
  return settings_.localModel + QStringLiteral(" · local");
}

void LocalSqlGenerator::ask(const QString& question, const Schema& schema,
                            Dialect dialect) {
  cancel();

  if (settings_.localBaseUrl.isEmpty() || settings_.localModel.isEmpty()) {
    emit failed(QStringLiteral(
        "No local model chosen. Configure one under Query ▸ AI settings…"));
    return;
  }
  if (schema.tables.empty()) {
    emit failed(QStringLiteral("The schema is empty — connect to a database "
                               "first."));
    return;
  }

  QJsonObject system;
  system["role"] = "system";
  system["content"] = systemPrompt(dialect);

  QJsonObject user;
  user["role"] = "user";
  user["content"] =
      QStringLiteral("Schema:\n\n%1\n\nQuestion: %2")
          .arg(describeSchema(schema, dialect, question,
                              settings_.maxTablesInPrompt),
               question);

  QJsonObject format;
  format["name"] = "sql_answer";
  format["strict"] = true;
  format["schema"] = responseSchema();

  QJsonObject responseFormat;
  responseFormat["type"] = "json_schema";
  responseFormat["json_schema"] = format;

  QJsonObject body;
  body["model"] = settings_.localModel;
  body["messages"] = QJsonArray{system, user};
  // Near-greedy: SQL generation wants the most likely token, not variety.
  body["temperature"] = 0.1;
  body["max_tokens"] = 1200;
  body["stream"] = false;
  // Servers that don't understand this ignore it; the reply parser copes
  // either way.
  body["response_format"] = responseFormat;

  QNetworkRequest request{
      QUrl(settings_.localBaseUrl + QStringLiteral("/chat/completions"))};
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  if (!settings_.localApiKey.isEmpty()) {
    request.setRawHeader("Authorization",
                         "Bearer " + settings_.localApiKey.toUtf8());
  }
  // A cold model has to be loaded into memory before the first token, which on
  // a 7B can take a while.
  request.setTransferTimeout(180000);

  reply_ = network_->post(request,
                          QJsonDocument(body).toJson(QJsonDocument::Compact));
  connect(reply_, &QNetworkReply::finished, this,
          &LocalSqlGenerator::handleReply);
}

void LocalSqlGenerator::cancel() {
  if (!reply_) return;
  QNetworkReply* dying = reply_;
  reply_ = nullptr;
  disconnect(dying, nullptr, this, nullptr);
  dying->abort();
  dying->deleteLater();
}

void LocalSqlGenerator::handleReply() {
  QNetworkReply* reply = reply_;
  if (!reply) return;
  reply_ = nullptr;
  reply->deleteLater();

  const QByteArray payload = reply->readAll();
  const QJsonObject root = QJsonDocument::fromJson(payload).object();

  if (reply->error() != QNetworkReply::NoError) {
    QString detail = root.value(QStringLiteral("error"))
                         .toObject()
                         .value(QStringLiteral("message"))
                         .toString();
    if (detail.isEmpty()) {
      detail = root.value(QStringLiteral("error")).toString();
    }
    if (detail.isEmpty()) detail = reply->errorString();

    if (reply->error() == QNetworkReply::ConnectionRefusedError ||
        reply->error() == QNetworkReply::HostNotFoundError) {
      detail = QStringLiteral("Can't reach %1 — is the local server running?")
                   .arg(settings_.localBaseUrl);
    }
    emit failed(detail);
    return;
  }

  const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
  if (choices.isEmpty()) {
    emit failed(QStringLiteral("The model returned no choices."));
    return;
  }

  const QJsonObject choice = choices.first().toObject();
  const QString text = choice.value(QStringLiteral("message"))
                           .toObject()
                           .value(QStringLiteral("content"))
                           .toString();

  NlSqlResult result;
  result.providerLabel = label();
  if (!parseModelReply(text, &result)) {
    const QString reason =
        choice.value(QStringLiteral("finish_reason")).toString();
    if (reason == QStringLiteral("length")) {
      emit failed(QStringLiteral(
          "The model ran out of output tokens before finishing. Try a narrower "
          "question, or a model with more room."));
    } else {
      emit failed(QStringLiteral("Couldn't find SQL in the model's reply:\n\n") +
                  text.left(400));
    }
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
