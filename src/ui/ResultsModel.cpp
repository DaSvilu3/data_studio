#include "ui/ResultsModel.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QSet>

#include <algorithm>

namespace ds {

ResultsModel::ResultsModel(QObject* parent) : QAbstractTableModel(parent) {}

void ResultsModel::setResult(const ResultSet& result) {
  beginResetModel();
  result_ = result;
  endResetModel();
}

int ResultsModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(result_.rows.size());
}

int ResultsModel::columnCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(result_.columns.size());
}

QVariant ResultsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};
  const int r = index.row();
  const int c = index.column();
  if (r < 0 || r >= static_cast<int>(result_.rows.size())) return {};
  if (c < 0 || c >= static_cast<int>(result_.rows[r].size())) return {};

  const Value& v = result_.rows[r][c];

  switch (role) {
    case Qt::DisplayRole: {
      const QString text = QString::fromStdString(toDisplayString(v));
      // Keep the grid readable: long text is elided here, and the full value
      // stays available through the tooltip and the cell inspector.
      if (text.length() > 300) return text.left(300) + QStringLiteral("…");
      // Newlines would make rows uneven; show them as glyphs instead.
      QString flat = text;
      flat.replace(QLatin1Char('\n'), QChar(0x21B5));
      return flat;
    }
    case Qt::ToolTipRole:
      return QString::fromStdString(toDisplayString(v));
    case SortRole: {
      // NULLs sort together at one end rather than interleaving with values.
      if (isNull(v)) return QVariant();
      if (const auto* i = std::get_if<int64_t>(&v)) {
        return QVariant(static_cast<qlonglong>(*i));
      }
      if (const auto* d = std::get_if<double>(&v)) return QVariant(*d);
      return QString::fromStdString(toDisplayString(v));
    }
    case Qt::TextAlignmentRole:
      if (std::holds_alternative<int64_t>(v) ||
          std::holds_alternative<double>(v)) {
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
      }
      return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ForegroundRole:
      if (isNull(v)) return QBrush(QColor(150, 150, 150));
      return {};
    case Qt::FontRole:
      if (isNull(v)) {
        QFont f;
        f.setItalic(true);
        return f;
      }
      return {};
    default:
      return {};
  }
}

QVariant ResultsModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const {
  if (orientation == Qt::Vertical) {
    if (role == Qt::DisplayRole) return section + 1;
    return {};
  }
  if (section < 0 || section >= static_cast<int>(result_.columns.size())) {
    return {};
  }
  const ColumnMeta& m = result_.columns[section];
  switch (role) {
    case Qt::DisplayRole:
      return QString::fromStdString(m.name);
    case Qt::ToolTipRole: {
      QString tip = QString::fromStdString(m.name);
      if (!m.typeName.empty()) {
        tip += QStringLiteral("  —  ") + QString::fromStdString(m.typeName);
      }
      if (!m.tableName.empty()) {
        tip += QStringLiteral("\nfrom ") + QString::fromStdString(m.tableName);
      }
      return tip;
    }
    default:
      return {};
  }
}

namespace {

// Groups a selection into ordered rows of ordered columns.
std::pair<std::vector<int>, std::vector<int>> selectionExtent(
    const QModelIndexList& selection) {
  QSet<int> rowSet, colSet;
  for (const auto& i : selection) {
    rowSet.insert(i.row());
    colSet.insert(i.column());
  }
  std::vector<int> rows(rowSet.begin(), rowSet.end());
  std::vector<int> cols(colSet.begin(), colSet.end());
  std::sort(rows.begin(), rows.end());
  std::sort(cols.begin(), cols.end());
  return {rows, cols};
}

QString csvEscape(const QString& s) {
  if (!s.contains(QLatin1Char(',')) && !s.contains(QLatin1Char('"')) &&
      !s.contains(QLatin1Char('\n'))) {
    return s;
  }
  QString out = s;
  out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  return QLatin1Char('"') + out + QLatin1Char('"');
}

}  // namespace

QString ResultsModel::copyAsText(const QModelIndexList& selection) const {
  auto [rows, cols] = selectionExtent(selection);
  if (rows.empty() || cols.empty()) return {};

  QStringList lines;
  QStringList header;
  for (int c : cols) header << QString::fromStdString(result_.columns[c].name);
  lines << header.join(QLatin1Char('\t'));

  for (int r : rows) {
    QStringList cells;
    for (int c : cols) {
      cells << QString::fromStdString(toDisplayString(result_.rows[r][c]));
    }
    lines << cells.join(QLatin1Char('\t'));
  }
  return lines.join(QLatin1Char('\n'));
}

QString ResultsModel::copyAsCsv(const QModelIndexList& selection) const {
  auto [rows, cols] = selectionExtent(selection);
  if (rows.empty() || cols.empty()) return {};

  QStringList lines;
  QStringList header;
  for (int c : cols) {
    header << csvEscape(QString::fromStdString(result_.columns[c].name));
  }
  lines << header.join(QLatin1Char(','));

  for (int r : rows) {
    QStringList cells;
    for (int c : cols) {
      const Value& v = result_.rows[r][c];
      // A NULL is an empty field, not the literal text "NULL".
      cells << (isNull(v)
                    ? QString()
                    : csvEscape(QString::fromStdString(toDisplayString(v))));
    }
    lines << cells.join(QLatin1Char(','));
  }
  return lines.join(QLatin1Char('\n'));
}

QString ResultsModel::copyAsInsert(const QModelIndexList& selection,
                                   const QString& tableName,
                                   Dialect dialect) const {
  auto [rows, cols] = selectionExtent(selection);
  if (rows.empty() || cols.empty()) return {};

  const QChar q = dialect == Dialect::MySql ? QLatin1Char('`') : QLatin1Char('"');
  auto quote = [&](const std::string& s) {
    return q + QString::fromStdString(s) + q;
  };

  QStringList colNames;
  for (int c : cols) colNames << quote(result_.columns[c].name);

  const QString table =
      tableName.isEmpty() ? QStringLiteral("<table>") : q + tableName + q;

  QStringList lines;
  for (int r : rows) {
    QStringList values;
    for (int c : cols) {
      values << QString::fromStdString(toSqlLiteral(result_.rows[r][c]));
    }
    lines << QStringLiteral("INSERT INTO %1 (%2) VALUES (%3);")
                 .arg(table, colNames.join(QStringLiteral(", ")),
                      values.join(QStringLiteral(", ")));
  }
  return lines.join(QLatin1Char('\n'));
}

}  // namespace ds
