#include "ui/MainWindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "ai/AiSettingsDialog.h"
#include "ui/AnalyzerPanel.h"
#include "ui/ConnectionDialog.h"
#include "ui/ConnectionStore.h"
#include "ui/NlPromptBar.h"
#include "ui/QueryBuilderPanel.h"
#include "ui/QueryExecutor.h"
#include "ui/ResultsView.h"
#include "ui/SchemaTree.h"
#include "ui/SqlEditor.h"

namespace ds {
namespace {
constexpr int kIdRole = Qt::UserRole + 1;
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Data Studio"));
  resize(1440, 900);

  store_ = new ConnectionStore(this);
  buildUi();
  buildMenus();
  rebuildConnectionList();
  setBusy(false);

  QSettings settings(QStringLiteral("DataStudio"), QStringLiteral("DataStudio"));
  restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
  restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
  editor_->setPlainText(
      settings.value(QStringLiteral("editor/text")).toString());
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
  QSettings settings(QStringLiteral("DataStudio"), QStringLiteral("DataStudio"));
  settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("window/state"), saveState());
  settings.setValue(QStringLiteral("editor/text"), editor_->toPlainText());

  // Tear the session down before the widgets it signals into.
  delete session_;
  session_ = nullptr;
  event->accept();
}

