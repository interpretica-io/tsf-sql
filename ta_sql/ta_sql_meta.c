/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL database sessions on an agent: schema introspection
 *
 * Each engine keeps its catalogue its own way (SQLite pragmas,
 * PostgreSQL pg_catalog, MySQL information_schema). One query per
 * engine and object kind brings the catalogue into the same column
 * order, one row per (object, column); this unit then folds the rows
 * of one object together, so the answer has the same shape whatever
 * the engine. The queries go through the driver like any other
 * statement.
 */

#define TE_LGR_USER     "TA SQL META"

#include "te_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_sql.h"
#include "ta_sql_drv.h"
#include "ta_sql_int.h"

/** The widest catalogue query, in columns. */
#define META_MAX_COLS   6

/** One catalogue row: decoded fields, @c NULL for SQL NULL. */
typedef struct meta_row {
    char *f[META_MAX_COLS];
} meta_row;

/** The catalogue rows of one query. */
typedef struct meta_rows {
    meta_row *rows;
    size_t    n;
    size_t    cap;
} meta_rows;

/* ---- the catalogue queries ---------------------------------------- */

static const char sqlite_tables[] =
    "SELECT name, type FROM sqlite_master "
    "WHERE type IN ('table', 'view') AND name NOT LIKE 'sqlite_%' "
    "ORDER BY name";

static const char sqlite_columns[] =
    "SELECT name, cid + 1, type, "
    "CASE WHEN \"notnull\" THEN 0 ELSE 1 END, dflt_value, "
    "CASE WHEN pk > 0 THEN 1 ELSE 0 END "
    "FROM pragma_table_info(?) ORDER BY cid";

static const char sqlite_indexes[] =
    "SELECT il.name, il.\"unique\", "
    "CASE WHEN il.origin = 'pk' THEN 1 ELSE 0 END, ii.name "
    "FROM pragma_index_list(?) il, pragma_index_info(il.name) ii "
    "ORDER BY il.name, ii.seqno";

/* SQLite does not name its key constraints; the names are made up. */
static const char sqlite_constraints[] =
    "SELECT 'primary' AS cname, 'primary' AS kind, name AS col, pk AS seq "
    "FROM pragma_table_info(?1) WHERE pk > 0 "
    "UNION ALL "
    "SELECT il.name, 'unique', ii.name, ii.seqno "
    "FROM pragma_index_list(?1) il, pragma_index_info(il.name) ii "
    "WHERE il.origin = 'u' "
    "UNION ALL "
    "SELECT 'fk_' || id, 'foreign', \"from\", seq "
    "FROM pragma_foreign_key_list(?1) "
    "ORDER BY 1, 4";

static const char sqlite_fkeys[] =
    "SELECT 'fk_' || id, \"from\", \"table\", \"to\", "
    "lower(on_delete), lower(on_update) "
    "FROM pragma_foreign_key_list(?) ORDER BY id, seq";

static const char pg_tables[] =
    "SELECT table_name::text, "
    "CASE table_type WHEN 'VIEW' THEN 'view' ELSE 'table' END "
    "FROM information_schema.tables "
    "WHERE table_schema = current_schema() ORDER BY 1";

static const char pg_columns[] =
    "SELECT c.column_name::text, c.ordinal_position::text, "
    "c.data_type::text, "
    "CASE c.is_nullable WHEN 'YES' THEN '1' ELSE '0' END, "
    "c.column_default::text, "
    "CASE WHEN EXISTS ("
    "SELECT 1 FROM information_schema.table_constraints tc "
    "JOIN information_schema.key_column_usage k "
    "ON k.constraint_schema = tc.constraint_schema "
    "AND k.constraint_name = tc.constraint_name "
    "AND k.table_name = tc.table_name "
    "WHERE tc.constraint_type = 'PRIMARY KEY' "
    "AND tc.table_schema = c.table_schema "
    "AND tc.table_name = c.table_name "
    "AND k.column_name = c.column_name) THEN '1' ELSE '0' END "
    "FROM information_schema.columns c "
    "WHERE c.table_schema = current_schema() AND c.table_name = ? "
    "ORDER BY c.ordinal_position";

