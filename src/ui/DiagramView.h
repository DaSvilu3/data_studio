#pragma once
#include <QGraphicsView>
#include <QHash>

#include "core/Schema.h"
#include "core/Value.h"

class QGraphicsScene;
class QComboBox;
class QSpinBox;
class QLabel;
class QLineEdit;

namespace ds {

class TableNode;

// Interactive map of the foreign-key graph. A large schema can't be drawn all
// at once and stay readable, so the view works in "focus" mode: one table at
// the centre, everything within N foreign-key hops laid out in rings around it.
class DiagramView : public QWidget {
  Q_OBJECT
 public:
  explicit DiagramView(QWidget* parent = nullptr);

  void setSchema(const Schema& schema);
  void focusTable(const QString& table);
  QString focusedTable() const { return focus_; }

 signals:
  void previewTableRequested(const QString& table);
  // "Build a SELECT joining these two" -- the diagram is a query-building
  // surface, not just a picture.
  void joinRequested(const QString& fromTable, const QString& toTable);
  void insertText(const QString& text);

 protected:
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void relayout();

 private:
  void buildScene();
  void fitToScene();
  // Tables within `depth` hops of `focus_`, grouped by hop distance.
  QList<QStringList> neighbourhood(const QString& root, int depth) const;

  QGraphicsView* view_ = nullptr;
  QGraphicsScene* scene_ = nullptr;
  QComboBox* tablePicker_ = nullptr;
  QSpinBox* depthPicker_ = nullptr;
  QLabel* summary_ = nullptr;

  Schema schema_;
  QString focus_;
  QHash<QString, TableNode*> nodes_;
  // The scene is built before the tab is ever shown, when the view still has
  // no size; the fit has to be redone once it does.
  bool fitted_ = false;
};

}  // namespace ds
