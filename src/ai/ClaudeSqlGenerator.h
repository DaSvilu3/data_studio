#pragma once
#include "ai/AiSettings.h"
#include "ai/SqlGenerator.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace ds {

// Sends the schema to the Claude Messages API and asks for structured output.
// Only schema metadata is transmitted -- never row data.
class ClaudeSqlGenerator : public SqlGenerator {
  Q_OBJECT
 public:
  explicit ClaudeSqlGenerator(AiSettings settings, QObject* parent = nullptr);

  void ask(const QString& question, const Schema& schema,
           Dialect dialect) override;
  void cancel() override;
  QString label() const override;

  // Reads ANTHROPIC_API_KEY, falling back to the Keychain entry the settings
  // dialog writes. Empty when the feature isn't configured.
  static QString apiKey();
  static bool hasApiKey();
  static void setApiKey(const QString& key);

 private:
  void handleReply();

  AiSettings settings_;
  QNetworkAccessManager* network_ = nullptr;
  QNetworkReply* reply_ = nullptr;
};

}  // namespace ds
