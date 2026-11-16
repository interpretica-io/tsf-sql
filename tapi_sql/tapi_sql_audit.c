/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: security posture of a database
 */

#define TE_LGR_USER     "TAPI SQL AUDIT"

#include "te_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_sql.h"
#include "tapi_sql_audit.h"

/* ---- the report --------------------------------------------------- */

/* See description in tapi_sql_audit.h */
const char *
tapi_sql_severity2str(tapi_sql_severity severity)
{
    switch (severity)
    {
        case TAPI_SQL_SEV_INFO: return "info";
        case TAPI_SQL_SEV_LOW: return "low";
        case TAPI_SQL_SEV_MEDIUM: return "medium";
        case TAPI_SQL_SEV_HIGH: return "high";
        case TAPI_SQL_SEV_CRITICAL: return "critical";
        default: return "unknown";
    }
}

/* See description in tapi_sql_audit.h */
void
tapi_sql_report_add(tapi_sql_report *report, tapi_sql_severity severity,
                    const char *check, const char *subject,
                    const char *detail_fmt, ...)
{
    te_string         detail = TE_STRING_INIT;
    tapi_sql_finding *f;
    va_list           ap;

    va_start(ap, detail_fmt);
    te_string_append_va(&detail, detail_fmt, ap);
    va_end(ap);

    report->items = realloc(report->items,
                            (report->n + 1) * sizeof(*report->items));
    if (report->items == NULL)
        TE_FATAL_ERROR("Out of memory");

    f = &report->items[report->n++];
    f->severity = severity;
    f->check = TE_STRDUP(check);
    f->subject = TE_STRDUP(subject != NULL ? subject : "");
    f->detail = detail.ptr != NULL ? detail.ptr : TE_STRDUP("");
}

/* See description in tapi_sql_audit.h */
unsigned int
tapi_sql_report_count(const tapi_sql_report *report,
                      tapi_sql_severity min_severity)
{
    unsigned int count = 0;
    unsigned int i;

    for (i = 0; i < report->n; i++)
        count += report->items[i].severity >= min_severity ? 1 : 0;
    return count;
}

/* See description in tapi_sql_audit.h */
bool
tapi_sql_report_has(const tapi_sql_report *report, const char *check)
{
    unsigned int i;

    for (i = 0; i < report->n; i++)
    {
        if (strcmp(report->items[i].check, check) == 0)
            return true;
    }
    return false;
}

