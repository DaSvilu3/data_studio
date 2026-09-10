#pragma once
#include <QWidget>

#include "core/Schema.h"
#include "query/Profile.h"

class QLabel;
class QTreeWidget;
class QStackedWidget;

namespace ds {

// Shows what a column actually contains: nulls, distinct values, range, the
// most common values with proportion bars, and what that implies for indexing.
class ProfilePanel : public QWidget {
  Q_OBJECT
 public:
  explicit ProfilePanel(QWidget* parent = nullptr);

  void setSchema(const Schema& schema) { schema_ = schema; }
  void showProfile(const ColumnProfile& profile);
  void showPending(const QString& table, const QString& column);
  void clear();

 signals:
  void insertText(const QString& text);

 private:
  QStackedWidget* stack_ = nullptr;
  QLabel* placeholder_ = nullptr;
  QLabel* heading_ = nullptr;
  QLabel* stats_ = nullptr;
  QLabel* insights_ = nullptr;
  QTreeWidget* values_ = nullptr;
  Schema schema_;
};

}  // namespace ds