void MainWindow::buildUi() {
  // ---- left rail: connections + schema ----
  connectionList_ = new QListWidget(this);
  connectionList_->setMaximumHeight(150);
  connect(connectionList_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem*) { connectToSelected(); });

  databasePicker_ = new QComboBox(this);
  databasePicker_->setEnabled(false);
  connect(databasePicker_, &QComboBox::activated, this, [this](int index) {
    if (session_ && index >= 0) {
      session_->useDatabase(databasePicker_->itemText(index));
    }
  });

  schemaTree_ = new SchemaTree(this);
  connect(schemaTree_, &SchemaTree::insertText, this, [this](const QString& t) {
    editor_->insertPlainText(t);
    editor_->setFocus();
  });
  connect(schemaTree_, &SchemaTree::previewTableRequested, this,
          &MainWindow::previewTable);
  connect(schemaTree_, &SchemaTree::refreshRequested, this,
          &MainWindow::refreshSchema);

  auto* left = new QWidget(this);
  auto* leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(4, 4, 4, 4);
  leftLayout->setSpacing(4);
  leftLayout->addWidget(new QLabel(QStringLiteral("Connections"), left));
  leftLayout->addWidget(connectionList_);
  leftLayout->addWidget(databasePicker_);
  leftLayout->addWidget(schemaTree_, 1);
  left->setMinimumWidth(260);

  // ---- editor workspace ----
  nlBar_ = new NlPromptBar(this);
  editor_ = new SqlEditor(this);

  connect(nlBar_, &NlPromptBar::sqlGenerated, this,
          [this](const QString& sql, const QString& explanation) {
            // Generated SQL is inserted for review, never run outright.
            QString comment;
            if (!explanation.isEmpty()) {
              comment = QStringLiteral("-- ") +
                        QString(explanation).replace(QLatin1Char('\n'),
                                                     QStringLiteral("\n-- ")) +
                        QLatin1Char('\n');
            }
            editor_->setPlainText(comment + sql);
            editor_->setFocus();
            analyzer_->analyze(editor_->currentStatement());
          });
  connect(nlBar_, &NlPromptBar::configureRequested, this,
          &MainWindow::setApiKey);
  connect(nlBar_, &NlPromptBar::statusMessage, this,
          [this](const QString& text) { statusBar()->showMessage(text, 12000); });

  connect(editor_, &SqlEditor::runRequested, this,
          &MainWindow::runCurrentStatement);
  connect(editor_, &SqlEditor::textSettled, this, [this] {
    analyzer_->analyze(editor_->currentStatement());
  });

  auto* editorPane = new QWidget(this);
  auto* editorLayout = new QVBoxLayout(editorPane);
  editorLayout->setContentsMargins(0, 0, 0, 0);
  editorLayout->setSpacing(0);
  editorLayout->addWidget(nlBar_);
  editorLayout->addWidget(editor_, 1);

  // ---- bottom tabs ----
  results_ = new ResultsView(this);
  connect(results_, &ResultsView::statusMessage, this,
          [this](const QString& t) { statusBar()->showMessage(t, 6000); });

  analyzer_ = new AnalyzerPanel(this);
  connect(analyzer_, &AnalyzerPanel::explainRequested, this,
          &MainWindow::runExplain);
  connect(analyzer_, &AnalyzerPanel::applySuggestion, this,
          [this](const QString& sql) {
            editor_->appendPlainText(QStringLiteral("\n") + sql);
            editor_->setFocus();
          });
  connect(analyzer_, &AnalyzerPanel::revealRange, this,
          [this](int offset, int length) {
            // Offsets are relative to the statement, so rebase onto the
            // document before selecting.
            const std::string all = editor_->toPlainText().toStdString();
            const auto [begin, end] = statementRangeAt(
                all, static_cast<size_t>(editor_->textCursor().position()));
            Q_UNUSED(end);
            QTextCursor c = editor_->textCursor();
            c.setPosition(static_cast<int>(begin) + offset);
            c.setPosition(static_cast<int>(begin) + offset + length,
                          QTextCursor::KeepAnchor);
            editor_->setTextCursor(c);
            editor_->setFocus();
          });

  bottomTabs_ = new QTabWidget(this);
  bottomTabs_->addTab(results_, QStringLiteral("Results"));
  bottomTabs_->addTab(analyzer_, QStringLiteral("Analysis"));

  auto* rightSplit = new QSplitter(Qt::Vertical, this);
  rightSplit->addWidget(editorPane);
  rightSplit->addWidget(bottomTabs_);
  rightSplit->setStretchFactor(0, 2);
  rightSplit->setStretchFactor(1, 3);

  // ---- workspace tabs: editor vs visual builder ----
  builder_ = new QueryBuilderPanel(this);
  connect(builder_, &QueryBuilderPanel::sendToEditor, this,
          [this](const QString& sql) {
            editor_->setPlainText(sql);
            workspaceTabs_->setCurrentIndex(0);
            editor_->setFocus();
            analyzer_->analyze(sql);
          });
  connect(builder_, &QueryBuilderPanel::runRequested, this,
          [this](const QString& sql) {
            editor_->setPlainText(sql);
            workspaceTabs_->setCurrentIndex(0);
            runSql(sql);
          });

  workspaceTabs_ = new QTabWidget(this);
  workspaceTabs_->addTab(rightSplit, QStringLiteral("SQL editor"));
  workspaceTabs_->addTab(builder_, QStringLiteral("Query builder"));

  auto* main = new QSplitter(Qt::Horizontal, this);
  main->addWidget(left);
  main->addWidget(workspaceTabs_);
  main->setStretchFactor(0, 0);
  main->setStretchFactor(1, 1);
  main->setSizes({280, 1160});
  setCentralWidget(main);

  // ---- status bar ----
  statusLabel_ = new QLabel(QStringLiteral("Not connected"), this);
  busyBar_ = new QProgressBar(this);
  busyBar_->setRange(0, 0);        // indeterminate
  busyBar_->setMaximumWidth(120);
  busyBar_->setVisible(false);
  statusBar()->addPermanentWidget(busyBar_);
  statusBar()->addPermanentWidget(statusLabel_);
}

