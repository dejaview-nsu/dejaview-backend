# База данных и миграции

- Схема - миграции dbmate в `db/migrations/`. Их применяет контейнер backend при старте
  (`docker-entrypoint.sh`): ждёт PostgreSQL до 1 минуты, применяет новые миграции, затем запускает
  сервер. Адрес - `POSTGRES_URL`, `sslmode` в нём обязателен: без него dbmate требует SSL.
- Упавшая миграция откатывается, контейнер завершается с кодом 1, сервер не запускается. Причина -
  в `docker compose logs backend` перед строкой `Миграции не применены, backend не запущен`.
- Локально миграции применяет и `db-migrate` из `dejaview-infra`. Учёт общий (`schema_migrations`),
  повторно ничего не применяется. В Dev Container сервер запускается без образа - там миграции
  только из `db-migrate`.
- Новая миграция: `docker run --rm -v "$PWD/db:/db" ghcr.io/amacneil/dbmate:2.36.0 new <имя>`.
  Нужны оба блока: `-- migrate:up` и непустой `-- migrate:down`.
- Применённую миграцию не правят, исправление - новой миграцией. Изменение схемы - PR сюда
  и в `dejaview-docs` (`contracts/db-schema.sql`).
- Применить новые миграции - перезапустить контейнер backend с новым образом или повторить
  команду из [«Быстрого старта»](../README.md#быстрый-старт) в `dejaview-infra`: применяются
  только ещё не применённые.
- Проверка образа: `tests/image/migrations_test.sh <образ>`, в CI - задача `build-image`.

Полный сброс БД - **удаляет все данные**:

```sh
cd dejaview-infra
docker compose down
docker volume rm dejaview_pg-data
docker compose --profile infra --profile app up -d postgres db-migrate
```
