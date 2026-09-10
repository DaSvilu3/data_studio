#include "ui/ConnectionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <QFutureWatcher>

namespace ds {

ConnectionDialog::ConnectionDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Connection"));
  setMinimumWidth(460);

  name_ = new QLineEdit(this);
  name_->setPlaceholderText(QStringLiteral("My database"));

  dialect_ = new QComboBox(this);
  dialect_->addItem(QStringLiteral("SQLite"), static_cast<int>(Dialect::Sqlite));
  dialect_->addItem(QStringLiteral("MySQL"), static_cast<int>(Dialect::MySql));
#if !DS_HAVE_MYSQL
  // Built without libmysqlclient -- show the option but make it unusable
  // rather than pretending MySQL doesn't exist.
  auto* model = qobject_cast<QStandardItemModel*>(dialect_->model());
  Q_UNUSED(model);
  dialect_->setItemData(1, QStringLiteral("This build has no MySQL support."),
                        Qt::ToolTipRole);
#endif

  // ---- SQLite page ----
  auto* sqlitePage = new QWidget(this);
  filePath_ = new QLineEdit(sqlitePage);
  filePath_->setPlaceholderText(QStringLiteral("/path/to/database.db"));
  auto* browse = new QPushButton(QStringLiteral("Browse…"), sqlitePage);
  readOnly_ = new QCheckBox(QStringLiteral("Open read-only"), sqlitePage);

  auto* pathRow = new QHBoxLayout;
  pathRow->setContentsMargins(0, 0, 0, 0);
  pathRow->addWidget(filePath_, 1);
  pathRow->addWidget(browse, 0);

  auto* sqliteForm = new QFormLayout(sqlitePage);
  sqliteForm->setContentsMargins(0, 0, 0, 0);
  sqliteForm->addRow(QStringLiteral("File"), pathRow);
  sqliteForm->addRow(QString(), readOnly_);
  auto* hint = new QLabel(
      QStringLiteral("A file that doesn't exist yet will be created."),
      sqlitePage);
  hint->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
  sqliteForm->addRow(QString(), hint);

  // ---- MySQL page ----
  auto* mysqlPage = new QWidget(this);
  host_ = new QLineEdit(QStringLiteral("127.0.0.1"), mysqlPage);
  port_ = new QSpinBox(mysqlPage);
  port_->setRange(1, 65535);
  port_->setValue(3306);
  user_ = new QLineEdit(mysqlPage);
  password_ = new QLineEdit(mysqlPage);
  password_->setEchoMode(QLineEdit::Password);
  database_ = new QLineEdit(mysqlPage);
  database_->setPlaceholderText(QStringLiteral("optional — pick one later"));
  useSsl_ = new QCheckBox(QStringLiteral("Prefer TLS"), mysqlPage);
  useSsl_->setChecked(true);
  savePassword_ = new QCheckBox(
      QStringLiteral("Save password in the macOS Keychain"), mysqlPage);
  savePassword_->setChecked(true);

  auto* hostRow = new QHBoxLayout;
  hostRow->setContentsMargins(0, 0, 0, 0);
  hostRow->addWidget(host_, 1);
  hostRow->addWidget(new QLabel(QStringLiteral("Port"), mysqlPage), 0);
  hostRow->addWidget(port_, 0);

  auto* mysqlForm = new QFormLayout(mysqlPage);
  mysqlForm->setContentsMargins(0, 0, 0, 0);
  mysqlForm->addRow(QStringLiteral("Host"), hostRow);
  mysqlForm->addRow(QStringLiteral("User"), user_);
  mysqlForm->addRow(QStringLiteral("Password"), password_);
  mysqlForm->addRow(QStringLiteral("Database"), database_);
  mysqlForm->addRow(QString(), useSsl_);
  mysqlForm->addRow(QString(), savePassword_);

  pages_ = new QStackedWidget(this);
  pages_->addWidget(sqlitePage);
  pages_->addWidget(mysqlPage);

  testResult_ = new QLabel(this);
  testResult_->setWordWrap(true);

  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  testButton_ = buttons->addButton(QStringLiteral("Test"),
                                   QDialogButtonBox::ActionRole);

