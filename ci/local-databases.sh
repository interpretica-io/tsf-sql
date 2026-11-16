#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Start (or stop) the throw-away PostgreSQL and MariaDB servers the
# driver contract test talks to, as Docker containers on loopback
# ports, and print the environment ci/driver-test.sh needs.
#
# Usage: ci/local-databases.sh up|down
#
# The credentials are test fixtures of containers that live for one
# run and listen on loopback only; they protect nothing.

set -euo pipefail

pg_name=tsf-sql-ci-pg
my_name=tsf-sql-ci-my
pg_port="${TSF_SQL_PG_PORT:-55432}"
my_port="${TSF_SQL_MY_PORT:-53306}"
pg_image="${TSF_SQL_PG_IMAGE:-postgres:17}"
my_image="${TSF_SQL_MY_IMAGE:-mariadb:11.4}"

case "${1:-}" in
    up)
        docker rm -f "${pg_name}" "${my_name}" >/dev/null 2>&1 || true
        docker run -d --name "${pg_name}" \
            -p "127.0.0.1:${pg_port}:5432" \
            -e POSTGRES_USER=tsf -e POSTGRES_PASSWORD=tsf-ci-secret \
            -e POSTGRES_DB=tsf "${pg_image}" >/dev/null
        docker run -d --name "${my_name}" \
            -p "127.0.0.1:${my_port}:3306" \
            -e MARIADB_USER=tsf -e MARIADB_PASSWORD=tsf-ci-secret \
            -e MARIADB_DATABASE=tsf -e MARIADB_ROOT_PASSWORD=tsf-ci-root \
            "${my_image}" >/dev/null

        for _ in $(seq 1 60) ; do
            if docker exec "${pg_name}" pg_isready -q -U tsf -d tsf \
                    >/dev/null 2>&1 &&
               docker exec "${my_name}" mariadb -utsf -ptsf-ci-secret \
                    -h127.0.0.1 -e 'SELECT 1' tsf >/dev/null 2>&1 ; then
                # The account may kill its own sessions (the lost
                # connection check) without further privileges.
                echo "export TSF_SQL_PG='127.0.0.1 ${pg_port} tsf" \
                     "tsf-ci-secret tsf'"
                echo "export TSF_SQL_MY='127.0.0.1 ${my_port} tsf" \
                     "tsf-ci-secret tsf'"
                exit 0
            fi
            sleep 2
        done
        echo "local-databases: the servers did not come up" >&2
        docker logs --tail 20 "${pg_name}" >&2 || true
        docker logs --tail 20 "${my_name}" >&2 || true
        exit 1
        ;;
    down)
        docker rm -f "${pg_name}" "${my_name}" >/dev/null 2>&1 || true
        ;;
    *)
        echo "usage: $0 up|down" >&2
        exit 2
        ;;
esac
