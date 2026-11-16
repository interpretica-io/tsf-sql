# tsf-sql

tsf-sql lets a Test Environment (TE) test work with SQL databases:
connect, run statements, bind typed parameters, read results in bounded
batches, control transactions, read the schema, check the state of the
data, coordinate concurrent sessions, measure, and read a server's
security posture. It is an external TE repository, consumed with the
`TE_EXT_REPO` builder directive.

The database is reached from a Test Agent. The connection, its prepared
statements and its transaction live in the agent's RPC server, over the
engine's own client library, linked in - no `psql`, `mysql` or `sqlite3`
program is run and nothing is scraped. The test holds an opaque
connection and drives it through one API whatever the engine.

| Engine | Driver | Status |
|---|---|---|
| PostgreSQL | libpq | supported |
| SQLite | SQLite C API | supported |
| MySQL / MariaDB | MariaDB Connector/C | supported |
| Microsoft SQL Server | ODBC | planned |
| DuckDB | DuckDB C API | planned |
| ClickHouse | HTTP / native | planned |

Every driver is optional at build time: a driver is compiled in when its
client library is found, the module builds with any one of them, and
`tapi_sql_supports()` tells a test which engines the agent has.

## Libraries

- `ta_sql` (agent side) keeps the connections and statements, keyed by
  small integers, and everything engine-neutral: the value format, the
  bounded fetch, transaction control, the asynchronous statements, the
  redaction of secrets (`ta_sql.c`) and schema introspection
  (`ta_sql_meta.c`). An engine is one translation unit behind the driver
  interface `ta_sql_drv.h`: `ta_sql_sqlite.c`, `ta_sql_pg.c`,
  `ta_sql_mysql.c`.
- `rpcs_sql` holds the `sql_*` RPCs, thin wrappers over `ta_sql`.
- `tapi_sql` (engine side): `tapi_sql.h` is the API tests use;
  `tapi_sql_assert.h` the database assertions, `tapi_sql_perf.h` the
  measurements, `tapi_sql_audit.h` the security checks,
  `tapi_sql_rpc.h` the layer with one function per RPC.

tsf-sql depends on TE and the three client libraries only. It does not
depend on tsf-data or tsf-ml. tsf-cybersec is optional (see Security
checks).

## Example

```c
tapi_sql_config cfg = {
    .engine = TAPI_SQL_ENGINE_POSTGRES,
    .host = "db.lab", .port = 5432, .dbname = "shop",
    .user = "tester", .password = getenv("SHOP_DB_PASSWORD"),
    .tls_mode = TAPI_SQL_TLS_VERIFY_FULL, .tls_ca = "/etc/lab/ca.pem",
};
tapi_sql_conn  *db = NULL;
tapi_sql_stmt  *st = NULL;
tapi_sql_result rows = TAPI_SQL_RESULT_INIT;
bool            eof = false;

if (!tapi_sql_supports(pco, cfg.engine))
    TEST_SKIP("The agent has no PostgreSQL driver");

CHECK_RC(tapi_sql_connect(pco, &cfg, &db));

CHECK_RC(tapi_sql_begin(db));
CHECK_RC(tapi_sql_prepare(db, "INSERT INTO orders (id, customer, total) "
                              "VALUES (?, ?, ?)", &st));
CHECK_RC(tapi_sql_bind(st, 1, tapi_sql_int(1001)));
CHECK_RC(tapi_sql_bind(st, 2, tapi_sql_text("o'brien")));
CHECK_RC(tapi_sql_bind(st, 3, tapi_sql_decimal("149.90")));
CHECK_RC(tapi_sql_stmt_execute(st, NULL));
CHECK_RC(tapi_sql_commit(db));
tapi_sql_statement_free(st);

CHECK_RC(tapi_sql_assert_row_count(db, "orders WHERE customer = 'o''brien'", 1));
CHECK_RC(tapi_sql_assert_referential(db, "order_lines", "order_id",
                                     "orders", "id"));

CHECK_RC(tapi_sql_prepare(db, "SELECT id, total FROM orders ORDER BY id", &st));
CHECK_RC(tapi_sql_stmt_execute(st, NULL));
while (!eof)
{
    CHECK_RC(tapi_sql_fetch(st, 500, &rows, &eof));   /* 500 rows at a time */
    /* ... tapi_sql_result_cell(&rows, row, col) ... */
}
tapi_sql_result_free(&rows);
tapi_sql_statement_free(st);
tapi_sql_disconnect(db);
```

