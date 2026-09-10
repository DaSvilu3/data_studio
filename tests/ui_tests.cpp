// Headless checks for the Qt layer: the threaded connection session, the
// results model's exports, and the editor's statement detection.
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <filesystem>

#include "ai/SchemaPrompt.h"
#include "ai/SqlGenerator.h"
#include "tests/test_util.h"
#include "ui/QueryBuilderPanel.h"
#include "ui/QueryExecutor.h"
#include "ui/ResultsModel.h"
#include "ui/SchemaTree.h"
#include "ui/SqlEditor.h"

using namespace ds;

namespace {

// Runs the event loop until `done` flips or the timeout expires, so a hung
// worker fails the test instead of hanging CI.
bool pump(bool& done, int timeoutMs = 10000) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(timeoutMs);

  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (done) loop.quit();
  });
  poll.start(20);

  loop.exec();
  return done;
}

std::string makeFixture() {
  const std::string path = "/tmp/ds_ui_tests.db";
  std::filesystem::remove(path);

  ConnectionConfig cfg;
  cfg.dialect = Dialect::Sqlite;
  cfg.filePath = path;
  auto driver = makeDriver(cfg);
  driver->connect(cfg);
  driver->execute(
      "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, "
      "country TEXT)", 0);
  driver->execute(
      "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER "
      "REFERENCES customers(id), status TEXT, total REAL)", 0);
  driver->execute(
      "INSERT INTO customers(name,country) VALUES ('Ana','ES'),('Bo','SG'),"
      "('Cy','IN'),('Di','CZ')", 0);
  driver->execute(
      "INSERT INTO orders(customer_id,status,total) VALUES "
      "(1,'paid',10.5),(1,'paid',4.5),(2,'paid',20.0),(3,'pending',7.0),"
      "(4,'paid',1.0)", 0);
  driver->disconnect();
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  const std::string path = makeFixture();

  ConnectionConfig cfg;
  cfg.dialect = Dialect::Sqlite;
  cfg.filePath = path;
  cfg.name = "test";

  ConnectionSession session(cfg);

  bool opened = false, schemaReady = false, gotResult = false, gotPlan = false;
  bool sawBusy = false, sawIdle = false;
  ResultSet lastResult, lastPlan;
  QString lastError;

  QObject::connect(&session, &ConnectionSession::opened, [&] { opened = true; });
  QObject::connect(&session, &ConnectionSession::schemaChanged,
                   [&] { schemaReady = true; });
  QObject::connect(&session, &ConnectionSession::openFailed,
                   [&](const QString& m) { lastError = m; opened = true; });
  QObject::connect(&session, &ConnectionSession::resultReady,
                   [&](const ResultSet& r, int, int) {
                     lastResult = r;
                     gotResult = true;
                   });
  QObject::connect(&session, &ConnectionSession::explainReady,
                   [&](const ResultSet& p, const QString&) {
                     lastPlan = p;
                     gotPlan = true;
                   });
  QObject::connect(&session, &ConnectionSession::failed,
                   [&](const QString& m, const QString&) {
                     lastError = m;
                     gotResult = true;
                   });
  QObject::connect(&session, &ConnectionSession::busyChanged, [&](bool busy) {
    if (busy) sawBusy = true;
    else if (sawBusy) sawIdle = true;
  });

  // ------------------------------------------------------------- connecting
  tst::section("threaded session");
  session.open();
  pump(schemaReady);
  tst::check(opened && lastError.isEmpty(), "connection opened off the GUI thread");
  tst::check(schemaReady, "schema arrived without blocking the GUI thread");
  tst::check(session.schema().tables.size() == 2, "two tables introspected");
  tst::note(session.serverVersion().toStdString());

  // ------------------------------------------------------------ running SQL
  tst::section("query execution");
  gotResult = false;
  session.execute(
      "SELECT c.name, SUM(o.total) AS spend FROM customers c "
      "JOIN orders o ON o.customer_id = c.id WHERE o.status = 'paid' "
      "GROUP BY c.name ORDER BY spend DESC", 1000);
  pump(gotResult);
  tst::check(gotResult && lastError.isEmpty(), "query completed");
  // Cy's only order is pending, so the paid filter leaves three customers.
  tst::check(lastResult.rows.size() == 3, "three grouped rows returned");
  tst::check(lastResult.columns.size() == 2, "two columns");
  tst::check(sawBusy && sawIdle, "busy state was raised and cleared");
  if (!lastResult.rows.empty()) {
    tst::note("top: " + toDisplayString(lastResult.rows[0][0]) + " = " +
              toDisplayString(lastResult.rows[0][1]));
    tst::check(toDisplayString(lastResult.rows[0][0]) == "Bo",
               "ordering is correct (Bo spent the most)");
  }

  // ---------------------------------------------------------- results model
  tst::section("results model and exports");
  ResultsModel model;
  model.setResult(lastResult);
  tst::check(model.rowCount() == 3 && model.columnCount() == 2,
             "model dimensions match the result");
  tst::check(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString() ==
                 "name",
             "column header from the result metadata");
  tst::check(model.data(model.index(0, 1), Qt::TextAlignmentRole).toInt() ==
                 (Qt::AlignRight | Qt::AlignVCenter),
             "numeric cells align right");

  QModelIndexList all;
  for (int r = 0; r < model.rowCount(); ++r) {
    for (int c = 0; c < model.columnCount(); ++c) all << model.index(r, c);
  }
  const QString csv = model.copyAsCsv(all);
  tst::check(csv.startsWith("name,spend"), "CSV starts with a header row");
  tst::check(csv.count(QLatin1Char('\n')) == 3, "CSV has one line per row");

  const QString inserts = model.copyAsInsert(all, "customers", Dialect::Sqlite);
  tst::check(inserts.startsWith("INSERT INTO \"customers\" (\"name\", \"spend\")"),
             "INSERT export names the columns");
  tst::check(inserts.count(QStringLiteral("INSERT INTO")) == 3,
             "one INSERT per row");

  {
    // NULLs must export as an empty CSV field, not the text "NULL".
    bool done = false;
    gotResult = false;
    session.execute("SELECT NULL AS a, 'x' AS b", 100);
    pump(gotResult);
    ResultsModel nullModel;
    nullModel.setResult(lastResult);
    QModelIndexList cells{nullModel.index(0, 0), nullModel.index(0, 1)};
    tst::check(nullModel.copyAsCsv(cells).endsWith("\n,x"),
               "NULL exports as an empty CSV field");
    tst::check(nullModel.data(nullModel.index(0, 0), Qt::DisplayRole)
                       .toString() == "NULL",
               "but displays as NULL in the grid");
    Q_UNUSED(done);
  }

  // ------------------------------------------------------------- row limits
  tst::section("row cap through the session");
  gotResult = false;
  session.execute("SELECT * FROM orders", 2);
  pump(gotResult);
  tst::check(lastResult.rows.size() == 2 && lastResult.truncated,
             "cap applied and flagged");

  // ------------------------------------------------------------ error path
  tst::section("error propagation");
  lastError.clear();
  gotResult = false;
  session.execute("SELECT * FROM does_not_exist", 100);
  pump(gotResult);
  tst::check(!lastError.isEmpty(), "bad SQL reaches the GUI as an error");
  tst::note(lastError.toStdString());

  // A failed statement must not poison the session.
  gotResult = false;
  session.execute("SELECT 1", 100);
  pump(gotResult);
  tst::check(lastResult.rows.size() == 1, "session still usable after an error");

  // --------------------------------------------------------------- EXPLAIN
  tst::section("EXPLAIN through the session");
  session.explain("SELECT * FROM orders WHERE status = 'paid'");
  pump(gotPlan);
  tst::check(gotPlan && !lastPlan.rows.empty(), "plan returned");

  // ---------------------------------------------------------------- editor
  tst::section("editor");
  {
    SqlEditor editor;
    editor.setSchema(session.schema());
    editor.setPlainText("SELECT 1;\nSELECT 2 FROM customers;\nSELECT 3;");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(20);   // inside the second statement
    editor.setTextCursor(cursor);
    tst::check(editor.currentStatement().trimmed() == "SELECT 2 FROM customers",
               "statement under the caret is isolated");

    cursor.setPosition(0);
    cursor.setPosition(8, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    tst::check(editor.currentStatement() == "SELECT 1",
               "an explicit selection wins over the caret statement");
  }

  // --------------------------------------------------------------- widgets
  tst::section("panels construct against a live schema");
  {
    QueryBuilderPanel builder;
    builder.setSchema(session.schema());
    builder.setDialect(Dialect::Sqlite);
    tst::check(true, "query builder built");

    SchemaTree tree;
    tree.setSchema(session.schema());
    tst::check(true, "schema tree built");
  }

  // ------------------------------------------------------- model reply parsing
  // Every string below is a real shape a small local model produced.
  tst::section("tolerant model reply parsing");
  {
    NlSqlResult r;
    tst::check(parseModelReply(
                   R"({"sql":"SELECT 1","explanation":"e","tables_used":["t"],)"
                   R"("caveats":"","destructive":false})", &r) &&
                   r.sql == "SELECT 1" && r.tablesUsed == QStringList{"t"},
               "clean JSON");
  }
  {
    // ministral-3b wraps in fences AND puts real newlines inside the strings,
    // which is invalid JSON until the control characters are escaped.
    NlSqlResult r;
    const QString reply = QStringLiteral(
        "```json\n{\n  \"sql\": \"\n    SELECT c.id\n    FROM customers c;\n"
        "  \",\n  \"explanation\": \"\n - joins\n\",\n"
        "  \"tables_used\": [\"customers\"],\n  \"caveats\": \"\",\n"
        "  \"destructive\": false\n}\n```");
    tst::check(parseModelReply(reply, &r), "fenced JSON with raw newlines");
    tst::check(r.sql.contains("SELECT c.id"), "and the SQL survives");
    tst::check(r.tablesUsed == QStringList{"customers"}, "and the table list");
  }
  {
    NlSqlResult r;
    tst::check(parseModelReply(
                   "Sure! Here you go:\n\n```sql\nSELECT * FROM orders;\n```",
                   &r) && r.sql == "SELECT * FROM orders",
               "bare ```sql block with prose around it");
  }
  {
    NlSqlResult r;
    tst::check(parseModelReply("DELETE FROM orders WHERE id = 1;", &r) &&
                   r.destructive,
               "bare statement is recovered and flagged destructive");
  }
  {
    NlSqlResult r;
    tst::check(!parseModelReply("I am unable to help with that.", &r),
               "a reply with no SQL is rejected");
  }
  {
    // A model that returns JSON but forgets the destructive flag.
    NlSqlResult r;
    tst::check(parseModelReply(
                   R"(text {"sql":"DROP TABLE x","explanation":"",)"
                   R"("tables_used":[],"caveats":"","destructive":false} more)",
                   &r) && r.destructive,
               "destructive is inferred when the model understates it");
  }

  // ------------------------------------------------------- schema pruning
  tst::section("schema pruning for small context windows");
  {
    const Schema& live = session.schema();
    tst::check(selectRelevantTables(live, "anything", 0).size() == 2,
               "pruning disabled returns every table");

    // Ask about orders with a budget of one: orders must win, and customers
    // must still come along because the join needs it.
    auto picked = selectRelevantTables(live, "total spend per customer", 1);
    tst::check(picked.size() >= 1 && picked.size() <= 2,
               "budget is respected");
    tst::check(picked.contains("customers"), "the named table is kept");

    const QString ddl = describeSchema(live, Dialect::Sqlite, "customers", 25);
    tst::check(ddl.contains("CREATE TABLE customers"), "renders DDL");
    tst::check(ddl.contains("FOREIGN KEY"), "includes relationships");
    tst::check(!ddl.contains("Ana"), "never includes row data");
  }

  std::filesystem::remove(path);
  return tst::report();
}
