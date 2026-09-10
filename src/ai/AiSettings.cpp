#include "ai/AiSettings.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>

namespace ds {
namespace {

QSettings settings() {
  return QSettings(QStringLiteral("DataStudio"), QStringLiteral("DataStudio"));
}

QString runtimeNameFor(const QString& baseUrl) {
  if (baseUrl.contains(QStringLiteral(":1234"))) return QStringLiteral("LM Studio");
  if (baseUrl.contains(QStringLiteral(":11434"))) return QStringLiteral("Ollama");
  if (baseUrl.contains(QStringLiteral(":8080"))) return QStringLiteral("llama.cpp");
  if (baseUrl.contains(QStringLiteral(":8000"))) return QStringLiteral("vLLM");
  return QStringLiteral("OpenAI-compatible");
}

}  // namespace

QStringList AiSettings::knownLocalEndpoints() {
  return {
      QStringLiteral("http://127.0.0.1:1234/v1"),   // LM Studio
      QStringLiteral("http://127.0.0.1:11434/v1"),  // Ollama
      QStringLiteral("http://127.0.0.1:8080/v1"),   // llama.cpp server
      QStringLiteral("http://127.0.0.1:8000/v1"),   // vLLM
  };
}

AiSettings AiSettings::load() {
  AiSettings s;
  QSettings st = settings();
  s.provider = static_cast<AiProvider>(
      st.value(QStringLiteral("ai/provider"),
               static_cast<int>(AiProvider::None)).toInt());
  s.localBaseUrl = st.value(QStringLiteral("ai/localBaseUrl"), s.localBaseUrl)
                       .toString();
  s.localModel = st.value(QStringLiteral("ai/localModel")).toString();
  s.localApiKey = st.value(QStringLiteral("ai/localApiKey")).toString();
  s.maxTablesInPrompt =
      st.value(QStringLiteral("ai/maxTables"), s.maxTablesInPrompt).toInt();
  s.claudeModel =
      st.value(QStringLiteral("ai/claudeModel"), s.claudeModel).toString();
  return s;
}

void AiSettings::save() const {
  QSettings st = settings();
  st.setValue(QStringLiteral("ai/provider"), static_cast<int>(provider));
  st.setValue(QStringLiteral("ai/localBaseUrl"), localBaseUrl);
  st.setValue(QStringLiteral("ai/localModel"), localModel);
  st.setValue(QStringLiteral("ai/localApiKey"), localApiKey);
  st.setValue(QStringLiteral("ai/maxTables"), maxTablesInPrompt);
  st.setValue(QStringLiteral("ai/claudeModel"), claudeModel);
  st.sync();
}

bool AiSettings::isConfigured() const {
  switch (provider) {
    case AiProvider::Local:
      return !localBaseUrl.isEmpty() && !localModel.isEmpty();
    case AiProvider::Claude:
      return true;   // the key is checked by the generator at request time
    case AiProvider::None:
      return false;
  }
  return false;
}

QString AiSettings::label() const {
  switch (provider) {
    case AiProvider::Local:
      return localModel.isEmpty()
                 ? QStringLiteral("local model (none chosen)")
                 : QStringLiteral("%1 · local").arg(localModel);
    case AiProvider::Claude:
      return claudeModel;
    case AiProvider::None:
      return QStringLiteral("not configured");
  }
  return {};
}

QList<LocalEndpoint> discoverLocalEndpoints() {
  QList<LocalEndpoint> found;
  QNetworkAccessManager network;

  for (const QString& base : AiSettings::knownLocalEndpoints()) {
    QNetworkRequest request{QUrl(base + QStringLiteral("/models"))};
    request.setTransferTimeout(1500);
    QNetworkReply* reply = network.get(request);

    // Blocking wait, bounded by the transfer timeout above.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray payload = reply->readAll();
    const bool ok = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    if (!ok) continue;

    LocalEndpoint endpoint;
    endpoint.baseUrl = base;
    endpoint.runtime = runtimeNameFor(base);
    for (const QJsonValue& v :
         QJsonDocument::fromJson(payload).object()
             .value(QStringLiteral("data")).toArray()) {
      const QString id = v.toObject().value(QStringLiteral("id")).toString();
      // Embedding models can't answer a question; don't offer them.
      if (id.isEmpty() || id.contains(QStringLiteral("embed"),
                                      Qt::CaseInsensitive)) {
        continue;
      }
      endpoint.models << id;
    }
    if (!endpoint.models.isEmpty()) found << endpoint;
  }
  return found;
}

}  // namespace ds
