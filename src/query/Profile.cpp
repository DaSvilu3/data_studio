#include "query/Profile.h"

#include <algorithm>
#include <cmath>

namespace ds {
namespace {

bool isNumericKind(Column::Kind k) {
  return k == Column::Kind::Integer || k == Column::Kind::Real;
}

bool isOrderedKind(Column::Kind k) {
  return isNumericKind(k) || k == Column::Kind::Date ||
         k == Column::Kind::DateTime || k == Column::Kind::Text;
}

}  // namespace

ProfileQueries::ProfileQueries(Dialect dialect, const Table& table,
                               const Column& column)
    : dialect_(dialect), table_(table.name), column_(column.name) {
  supportsRange_ = isOrderedKind(column.kind);
  // Averaging text or a date is meaningless, and on MySQL it also warns.
  supportsMean_ = isNumericKind(column.kind);
}

std::string ProfileQueries::quote(const std::string& ident) const {
  const char q = dialect_ == Dialect::MySql ? '`' : '"';
  std::string out(1, q);
  for (char c : ident) {
    if (c == q) out.push_back(q);
    out.push_back(c);
  }
  out.push_back(q);
  return out;
}

std::string ProfileQueries::counts() const {
  const std::string col = quote(column_);
  return "SELECT COUNT(*) AS total_rows, "
         "COUNT(*) - COUNT(" + col + ") AS null_count, "
         "COUNT(DISTINCT " + col + ") AS distinct_count "
         "FROM " + quote(table_);
}

std::string ProfileQueries::range() const {
  const std::string col = quote(column_);
  std::string sql = "SELECT MIN(" + col + ") AS min_value, MAX(" + col +
                    ") AS max_value";
  if (supportsMean_) sql += ", AVG(" + col + ") AS mean_value";
  sql += " FROM " + quote(table_);
  return sql;
}

std::string ProfileQueries::topValues(int limit) const {
  const std::string col = quote(column_);
  return "SELECT " + col + " AS value, COUNT(*) AS occurrences "
         "FROM " + quote(table_) + " WHERE " + col + " IS NOT NULL "
         "GROUP BY " + col + " ORDER BY occurrences DESC, " + col + " "
         "LIMIT " + std::to_string(limit);
}

ColumnProfile assembleProfile(const Table& table, const Column& column,
                              const ResultSet& counts, const ResultSet& range,
                              const ResultSet& topValues) {
  ColumnProfile p;
  p.table = table.name;
  p.column = column.name;
  p.declaredType = column.type;

  if (!counts.rows.empty() && counts.rows[0].size() >= 3) {
    p.totalRows = toInt(counts.rows[0][0]);
    p.nullCount = toInt(counts.rows[0][1]);
    p.distinctCount = toInt(counts.rows[0][2]);
  }

  if (!range.rows.empty() && range.rows[0].size() >= 2) {
    const Row& r = range.rows[0];
    if (!isNull(r[0]) || !isNull(r[1])) {
      p.minValue = toDisplayString(r[0]);
      p.maxValue = toDisplayString(r[1]);
      p.hasRange = true;
    }
    if (r.size() >= 3 && !isNull(r[2])) {
      p.meanValue = toReal(r[2]);
      p.hasMean = true;
    }
  }

  const int64_t nonNull = p.totalRows - p.nullCount;
  for (const auto& row : topValues.rows) {
    if (row.size() < 2) continue;
    ValueFrequency f;
    f.value = isNull(row[0]) ? "NULL" : toDisplayString(row[0]);
    f.count = toInt(row[1]);
    f.share = nonNull > 0 ? static_cast<double>(f.count) / nonNull : 0.0;
    p.topValues.push_back(std::move(f));
  }
  return p;
}

std::vector<std::string> profileInsights(const ColumnProfile& p,
                                         const Table& table) {
  std::vector<std::string> out;
  if (p.totalRows == 0) {
    out.push_back("The table is empty, so there is nothing to characterise.");
    return out;
  }

  auto indexed = [&] {
    for (const auto& idx : table.indexes) {
      if (!idx.columns.empty() && iequals(idx.columns.front(), p.column)) {
        return true;
      }
    }
    const Column* c = table.findColumn(p.column);
    return c && c->primaryKey;
  }();

  if (p.looksConstant()) {
    out.push_back(
        "Every row holds the same value. Filtering on this column cannot "
        "narrow anything down, and an index on it would be dead weight.");
  } else if (p.looksUnique() && !indexed) {
    out.push_back(
        "Values are unique across all " + std::to_string(p.totalRows) +
        " rows but the column is not indexed. If you look rows up by it, a "
        "unique index would turn a full scan into a single seek.");
  } else if (p.selectivity() < 0.02 && p.totalRows > 1000 && indexed) {
    out.push_back(
        "Only " + std::to_string(p.distinctCount) +
        " distinct values across " + std::to_string(p.totalRows) +
        " rows. The index here is weakly selective -- the planner may ignore "
        "it and scan instead.");
  }

  if (p.nullShare() > 0.5) {
    out.push_back(
        "Over half the rows are NULL. Remember that = and <> never match NULL "
        "-- use IS NULL, or the rows will silently disappear from results.");
  } else if (p.nullCount > 0) {
    const Column* c = table.findColumn(p.column);
    if (c && !c->nullable) {
      out.push_back("Declared NOT NULL but nulls were counted -- worth a look.");
    }
  }

  if (!p.topValues.empty() && p.topValues.front().share > 0.9 &&
      p.distinctCount > 1) {
    out.push_back(
        "'" + p.topValues.front().value + "' accounts for " +
        std::to_string(static_cast<int>(p.topValues.front().share * 100)) +
        "% of rows. A filter matching it reads almost the whole table; one "
        "matching anything else is highly selective.");
  }

  return out;
}

}  // namespace ds
