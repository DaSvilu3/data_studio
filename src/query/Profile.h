#pragma once
#include <string>
#include <vector>

#include "core/Schema.h"
#include "core/Value.h"

namespace ds {

// One frequent value and how often it occurs.
struct ValueFrequency {
  std::string value;
  int64_t count = 0;
  double share = 0.0;   // 0..1 of non-null rows
};

// What a column actually contains, as opposed to what it was declared as.
struct ColumnProfile {
  std::string table;
  std::string column;
  std::string declaredType;

  int64_t totalRows = 0;
  int64_t nullCount = 0;
  int64_t distinctCount = 0;
  std::vector<ValueFrequency> topValues;

  std::string minValue;
  std::string maxValue;
  bool hasRange = false;
  double meanValue = 0.0;
  bool hasMean = false;

  double nullShare() const {
    return totalRows > 0 ? static_cast<double>(nullCount) / totalRows : 0.0;
  }
  // Fraction of rows that are distinct. Near 1 means a good index candidate;
  // near 0 means an index would barely narrow anything down.
  double selectivity() const {
    const int64_t nonNull = totalRows - nullCount;
    return nonNull > 0 ? static_cast<double>(distinctCount) / nonNull : 0.0;
  }
  bool looksUnique() const {
    return totalRows > 0 && distinctCount == totalRows - nullCount;
  }
  bool looksConstant() const { return distinctCount <= 1 && totalRows > 0; }
};

// Builds the statements a profile needs. Kept separate from execution so the
// caller can run them on its worker thread and so they are testable.
class ProfileQueries {
 public:
  ProfileQueries(Dialect dialect, const Table& table, const Column& column);

  std::string counts() const;      // rows, nulls, distinct
  std::string range() const;       // min / max / mean, when meaningful
  std::string topValues(int limit) const;

  bool supportsRange() const { return supportsRange_; }
  bool supportsMean() const { return supportsMean_; }

 private:
  std::string quote(const std::string& ident) const;

  Dialect dialect_;
  std::string table_;
  std::string column_;
  bool supportsRange_ = false;
  bool supportsMean_ = false;
};

// Turns the three result sets into a profile.
ColumnProfile assembleProfile(const Table& table, const Column& column,
                              const ResultSet& counts, const ResultSet& range,
                              const ResultSet& topValues);

// Observations worth surfacing: a column that is effectively constant, one
// that is unique but unindexed, unexpected nulls, and so on.
std::vector<std::string> profileInsights(const ColumnProfile& profile,
                                         const Table& table);

}  // namespace ds