void MainWindow::buildMenus() {
  auto* toolbar = addToolBar(QStringLiteral("Main"));
  toolbar->setObjectName(QStringLiteral("mainToolbar"));
  toolbar->setMovable(false);

  runAction_ = new QAction(QStringLiteral("Run"), this);
  runAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
  runAction_->setToolTip(
      QStringLiteral("Run the statement under the cursor (⌘↩)"));
  connect(runAction_, &QAction::triggered, this,
          &MainWindow::runCurrentStatement);

  runScriptAction_ = new QAction(QStringLiteral("Run all"), this);
  runScriptAction_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return));
  connect(runScriptAction_, &QAction::triggered, this,
          &MainWindow::runWholeScript);

  explainAction_ = new QAction(QStringLiteral("Explain"), this);
  explainAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
  connect(explainAction_, &QAction::triggered, this, &MainWindow::runExplain);

  cancelAction_ = new QAction(QStringLiteral("Cancel"), this);
  cancelAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Period));
  connect(cancelAction_, &QAction::triggered, this, &MainWindow::cancelQuery);

  refreshAction_ = new QAction(QStringLiteral("Refresh schema"), this);
  refreshAction_->setShortcut(QKeySequence::Refresh);
  connect(refreshAction_, &QAction::triggered, this, &MainWindow::refreshSchema);

  disconnectAction_ = new QAction(QStringLiteral("Disconnect"), this);
  connect(disconnectAction_, &QAction::triggered, this,
          &MainWindow::disconnectCurrent);

  rowLimit_ = new QSpinBox(this);
  rowLimit_->setRange(0, 1000000);
  rowLimit_->setValue(1000);
  rowLimit_->setPrefix(QStringLiteral("max rows "));
  rowLimit_->setSpecialValueText(QStringLiteral("max rows unlimited"));
  rowLimit_->setToolTip(
      QStringLiteral("Rows fetched per SELECT. Guards against pulling a huge "
                     "table into memory by accident."));

  toolbar->addAction(runAction_);
  toolbar->addAction(runScriptAction_);
  toolbar->addAction(explainAction_);
  toolbar->addAction(cancelAction_);
  toolbar->addSeparator();
  toolbar->addAction(refreshAction_);
  toolbar->addSeparator();
  toolbar->addWidget(rowLimit_);

  auto* connectionMenu = menuBar()->addMenu(QStringLiteral("&Connection"));
  connectionMenu->addAction(QStringLiteral("New connection…"), this,
                            &MainWindow::newConnection);
  connectionMenu->addAction(QStringLiteral("Edit…"), this,
                            &MainWindow::editConnection);
  connectionMenu->addAction(QStringLiteral("Delete"), this,
                            &MainWindow::deleteConnection);
  connectionMenu->addSeparator();
  connectionMenu->addAction(QStringLiteral("Connect"), this,
                            &MainWindow::connectToSelected);
  connectionMenu->addAction(disconnectAction_);

  auto* queryMenu = menuBar()->addMenu(QStringLiteral("&Query"));
  queryMenu->addAction(runAction_);
  queryMenu->addAction(runScriptAction_);
  queryMenu->addAction(explainAction_);
  queryMenu->addAction(cancelAction_);
  queryMenu->addSeparator();
  queryMenu->addAction(refreshAction_);
  queryMenu->addSeparator();
  queryMenu->addAction(QStringLiteral("AI settings…"), this,
                       &MainWindow::setApiKey);
}

void MainWindow::rebuildConnectionList() {
  connectionList_->clear();
  for (const auto& c : store_->connections()) {
    auto* item = new QListWidgetItem(
        QStringLiteral("%1  ·  %2")
            .arg(QString::fromStdString(c.name),
                 QString::fromLatin1(dialectName(c.dialect))),
        connectionList_);
    item->setData(kIdRole, QString::fromStdString(c.id));
  }
}

void MainWindow::newConnection() {
  ConnectionDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) return;
  store_->save(dialog.config(), dialog.shouldSavePassword());
  rebuildConnectionList();
}

