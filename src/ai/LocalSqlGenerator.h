#pragma once
#include "ai/AiSettings.h"
#include "ai/SqlGenerator.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace ds {

// Talks to any OpenAI-compatible /chat/completions server -- LM Studio,
// Ollama, llama.cpp, vLLM, mlx_lm. Nothing leaves the machine.
class LocalSqlGenerator : public SqlGenerator {
  Q_OBJECT
 public:
  explicit LocalSqlGenerator(AiSettings settings, QObject* parent = nullptr);

  void ask(const QString& question, const Schema& schema,
           Dialect dialect) override;
  void cancel() override;
  QString label() const override;

 private:
  void handleReply();

  AiSettings settings_;
  QNetworkAccessManager* network_ = nullptr;
  QNetworkReply* reply_ = nullptr;
};

}  // namespace ds