/* See description in tapi_sql_audit.h */
void
tapi_sql_report_log(const tapi_sql_report *report)
{
    te_string out = TE_STRING_INIT;
    int       sev;
    unsigned  i;

    for (sev = TAPI_SQL_SEV_CRITICAL; sev >= TAPI_SQL_SEV_INFO; sev--)
    {
        for (i = 0; i < report->n; i++)
        {
            const tapi_sql_finding *f = &report->items[i];

            if ((int)f->severity != sev)
                continue;
            te_string_append(&out, "[%s] %s (%s): %s\n",
                             tapi_sql_severity2str(f->severity), f->check,
                             f->subject, f->detail);
        }
    }
    RING("SQL security report: %u finding(s)\n%s", report->n,
         out.ptr != NULL ? out.ptr : "");
    te_string_free(&out);
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* See description in tapi_sql_audit.h */
bool
tapi_sql_report_verdict(const tapi_sql_report *report,
                        tapi_sql_severity min_severity, te_string *dest)
{
    const char **checks;
    unsigned int n = 0;
    unsigned int i;

    checks = TE_ALLOC((report->n + 1) * sizeof(*checks));
    for (i = 0; i < report->n; i++)
    {
        if (report->items[i].severity >= min_severity)
            checks[n++] = report->items[i].check;
    }
    if (n == 0)
    {
        free(checks);
        return false;
    }

    /* Sorted and without repeats, so the text does not depend on the
     * order the checks ran in. */
    qsort(checks, n, sizeof(*checks), cmp_str);
    te_string_append(dest, "%u SQL security finding(s) at or above %s:", n,
                     tapi_sql_severity2str(min_severity));
    for (i = 0; i < n; i++)
    {
        if (i > 0 && strcmp(checks[i], checks[i - 1]) == 0)
            continue;
        te_string_append(dest, " %s", checks[i]);
    }
    free(checks);
    return true;
}

/* See description in tapi_sql_audit.h */
void
tapi_sql_report_free(tapi_sql_report *report)
{
    unsigned int i;

    for (i = 0; i < report->n; i++)
    {
        free(report->items[i].check);
        free(report->items[i].subject);
        free(report->items[i].detail);
    }
    free(report->items);
    report->items = NULL;
    report->n = 0;
}

/* ---- reading settings --------------------------------------------- */

/**
 * The first cell of a read-only query, as text.
 *
 * @return 0; TE_EPERM when the account may not read it; another status
 *         when the query failed otherwise.
 */
static te_errno
setting(tapi_sql_conn *conn, const char *sql, char *buf, size_t size)
{
    tapi_sql_result       res = TAPI_SQL_RESULT_INIT;
    const tapi_sql_value *v;
    te_errno              rc;

    buf[0] = '\0';
    rc = tapi_sql_query(conn, sql, &res);
    if (rc != 0)
    {
        tapi_sql_result_free(&res);
        if (tapi_sql_last_error(conn)->cls == TAPI_SQL_ERR_PERMISSION)
            return TE_RC(TE_TAPI, TE_EPERM);
        return rc;
    }

    v = tapi_sql_result_cell(&res, 0, 0);
    if (v != NULL && v->type != TAPI_SQL_NULL)
    {
        if (v->type == TAPI_SQL_BOOL)
            te_strlcpy(buf, v->i != 0 ? "on" : "off", size);
        else if (v->str != NULL)
            te_strlcpy(buf, v->str, size);
    }
    tapi_sql_result_free(&res);
    return 0;
}

/* All the rows of a query, first column, joined by newlines. */
static te_errno
listing(tapi_sql_conn *conn, const char *sql, te_string *out)
{
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    unsigned int    row;
    te_errno        rc;

    rc = tapi_sql_query(conn, sql, &res);
    if (rc == 0)
    {
        for (row = 0; row < res.nrows; row++)
        {
            const tapi_sql_value *v = tapi_sql_result_cell(&res, row, 0);

            te_string_append(out, "%s\n",
                             v != NULL && v->str != NULL ? v->str : "");
        }
    }
    else if (tapi_sql_last_error(conn)->cls == TAPI_SQL_ERR_PERMISSION)
    {
        rc = TE_RC(TE_TAPI, TE_EPERM);
    }
    tapi_sql_result_free(&res);
    return rc;
}

/* Record that a check could not look. */
static void
not_assessed(tapi_sql_report *report, const char *check, const char *subject,
             tapi_sql_conn *conn)
{
    te_string id = TE_STRING_INIT;

    te_string_append(&id, "%s.not-assessed", check);
    tapi_sql_report_add(report, TAPI_SQL_SEV_INFO, id.ptr, subject,
                        "could not be read: %s",
                        tapi_sql_last_error(conn)->message);
    te_string_free(&id);
}

static bool
is_on(const char *value)
{
    return strcasecmp(value, "on") == 0 || strcasecmp(value, "1") == 0 ||
           strcasecmp(value, "yes") == 0 || strcasecmp(value, "true") == 0;
}

/* ---- the transport, common to the networked engines --------------- */

static void
audit_transport(tapi_sql_conn *conn, const char *subject,
                tapi_sql_report *report)
{
    const tapi_sql_config *cfg = tapi_sql_conn_config(conn);
    tapi_sql_caps          caps;

    if (tapi_sql_capabilities(conn, &caps) != 0)
        return;

    if (!caps.tls)
        tapi_sql_report_add(report, TAPI_SQL_SEV_HIGH, "sql.tls.off",
                            subject, "the session is not encrypted: "
                            "credentials and data cross the network in "
                            "the clear");
    else if (cfg->tls_mode < TAPI_SQL_TLS_VERIFY_CA)
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.tls.unverified", subject,
                            "the session is encrypted but the server's "
                            "certificate was not verified: an active "
                            "attacker can sit in the middle");
}

