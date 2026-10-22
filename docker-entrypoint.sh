#!/bin/sh
set -eu

: "${POSTGRES_URL:?не задан}"

# migrate, а не up: базу создаёт PostgreSQL, права CREATEDB пользователю backend не нужны
dbmate --env POSTGRES_URL --migrations-dir /usr/local/share/dejaview/migrations \
    --no-dump-schema --wait migrate \
    || { echo "Миграции не применены, backend не запущен" >&2; exit 1; }

exec "$@"
