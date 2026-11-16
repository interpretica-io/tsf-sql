# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): engine-side TAPI for
# SQL database sessions on agents. Built as a shared library on the
# engine platform. It needs the sql_* RPCs in rpcxdr, so add
# ../ta_sql/sql_rpc.x.m4 to the rpcxdr definitions of the engine
# platform.

if get_option('engine')
    build_lib_shared = true
endif
build_lib_static = not get_option('engine')
install_lib = install_dev

headers += files(
    'tapi_sql.h',
    'tapi_sql_assert.h',
    'tapi_sql_audit.h',
    'tapi_sql_cybersec.h',
    'tapi_sql_perf.h',
    'tapi_sql_rpc.h',
)

sources += files(
    'tapi_sql.c',
    'tapi_sql_assert.c',
    'tapi_sql_audit.c',
    'tapi_sql_perf.c',
    'tapi_sql_rpc.c',
)

te_libs += [
    'tapi',
    'tapi_rpc',
    'rcfrpc',
    'confapi',
    'rcfapi',
    'rpcxdr',
    'logger_core',
    'tools',
]
