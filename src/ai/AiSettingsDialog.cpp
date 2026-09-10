#include "ai/AiSettingsDialog.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "ai/ClaudeSqlGenerator.h"
#include "ai/LocalSqlGenerator.h"

namespace ds {

AiSettingsDialog::AiSettingsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Natural-language SQL"));
  setMinimumWidth(560);
  loaded_ = AiSettings::load();

  offRadio_ = new QRadioButton(QStringLiteral("Off"), this);
  localRadio_ = new QRadioButton(
      QStringLiteral("Local model  —  nothing leaves this machine"), this);
  claudeRadio_ = new QRadioButton(
      QStringLiteral("Claude API  —  schema is sent to Anthropic"), this);

  auto* group = new QButtonGroup(this);
  group->addButton(offRadio_);
  group->addButton(localRadio_);
  group->addButton(claudeRadio_);

  // ---- local page ----
  auto* localPage = new QWidget(this);
  baseUrl_ = new QComboBox(localPage);
  baseUrl_->setEditable(true);
  baseUrl_->addItems(AiSettings::knownLocalEndpoints());
  baseUrl_->setCurrentText(loaded_.localBaseUrl);

  model_ = new QComboBox(localPage);
  model_->setEditable(true);
  if (!loaded_.localModel.isEmpty()) model_->addItem(loaded_.localModel);

  detectButton_ = new QPushButton(QStringLiteral("Detect"), localPage);
  detectButton_->setToolTip(
      QStringLiteral("Probe the usual ports for LM Studio, Ollama, llama.cpp "
                     "and vLLM."));
  testButton_ = new QPushButton(QStringLiteral("Test"), localPage);

  localKey_ = new QLineEdit(loaded_.localApiKey, localPage);
  localKey_->setEchoMode(QLineEdit::Password);
  localKey_->setPlaceholderText(
      QStringLiteral("usually blank — only vLLM-style servers need one"));

  maxTables_ = new QSpinBox(localPage);
  maxTables_->setRange(0, 500);
  maxTables_->setValue(loaded_.maxTablesInPrompt);
  maxTables_->setSpecialValueText(QStringLiteral("send every table"));
  maxTables_->setToolTip(QStringLiteral(
      "A small model has a small context window. Above this many tables, only "
      "the ones relevant to the question (and their foreign-key neighbours) "
      "are sent."));

  localStatus_ = new QLabel(localPage);
  localStatus_->setWordWrap(true);

  auto* urlRow = new QHBoxLayout;
  urlRow->setContentsMargins(0, 0, 0, 0);
  urlRow->addWidget(baseUrl_, 1);
  urlRow->addWidget(detectButton_, 0);

  auto* modelRow = new QHBoxLayout;
  modelRow->setContentsMargins(0, 0, 0, 0);
  modelRow->addWidget(model_, 1);
  modelRow->addWidget(testButton_, 0);

  auto* localForm = new QFormLayout(localPage);
  localForm->setContentsMargins(0, 0, 0, 0);
  localForm->addRow(QStringLiteral("Server"), urlRow);
  localForm->addRow(QStringLiteral("Model"), modelRow);
  localForm->addRow(QStringLiteral("API key"), localKey_);
  localForm->addRow(QStringLiteral("Schema limit"), maxTables_);
  localForm->addRow(QString(), localStatus_);

  auto* hint = new QLabel(
      QStringLiteral("Works with any OpenAI-compatible server. A 3B "
                     "instruction model handles everyday questions; a 7B "
                     "coding model does better on multi-table joins."),
      localPage);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
  localForm->addRow(QString(), hint);

  // ---- Claude page ----
  auto* claudePage = new QWidget(this);
  claudeKey_ = new QLineEdit(claudePage);
  claudeKey_->setEchoMode(QLineEdit::Password);
  claudeKey_->setPlaceholderText(
      ClaudeSqlGenerator::hasApiKey()
          ? QStringLiteral("a key is already stored — type to replace it")
          : QStringLiteral("sk-ant-…"));

