#pragma once
#include <QString>
#include <QStringList>

namespace ds {

enum class AiProvider { None, Local, Claude };

// Where natural-language SQL generation is sent, persisted in QSettings.
// The Claude API key lives in the Keychain, never here.
struct AiSettings {
  AiProvider provider = AiProvider::None;

  // Any OpenAI-compatible chat-completions server: LM Studio, Ollama,
  // llama.cpp, vLLM, mlx_lm.
  QString localBaseUrl = QStringLiteral("http://127.0.0.1:1234/v1");
  QString localModel;
  QString localApiKey;      // most local servers ignore it; vLLM may not

  // Small models can't be handed a large schema. 0 disables pruning.
  int maxTablesInPrompt = 25;

  QString claudeModel = QStringLiteral("claude-opus-5");

  static AiSettings load();
  void save() const;

  bool isConfigured() const;
  QString label() const;

  // Base URLs worth probing, in preference order.
  static QStringList knownLocalEndpoints();
};

// One reachable local server: its base URL and the models it advertises.
struct LocalEndpoint {
  QString baseUrl;
  QString runtime;        // "LM Studio", "Ollama", … best-effort
  QStringList models;
};

// Synchronously probes the usual local ports. Fast: short timeouts, and every
// endpoint is a plain GET. Call it off the GUI thread.
QList<LocalEndpoint> discoverLocalEndpoints();

}  // namespace ds