/* ---- PostgreSQL --------------------------------------------------- */

static void
audit_pg(tapi_sql_conn *conn, const char *subject, tapi_sql_report *report)
{
    char     value[256];
    te_errno rc;

    if (setting(conn, "SHOW ssl", value, sizeof(value)) == 0 &&
        !is_on(value))
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.tls.server-off", subject,
                            "ssl = %s: the server cannot encrypt", value);

    if (setting(conn, "SELECT rolsuper FROM pg_roles "
                      "WHERE rolname = current_user", value,
                sizeof(value)) == 0 && is_on(value))
        tapi_sql_report_add(report, TAPI_SQL_SEV_HIGH,
                            "sql.account.superuser", subject,
                            "the account is a superuser: it bypasses "
                            "every permission check");

    if (setting(conn, "SELECT (rolcreaterole OR rolcreatedb OR "
                      "rolbypassrls OR rolreplication) AND NOT rolsuper "
                      "FROM pg_roles WHERE rolname = current_user", value,
                sizeof(value)) == 0 && is_on(value))
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.account.excessive", subject,
                            "the account can create roles or databases, "
                            "replicate or bypass row security");

    if (setting(conn, "SHOW listen_addresses", value, sizeof(value)) == 0 &&
        (strcmp(value, "*") == 0 || strstr(value, "0.0.0.0") != NULL ||
         strstr(value, "::") != NULL))
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.net.listen-all", subject,
                            "listen_addresses = '%s': reachable on every "
                            "interface of the host", value);

    if (setting(conn, "SHOW password_encryption", value,
                sizeof(value)) == 0 && strcasecmp(value, "md5") == 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.auth.weak-hash", subject,
                            "password_encryption = md5");

    if (setting(conn, "SHOW log_connections", value, sizeof(value)) == 0 &&
        (strcasecmp(value, "off") == 0 || value[0] == '\0'))
        tapi_sql_report_add(report, TAPI_SQL_SEV_LOW,
                            "sql.log.connections-off", subject,
                            "log_connections is off: a break-in attempt "
                            "leaves no record");

    rc = setting(conn, "SELECT count(*) FROM pg_hba_file_rules "
                       "WHERE auth_method = 'trust' AND type <> 'local'",
                 value, sizeof(value));
    if (rc != 0)
        not_assessed(report, "sql.auth.trust", subject, conn);
    else if (atoi(value) > 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_CRITICAL, "sql.auth.trust",
                            subject, "%s host-based rule(s) admit network "
                            "clients without a password", value);
}

/* ---- MySQL / MariaDB ---------------------------------------------- */

