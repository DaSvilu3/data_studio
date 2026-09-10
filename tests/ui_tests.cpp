// Headless checks for the Qt layer: the threaded connection session, the
// results model's exports, and the editor's statement detection.
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <filesystem>

#include <QPixmap>

#include "ai/SchemaPrompt.h"
#include "ai/SqlGenerator.h"
#include "tests/test_util.h"
#include "ui/QueryBuilderPanel.h"
#include "ui/QueryExecutor.h"
#include "ui/ResultsModel.h"
#include "ui/SchemaTree.h"
#include "ui/MainWindow.h"
#include "ui/SqlEditor.h"
#include "ui/Theme.h"

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

    // Caret on the blank line after the final semicolon: running nothing looks
    // like the keystroke was ignored, so fall back to the statement above.
    editor.setPlainText("SELECT 1;\nSELECT 2 FROM customers;\n");
    QTextCursor tail = editor.textCursor();
    tail.movePosition(QTextCursor::End);
    editor.setTextCursor(tail);
    tst::check(editor.currentStatement().trimmed() == "SELECT 2 FROM customers",
               "caret past the last semicolon runs the statement above it");

    editor.setPlainText("   \n\n  ");
    tail.movePosition(QTextCursor::End);
    editor.setTextCursor(tail);
    tst::check(editor.currentStatement().trimmed().isEmpty(),
               "an empty document still yields nothing");
  }

  // ------------------------------------------------------------- formatting
  tst::section("SQL formatting");
  {
    // The exact one-liner llama-3.2-3b produced: unreadable without wrapping.
    const QString oneLine =
        "SELECT p.name, SUM(oi.qty) AS total_qty FROM order_items oi JOIN "
        "products p ON oi.product_id = p.id GROUP BY p.name ORDER BY "
        "total_qty DESC LIMIT 200";
    const QString formatted = SqlEditor::formatSql(oneLine);
    std::cout << "\n" << formatted.toStdString() << "\n\n";

    tst::check(formatted.count(QLatin1Char('\n')) >= 5,
               "long statement is broken across clauses");
    tst::check(formatted.startsWith("SELECT p.name"), "SELECT list intact");
    tst::check(formatted.contains("\nFROM order_items oi"),
               "FROM starts its own line");
    tst::check(formatted.contains("\n  JOIN products p"),
               "JOIN is indented under the clause");
    tst::check(formatted.contains("\n  ON oi.product_id = p.id"),
               "ON is indented too");
    tst::check(formatted.contains("\nGROUP BY p.name"),
               "GROUP BY stays together on one line");
    tst::check(formatted.contains("\nORDER BY total_qty DESC"),
               "ORDER BY stays together");
    tst::check(!formatted.contains(" ,"), "no space before a comma");
    tst::check(formatted.contains("SUM(oi.qty)"),
               "function call is not pulled apart");

    // Formatting must not change what the statement means.
    tst::check(SqlEditor::formatSql(formatted) == formatted,
               "formatting is idempotent");
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

  // ----------------------------------------------- cross-database recovery
  // Regression: switching database left the sidebar showing the previous
  // database's tables while the switch was still in flight, so clicking one
  // ran it against the database we had just moved to. The switch must report
  // busy like every other worker operation, and a statement naming a table
  // that lives in another database must be explainable rather than just
  // failing with the server's raw message.
  if (const char* dsn = std::getenv("DS_TEST_MYSQL")) {
    tst::section("switching database");

    QStringList parts = QString::fromUtf8(dsn).split(QLatin1Char(':'));
    if (parts.size() >= 4) {
      ConnectionConfig my;
      my.dialect = Dialect::MySql;
      my.host = parts[0].toStdString();
      my.port = parts[1].toInt();
      my.user = parts[2].toStdString();
      my.password = parts[3].toStdString();

      ConnectionSession mysql(my);
      bool ready = false, busySeen = false, busyCleared = false;
      QStringList located;
      QString locatedTable;

      QObject::connect(&mysql, &ConnectionSession::schemaChanged,
                       [&] { ready = true; });
      QObject::connect(&mysql, &ConnectionSession::openFailed,
                       [&](const QString&) { ready = true; });
      QObject::connect(&mysql, &ConnectionSession::busyChanged,
                       [&](bool busy) {
                         if (busy) busySeen = true;
                         else if (busySeen) busyCleared = true;
                       });
      QObject::connect(&mysql, &ConnectionSession::tableLocated,
                       [&](const QString& t, const QStringList& dbs) {
                         locatedTable = t;
                         located = dbs;
                       });

      mysql.open();
      pump(ready, 20000);
      tst::check(mysql.isOpen(), "connected to the live server");

      const QStringList databases = mysql.databases();
      tst::check(databases.size() >= 2,
                 "server has at least two databases to switch between");

      if (mysql.isOpen() && databases.size() >= 2) {
        // Find a table that exists in one database but not another.
        QString uniqueTable, homeDb, otherDb;
        for (const QString& db : databases) {
          ready = false;
          mysql.useDatabase(db);
          pump(ready, 30000);
          if (mysql.schema().tables.empty()) continue;
          if (homeDb.isEmpty()) {
            homeDb = db;
            uniqueTable = QString::fromStdString(mysql.schema().tables[0].name);
          } else if (!mysql.schema().findTable(uniqueTable.toStdString())) {
            otherDb = db;
            break;
          }
        }
        tst::check(!homeDb.isEmpty() && !otherDb.isEmpty(),
                   "found a table present in one database and absent in another");
        tst::note(uniqueTable.toStdString() + " is in " + homeDb.toStdString() +
                  " but not " + otherDb.toStdString());

        // We are now on otherDb. Busy must have been raised and cleared by the
        // switch itself -- that is the bug that let a stale click through.
        tst::check(busySeen && busyCleared,
                   "switching database reports busy and then idle");

        if (!uniqueTable.isEmpty()) {
          mysql.locateTable(uniqueTable);
          bool got = false;
          QEventLoop wait;
          QTimer::singleShot(8000, &wait, &QEventLoop::quit);
          QObject::connect(&mysql, &ConnectionSession::tableLocated, &wait,
                           [&](const QString&, const QStringList&) {
                             got = true;
                             wait.quit();
                           });
          wait.exec();
          tst::check(got && located.contains(homeDb),
                     "the missing table is traced back to its real database");
          tst::note("located in: " + located.join(", ").toStdString());
        }
      }
    }
  }

  // ---------------------------------------------------------------- theming
  // The offscreen platform gives a light palette, so this exercises the light
  // branch of the theme that the running app (dark) never reaches here.
  tst::section("theme");
  {
    tst::check(!theme::appStyleSheet().isEmpty(), "stylesheet generated");
    tst::check(theme::textPrimary() != theme::surface(),
               "text contrasts with the surface it sits on");
    tst::check(theme::icon(QStringLiteral("run")).availableSizes().size() > 0,
               "icons render to a pixmap");
    tst::check(!theme::icon(QStringLiteral("table")).isNull(), "table icon");
    tst::check(!theme::statusDot(theme::success()).isNull(), "status dot");

    // Render the real window offscreen and confirm it actually paints, which
    // catches a stylesheet that blanks the UI in the theme we don't run in.
    qApp->setStyleSheet(theme::appStyleSheet());
    MainWindow window;
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();

    const QPixmap shot = window.grab();
    tst::check(!shot.isNull() && shot.width() > 0, "main window renders");

    const QImage image = shot.toImage();
    QSet<QRgb> distinct;
    for (int y = 0; y < image.height(); y += 7) {
      for (int x = 0; x < image.width(); x += 7) distinct.insert(image.pixel(x, y));
    }
    // A single flat colour would mean the stylesheet painted over everything.
    tst::check(distinct.size() > 12, "window is not a blank field of colour");
    tst::note(std::to_string(distinct.size()) + " distinct colours sampled");

    if (const char* out = std::getenv("DS_TEST_SHOT")) {
      shot.save(QString::fromUtf8(out));
      tst::note(std::string("saved ") + out);
    }
  }

  std::filesystem::remove(path);
  return tst::report();
}
