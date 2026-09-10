#pragma once
#include <QDateTime>
#include <QVector>
#include <QWidget>

class QTreeWidget;
class QLineEdit;

namespace ds {

struct HistoryEntry {
  QString sql;
  QString connectionName;
  QDateTime when;
  double elapsedMs = 0;
  int64_t rowCount = -1;
  bool succeeded = true;
  QString error;
};

// Everything run this session and before, persisted, searchable, re-runnable.
class HistoryPanel : public QWidget {
  Q_OBJECT
 public:
  explicit HistoryPanel(QWidget* parent = nullptr);
  ~HistoryPanel() override;

  void record(const HistoryEntry& entry);

 signals:
  void restoreRequested(const QString& sql);
  void runRequested(const QString& sql);

 private:
  void rebuild();
  void load();
  void persist() const;

  QTreeWidget* tree_ = nullptr;
  QLineEdit* filter_ = nullptr;
  QVector<HistoryEntry> entries_;
};

}  // namespace ds