## Operations

| Call | What it does |
|---|---|
| `tapi_sql_connect` / `_connect_err` | Establish a connection; the second hands back why it failed |
| `tapi_sql_disconnect` / `_reconnect` | Close it; close and open again with the same parameters |
| `tapi_sql_ping` | One round trip to the server |
| `tapi_sql_capabilities` / `_has_cap` | Driver features, isolation levels, TLS state, server version |
| `tapi_sql_execute` / `_execute_fmt` | Run a statement, discard rows, report rows changed |
| `tapi_sql_query` / `_query_params` / `_query_int` | Run a statement and bring back all rows / with parameters / one number |
| `tapi_sql_prepare` / `_bind` / `_stmt_execute` | Prepared statement with typed parameters, written `?` |
| `tapi_sql_fetch` / `_fetch_bounded` | The next batch, bounded in rows (and bytes) |
| `tapi_sql_statement_free` | Release a statement |
| `tapi_sql_begin` / `_begin_iso` / `_commit` / `_rollback` | Transactions, optionally at an isolation level |
| `tapi_sql_savepoint` / `_rollback_to` / `_release` | Savepoints |
| `tapi_sql_tables` / `_columns` / `_indexes` / `_constraints` / `_foreign_keys` | Schema introspection, normalised |
| `tapi_sql_set_timeout` | A statement or lock-wait deadline |
| `tapi_sql_execute_start` / `_execute_wait` / `_cancel` | Start a statement, look or wait for it, cancel it |
| `tapi_sql_stats` | Connections and statements the agent still holds |

A failure of the database comes back as a TE status and, on the
connection, as a normalised error (`tapi_sql_last_error()`): an error
class that is the same on every engine - syntax, not found, constraint,
deadlock, serialization, lock timeout, timeout, cancelled, connection
lost, authentication, TLS, permission, connect - plus the engine's own
SQLSTATE, code and message.

## Values

| Type | Constructor | Notes |
|---|---|---|
| NULL | `tapi_sql_null()` | |
| Boolean | `tapi_sql_bool()` | An integer 0/1 where the engine has no boolean |
| Signed integer | `tapi_sql_int()` | 64-bit, exact |
| Unsigned integer | `tapi_sql_uint()` | Full range where the engine has it (`TAPI_SQL_CAP_UINT64`) |
| Floating point | `tapi_sql_float()` | Round-trips a double (17 significant digits, inf, nan) |
| Decimal | `tapi_sql_decimal()` | Its digits, never through a double |
| Text | `tapi_sql_text()` | UTF-8 |
| Binary | `tapi_sql_binary()` | Any bytes, NUL included |
| Date / time | `tapi_sql_datetime()` | As the engine prints it |
| JSON | `tapi_sql_json()` | Text; the engine's JSON type where it has one |

Parameters are written `?` on every engine (the PostgreSQL driver
renumbers them to `$1`..; write `??` for a literal question mark). That
is the only thing translated. tsf-sql is not an SQL dialect layer: the
statement a test writes is the statement the database gets.

A result is never materialised whole unless the test asks for that with
`tapi_sql_query()`. `tapi_sql_fetch()` returns one bounded batch; the
agent reads the rows from the driver as they are asked for (libpq
single-row mode, an unbuffered MariaDB statement, `sqlite3_step`).
PostgreSQL and MariaDB carry one open result per connection: sending
another command on the connection drops the rest of an open result
(`TAPI_SQL_CAP_MULTI_CURSOR` says when that is not so).

## Transactions

Supported isolation levels are reported by `tapi_sql_capabilities()`;
asking for one the engine lacks is `TE_EOPNOTSUPP`.

| | Levels | Default | Writers |
|---|---|---|---|
| SQLite | serializable | serializable | One at a time, database-wide: a second writer gets "busy" at once, or after the lock deadline |
| PostgreSQL | read uncommitted (= read committed), read committed, repeatable read, serializable | read committed | Row locks; a blocked writer waits; deadlocks detected |
| MariaDB | all four | repeatable read | Row locks; a blocked writer waits; deadlocks detected |

What the levels do, as tsf-sql-ts observes it (yes = the anomaly
happens):