static const char pg_indexes[] =
    "SELECT i.relname::text, "
    "CASE WHEN ix.indisunique THEN '1' ELSE '0' END, "
    "CASE WHEN ix.indisprimary THEN '1' ELSE '0' END, a.attname::text "
    "FROM pg_class t "
    "JOIN pg_namespace n ON n.oid = t.relnamespace "
    "JOIN pg_index ix ON ix.indrelid = t.oid "
    "JOIN pg_class i ON i.oid = ix.indexrelid "
    "CROSS JOIN LATERAL unnest(ix.indkey::int2[]) "
    "WITH ORDINALITY AS k(attnum, ord) "
    "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = k.attnum "
    "WHERE n.nspname = current_schema() AND t.relname = ? "
    "ORDER BY i.relname, k.ord";

/* PostgreSQL lists NOT NULL as a CHECK constraint; that is a column
 * property here (see the columns), not a constraint. */
static const char pg_constraints[] =
    "SELECT tc.constraint_name::text, "
    "CASE tc.constraint_type WHEN 'PRIMARY KEY' THEN 'primary' "
    "WHEN 'UNIQUE' THEN 'unique' WHEN 'FOREIGN KEY' THEN 'foreign' "
    "ELSE 'check' END, k.column_name::text "
    "FROM information_schema.table_constraints tc "
    "LEFT JOIN information_schema.key_column_usage k "
    "ON k.constraint_schema = tc.constraint_schema "
    "AND k.constraint_name = tc.constraint_name "
    "AND k.table_name = tc.table_name "
    "WHERE tc.table_schema = current_schema() AND tc.table_name = ? "
    "AND NOT (tc.constraint_type = 'CHECK' "
    "AND tc.constraint_name LIKE '%not_null') "
    "ORDER BY 1, 2, k.ordinal_position";

#define PG_FK_ACTION(_col) \
    "CASE " _col " WHEN 'a' THEN 'no action' WHEN 'r' THEN 'restrict' " \
    "WHEN 'c' THEN 'cascade' WHEN 'n' THEN 'set null' "                 \
    "ELSE 'set default' END"

static const char pg_fkeys[] =
    "SELECT c.conname::text, a.attname::text, rt.relname::text, "
    "ra.attname::text, " PG_FK_ACTION("c.confdeltype") ", "
    PG_FK_ACTION("c.confupdtype") " "
    "FROM pg_constraint c "
    "JOIN pg_class t ON t.oid = c.conrelid "
    "JOIN pg_namespace n ON n.oid = t.relnamespace "
    "JOIN pg_class rt ON rt.oid = c.confrelid "
    "CROSS JOIN LATERAL unnest(c.conkey, c.confkey) "
    "WITH ORDINALITY AS k(att, ratt, ord) "
    "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = k.att "
    "JOIN pg_attribute ra ON ra.attrelid = rt.oid AND ra.attnum = k.ratt "
    "WHERE c.contype = 'f' AND n.nspname = current_schema() "
    "AND t.relname = ? ORDER BY c.conname, k.ord";

static const char my_tables[] =
    "SELECT TABLE_NAME, "
    "CASE TABLE_TYPE WHEN 'VIEW' THEN 'view' ELSE 'table' END "
    "FROM information_schema.TABLES "
    "WHERE TABLE_SCHEMA = DATABASE() ORDER BY 1";

static const char my_columns[] =
    "SELECT COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, "
    "CASE IS_NULLABLE WHEN 'YES' THEN '1' ELSE '0' END, COLUMN_DEFAULT, "
    "CASE COLUMN_KEY WHEN 'PRI' THEN '1' ELSE '0' END "
    "FROM information_schema.COLUMNS "
    "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? "
    "ORDER BY ORDINAL_POSITION";

static const char my_indexes[] =
    "SELECT INDEX_NAME, CASE NON_UNIQUE WHEN 0 THEN '1' ELSE '0' END, "
    "CASE INDEX_NAME WHEN 'PRIMARY' THEN '1' ELSE '0' END, COLUMN_NAME "
    "FROM information_schema.STATISTICS "
    "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? "
    "ORDER BY INDEX_NAME, SEQ_IN_INDEX";

