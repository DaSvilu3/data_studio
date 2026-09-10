#pragma once
#include <QWidget>

#include "core/Schema.h"
#include "core/Value.h"
#include "query/Analyzer.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;

namespace ds {

// Lists what the analyzer found for the current statement: static schema
// checks always, plus EXPLAIN findings once a plan has been fetched.
class AnalyzerPanel : public QWidget {
  Q_OBJECT
 public:
  explicit AnalyzerPanel(QWidget* parent = nullptr);

  void setSchema(const Schema& schema) { schema_ = schema; }
  void setDialect(Dialect d) { dialect_ = d; }

  // Re-runs the static rules. Cheap enough to call on every keystroke pause.
  void analyze(const QString& sql);
  // Merges in findings from an EXPLAIN the session just returned.
  void setPlan(const ResultSet& plan, const QString& sql);
  void clearPlan();

 signals:
  // A diagnostic with a runnable fix was double-clicked.
  void applySuggestion(const QString& sql);
  // The user asked to jump to where a finding is in the editor.
  void revealRange(int offset, int length);
  void explainRequested();

 private:
  void rebuild();

  QTreeWidget* tree_ = nullptr;
  QLabel* summary_ = nullptr;
  QPushButton* explainButton_ = nullptr;
  QLabel* planTable_ = nullptr;

  Schema schema_;
  Dialect dialect_ = Dialect::Sqlite;
  QString sql_;
  std::vector<Diagnostic> staticDiags_;
  std::vector<Diagnostic> planDiags_;
  bool havePlan_ = false;
};

}  // namespace ds
