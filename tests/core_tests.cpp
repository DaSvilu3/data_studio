// Integration tests for the driver, schema introspection and query
// intelligence layers, run against a throwaway SQLite database.
#include <cstdio>
#include <filesystem>

#include "core/Driver.h"
#include "query/Analyzer.h"
#include "query/JoinGraph.h"
#include "query/QueryBuilder.h"
#include "query/SqlContext.h"
#include "tests/test_util.h"

using namespace ds;

namespace {

bool hasDiagnostic(const std::vector<Diagnostic>& diags, const char* code) {
  for (const auto& d : diags) {
    if (d.code == code) return true;
  }
  return false;
}

const Diagnostic* findDiagnostic(const std::vector<Diagnostic>& diags,
                                 const char* code) {
  for (const auto& d : diags) {
    if (d.code == code) return &d;
  }
  return nullptr;
}

bool hasCompletion(const std::vector<Completion>& items, const char* label) {
  for (const auto& c : items) {
    if (c.label == label) return true;
  }
  return false;
}

DriverPtr openFixture(const std::string& path) {
  std::filesystem::remove(path);
  ConnectionConfig cfg;
  cfg.dialect = Dialect::Sqlite;
  cfg.filePath = path;

  auto driver = makeDriver(cfg);
  driver->connect(cfg);

  driver->execute(
      "CREATE TABLE customers (id INTEGER PRIMARY KEY, email TEXT NOT NULL, "
      "name TEXT, country TEXT, created_at DATETIME)", 0);
  driver->execute(
      "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER NOT "
      "NULL REFERENCES customers(id), status TEXT, total REAL)", 0);
  driver->execute(
      "CREATE TABLE order_items (id INTEGER PRIMARY KEY, order_id INTEGER "
      "REFERENCES orders(id), product_id INTEGER REFERENCES products(id), "
      "qty INTEGER)", 0);
  driver->execute(
      "CREATE TABLE products (id INTEGER PRIMARY KEY, sku TEXT, name TEXT, "
      "price REAL)", 0);
  driver->execute("CREATE INDEX idx_orders_customer ON orders(customer_id)", 0);
  driver->execute(
      "INSERT INTO customers(email,name,country) VALUES "
      "('a@x.com','Ana','ES'),('b@x.com','Bo','SG')", 0);
  driver->execute(
      "INSERT INTO orders(customer_id,status,total) VALUES "
      "(1,'paid',10.5),(1,'paid',4.5),(2,'pending',20.0)", 0);
  return driver;
}

// A table large enough that the analyzer's small-table guard doesn't apply.
void makeLargeTable(Driver& driver) {
  driver.execute(
      "CREATE TABLE events (id INTEGER PRIMARY KEY, kind TEXT, "
      "created_at DATETIME, payload TEXT)", 0);
  driver.execute(
      "INSERT INTO events(kind,created_at,payload) "
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE "
      "n<60000) SELECT 'k'||(n%7), '2024-01-01', 'p' FROM c", 0);
}

}  // namespace

