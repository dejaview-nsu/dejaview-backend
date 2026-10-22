#!/usr/bin/env bash
# Миграции при старте контейнера: tests/image/migrations_test.sh <образ>
set -euo pipefail

image=$1
net=migrations-test-$$
pg=$net-pg
migrations=$(find "$(dirname "$0")/../../db/migrations" -name '*.sql' | wc -l)

cleanup() {
    docker rm -f "$pg" >/dev/null 2>&1 || true
    docker network rm "$net" >/dev/null 2>&1 || true
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

url() { echo "POSTGRES_URL=postgres://test:test@$pg:5432/$1?sslmode=disable"; }
sql() { docker exec "$pg" psql -U test -d "$1" -Atc "$2"; }
# pg_dump 16.10+ без --restrict-key пишет случайный ключ, и одинаковые схемы не совпадают
schema() { docker exec "$pg" pg_dump -U test -d test --schema-only --restrict-key=test; }
# echo вместо сервера: видно, запустилась ли команда после миграций
start() { docker run --rm --network "$net" "$@" "$image" echo STARTED 2>&1; }

docker network create "$net" >/dev/null
docker run -d --name "$pg" --network "$net" -e POSTGRES_USER=test -e POSTGRES_PASSWORD=test \
    postgres:16.11 >/dev/null

echo "пустая БД"
out=$(start -e "$(url test)") || fail "$out"
[[ $out == *STARTED* ]] || fail "команда не запущена: $out"
[[ $(sql test "SELECT count(*) FROM schema_migrations") == "$migrations" ]] \
    || fail "применены не все миграции"
before=$(schema)

echo "повторный старт"
out=$(start -e "$(url test)") || fail "$out"
[[ $out == *STARTED* ]] || fail "команда не запущена: $out"
[[ $(schema) == "$before" ]] || fail "схема изменилась"

echo "упавшая миграция"
sql test "CREATE DATABASE broken" >/dev/null
sql broken "CREATE TABLE users (id integer)" >/dev/null
out=$(start -e "$(url broken)") && fail "контейнер стартовал: $out"
[[ $out != *STARTED* && $out == *"Миграции не применены"* ]] || fail "$out"

echo "без POSTGRES_URL"
out=$(start) && fail "контейнер стартовал: $out"
[[ $out != *STARTED* && $out == *POSTGRES_URL* ]] || fail "$out"

echo OK