static void
audit_mysql(tapi_sql_conn *conn, const char *subject,
            tapi_sql_report *report)
{
    te_string grants = TE_STRING_INIT;
    char      value[256];
    te_errno  rc;

    if (setting(conn, "SELECT @@have_ssl", value, sizeof(value)) == 0 &&
        strcasecmp(value, "YES") != 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.tls.server-off", subject,
                            "have_ssl = %s: the server cannot encrypt",
                            value);

    if (listing(conn, "SHOW GRANTS", &grants) == 0 && grants.ptr != NULL)
    {
        if (strstr(grants.ptr, "ALL PRIVILEGES ON *.*") != NULL ||
            strstr(grants.ptr, "SUPER") != NULL)
            tapi_sql_report_add(report, TAPI_SQL_SEV_HIGH,
                                "sql.account.superuser", subject,
                                "the account holds every privilege on "
                                "every database");
        else if (strstr(grants.ptr, "WITH GRANT OPTION") != NULL ||
                 strstr(grants.ptr, "FILE") != NULL ||
                 strstr(grants.ptr, "CREATE USER") != NULL)
            tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                                "sql.account.excessive", subject,
                                "the account can grant its privileges on, "
                                "create users or read server files");
    }
    te_string_free(&grants);

    if (setting(conn, "SELECT @@bind_address", value, sizeof(value)) == 0 &&
        (value[0] == '\0' || strcmp(value, "*") == 0 ||
         strcmp(value, "0.0.0.0") == 0 || strcmp(value, "::") == 0))
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.net.listen-all", subject,
                            "bind_address = '%s': reachable on every "
                            "interface of the host", value);

    if (setting(conn, "SELECT @@local_infile", value, sizeof(value)) == 0 &&
        is_on(value))
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.file.local-infile", subject,
                            "local_infile is on: a statement can make a "
                            "client send a local file");

    /* NULL (read here as "") disables it; an empty string allows any
     * path, and the two are told apart by asking IS NULL. */
    if (setting(conn, "SELECT CASE WHEN @@secure_file_priv IS NULL "
                      "THEN 'disabled' WHEN @@secure_file_priv = '' "
                      "THEN 'anywhere' ELSE 'restricted' END", value,
                sizeof(value)) == 0 && strcmp(value, "anywhere") == 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_MEDIUM,
                            "sql.file.unrestricted", subject,
                            "secure_file_priv is empty: the server reads "
                            "and writes files anywhere it can");

    rc = setting(conn, "SELECT count(*) FROM mysql.user WHERE user = ''",
                 value, sizeof(value));
    if (rc != 0)
        not_assessed(report, "sql.account.anonymous", subject, conn);
    else if (atoi(value) > 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_HIGH,
                            "sql.account.anonymous", subject,
                            "%s anonymous account(s)", value);

    rc = setting(conn, "SELECT count(*) FROM mysql.user "
                       "WHERE authentication_string = '' AND plugin NOT IN "
                       "('unix_socket', 'auth_socket', 'mysql_no_login')",
                 value, sizeof(value));
    if (rc != 0)
        not_assessed(report, "sql.account.empty-password", subject, conn);
    else if (atoi(value) > 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_CRITICAL,
                            "sql.account.empty-password", subject,
                            "%s account(s) with an empty password", value);
}

/* ---- the entry points --------------------------------------------- */

/* See description in tapi_sql_audit.h */
te_errno
tapi_sql_audit(tapi_sql_conn *conn, tapi_sql_report *report)
{
    const tapi_sql_config *cfg = tapi_sql_conn_config(conn);
    te_string              subject = TE_STRING_INIT;

    switch (cfg->engine)
    {
        case TAPI_SQL_ENGINE_SQLITE:
            tapi_sql_report_add(report, TAPI_SQL_SEV_INFO, "sql.sqlite.file",
                                "sqlite", "a file database has no accounts "
                                "and no network: what protects it is the "
                                "permissions of the file and its directory");
            break;

        case TAPI_SQL_ENGINE_POSTGRES:
        case TAPI_SQL_ENGINE_MYSQL:
            /* The subject names the engine and the account, not the
             * address: a verdict must not change with the lab. */
            te_string_append(&subject, "%s account %s",
                             tapi_sql_engine2str(cfg->engine),
                             cfg->user != NULL ? cfg->user : "");
            audit_transport(conn, subject.ptr, report);
            if (cfg->engine == TAPI_SQL_ENGINE_POSTGRES)
                audit_pg(conn, subject.ptr, report);
            else
                audit_mysql(conn, subject.ptr, report);
            break;

        default:
            te_string_free(&subject);
            return TE_RC(TE_TAPI, TE_EINVAL);
    }

    te_string_free(&subject);
    return 0;
}

