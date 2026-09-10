#pragma once
#include <string>
#include <vector>

#include "core/Schema.h"
#include "core/Value.h"

namespace ds {

enum class Severity { Info, Warning, Error };

const char* severityName(Severity s);

struct Diagnostic {
  Severity severity = Severity::Info;
  std::string code;        // stable id, e.g. "missing-index"
  std::string title;       // one line, shown in the list
  std::string detail;      // why it matters
  std::string suggestion;  // runnable SQL when there is one (e.g. CREATE INDEX)
  size_t offset = 0;       // byte range in the analyzed statement, for
  size_t length = 0;       // underlining in the editor
};

// Checks a statement against the schema without executing it: unknown names,
// predicates that can't use an index, missing WHERE on destructive statements,
// SELECT * on wide tables, and so on.
std::vector<Diagnostic> analyzeStatic(const Schema& schema,
                                      const std::string& sql, Dialect dialect);

// Interprets the rows an EXPLAIN returned. `sql` is the statement the plan
// belongs to, used to attribute findings back to a table.
std::vector<Diagnostic> analyzePlan(const Schema& schema, const ResultSet& plan,
                                    const std::string& sql, Dialect dialect);

// A column reference found in a WHERE/ON/ORDER BY clause.
struct PredicateRef {
  std::string qualifier;   // alias or table name as written
  std::string column;
  std::string op;
  bool wrappedInFunction = false;   // DATE(created_at) = ... -- kills the index
  bool leadingWildcard = false;     // LIKE '%foo'
  size_t offset = 0;
  size_t length = 0;
};

// Pulls the filterable column references out of a statement.
std::vector<PredicateRef> extractPredicates(const std::string& sql);

}  // namespace ds