| | dirty read | non-repeatable read | phantom | lost update |
|---|---|---|---|---|
| SQLite | no | no | no | no |
| PostgreSQL read uncommitted / committed | no | yes | yes | yes |
| PostgreSQL repeatable read / serializable | no | no | no | no (serialization failure) |
| MariaDB read uncommitted | yes | yes | yes | yes |
| MariaDB read committed | no | yes | yes | yes |
| MariaDB repeatable read | no | no | no | yes (no, with `innodb_snapshot_isolation`) |
| MariaDB serializable | no | no | no | no (lock wait or deadlock) |

Savepoints are available on all three. A savepoint name is letters,
digits and underscores; anything else is refused before it reaches the
database.

## Concurrency

A concurrency test opens several connections (on one RPC server or
several) and steps them. Two things make that deterministic:

- every call is one statement on one session and returns before the
  next is sent, so the interleaving is the order of the calls;
- a statement that is expected to block is started with
  `tapi_sql_execute_start()`, which returns at once. The agent runs it
  on a thread; `tapi_sql_execute_wait(conn, ms, &done, ...)` looks at it
  or waits for it. "Started, not done after 500 ms, done after the
  other session commits" is the synchronisation point of a lock test.

Deadlines: `tapi_sql_set_timeout()` sets a statement deadline or a
lock-wait deadline; the statement then fails with `TE_ETIMEDOUT` and the
class `TAPI_SQL_ERR_TIMEOUT` or `TAPI_SQL_ERR_LOCK_TIMEOUT`.
Cancellation: `tapi_sql_cancel()`; the statement fails with
`TE_ECANCELED`. MariaDB counts lock waits in whole seconds.

A plain `tapi_sql_execute()` returns when the statement has finished, so
it must fit in the RPC server's call timeout (ten seconds unless the
test raises `rpcs->timeout`). Longer statements are for
`tapi_sql_execute_start()`.

## Database assertions

`tapi_sql_assert.h`. Each returns 0 when the property holds. When it
does not, it logs what it found (which rows, expected against actual),
emits a TE verdict with a stable text, and returns `TE_EFAIL`.

| Call | Property |
|---|---|
| `tapi_sql_assert_row_count` | A table (or a table with a condition) has N rows |
| `tapi_sql_assert_pk_unique` | A key is unique and has no NULL |
| `tapi_sql_assert_referential` | Every reference has its row - checked in the data, so it finds what a missing constraint let in |
| `tapi_sql_assert_not_null` | No NULL in the columns |
| `tapi_sql_assert_schema` | The table has the expected columns, normalised types, nullability and key |
| `tapi_sql_assert_query_equal` / `_assert_result` | Two results are equal |
| `tapi_sql_assert_number` | A number is within an absolute or relative tolerance |
| `tapi_sql_assert_atomic` | A transaction with a failing statement leaves no trace |
| `tapi_sql_assert_persistent` | The state is the same after a reconnection |

Results are compared in order or as bags of rows, with NULL equal to
NULL or to nothing, with numbers by value or by type, exactly or within
a tolerance (`tapi_sql_cmp_opts`). `tapi_sql_result_equal()` is the
comparison without the verdict.

## Performance measurements

`tapi_sql_perf.h`: query latency, rows processed per second, transaction
throughput, connection establishment latency, error rate and timeout
frequency, reported as count, minimum, maximum, mean and the percentiles
asked for, in the log as a TE measurement artifact with a line graph of
the samples. A statement's latency is measured on the agent around the
driver call, so the RPC is not in it. One statement at a time from one
session: an instrument for a functional suite, not a load generator.

## Security checks

`tapi_sql_audit.h`. Authorized use only: servers you own or are engaged
to test.

`tapi_sql_audit()` reads a server's posture and changes nothing: an
unencrypted or unverified session, a server with TLS off, a superuser or
over-privileged account, a server listening on every address, weak
password hashing, host rules that let clients in without a password,
anonymous or empty-password accounts, file access left open, connection
logging off. A check the account may not read reports
`<check>.not-assessed` instead of passing.

`tapi_sql_audit_auth()` tries passwords that must be refused.
`tapi_sql_audit_denied()` runs a statement a restricted account must not
be allowed, inside a transaction it rolls back; a mutating statement
there is the test's explicit choice.

SQL injection is a property of an application, not of a driver, and is
tested where the application takes input: `tapi_sql_injection_payloads()`
gives hostile inputs and `tapi_sql_audit_injection()` hands each to a
callback of the test that drives the application and says whether it
misbehaved. Nothing assumes the database driver is vulnerable.

