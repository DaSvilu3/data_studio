# Data Studio

A native macOS desktop database client in C++20 and Qt 6, for MySQL and SQLite.
Its point of difference is that it reads your schema and uses it: completion,
join generation, and query analysis are all driven by real tables, columns,
foreign keys and indexes rather than by a generic SQL grammar.

## What it does

**Schema-aware editor.** Completion is driven by the live schema and knows where
the caret is. After `u.` it offers only that table's columns; in a `FROM` clause
it offers tables; with several tables in scope it inserts the *qualified* column
so the query stays unambiguous. Foreign keys become one-keystroke `JOIN … ON …`
suggestions with the condition already filled in, and the highlighter marks
identifiers that don't exist in the schema before you ever run the statement.

**Query analyzer.** Two layers. Static rules check the statement against the
schema with no database round trip — unknown tables and columns, `UPDATE`/
`DELETE` with no `WHERE`, cartesian products (with the missing join condition
derived from the FK graph and offered as the fix), predicates that can't use an
index (`DATE(created_at) = …`, `LIKE '%foo'`), unindexed join columns, numeric-
vs-string comparisons that silently defeat an index, unbounded `ORDER BY` over a
large table. Then `EXPLAIN` findings layer on top: full scans, filesorts,
temporary tables, block nested-loop joins, low-selectivity access. Findings that
have a fix carry a runnable `CREATE INDEX` you can insert with a double-click.

**Visual query builder.** Pick tables; the joins are worked out from the foreign
key graph, including pulling in junction tables you didn't select. Tick columns,
add filters and aggregates, and read the SQL as it's generated. Values are quoted
according to each column's declared type, and one-to-many hops become `LEFT JOIN`
with a warning that they can multiply rows.

**Natural language to SQL.** Ask a question in English and get SQL back for
review — it is never run automatically. Generation runs either against a **local
model** (LM Studio, Ollama, llama.cpp, vLLM — nothing leaves the machine at all)
or the Claude API. Only schema metadata is ever sent: names, types, keys and
indexes. No row data leaves the machine under either provider.

## Building

Requires macOS, CMake ≥ 3.21, and a C++20 compiler.

```sh
brew install qt sqlite mysql-client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/data-studio.app
```

MySQL support is optional: if `libmysqlclient` isn't found at configure time the
build prints a warning, `DS_HAVE_MYSQL` is set to 0, and SQLite-only binaries are
produced.

### Tests

```sh
cd build && ctest -V
```

`core` exercises the drivers, introspection and query intelligence against a
throwaway SQLite database — including a round trip proving a suggested
`CREATE INDEX` actually removes the finding and changes the query plan. `ui`
covers the threaded session, the results model's CSV/`INSERT` exports and the
editor, headless via Qt's offscreen platform plugin.

### Trying it out

```sh
sqlite3 demo.db < examples/demo.sql
```

Add `demo.db` as a SQLite connection. `orders.status` is deliberately left
unindexed, so `SELECT * FROM orders WHERE status = 'paid'` gives the analyzer
something to say.

## Architecture

Two libraries and a thin executable, so the interesting logic is testable
without a GUI.

```
src/
  core/          dscore — no Qt at all
    Value.h        cell values (std::variant), ResultSet, DbError, Dialect
    Schema.h       Table / Column / Index / ForeignKey, type classification
    Driver.h       the driver interface + statement splitter
    sqlite/        SqliteDriver   — sqlite3 C API
    mysql/         MySqlDriver    — libmysqlclient
  query/         dscore — schema-driven query intelligence
    SqlLexer       tokenizer shared by the highlighter and everything below
    SqlContext     what is the caret in? which tables are in scope? completion
    JoinGraph      FK graph, shortest join path, multi-table join planning
    QueryBuilder   QuerySpec -> SQL (pure; no database access)
    Analyzer       static schema rules + EXPLAIN interpretation
  ai/            dsui
    SqlGenerator   provider interface + the tolerant model-reply parser
    SchemaPrompt   schema -> DDL prompt, with relevance pruning for small models
    LocalSqlGenerator   any OpenAI-compatible server (LM Studio, Ollama, …)
    ClaudeSqlGenerator  Claude Messages API, structured outputs
    AiSettings     provider config + local-server discovery
  ui/            dsui
    QueryExecutor  ConnectionSession: driver on a worker thread, queued signals
    MainWindow, SqlEditor, SchemaTree, ResultsView, AnalyzerPanel,
    QueryBuilderPanel, ConnectionDialog, ConnectionStore, …
tests/           core_tests.cpp, ui_tests.cpp
```

