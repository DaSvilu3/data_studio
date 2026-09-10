#pragma once
#include <QWidget>

#include "ai/AiSettings.h"
#include "ai/SqlGenerator.h"
#include "core/Schema.h"
#include "core/Value.h"

class QLineEdit;
class QPushButton;
class QLabel;

namespace ds {

// One-line "ask in English" bar above the editor. Generated SQL is always
// shown for review rather than run automatically.
class NlPromptBar : public QWidget {
  Q_OBJECT
 public:
  explicit NlPromptBar(QWidget* parent = nullptr);

  void setSchema(const Schema& schema) { schema_ = schema; }
  void setDialect(Dialect d) { dialect_ = d; }
  void focusInput();

  // Re-reads the provider configuration and rebuilds the generator.
  void reloadSettings();

 signals:
  void sqlGenerated(const QString& sql, const QString& explanation);
  void statusMessage(const QString& text);
  void configureRequested();

 private slots:
  void submit();

 private:
  void setBusy(bool busy);

  QLineEdit* input_ = nullptr;
  QPushButton* askButton_ = nullptr;
  QPushButton* providerButton_ = nullptr;
  QLabel* status_ = nullptr;
  SqlGenerator* generator_ = nullptr;
  AiSettings settings_;
  Schema schema_;
  Dialect dialect_ = Dialect::Sqlite;
};

}  // namespace ds