int main() {
  const std::string path = "/tmp/ds_core_tests.db";
  auto driver = openFixture(path);

  // ------------------------------------------------------------------ schema
  tst::section("schema introspection");
  Schema schema = driver->introspect();
  tst::check(schema.tables.size() == 4, "four tables discovered");

  const Table* orders = schema.findTable("orders");
  tst::check(orders != nullptr, "orders table found");
  tst::check(orders && orders->columns.size() == 4, "orders has 4 columns");
  tst::check(orders && orders->estimatedRows == 3, "orders row count is exact");
  tst::check(orders && orders->foreignKeys.size() == 1, "orders has 1 FK");
  tst::check(orders && orders->foreignKeys[0].toTable == "customers" &&
                 orders->foreignKeys[0].toColumns[0] == "id",
             "FK resolves to customers.id");

  const Column* pk = orders ? orders->findColumn("id") : nullptr;
  tst::check(pk && pk->primaryKey && pk->autoIncrement,
             "INTEGER PRIMARY KEY detected as autoincrement rowid alias");

  const Column* email = schema.findTable("customers")->findColumn("email");
  tst::check(email && !email->nullable && email->kind == Column::Kind::Text,
             "NOT NULL and type classification");

  // ------------------------------------------------------------ statements
  tst::section("statement splitting");
  auto stmts = splitStatements(
      "SELECT ';' AS a; -- trailing; comment\nSELECT 2; /* mid; block */ "
      "SELECT `we;ird`;");
  tst::check(stmts.size() == 3, "splits on ; while respecting quotes/comments");
  tst::note("got " + std::to_string(stmts.size()) + " statements");

  auto [b, e] = statementRangeAt("SELECT 1;\nSELECT 2 FROM t;\nSELECT 3;", 20);
  tst::check(std::string("SELECT 2 FROM t") ==
                 std::string("SELECT 1;\nSELECT 2 FROM t;\nSELECT 3;")
                     .substr(b, e - b),
             "statement range under a cursor");

  // ------------------------------------------------------------- completion
  tst::section("schema-aware completion");
  {
    const std::string sql =
        "SELECT c.na FROM customers c JOIN orders o ON o.customer_id=c.id";
    auto items = completeAt(schema, sql, 11);
    tst::check(hasCompletion(items, "name"),
               "qualified prefix completes to the right column");
    tst::check(!hasCompletion(items, "status"),
               "columns of the other table are excluded after a qualifier");
  }
  {
    auto items = completeAt(schema, "SELECT * FROM ord", 17);
    tst::check(!items.empty() && items[0].label == "orders",
               "table completion ranks the closest prefix match first");
  }
  {
    // Two tables in scope, so an accepted column must arrive qualified.
    const std::string sql = "SELECT sta FROM customers c JOIN orders o ON 1=1";
    auto items = completeAt(schema, sql, 10);
    bool qualified = false;
    for (const auto& c : items) {
      if (c.label == "status" && c.insertText == "o.status") qualified = true;
    }
    tst::check(qualified, "multi-table scope inserts a qualified column");
  }

  // -------------------------------------------------------------- join graph
  tst::section("foreign-key join graph");
  {
    JoinGraph graph(schema);
    auto path = graph.shortestPath("customers", "products");
    tst::check(path.size() == 3,
               "three hops from customers to products via the junction table");
    tst::check(!path.empty() && path.back().toTable == "products",
               "path terminates at the requested table");
    tst::check(!path.empty() && path[0].reversed,
               "parent -> child hop is flagged one-to-many");
    tst::check(graph.shortestPath("customers", "customers").empty(),
               "path to self is empty");
  }

  // ------------------------------------------------------------- the builder
  tst::section("visual builder SQL generation");
  {
    QuerySpec spec;
    spec.tables = {"customers", "products"};
    spec.select = {{"customers", "name", "", ""},
                   {"order_items", "qty", "SUM", "units"}};
    spec.filters = {{"customers", "country", "=", "ES", "", false}};
    spec.groupBy = {{"customers", "name"}};
    spec.limit = 50;

    BuildResult built = buildQuery(schema, spec, Dialect::Sqlite);
    tst::check(built.sql.find("FROM \"customers\" AS c") != std::string::npos,
               "aliases the root table");
    tst::check(built.sql.find("LEFT JOIN \"orders\"") != std::string::npos,
               "one-to-many hop becomes a LEFT JOIN");
    tst::check(built.sql.find("ON c.id = o.customer_id") != std::string::npos,
               "join condition comes from the foreign key");
    tst::check(built.sql.find("SUM(oi.\"qty\")") != std::string::npos,
               "aggregate applied to the pulled-in junction table");
    tst::check(built.sql.find("= 'ES'") != std::string::npos,
               "text filter is quoted");
    tst::check(built.sql.find("LIMIT 50") != std::string::npos, "limit applied");
    tst::check(!built.warnings.empty(), "row-multiplication warning raised");
  }
  {
    // A numeric column must not get quoted, or MySQL drops the index.
    QuerySpec spec;
    spec.tables = {"orders"};
    spec.filters = {{"orders", "total", ">", "10", "", false}};
    BuildResult built = buildQuery(schema, spec, Dialect::MySql);
    tst::check(built.sql.find("> 10") != std::string::npos,
               "numeric filter is left unquoted");
    tst::check(built.sql.find("`orders`") != std::string::npos,
               "MySQL dialect uses backtick quoting");
  }
  {
    QuerySpec spec;
    spec.tables = {"customers", "products"};   // unrelated without the chain
    spec.select = {{"customers", "name", "", ""}};
    BuildResult built = buildQuery(schema, spec, Dialect::Sqlite);
    tst::check(built.unjoinedTables.empty(),
               "reachable tables are joined rather than reported unjoined");
  }

  // ---------------------------------------------------------- static analysis
  tst::section("static analysis");
  {
    auto d = analyzeStatic(schema, "DELETE FROM orders", Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "unbounded-write"),
               "DELETE without WHERE is flagged");
    const Diagnostic* found = findDiagnostic(d, "unbounded-write");
    tst::check(found && found->severity == Severity::Error,
               "and flagged as an error");
  }
  {
    auto d = analyzeStatic(schema, "SELECT * FROM customers c, orders o",
                           Dialect::Sqlite);
    const Diagnostic* cross = findDiagnostic(d, "cross-join");
    tst::check(cross != nullptr, "cartesian product detected");
    tst::check(cross && cross->suggestion.find("c.id = o.customer_id") !=
                            std::string::npos,
               "and the FK-derived join condition is suggested");
  }
  {
    auto d = analyzeStatic(schema, "SELECT * FROM customers WHERE id = 'abc'",
                           Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "type-mismatch"),
               "numeric column vs string literal flagged");
  }
  {
    auto d = analyzeStatic(schema, "SELECT * FROM customers WHERE nope = 1",
                           Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "unknown-column"), "unknown column flagged");
  }
  {
    auto d = analyzeStatic(schema, "SELECT * FROM nosuchtable", Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "unknown-table"), "unknown table flagged");
  }
  {
    // Valid, indexed, bounded -- nothing but the SELECT * note.
    auto d = analyzeStatic(
        schema, "SELECT id FROM orders WHERE customer_id = 1 LIMIT 10",
        Dialect::Sqlite);
    tst::check(!hasDiagnostic(d, "missing-index"),
               "indexed predicate produces no missing-index finding");
    tst::check(!hasDiagnostic(d, "cross-join"), "single table is not a join");
  }

  // Rules that depend on table size need a big table.
  makeLargeTable(*driver);
  schema = driver->introspect();
  {
    auto d = analyzeStatic(
        schema, "SELECT * FROM events WHERE DATE(created_at) = '2024-01-01'",
        Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "non-sargable-function"),
               "function-wrapped predicate flagged as non-sargable");
  }
  {
    auto d = analyzeStatic(schema, "SELECT * FROM events WHERE kind LIKE '%x'",
                           Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "leading-wildcard"),
               "leading-wildcard LIKE flagged");
  }
  {
    auto d = analyzeStatic(schema, "SELECT id FROM events WHERE kind = 'k1'",
                           Dialect::Sqlite);
    const Diagnostic* missing = findDiagnostic(d, "missing-index");
    tst::check(missing != nullptr, "unindexed predicate on a big table flagged");
    tst::check(missing && missing->suggestion.find("CREATE INDEX") !=
                              std::string::npos,
               "and a CREATE INDEX statement is offered");
    if (missing) tst::note(missing->suggestion);
  }
  {
    auto d = analyzeStatic(schema, "SELECT id FROM events ORDER BY created_at",
                           Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "unbounded-sort"),
               "large unbounded ORDER BY flagged");
  }

  // ------------------------------------------------------------ plan analysis
  tst::section("EXPLAIN plan analysis");
  {
    const std::string sql = "SELECT id FROM events WHERE kind = 'k1'";
    ResultSet plan = driver->explain(sql);
    tst::check(!plan.rows.empty(), "SQLite returned a plan");
    auto d = analyzePlan(schema, plan, sql, Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "plan-full-scan"),
               "full scan read out of the plan");
  }
  {
    const std::string sql = "SELECT id FROM events ORDER BY created_at";
    ResultSet plan = driver->explain(sql);
    auto d = analyzePlan(schema, plan, sql, Dialect::Sqlite);
    tst::check(hasDiagnostic(d, "plan-temp-btree"),
               "temporary B-tree sort read out of the plan");
  }
  {
    // The suggested index should actually remove the finding.
    driver->execute("CREATE INDEX idx_events_kind ON events(kind)", 0);
    Schema after = driver->introspect();
    const std::string sql = "SELECT id FROM events WHERE kind = 'k1'";
    auto stat = analyzeStatic(after, sql, Dialect::Sqlite);
    tst::check(!hasDiagnostic(stat, "missing-index"),
               "adding the suggested index clears the static finding");
    auto d = analyzePlan(after, driver->explain(sql), sql, Dialect::Sqlite);
    tst::check(!hasDiagnostic(d, "plan-full-scan"),
               "and the planner stops scanning");
  }

  // ----------------------------------------------------------------- values
  tst::section("value handling");
  {
    ResultSet rs = driver->execute(
        "SELECT NULL, 42, 1.5, 'text', X'00FF'", 0);
    tst::check(rs.rows.size() == 1 && rs.rows[0].size() == 5, "five columns");
    const Row& r = rs.rows[0];
    tst::check(isNull(r[0]), "NULL round-trips");
    tst::check(std::holds_alternative<int64_t>(r[1]), "integer typed");
    tst::check(std::holds_alternative<double>(r[2]), "real typed");
    tst::check(std::holds_alternative<std::string>(r[3]), "text typed");
    tst::check(std::holds_alternative<Blob>(r[4]), "blob typed");
    tst::check(toSqlLiteral(r[3]) == "'text'", "text literal quoting");
    tst::check(toSqlLiteral(r[4]) == "X'00FF'", "blob literal is hex");
  }
  {
    // Quote escaping has to survive a round trip through the database.
    driver->execute("CREATE TABLE quoting (v TEXT)", 0);
    driver->execute("INSERT INTO quoting VALUES ('it''s \"quoted\"')", 0);
    ResultSet rs = driver->execute("SELECT v FROM quoting", 0);
    const std::string literal = toSqlLiteral(rs.rows[0][0]);
    ResultSet back = driver->execute("SELECT " + literal, 0);
    tst::check(toDisplayString(back.rows[0][0]) ==
                   toDisplayString(rs.rows[0][0]),
               "escaped literal round-trips through the engine");
  }

  // ---------------------------------------------------------------- limits
  tst::section("row cap");
  {
    ResultSet rs = driver->execute("SELECT * FROM events", 5);
    tst::check(rs.rows.size() == 5 && rs.truncated,
               "row cap stops fetching and sets the flag");
    ResultSet all = driver->execute("SELECT COUNT(*) FROM events", 0);
    tst::check(std::get<int64_t>(all.rows[0][0]) == 60000,
               "cap does not affect the data itself");
  }
  {
    ResultSet rs = driver->execute("UPDATE orders SET status='paid'", 0);
    tst::check(rs.rowsAffected == 3 && !rs.isSelect(),
               "DML reports rows affected");
  }

  tst::section("error reporting");
  {
    bool threw = false;
    try {
      driver->execute("SELECT nope FROM nowhere", 0);
    } catch (const DbError& e) {
      threw = true;
      tst::note(e.what());
    }
    tst::check(threw, "bad SQL raises DbError");
  }

  driver->disconnect();
  std::filesystem::remove(path);
  return tst::report();
}