  auto* topForm = new QFormLayout;
  topForm->addRow(QStringLiteral("Name"), name_);
  topForm->addRow(QStringLiteral("Type"), dialect_);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(topForm);
  layout->addWidget(pages_);
  layout->addWidget(testResult_);
  layout->addStretch(1);
  layout->addWidget(buttons);

  connect(browse, &QPushButton::clicked, this,
          &ConnectionDialog::browseForFile);
  connect(testButton_, &QPushButton::clicked, this,
          &ConnectionDialog::testConnection);
  connect(dialect_, &QComboBox::currentIndexChanged, this,
          &ConnectionDialog::dialectChanged);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  dialectChanged(0);
}

void ConnectionDialog::dialectChanged(int index) {
  pages_->setCurrentIndex(index);
  // Suggest a sensible default name as the user switches type.
  if (name_->text().isEmpty()) {
    name_->setPlaceholderText(index == 0 ? QStringLiteral("My SQLite file")
                                         : QStringLiteral("My MySQL server"));
  }
  testResult_->clear();
}

void ConnectionDialog::browseForFile() {
  const QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("SQLite database"), filePath_->text(),
      QStringLiteral("SQLite databases (*.db *.sqlite *.sqlite3);;All files (*)"),
      nullptr, QFileDialog::DontConfirmOverwrite);
  if (!path.isEmpty()) {
    filePath_->setText(path);
    if (name_->text().isEmpty()) {
      name_->setText(QFileInfo(path).completeBaseName());
    }
  }
}

void ConnectionDialog::testConnection() {
  testButton_->setEnabled(false);
  testResult_->setText(QStringLiteral("Connecting…"));
  testResult_->setStyleSheet(QStringLiteral("color: gray;"));

  const ConnectionConfig cfg = config();

  // Connecting can block for the full TCP timeout, so it goes off-thread.
  auto* watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString error = watcher->result();
    testButton_->setEnabled(true);
    if (error.isEmpty()) {
      testResult_->setText(QStringLiteral("✓ Connected."));
      testResult_->setStyleSheet(QStringLiteral("color: #2d8a4e;"));
    } else {
      testResult_->setText(QStringLiteral("✗ ") + error);
      testResult_->setStyleSheet(QStringLiteral("color: #d05050;"));
    }
    watcher->deleteLater();
  });

  watcher->setFuture(QtConcurrent::run([cfg]() -> QString {
    try {
      auto driver = makeDriver(cfg);
      driver->connect(cfg);
      driver->disconnect();
      return {};
    } catch (const std::exception& e) {
      return QString::fromUtf8(e.what());
    }
  }));
}

void ConnectionDialog::setConfig(const ConnectionConfig& cfg) {
  id_ = cfg.id;
  name_->setText(QString::fromStdString(cfg.name));
  dialect_->setCurrentIndex(cfg.dialect == Dialect::MySql ? 1 : 0);
  filePath_->setText(QString::fromStdString(cfg.filePath));
  readOnly_->setChecked(cfg.readOnly);
  host_->setText(QString::fromStdString(cfg.host));
  port_->setValue(cfg.port);
  user_->setText(QString::fromStdString(cfg.user));
  password_->setText(QString::fromStdString(cfg.password));
  database_->setText(QString::fromStdString(cfg.database));
  useSsl_->setChecked(cfg.useSsl);
  savePassword_->setChecked(!cfg.password.empty());
}

ConnectionConfig ConnectionDialog::config() const {
  ConnectionConfig cfg;
  cfg.id = id_;
  cfg.name = name_->text().isEmpty()
                 ? name_->placeholderText().toStdString()
                 : name_->text().toStdString();
  cfg.dialect = dialect_->currentIndex() == 1 ? Dialect::MySql
                                              : Dialect::Sqlite;
  cfg.filePath = filePath_->text().toStdString();
  cfg.readOnly = readOnly_->isChecked();
  cfg.host = host_->text().toStdString();
  cfg.port = port_->value();
  cfg.user = user_->text().toStdString();
  cfg.password = password_->text().toStdString();
  cfg.database = database_->text().toStdString();
  cfg.useSsl = useSsl_->isChecked();
  return cfg;
}

bool ConnectionDialog::shouldSavePassword() const {
  return savePassword_->isChecked();
}

}  // namespace ds