/* See description in tapi_sql_audit.h */
te_errno
tapi_sql_audit_auth(rcf_rpc_server *rpcs, const tapi_sql_config *cfg,
                    const char *const *passwords, unsigned int n,
                    tapi_sql_report *report)
{
    te_string    subject = TE_STRING_INIT;
    unsigned int i;

    te_string_append(&subject, "%s account %s",
                     tapi_sql_engine2str(cfg->engine),
                     cfg->user != NULL ? cfg->user : "");

    for (i = 0; i < n; i++)
    {
        tapi_sql_config attempt = *cfg;
        tapi_sql_conn  *conn = NULL;
        tapi_sql_error  err;
        te_errno        rc;

        memset(&err, 0, sizeof(err));
        attempt.password = passwords[i];
        rc = tapi_sql_connect_err(rpcs, &attempt, &conn, &err);
        if (rc == 0)
        {
            /* Which password it was is not for the log; its position
             * in the test's own list is enough to find it. */
            tapi_sql_report_add(report, TAPI_SQL_SEV_CRITICAL,
                                "sql.auth.accepted", subject.ptr,
                                "password #%u of the list (%s) was accepted",
                                i, passwords[i][0] == '\0' ? "empty" :
                                                             "non-empty");
            tapi_sql_disconnect(conn);
        }
        else if (err.cls != TAPI_SQL_ERR_AUTH)
        {
            tapi_sql_report_add(report, TAPI_SQL_SEV_INFO,
                                "sql.auth.not-assessed", subject.ptr,
                                "password #%u could not be tried: %s (%s)",
                                i, err.message,
                                tapi_sql_err_class2str(err.cls));
        }
    }

    te_string_free(&subject);
    return 0;
}

/* See description in tapi_sql_audit.h */
te_errno
tapi_sql_audit_denied(tapi_sql_conn *conn, const char *sql, const char *what,
                      tapi_sql_report *report)
{
    tapi_sql_err_class cls;
    te_errno           rc;
    te_errno           rc_stmt;

    rc = tapi_sql_begin(conn);
    if (rc != 0)
        return rc;

    rc_stmt = tapi_sql_execute(conn, sql, NULL);
    cls = tapi_sql_last_error(conn)->cls;
    if (rc_stmt == 0)
        tapi_sql_report_add(report, TAPI_SQL_SEV_HIGH, "sql.privsep.allowed",
                            what, "the account was allowed to: %s", what);
    else if (cls != TAPI_SQL_ERR_PERMISSION)
        tapi_sql_report_add(report, TAPI_SQL_SEV_INFO,
                            "sql.privsep.not-assessed", what,
                            "the statement failed for another reason: "
                            "%s (%s)", tapi_sql_last_error(conn)->message,
                            tapi_sql_err_class2str(cls));

    return tapi_sql_rollback(conn);
}

/* See description in tapi_sql_audit.h */
const char *const *
tapi_sql_injection_payloads(unsigned int *n)
{
    static const char *const payloads[] = {
        "'",
        "' OR '1'='1",
        "' OR 1=1 -- ",
        "\" OR \"\"=\"",
        "') OR ('1'='1",
        "' UNION SELECT NULL -- ",
        "'; SELECT 1 -- ",
        "1 OR 1=1",
        "1; SELECT 1",
        "\\' OR 1=1 -- ",
        "' /* */ OR /* */ '1'='1",
        "admin' -- ",
    };

    if (n != NULL)
        *n = TE_ARRAY_LEN(payloads);
    return payloads;
}

/* See description in tapi_sql_audit.h */
unsigned int
tapi_sql_audit_injection(const char *input_name,
                         tapi_sql_injection_probe probe, void *opaque,
                         tapi_sql_report *report)
{
    const char *const *payloads;
    unsigned int       n = 0;
    unsigned int       hits = 0;
    unsigned int       i;

    payloads = tapi_sql_injection_payloads(&n);
    for (i = 0; i < n; i++)
    {
        if (!probe(payloads[i], opaque))
            continue;
        hits++;
        tapi_sql_report_add(report, TAPI_SQL_SEV_HIGH, "sql.injection",
                            input_name, "the application fell for: %s",
                            payloads[i]);
    }
    return hits;
}
