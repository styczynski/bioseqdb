#!/bin/bash

if [[ "$RUN_SMOKE_TEST" != "true" ]]; then
  echo "Smoke test is disabled"
  exit 0
fi

set -e

TEST_PATH="$1"
timer="5"

export POSTGRES_USER=postgres
export POSTGRES_PASSWORD=changeme
export PGPASSWORD=${POSTGRES_PASSWORD}
export PGDATA=/var/lib/postgresql/data
./pg_start.sh postgres &

until runuser -l postgres -c 'pg_isready' 2>/dev/null; do
  >&2 echo "Postgres is unavailable - sleeping for $timer seconds"
  sleep $timer
done

sleep 5

>&2 echo "Postgres is up - executing command"

psql -h localhost --username=${POSTGRES_USER} -f ${TEST_PATH}
PSQL_STATUS="$?"

if [[ "${PSQL_STATUS}" != "0" ]]; then
  echo "\n ==== SMOKE TEST HAS FAILED ====\n"
  exit 1
fi