void MainWindow::editConnection() {
  QListWidgetItem* item = connectionList_->currentItem();
  if (!item) return;

  ConnectionConfig cfg = store_->byId(item->data(kIdRole).toString());
  store_->loadPassword(&cfg);

  ConnectionDialog dialog(this);
  dialog.setConfig(cfg);
  if (dialog.exec() != QDialog::Accepted) return;
  store_->save(dialog.config(), dialog.shouldSavePassword());
  rebuildConnectionList();
}

void MainWindow::deleteConnection() {
  QListWidgetItem* item = connectionList_->currentItem();
  if (!item) return;
  const QString name = item->text();
  if (QMessageBox::question(
          this, QStringLiteral("Delete connection"),
          QStringLiteral("Remove “%1”? The database itself is untouched.")
              .arg(name)) != QMessageBox::Yes) {
    return;
  }
  store_->remove(item->data(kIdRole).toString());
  rebuildConnectionList();
}

void MainWindow::connectToSelected() {
  QListWidgetItem* item = connectionList_->currentItem();
  if (!item) {
    statusBar()->showMessage(QStringLiteral("Pick a connection first."), 4000);
    return;
  }

  ConnectionConfig cfg = store_->byId(item->data(kIdRole).toString());
  if (cfg.dialect == Dialect::MySql && cfg.password.empty()) {
    if (!store_->loadPassword(&cfg)) {
      bool ok = false;
      const QString password = QInputDialog::getText(
          this, QStringLiteral("Password"),
          QStringLiteral("Password for %1@%2:")
              .arg(QString::fromStdString(cfg.user),
                   QString::fromStdString(cfg.host)),
          QLineEdit::Password, QString(), &ok);
      if (!ok) return;
      cfg.password = password.toStdString();
    }
  }

  disconnectCurrent();

  session_ = new ConnectionSession(cfg, this);
  connect(session_, &ConnectionSession::opened, this, [this] {
    statusLabel_->setText(session_->serverVersion());
    disconnectAction_->setEnabled(true);

    databasePicker_->clear();
    databasePicker_->addItems(session_->databases());
    databasePicker_->setEnabled(!session_->databases().isEmpty());
    const QString active =
        QString::fromStdString(session_->config().database);
    if (!active.isEmpty()) databasePicker_->setCurrentText(active);

    editor_->setDialect(session_->config().dialect);
    analyzer_->setDialect(session_->config().dialect);
    builder_->setDialect(session_->config().dialect);
    nlBar_->setDialect(session_->config().dialect);
    results_->setDialect(session_->config().dialect);

    statusBar()->showMessage(QStringLiteral("Connected."), 4000);
  });

  connect(session_, &ConnectionSession::openFailed, this,
          [this](const QString& message) {
            statusLabel_->setText(QStringLiteral("Not connected"));
            QMessageBox::warning(this, QStringLiteral("Could not connect"),
                                 message);
          });

  connect(session_, &ConnectionSession::schemaChanged, this,
          &MainWindow::applySchemaEverywhere);

  connect(session_, &ConnectionSession::resultReady, this,
          [this](const ResultSet& result, int index, int total) {
            results_->setResult(result);
            bottomTabs_->setCurrentWidget(results_);
            if (total > 1) {
              statusBar()->showMessage(
                  QStringLiteral("Statement %1 of %2 finished.")
                      .arg(index + 1)
                      .arg(total),
                  6000);
            }
            // A DDL statement changes the schema the editor completes against.
            const QString sql = QString::fromStdString(result.statement).trimmed();
            static const QStringList kDdl = {
                QStringLiteral("CREATE"), QStringLiteral("ALTER"),
                QStringLiteral("DROP"),   QStringLiteral("RENAME")};
            for (const QString& kw : kDdl) {
              if (sql.startsWith(kw, Qt::CaseInsensitive)) {
                session_->refreshSchema();
                break;
              }
            }
          });

  connect(session_, &ConnectionSession::explainReady, this,
          [this](const ResultSet& plan, const QString& sql) {
            analyzer_->setPlan(plan, sql);
            bottomTabs_->setCurrentWidget(analyzer_);
          });

  connect(session_, &ConnectionSession::failed, this,
          [this](const QString& message, const QString& statement) {
            results_->setError(message, statement);
            bottomTabs_->setCurrentWidget(results_);
            statusBar()->showMessage(message, 12000);
          });

  connect(session_, &ConnectionSession::busyChanged, this,
          &MainWindow::setBusy);

  statusLabel_->setText(QStringLiteral("Connecting…"));
  session_->open();
}

