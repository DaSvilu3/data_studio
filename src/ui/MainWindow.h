#pragma once
#include <QHash>
#include <QMainWindow>

#include "core/Driver.h"

class QComboBox;
class QLabel;
class QListWidget;
class QTabWidget;
class QSplitter;
class QSpinBox;
class QProgressBar;
class QAction;

namespace ds {

class AnalyzerPanel;
class ConnectionSession;
class ConnectionStore;
class NlPromptBar;
class QueryBuilderPanel;
class ResultsView;
class SchemaTree;
class SqlEditor;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void newConnection();
  void editConnection();
  void deleteConnection();
  void connectToSelected();
  void disconnectCurrent();
  void runCurrentStatement();
  void runWholeScript();
  void runExplain();
  void cancelQuery();
  void refreshSchema();
  void setApiKey();
  void previewTable(const QString& table);

 private:
  void buildUi();
  void buildMenus();
  void rebuildConnectionList();
  void applySchemaEverywhere();
  void setBusy(bool busy);
  void runSql(const QString& sql);
  ConnectionSession* session() const { return session_; }

  ConnectionStore* store_ = nullptr;
  ConnectionSession* session_ = nullptr;

  QListWidget* connectionList_ = nullptr;
  QComboBox* databasePicker_ = nullptr;
  SchemaTree* schemaTree_ = nullptr;
  SqlEditor* editor_ = nullptr;
  NlPromptBar* nlBar_ = nullptr;
  QTabWidget* bottomTabs_ = nullptr;
  ResultsView* results_ = nullptr;
  AnalyzerPanel* analyzer_ = nullptr;
  QueryBuilderPanel* builder_ = nullptr;
  QTabWidget* workspaceTabs_ = nullptr;
  QSpinBox* rowLimit_ = nullptr;
  QLabel* statusLabel_ = nullptr;
  QProgressBar* busyBar_ = nullptr;

  QAction* runAction_ = nullptr;
  QAction* runScriptAction_ = nullptr;
  QAction* explainAction_ = nullptr;
  QAction* cancelAction_ = nullptr;
  QAction* disconnectAction_ = nullptr;
  QAction* refreshAction_ = nullptr;

  // Results arriving for a multi-statement script land in numbered tabs.
  int resultTabCount_ = 0;
};

}  // namespace ds
