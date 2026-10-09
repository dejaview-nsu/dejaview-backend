# Разработка в Dev Container

VS Code работает внутри контейнера, собранного из стадии `dev` нашего `Dockerfile`: GCC 14,
CMake, Conan с готовыми библиотеками, clangd, gdb, clang-format, gcovr, git. На компьютер ничего,
кроме Docker и VS Code, ставить не нужно, окружение у всех одинаковое.

## Первый запуск

1. Установить в VS Code расширение **Dev Containers**.
2. Открыть папку `dejaview-backend`, `Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) →
   **Dev Containers: Reopen in Container**.
3. Дождаться окончания настройки: Conan и CMake готовят `build/` - наш код в Debug, библиотеки
   в Release.
4. Создать `.env`: `cp .env.example .env`. БД из `dejaview-infra` должна быть запущена, см.
   [«Быстрый старт»](../README.md#быстрый-старт).
5. Запустить почту для разработки - на компьютере, не в контейнере: `docker compose up -d mailpit`.
   Письма, которые отправляет backend, видно на http://localhost:8025, подробнее - [email.md](email.md).

## Работа

| Действие | Как |
|---|---|
| Собрать | `Ctrl+Shift+B` (на Mac `Cmd+Shift+B`) или `cmake --build build` |
| Запустить под отладчиком | `F5`: соберёт и запустит с переменными из `.env`; точка останова - клик слева от номера строки |
| Запустить в терминале | `set -a && . ./.env && set +a && ./build/dejaview-backend` |
| Тесты | `cmake --build build --target check` или `Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) → **Tasks: Run Test Task**; для интеграционных - `docker compose up -d postgres-test` на компьютере, см. [code.md](code.md#тесты) |
| Покрытие | отдельная Clang-сборка, команды - в [code.md](code.md#тесты) |
| Форматирование | само при сохранении, правила в `.clang-format` |
| Добавить библиотеку | в `conanfile.txt`, затем обновить `conan.lock` командами из шапки `conanfile.txt` |
| Запросы к API | коллекция Bruno, см. [ниже](#запросы-к-api-bruno) |
| База данных | SQLTools, см. [ниже](#база-данных-sqltools) |
| Git | как обычно; SSH-ключи берутся из ssh-agent компьютера, в контейнер не копируются |

Порт 8081 VS Code пробрасывает сам: `curl localhost:8081/health` работает с компьютера.

После изменения `Dockerfile`, `conanfile.txt`, `conan.lock` или `.devcontainer/devcontainer.json` -
`Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) → **Dev Containers: Rebuild Container**.

## Запросы к API: Bruno

Коллекция - папка `bruno/` в репозитории, расширение Bruno ставится вместе с контейнером.

1. Запустить backend (`F5`) и Mailpit (`docker compose up -d mailpit` на компьютере).
2. Панель **Bruno** слева → **Open Collection** → папка `bruno`.
3. Окружение выбирать не нужно: значения по умолчанию - для контейнера (`bruno/collection.bru`).
4. Папка **auth** → **Run** - весь путь сам: регистрация нового пользователя → ссылка из письма в
   Mailpit → подтверждение → сессия → вход → выход. Результат проверок (assert) - в отчёте
   запуска.
   Отдельный запрос - открыть и нажать стрелку отправки справа от адреса.
5. Запросы, которым нужна сессия, берут её из переменной `session`: её сохраняют «Подтверждение
   email» и «Вход». Значения переменных - вкладка **Variables** коллекции.

Папка **oidc** запускается вручную: вход у провайдера идёт в браузере, порядок - в описании
запросов. Bruno на компьютере, а не в контейнере, - окружение `local`.

Новый запрос - файл `.bru` в нужной папке: правится в том же PR, что и эндпоинт.

## База данных: SQLTools

Расширение и подключение к БД из `dejaview-infra` ставятся вместе с контейнером.

1. Панель **SQLTools** слева (иконка цилиндра) → подключение **«dejaview (локальная)»** →
   **Connect**. При первом подключении SQLTools может предложить установить драйвер - согласиться.
2. Таблицы - в дереве подключения, двойной клик - первые строки.
3. Свой запрос: `Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) → **SQLTools: New SQL File**, написать SQL,
   выделить и `Ctrl+E Ctrl+E` (на Mac `Cmd+E Cmd+E`).

Полезное при проверке авторизации:

```sql
SELECT user_id, username, email, status, failed_login_count, locked_until FROM users;
SELECT email_id, kind, status, attempts, last_error FROM email_outbox;
SELECT occurred_at, event_type, user_id, details FROM security_events ORDER BY occurred_at DESC;
```

С компьютера, без контейнера, - DBeaver или pgAdmin: `localhost:57432`, база, пользователь и
пароль - из `.env` в `dejaview-infra`.

## Частые проблемы

| Симптом | Решение |
|---|---|
| `Cannot connect to the Docker daemon` | запустить Docker Desktop |
| `port is already allocated` | порт 8081 занят: остановить процесс на нём или поменять левый порт в `compose.yaml` |
| `env file .../.env not found` или `F5` не находит `envFile` | `cp .env.example .env` |
| `/health` → 503 | БД не запущена: команда из [«Быстрого старта»](../README.md#быстрый-старт) в `dejaview-infra` |
| `Ошибка конфигурации: ...` при старте | неверная переменная в `.env`, см. [«Конфигурация»](../README.md#конфигурация) |
| `db-migrate` завершился с ошибкой | `docker compose logs db-migrate` в `dejaview-infra` |
| clangd подчёркивает код Drogon красным | `Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) → **clangd: Restart language server**, не помогло - Rebuild Container |
| `git push`: `Permission denied (publickey)` | на компьютере `ssh-add`, перезапустить окно VS Code |