void MainWindow::disconnectCurrent() {
  if (!session_) return;
  delete session_;
  session_ = nullptr;

  databasePicker_->clear();
  databasePicker_->setEnabled(false);
  schemaTree_->setSchema(Schema{});
  editor_->setSchema(Schema{});
  builder_->setSchema(Schema{});
  statusLabel_->setText(QStringLiteral("Not connected"));
  disconnectAction_->setEnabled(false);
}

void MainWindow::applySchemaEverywhere() {
  if (!session_) return;
  const Schema& schema = session_->schema();
  schemaTree_->setSchema(schema);
  editor_->setSchema(schema);
  builder_->setSchema(schema);
  analyzer_->setSchema(schema);
  nlBar_->setSchema(schema);
  analyzer_->analyze(editor_->currentStatement());
  statusBar()->showMessage(
      QStringLiteral("Schema: %1 tables.").arg(schema.tables.size()), 5000);
}

void MainWindow::setBusy(bool busy) {
  busyBar_->setVisible(busy);
  runAction_->setEnabled(!busy && session_ != nullptr);
  runScriptAction_->setEnabled(!busy && session_ != nullptr);
  explainAction_->setEnabled(!busy && session_ != nullptr);
  refreshAction_->setEnabled(!busy && session_ != nullptr);
  cancelAction_->setEnabled(busy);
  if (!session_) disconnectAction_->setEnabled(false);
}

void MainWindow::runSql(const QString& sql) {
  if (!session_) {
    statusBar()->showMessage(QStringLiteral("Connect to a database first."),
                             4000);
    return;
  }
  if (sql.trimmed().isEmpty()) return;

  // A statement the analyzer calls destructive-and-unbounded gets one
  // confirmation before it runs.
  for (const auto& d : analyzeStatic(session_->schema(), sql.toStdString(),
                                     session_->config().dialect)) {
    if (d.code != "unbounded-write") continue;
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Run without a WHERE clause?"),
        QString::fromStdString(d.title + "\n\n" + d.detail),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    break;
  }

  editor_->clearError();
  session_->execute(sql, rowLimit_->value());
}

void MainWindow::runCurrentStatement() {
  runSql(editor_->currentStatement());
}

void MainWindow::runWholeScript() { runSql(editor_->toPlainText()); }

void MainWindow::runExplain() {
  if (!session_) return;
  const QString sql = editor_->currentStatement();
  if (sql.trimmed().isEmpty()) return;
  session_->explain(sql);
}

void MainWindow::cancelQuery() {
  if (!session_) return;
  session_->cancel();
  statusBar()->showMessage(QStringLiteral("Cancelling…"), 4000);
}

void MainWindow::refreshSchema() {
  if (session_) session_->refreshSchema();
}

void MainWindow::previewTable(const QString& table) {
  if (!session_) return;
  const char q = session_->config().dialect == Dialect::MySql ? '`' : '"';
  const QString sql = QStringLiteral("SELECT * FROM %1%2%1 LIMIT 200")
                          .arg(QChar(q), table);
  editor_->setPlainText(sql);
  workspaceTabs_->setCurrentIndex(0);
  runSql(sql);
}

void MainWindow::setApiKey() {
  AiSettingsDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) return;
  dialog.applyAndSave();
  nlBar_->reloadSettings();
  statusBar()->showMessage(
      QStringLiteral("Natural-language SQL: %1").arg(dialog.settings().label()),
      6000);
}

}  // namespace ds
