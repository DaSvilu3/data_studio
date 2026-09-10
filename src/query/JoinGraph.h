#pragma once
#include <string>
#include <vector>

#include "core/Schema.h"

namespace ds {

// One hop between two tables, derived from a declared foreign key.
struct JoinEdge {
  std::string fromTable;
  std::string toTable;
  std::vector<std::string> fromColumns;
  std::vector<std::string> toColumns;
  // True when we traversed the FK backwards (parent -> child), which is the
  // one-to-many direction and can multiply rows.
  bool reversed = false;
  std::string constraintName;

  // "a.x = b.y AND a.z = b.w"
  std::string onClause(const std::string& fromHandle,
                       const std::string& toHandle) const;
};

// Foreign-key graph over a schema. Edges run both ways, so a path can be found
// between any two related tables regardless of which side declared the key.
class JoinGraph {
 public:
  explicit JoinGraph(const Schema& schema);

  // Every table directly reachable from `table` in one hop.
  std::vector<JoinEdge> neighbors(const std::string& table) const;

  // Shortest chain of joins connecting `from` to `to`. Empty when unrelated.
  std::vector<JoinEdge> shortestPath(const std::string& from,
                                     const std::string& to) const;

  // Orders `tables` into a join sequence and returns the edges linking them,
  // so the builder can emit FROM a JOIN b ... JOIN c. `unreached` receives any
  // table with no FK path to the rest.
  std::vector<JoinEdge> connect(const std::vector<std::string>& tables,
                                std::vector<std::string>* unreached) const;

 private:
  std::vector<JoinEdge> edges_;
};

}  // namespace ds