  claudeModel_ = new QComboBox(claudePage);
  claudeModel_->setEditable(true);
  claudeModel_->addItems({QStringLiteral("claude-opus-5"),
                          QStringLiteral("claude-sonnet-5"),
                          QStringLiteral("claude-haiku-4-5")});
  claudeModel_->setCurrentText(loaded_.claudeModel);

  claudeStatus_ = new QLabel(claudePage);
  claudeStatus_->setWordWrap(true);
  if (ClaudeSqlGenerator::hasApiKey()) {
    claudeStatus_->setText(QStringLiteral("✓ A key is configured."));
    claudeStatus_->setStyleSheet(QStringLiteral("color: #2d8a4e;"));
  }

  auto* claudeForm = new QFormLayout(claudePage);
  claudeForm->setContentsMargins(0, 0, 0, 0);
  claudeForm->addRow(QStringLiteral("API key"), claudeKey_);
  claudeForm->addRow(QStringLiteral("Model"), claudeModel_);
  claudeForm->addRow(QString(), claudeStatus_);
  auto* claudeHint = new QLabel(
      QStringLiteral("The key is stored in the macOS Keychain. "
                     "ANTHROPIC_API_KEY in the environment takes precedence."),
      claudePage);
  claudeHint->setWordWrap(true);
  claudeHint->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
  claudeForm->addRow(QString(), claudeHint);

  pages_ = new QStackedWidget(this);
  pages_->addWidget(new QWidget(this));   // Off
  pages_->addWidget(localPage);
  pages_->addWidget(claudePage);

  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

  auto* providerBox = new QGroupBox(QStringLiteral("Provider"), this);
  auto* providerLayout = new QVBoxLayout(providerBox);
  providerLayout->addWidget(offRadio_);
  providerLayout->addWidget(localRadio_);
  providerLayout->addWidget(claudeRadio_);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(providerBox);
  layout->addWidget(pages_, 1);
  layout->addWidget(buttons);

  connect(detectButton_, &QPushButton::clicked, this,
          &AiSettingsDialog::detectLocalServers);
  connect(testButton_, &QPushButton::clicked, this,
          &AiSettingsDialog::testLocalModel);
  connect(offRadio_, &QRadioButton::toggled, this,
          &AiSettingsDialog::providerChanged);
  connect(localRadio_, &QRadioButton::toggled, this,
          &AiSettingsDialog::providerChanged);
  connect(claudeRadio_, &QRadioButton::toggled, this,
          &AiSettingsDialog::providerChanged);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  switch (loaded_.provider) {
    case AiProvider::Local:  localRadio_->setChecked(true); break;
    case AiProvider::Claude: claudeRadio_->setChecked(true); break;
    case AiProvider::None:   offRadio_->setChecked(true); break;
  }
  providerChanged();

  // Nothing configured yet -- look for a local server straight away, since
  // that's the case where detection is most useful.
  if (loaded_.provider == AiProvider::None) detectLocalServers();
}

void AiSettingsDialog::providerChanged() {
  if (localRadio_->isChecked()) pages_->setCurrentIndex(1);
  else if (claudeRadio_->isChecked()) pages_->setCurrentIndex(2);
  else pages_->setCurrentIndex(0);
}

