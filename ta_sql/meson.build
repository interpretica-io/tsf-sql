# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): agent-side SQL database
# sessions. ta_sql owns the connections, the statements and the
# transaction state; each database engine is a driver behind a seam
# (ta_sql_drv.h), linked in-process over the engine's own client
# library. Nothing is spawned: no psql, no mysql, no sqlite3 program.
# The agent and its RPC server both link it.
#
# Every driver is optional. A driver is compiled in only when its
# client library is found, and the module builds with any one of them
# (or with none: tapi_sql_available() then reports no engine and a
# suite skips).

headers += files(
    'ta_sql.h',
    'ta_sql_drv.h',
)

sources += files(
    'ta_sql.c',
    'ta_sql_meta.c',
)

te_libs += [
    'logger_core',
    'tools',
]

cc = meson.get_compiler('c')

sql_defs = []

# SQLite: the SQLite C API (Debian: libsqlite3-dev).
sqlite_dep = dependency('sqlite3', required: false)
if sqlite_dep.found()
    deps += [ sqlite_dep ]
    sources += files('ta_sql_sqlite.c')
    sql_defs += '-DTA_SQL_HAVE_SQLITE'
endif

# PostgreSQL: libpq (Debian: libpq-dev).
pq_dep = dependency('libpq', required: false)
if pq_dep.found()
    deps += [ pq_dep ]
    sources += files('ta_sql_pg.c')
    sql_defs += '-DTA_SQL_HAVE_POSTGRES'
endif

# MySQL / MariaDB: MariaDB Connector/C (Debian: libmariadb-dev).
mariadb_dep = dependency('libmariadb', required: false)
if not mariadb_dep.found()
    mariadb_dep = dependency('mariadb', required: false)
endif
if mariadb_dep.found()
    deps += [ mariadb_dep ]
    sources += files('ta_sql_mysql.c')
    sql_defs += '-DTA_SQL_HAVE_MYSQL'
endif

if sql_defs.length() == 0
    warning('tsf-sql: none of sqlite3, libpq, libmariadb found; ' +
            'ta_sql will report no engine and the suite will skip.')
else
    deps += declare_dependency(compile_args: sql_defs)
endif

# The asynchronous statements run on threads.
deps += [ dependency('threads') ]
