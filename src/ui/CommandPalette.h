#pragma once
#include <QDialog>
#include <QVector>
#include <functional>

#include "core/Schema.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace ds {

// One thing the palette can do: jump to a table, insert a column, or run an
// action already on a menu.
struct PaletteEntry {
  enum class Kind { Table, Column, Action };
  Kind kind = Kind::Action;
  QString title;
  QString detail;
  QString payload;          // table name, "table.column", or an action id
  std::function<void()> run; // set for actions
};

// ⌘K launcher. On a schema with hundreds of tables this is the fastest way to
// get anywhere, and it uses the same fuzzy ranking as editor completion.
class CommandPalette : public QDialog {
  Q_OBJECT
 public:
  explicit CommandPalette(QWidget* parent = nullptr);

  void setSchema(const Schema& schema);
  void addAction(const QString& title, const QString& detail,
                 std::function<void()> run);
  void reset();

 signals:
  void tableChosen(const QString& table);
  void columnChosen(const QString& table, const QString& column);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void refilter(const QString& query);
  void accept() override;

 private:
  QLineEdit* input_ = nullptr;
  QListWidget* list_ = nullptr;
  QVector<PaletteEntry> entries_;    // schema entries, rebuilt on refresh
  QVector<PaletteEntry> actions_;    // registered once
};

}  // namespace ds