void AiSettingsDialog::detectLocalServers() {
  detectButton_->setEnabled(false);
  localStatus_->setText(QStringLiteral("Looking for local servers…"));
  localStatus_->setStyleSheet(QStringLiteral("color: gray;"));

  auto* watcher = new QFutureWatcher<QList<LocalEndpoint>>(this);
  connect(watcher, &QFutureWatcher<QList<LocalEndpoint>>::finished, this,
          [this, watcher] {
            const auto found = watcher->result();
            watcher->deleteLater();
            detectButton_->setEnabled(true);

            if (found.isEmpty()) {
              localStatus_->setText(QStringLiteral(
                  "No local server answered. Start LM Studio's server, or run "
                  "`ollama serve`, then press Detect again."));
              localStatus_->setStyleSheet(QStringLiteral("color: #c98a20;"));
              return;
            }

            const LocalEndpoint& best = found.first();
            baseUrl_->setCurrentText(best.baseUrl);

            const QString previous = model_->currentText();
            model_->clear();
            model_->addItems(best.models);
            if (best.models.contains(previous)) {
              model_->setCurrentText(previous);
            }

            QStringList summary;
            for (const auto& e : found) {
              summary << QStringLiteral("%1 (%2 model%3)")
                             .arg(e.runtime)
                             .arg(e.models.size())
                             .arg(e.models.size() == 1 ? "" : "s");
            }
            localStatus_->setText(QStringLiteral("✓ Found ") +
                                  summary.join(QStringLiteral(", ")));
            localStatus_->setStyleSheet(QStringLiteral("color: #2d8a4e;"));
            if (!localRadio_->isChecked() && !claudeRadio_->isChecked()) {
              localRadio_->setChecked(true);
            }
          });
  watcher->setFuture(QtConcurrent::run(&discoverLocalEndpoints));
}

void AiSettingsDialog::testLocalModel() {
  const QString base = baseUrl_->currentText().trimmed();
  const QString model = model_->currentText().trimmed();
  if (base.isEmpty() || model.isEmpty()) {
    localStatus_->setText(QStringLiteral("Pick a server and a model first."));
    localStatus_->setStyleSheet(QStringLiteral("color: #c98a20;"));
    return;
  }

  testButton_->setEnabled(false);
  localStatus_->setText(
      QStringLiteral("Asking %1 to write one query… (a cold model has to load "
                     "first)").arg(model));
  localStatus_->setStyleSheet(QStringLiteral("color: gray;"));

  // A real end-to-end round trip against a tiny throwaway schema: this proves
  // the server answers, the model exists, and the reply can be parsed.
  auto* generator = new LocalSqlGenerator(settings(), this);
  connect(generator, &SqlGenerator::succeeded, this,
          [this, generator](const NlSqlResult& result) {
            testButton_->setEnabled(true);
            localStatus_->setText(QStringLiteral("✓ Answered: %1")
                                      .arg(result.sql.simplified().left(120)));
            localStatus_->setStyleSheet(QStringLiteral("color: #2d8a4e;"));
            generator->deleteLater();
          });
  connect(generator, &SqlGenerator::failed, this,
          [this, generator](const QString& message) {
            testButton_->setEnabled(true);
            localStatus_->setText(QStringLiteral("✗ ") + message.left(300));
            localStatus_->setStyleSheet(QStringLiteral("color: #d05050;"));
            generator->deleteLater();
          });

  Schema probe;
  probe.name = "probe";
  Table t;
  t.name = "widgets";
  Column id;
  id.name = "id";
  id.type = "INTEGER";
  id.primaryKey = true;
  id.kind = Column::Kind::Integer;
  Column colour;
  colour.name = "colour";
  colour.type = "TEXT";
  colour.kind = Column::Kind::Text;
  t.columns = {id, colour};
  probe.tables = {t};

  generator->ask(QStringLiteral("how many widgets are red?"), probe,
                 Dialect::Sqlite);
}

AiSettings AiSettingsDialog::settings() const {
  AiSettings s = loaded_;
  if (localRadio_->isChecked()) s.provider = AiProvider::Local;
  else if (claudeRadio_->isChecked()) s.provider = AiProvider::Claude;
  else s.provider = AiProvider::None;

  s.localBaseUrl = baseUrl_->currentText().trimmed();
  s.localModel = model_->currentText().trimmed();
  s.localApiKey = localKey_->text();
  s.maxTablesInPrompt = maxTables_->value();
  s.claudeModel = claudeModel_->currentText().trimmed();
  return s;
}

void AiSettingsDialog::applyAndSave() const {
  settings().save();
  // Only overwrite the stored key when the user actually typed one.
  const QString typed = claudeKey_->text().trimmed();
  if (!typed.isEmpty()) ClaudeSqlGenerator::setApiKey(typed);
}

}  // namespace ds