static const char my_constraints[] =
    "SELECT tc.CONSTRAINT_NAME, "
    "CASE tc.CONSTRAINT_TYPE WHEN 'PRIMARY KEY' THEN 'primary' "
    "WHEN 'UNIQUE' THEN 'unique' WHEN 'FOREIGN KEY' THEN 'foreign' "
    "ELSE 'check' END, k.COLUMN_NAME "
    "FROM information_schema.TABLE_CONSTRAINTS tc "
    "LEFT JOIN information_schema.KEY_COLUMN_USAGE k "
    "ON k.CONSTRAINT_SCHEMA = tc.CONSTRAINT_SCHEMA "
    "AND k.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
    "AND k.TABLE_NAME = tc.TABLE_NAME "
    "WHERE tc.TABLE_SCHEMA = DATABASE() AND tc.TABLE_NAME = ? "
    "ORDER BY 1, 2, k.ORDINAL_POSITION";

static const char my_fkeys[] =
    "SELECT k.CONSTRAINT_NAME, k.COLUMN_NAME, k.REFERENCED_TABLE_NAME, "
    "k.REFERENCED_COLUMN_NAME, LOWER(r.DELETE_RULE), LOWER(r.UPDATE_RULE) "
    "FROM information_schema.KEY_COLUMN_USAGE k "
    "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
    "ON r.CONSTRAINT_SCHEMA = k.CONSTRAINT_SCHEMA "
    "AND r.CONSTRAINT_NAME = k.CONSTRAINT_NAME "
    "AND r.TABLE_NAME = k.TABLE_NAME "
    "WHERE k.TABLE_SCHEMA = DATABASE() AND k.TABLE_NAME = ? "
    "AND k.REFERENCED_TABLE_NAME IS NOT NULL "
    "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION";

/** What to ask and how to fold the answer, per object kind. */
typedef struct meta_spec {
    int         ncols;      /**< Columns the query returns (used). */
    unsigned    fold;       /**< Mask of columns folded into a list. */
    unsigned    key;        /**< Mask of columns that name the object. */
    const char *sqlite;
    const char *pg;
    const char *mysql;
} meta_spec;

static const meta_spec specs[] = {
    [TA_SQL_META_TABLES] =
        { 2, 0, 0, sqlite_tables, pg_tables, my_tables },
    [TA_SQL_META_COLUMNS] =
        { 6, 0, 0, sqlite_columns, pg_columns, my_columns },
    [TA_SQL_META_INDEXES] =
        { 4, 0x8, 0x1, sqlite_indexes, pg_indexes, my_indexes },
    [TA_SQL_META_CONSTRAINTS] =
        { 3, 0x4, 0x3, sqlite_constraints, pg_constraints, my_constraints },
    [TA_SQL_META_FOREIGN_KEYS] =
        { 6, 0xa, 0x1, sqlite_fkeys, pg_fkeys, my_fkeys },
};

/* ---- running a catalogue query ------------------------------------ */

static void
rows_free(meta_rows *r)
{
    size_t i;
    int    j;

    for (i = 0; i < r->n; i++)
    {
        for (j = 0; j < META_MAX_COLS; j++)
            free(r->rows[i].f[j]);
    }
    free(r->rows);
    memset(r, 0, sizeof(*r));
}

/* Split the cells of one encoded row into decoded fields. */
static void
row_decode(const char *line, size_t len, int ncols, meta_row *row)
{
    const char *p = line;
    const char *end = line + len;
    int         col;

    for (col = 0; col < ncols && p <= end; col++)
    {
        const char *tab = memchr(p, '\t', (size_t)(end - p));
        size_t      clen = tab != NULL ? (size_t)(tab - p) :
                                         (size_t)(end - p);
        char        tag = 0;
        char       *data = NULL;
        size_t      dlen = 0;

        if (ta_sql_cell_decode(p, clen, &tag, &data, &dlen) == 0)
        {
            if (tag == 'N')
                free(data);
            else
                row->f[col] = data;
        }
        if (tab == NULL)
            break;
        p = tab + 1;
    }
}