Findings have a stable check identifier, a subject and a severity - the
model of tsf-cybersec. `tapi_sql_cybersec.h` (header-only) copies a
report into a `tapi_cybersec_report` for a suite that builds
tsf-cybersec; `tapi_sql` itself does not link it.

## Secrets

The password of a connection is a field of `tapi_sql_config` and travels
to the agent inside the connect RPC. It is not on any command line (no
program is run), not in the log lines of the RPC wrappers (statement
texts are logged, cut at 240 characters; bound values and fetched rows
are not), not in `tapi_sql_conn_describe()`, and any driver message that
quotes it is blanked (`***`) on the agent before it is returned. The
copies kept in memory - for reconnecting, and for MariaDB's
cancel-by-second-connection - are zeroed when the connection is closed.

What a test puts in a statement text is the test's business:
`ALTER USER ... PASSWORD '...'` in a `tapi_sql_execute()` is logged like
any other statement. Bind it.

## Wire format

Values cross the RPC as text: a row is a line, cells are tab-separated,
a cell is a type letter and a payload (`N`, `B1`, `I42`, `U42`, `F0.5`,
`D12.50`, `Thello`, `J{..}`, `M2024-02-29`, `X00ff`). See
`ta_sql/sql_rpc.x.m4`. No float crosses XDR as a float and a binary
value with a NUL is carried faithfully. RPC program number 45.

## Using it from a suite

`conf/external.yml`:

```yaml
repositories:
  - name: tsf_sql
    url: https://github.com/interpretica-io/tsf-sql.git
    ref: main
    libs: [ta_sql, rpcs_sql, tapi_sql]
```

`conf/builder.conf`:

```
TE_EXT_REPO_USE([tsf_sql], [], [ta_sql rpcs_sql tapi_sql])
TE_LIB_PARMS([rpcxdr], [], [],
             [--with-rpcdefs=tarpc_job.x.m4,../ta_sql/sql_rpc.x.m4])

# The RPC server links ta_sql and rpcs_sql, and the client libraries by
# their pkg-config names (whichever are installed).
TE_TA_APP([ta_rpcprovider], [], [linux], [ta_rpcprovider], [], [],
          [ta_sql rpcs_sql ... rpcserver agentlib rpcxdrta ...],
          [\${EXT_SOURCES}/build.sh --extra-deps='libpcre2-8 sqlite3 libpq libmariadb'],
          [ta_rpcs], [])
```

The agent host needs the client libraries with their development
headers (Debian/Ubuntu: `libsqlite3-dev`, `libpq-dev`,
`libmariadb-dev`). [tsf-sql-ts](https://github.com/interpretica-io/tsf-sql-ts)
is a complete suite to copy from.

## Engine notes

- **SQLite.** The database is a file on the agent. Foreign keys are
  switched on for every connection so that a schema behaves the same as
  on the servers. SQLite stores by value, not by column: a decimal bound
  into a `DECIMAL` column comes back as a float, and types are reported
  from the declared column type. CHECK constraints do not appear in
  introspection and key constraints have no names (`primary`, `fk_0`).
- **PostgreSQL.** NOT NULL is reported as a column property, not as a
  constraint. `read uncommitted` is accepted and behaves as read
  committed. Verifying the server needs `tls_ca`.
- **MySQL / MariaDB.** Built against MariaDB Connector/C. `tinyint(1)`
  is a boolean. "Prefer TLS" tries an encrypted connection and falls
  back to plain when TLS itself fails. The two verify modes are the same
  (the connector checks CA and host name together). Data-definition
  statements commit implicitly, as always there. The statement deadline
  uses `max_statement_time` (MariaDB) and falls back to
  `max_execution_time` (MySQL, SELECT only).

## Out of scope

An ORM, a migration framework, a query optimiser, distributed SQL, ETL
orchestration, and translation between SQL dialects.

## CI

`ci/lint.sh` - layout, licence headers, cppcheck, shellcheck,
actionlint. `ci/driver-test.sh` builds `ta_sql` with the three drivers
under AddressSanitizer and UBSan and runs `ci/driver_test.c` against a
temporary SQLite file and real PostgreSQL and MariaDB servers
(`ci/local-databases.sh up` starts them in Docker; the workflow uses
service containers): values, bounded fetch, transactions,
introspection, error classes, deadlines, cancellation, lock waits, a
deadlock, a killed session, authentication failures. The RPC layer and
the engine-side TAPI need a TE build and are exercised end to end by
tsf-sql-ts.

## Licence

MIT. See `LICENSE`.
