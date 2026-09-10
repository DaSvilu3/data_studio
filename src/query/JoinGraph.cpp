#include "query/JoinGraph.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace ds {

std::string JoinEdge::onClause(const std::string& fromHandle,
                               const std::string& toHandle) const {
  std::string out;
  const size_t n = std::min(fromColumns.size(), toColumns.size());
  for (size_t i = 0; i < n; ++i) {
    if (i) out += " AND ";
    out += fromHandle + "." + fromColumns[i] + " = " + toHandle + "." +
           toColumns[i];
  }
  return out;
}

JoinGraph::JoinGraph(const Schema& schema) {
  for (const auto& t : schema.tables) {
    for (const auto& fk : t.foreignKeys) {
      if (fk.fromColumns.empty() || fk.toColumns.empty()) continue;
      if (!schema.findTable(fk.toTable)) continue;  // FK to a table we can't see

      JoinEdge fwd;
      fwd.fromTable = fk.fromTable;
      fwd.toTable = fk.toTable;
      fwd.fromColumns = fk.fromColumns;
      fwd.toColumns = fk.toColumns;
      fwd.reversed = false;
      fwd.constraintName = fk.name;
      edges_.push_back(fwd);

      JoinEdge back;
      back.fromTable = fk.toTable;
      back.toTable = fk.fromTable;
      back.fromColumns = fk.toColumns;
      back.toColumns = fk.fromColumns;
      back.reversed = true;
      back.constraintName = fk.name;
      edges_.push_back(std::move(back));
    }
  }
}

std::vector<JoinEdge> JoinGraph::neighbors(const std::string& table) const {
  std::vector<JoinEdge> out;
  for (const auto& e : edges_) {
    if (iequals(e.fromTable, table)) out.push_back(e);
  }
  // Child -> parent hops first: they're many-to-one and don't inflate the
  // result, so they're almost always what the user wants offered first.
  std::stable_sort(out.begin(), out.end(),
                   [](const JoinEdge& a, const JoinEdge& b) {
                     return !a.reversed && b.reversed;
                   });
  return out;
}

std::vector<JoinEdge> JoinGraph::shortestPath(const std::string& from,
                                              const std::string& to) const {
  if (iequals(from, to)) return {};

  // Plain BFS -- hop count is the right cost here, and schemas are small
  // enough that anything cleverer is wasted.
  std::map<std::string, JoinEdge> cameFrom;
  std::set<std::string> seen{from};
  std::deque<std::string> queue{from};

  while (!queue.empty()) {
    const std::string cur = queue.front();
    queue.pop_front();

    for (const auto& e : neighbors(cur)) {
      if (seen.count(e.toTable)) continue;
      seen.insert(e.toTable);
      cameFrom[e.toTable] = e;
      if (iequals(e.toTable, to)) {
        std::vector<JoinEdge> path;
        std::string node = e.toTable;
        while (cameFrom.count(node)) {
          const JoinEdge& step = cameFrom[node];
          path.push_back(step);
          node = step.fromTable;
        }
        std::reverse(path.begin(), path.end());
        return path;
      }
      queue.push_back(e.toTable);
    }
  }
  return {};
}

std::vector<JoinEdge> JoinGraph::connect(
    const std::vector<std::string>& tables,
    std::vector<std::string>* unreached) const {
  std::vector<JoinEdge> plan;
  if (unreached) unreached->clear();
  if (tables.empty()) return plan;

  // Grow a connected set one table at a time, always attaching the next table
  // by its shortest path to something already joined. Intermediate tables on
  // that path get pulled in too -- that's what makes a two-table selection
  // across a junction table produce the right SQL.
  std::set<std::string> joined{tables.front()};

  for (size_t i = 1; i < tables.size(); ++i) {
    const std::string& target = tables[i];
    if (joined.count(target)) continue;

    std::vector<JoinEdge> best;
    for (const auto& anchor : joined) {
      auto path = shortestPath(anchor, target);
      if (path.empty()) continue;
      if (best.empty() || path.size() < best.size()) best = std::move(path);
    }

    if (best.empty()) {
      if (unreached) unreached->push_back(target);
      continue;
    }
    for (auto& step : best) {
      if (joined.count(step.toTable)) continue;
      joined.insert(step.toTable);
      plan.push_back(std::move(step));
    }
  }
  return plan;
}

}  // namespace ds