### Notes on some decisions

**Drivers are written directly against `sqlite3` and `libmysqlclient` rather
than Qt SQL.** Homebrew's Qt ships only the SQLite plugin — there is no `QMYSQL`
— and going native also buys real query cancellation, streaming result reads,
and full control over introspection.

**`dscore` has no Qt dependency.** Values are a `std::variant`, not `QVariant`,
so the whole query-intelligence layer is plain C++ and testable on its own.

**Queries run on a per-connection worker thread.** `ConnectionSession` owns the
thread and forwards calls as queued invocations; results come back as signals.
`cancel()` is the single documented cross-thread entry point — SQLite uses
`sqlite3_interrupt`, MySQL opens a second connection and issues `KILL QUERY`,
which is what the `mysql` CLI does for Ctrl-C.

**MySQL reads results with `mysql_use_result`,** streaming rather than buffering,
so a stray `SELECT *` on a huge table hits the row cap instead of exhausting
memory. `DECIMAL` is deliberately kept as text — converting it to `double` would
throw away the precision the column type exists to provide.

**Introspection is one query per metadata kind, not per table.** On a schema with
a few hundred tables the per-table version takes seconds.

**Passwords go to the macOS Keychain,** never to the preferences file. Everything
else about a connection is stored in `QSettings`.

## Natural-language SQL setup

Open **Query ▸ AI settings…** and pick a provider. The whole feature is optional;
with it off, nothing else in the application changes.

### Local model (default, fully offline)

Press **Detect** and it probes the usual ports — LM Studio (1234), Ollama
(11434), llama.cpp (8080), vLLM (8000) — and lists the models each is serving.
Pick one and press **Test** to make a real round trip against a throwaway schema.
Any OpenAI-compatible `/chat/completions` server works.

A 3B instruction model is genuinely enough for everyday questions; a 7B coding
model does better on multi-table joins. Two things make small models workable:

- **Schema pruning.** A 3B model has a small context window, so above the
  *schema limit* setting (25 tables by default) only the tables relevant to the
  question are sent, plus their foreign-key neighbours so the joins remain
  expressible. Set it to 0 to always send everything.
- **Tolerant parsing.** The request asks for a JSON schema, which LM Studio and
  vLLM honour. When a model ignores it, the reply is still recovered: markdown
  fences are stripped, JSON is extracted from surrounding prose, unescaped
  newlines inside string values are repaired, and failing all that a bare `sql`
  block or statement is lifted out. A `destructive` flag the model understated is
  re-derived from the SQL itself.

### Claude API

Set `ANTHROPIC_API_KEY` in the environment, or paste a key into the same dialog
to store it in the Keychain. Requests use `claude-opus-5` with structured
outputs, so the response is a validated object rather than prose to be scraped.

Under both providers the generated SQL is inserted into the editor with its
explanation as a comment and the provider named, and statements that write data
are flagged before you run them.

## Keyboard

| | |
|---|---|
| `⌘↩` | Run the statement under the cursor |
| `⇧⌘↩` | Run the whole script |
| `⌘E` | `EXPLAIN` the current statement |
| `⌘.` | Cancel the running query |
| `⌃Space` | Completions |
| `F5` | Refresh schema |

## Status

Everything described above works and is covered by the test suite. Known gaps:
result grids are read-only (no in-place editing), there is no data export to file
beyond clipboard CSV/`INSERT`, and only macOS has been exercised — the Keychain
code is `#ifdef`'d and degrades to "don't save passwords" elsewhere.
