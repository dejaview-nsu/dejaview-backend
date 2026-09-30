# База данных и миграции

- Схема - миграции dbmate в `db/migrations/`. Применяет их сервис `db-migrate` из `dejaview-infra`
  до старта backend; сам backend миграции не применяет.
- Новая миграция: `docker run --rm -v "$PWD/db:/db" ghcr.io/amacneil/dbmate:2.36.0 new <имя>`.
  Нужны оба блока: `-- migrate:up` и непустой `-- migrate:down`.
- Применённую миграцию не правят, исправление - новой миграцией. Изменение схемы - PR сюда
  и в `dejaview-docs` (`contracts/db-schema.sql`).
- Применить новые миграции - повторить команду из [«Быстрого старта»](../README.md#быстрый-старт)
  в `dejaview-infra`: применяются только ещё не применённые.

Полный сброс БД - **удаляет все данные**:

```sh
cd dejaview-infra
docker compose down
docker volume rm dejaview_pg-data
docker compose --profile infra --profile app up -d postgres db-migrate
```
