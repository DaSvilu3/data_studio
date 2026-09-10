#pragma once
#include <QAbstractTableModel>

#include "core/Value.h"

namespace ds {

// Read-only table model over a ResultSet. Holds the rows by value; the grid is
// virtualised so only visible cells are ever formatted.
class ResultsModel : public QAbstractTableModel {
  Q_OBJECT
 public:
  // Sorting on DisplayRole would compare the rendered strings, putting "10"
  // before "9". This role hands the proxy the underlying typed value instead.
  static constexpr int SortRole = Qt::UserRole + 1;

  explicit ResultsModel(QObject* parent = nullptr);

  void setResult(const ResultSet& result);
  const ResultSet& result() const { return result_; }

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;

  // Tab-separated copy of the given cells, with a header row -- pastes
  // straight into a spreadsheet.
  QString copyAsText(const QModelIndexList& selection) const;
  QString copyAsCsv(const QModelIndexList& selection) const;
  // INSERT statements reconstructing the selected rows.
  QString copyAsInsert(const QModelIndexList& selection,
                       const QString& tableName, Dialect dialect) const;

 private:
  ResultSet result_;
};

}  // namespace ds