/* Run @p sql with @p table as its only parameter (when it has one). */
static int
query(const ta_sql_driver *drv, void *dc, const char *sql,
      const char *table, int ncols, meta_rows *out, ta_sql_errinfo *err)
{
    te_string line = TE_STRING_INIT;
    void     *ds = NULL;
    int       nparams = 0;
    int       cols = 0;
    int       res;
    int       i;

    if (drv->prepare(dc, sql, &ds, &nparams, err) != 0)
        return -1;

    for (i = 0; i < nparams; i++)
    {
        if (drv->bind(ds, i, 'T', table, strlen(table), err) != 0)
        {
            drv->finish(ds);
            return -1;
        }
    }

    if (drv->execute(ds, &cols, NULL, err) != 0)
    {
        drv->finish(ds);
        return -1;
    }
    if (cols < ncols)
    {
        drv->finish(ds);
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                       "catalogue query returned %d columns, not %d", cols,
                       ncols);
        return -1;
    }

    while ((res = drv->step(ds, err)) == 1)
    {
        te_string_reset(&line);
        for (i = 0; i < ncols; i++)
        {
            if (i > 0)
                te_string_append(&line, "\t");
            drv->cell(ds, i, &line);
        }
        if (out->n == out->cap)
        {
            out->cap = out->cap == 0 ? 16 : out->cap * 2;
            out->rows = realloc(out->rows, out->cap * sizeof(*out->rows));
            if (out->rows == NULL)
                abort();
        }
        memset(&out->rows[out->n], 0, sizeof(out->rows[out->n]));
        row_decode(line.ptr, line.len, ncols, &out->rows[out->n]);
        out->n++;
    }

    te_string_free(&line);
    drv->finish(ds);
    return res < 0 ? -1 : 0;
}

/* ---- shaping the answer ------------------------------------------- */

/* Append one text-or-NULL cell, tab-separated from the previous one. */
static void
put(te_string *rows, bool first, const char *value)
{
    if (!first)
        te_string_append(rows, "\t");
    if (value == NULL)
        ta_sql_cell_null(rows);
    else
        ta_sql_cell_text(rows, 'T', value, strlen(value));
}

static bool
same_key(const meta_row *a, const meta_row *b, unsigned key, int ncols)
{
    int i;

    for (i = 0; i < ncols; i++)
    {
        const char *x = a->f[i] != NULL ? a->f[i] : "";
        const char *y = b->f[i] != NULL ? b->f[i] : "";

        if ((key & (1u << i)) != 0 && strcmp(x, y) != 0)
            return false;
    }
    return true;
}

/* See description in ta_sql_int.h */
int
ta_sql_meta_build(const ta_sql_driver *drv, void *dc, int kind,
                  const char *table, te_string *rows, ta_sql_errinfo *err)
{
    const meta_spec *spec;
    const char      *sql;
    meta_rows        cat = { NULL, 0, 0 };
    bool             first_row = true;
    size_t           i;
    size_t           j;
    int              col;

    if (kind < TA_SQL_META_TABLES || kind > TA_SQL_META_FOREIGN_KEYS)
    {
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                       "unknown introspection kind %d", kind);
        return -1;
    }
    spec = &specs[kind];
    sql = drv->engine == TA_SQL_ENGINE_SQLITE ? spec->sqlite :
          drv->engine == TA_SQL_ENGINE_POSTGRES ? spec->pg : spec->mysql;

    if (query(drv, dc, sql, table != NULL ? table : "", spec->ncols, &cat,
              err) != 0)
    {
        rows_free(&cat);
        return -1;
    }

    for (i = 0; i < cat.n; i = j)
    {
        const meta_row *r = &cat.rows[i];

        /* The rows i..j-1 describe one object. */
        j = i + 1;
        if (spec->fold != 0)
        {
            while (j < cat.n &&
                   same_key(r, &cat.rows[j], spec->key, spec->ncols))
                j++;
        }

        if (!first_row)
            te_string_append(rows, "\n");
        first_row = false;

        for (col = 0; col < spec->ncols; col++)
        {
            if ((spec->fold & (1u << col)) != 0)
            {
                te_string list = TE_STRING_INIT;
                size_t    k;

                for (k = i; k < j; k++)
                {
                    if (cat.rows[k].f[col] == NULL)
                        continue;
                    te_string_append(&list, "%s%s",
                                     list.len > 0 ? "," : "",
                                     cat.rows[k].f[col]);
                }
                put(rows, col == 0, list.ptr != NULL ? list.ptr : "");
                te_string_free(&list);
                continue;
            }

            put(rows, col == 0, r->f[col]);

            /* Columns: the normalised type letter sits before the
             * native type name. */
            if (kind == TA_SQL_META_COLUMNS && col == 1)
            {
                const char letter[2] = { ta_sql_type_letter(r->f[2]),
                                         '\0' };

                put(rows, false, letter);
            }
        }
    }

    rows_free(&cat);
    return 0;
}
