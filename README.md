# dejaview-backend

REST API мультимодального поиска, бизнес-логика, база данных, интеграция с TMDB.

## Зона ответственности

- API поиска по четырём модальностям и формирование выдачи
- Профили, оценки, отзывы, списки «Хочу посмотреть» и «Просмотрено»
- Интеграция с TMDB: метаданные и постеры фильма по `movie_id`
- PostgreSQL: каталог фильмов и пользовательские данные
- Реализация контракта из `dejaview-docs/api/openapi.yaml`

## Стек

- C++23, GCC 14, CMake + Ninja
- [Drogon](https://github.com/drogonframework/drogon) 1.9.13: HTTP-сервер, JSON, клиент PostgreSQL
- Conan 2: зависимости C++, точные версии в `conan.lock`
- PostgreSQL 16, миграции [dbmate](https://github.com/amacneil/dbmate)
- GoogleTest, gcovr, clang-format 18
- Сборка и запуск в Docker: компилятор на компьютере не нужен

## Быстрый старт

Нужны Docker и Git. Репозитории кладём рядом: так compose из `dejaview-infra` находит миграции.

```sh
git clone git@github.com:dejaview-nsu/dejaview-backend.git
git clone git@github.com:dejaview-nsu/dejaview-infra.git

# PostgreSQL и миграции
cd dejaview-infra
docker compose --profile infra --profile app up -d postgres db-migrate

# backend
cd ../dejaview-backend
cp .env.example .env
docker compose up --build
```

`docker compose up --build` собирает образ, прогоняет модульные тесты (упавший тест останавливает
сборку) и запускает сервер. Остановить - `Ctrl+C`; в фоне - добавить `-d`, остановить -
`docker compose down`. Первая сборка ~5 минут (Conan собирает Drogon), дальше - секунды.

Проверка: `curl localhost:8081/health` → `{"database":"ok","status":"ok"}`, код 200.
503 - БД недоступна, ответ не дольше 2 с.

## Разработка в Dev Container

VS Code работает внутри контейнера, собранного из стадии `dev` нашего `Dockerfile`: GCC 14,
CMake, Conan с готовыми библиотеками, clangd, gdb, clang-format, gcovr, git. На компьютер ничего,
кроме Docker и VS Code, ставить не нужно, окружение у всех одинаковое.

### Первый запуск

1. Установить в VS Code расширение **Dev Containers**.
2. Открыть папку `dejaview-backend`, `Cmd+Shift+P` → **Dev Containers: Reopen in Container**.
3. Дождаться окончания настройки: Conan и CMake готовят `build/` - наш код в Debug, библиотеки
   в Release.
4. Создать `.env`: `cp .env.example .env`. БД из `dejaview-infra` должна быть запущена.

### Работа

| Действие | Как |
|---|---|
| Собрать | `Cmd+Shift+B` или `cmake --build build` |
| Запустить под отладчиком | `F5`: соберёт и запустит с переменными из `.env`; точка останова - клик слева от номера строки |
| Запустить в терминале | `set -a && . ./.env && set +a && ./build/dejaview-backend` |
| Тесты | `cmake --build build --target check` или `Cmd+Shift+P` → **Tasks: Run Test Task** |
| Покрытие | `cmake --build build --target coverage`, по строкам - `build/coverage/index.html` |
| Форматирование | само при сохранении, правила в `.clang-format` |
| Git | как обычно; SSH-ключи берутся из ssh-agent компьютера, в контейнер не копируются |

Порт 8081 VS Code пробрасывает сам: `curl localhost:8081/health` работает с компьютера.

После изменения `Dockerfile`, `conanfile.txt`, `conan.lock` или `.devcontainer/devcontainer.json` -
`Cmd+Shift+P` → **Dev Containers: Rebuild Container**.

### Если что-то не так

| Симптом | Решение |
|---|---|
| clangd подчёркивает код Drogon красным | `Cmd+Shift+P` → **clangd: Restart language server**, не помогло - Rebuild Container |
| `F5`: не найден `envFile` | `cp .env.example .env` |
| `git push`: `Permission denied (publickey)` | на компьютере `ssh-add`, перезапустить окно VS Code |

## Конфигурация

Только переменные окружения. Локально - файл `.env`: в git и в Docker-образ он не попадает.

| Переменная | Обязательная | По умолчанию | Назначение |
|---|---|---|---|
| `PORT` | нет | `8081` | порт HTTP-сервера |
| `DATABASE_URL` | да | - | `postgres://user:password@host:5432/db?sslmode=disable` |

При некорректном значении сервер не стартует: пишет причину в лог (без пароля), код выхода 1.

## Тесты

- Файлы `tests/<модуль>_test.cpp`, GoogleTest. Новый файл добавить в `add_executable(dejaview-tests ...)`
  в `CMakeLists.txt`.
- Модульные тесты не обращаются к БД и сети.
- Имя теста говорит, что проверяется: `LoadConfigTest.RejectsInvalidPort`.
- Порог покрытия 70% (#17098 п. 1.1): ниже - `coverage` завершается ошибкой.

## База данных и миграции

- Схема - миграции dbmate в `db/migrations/`. Применяет их сервис `db-migrate` из `dejaview-infra`
  до старта backend; сам backend миграции не применяет.
- Новая миграция: `docker run --rm -v "$PWD/db:/db" ghcr.io/amacneil/dbmate:2.36.0 new <имя>`.
  Нужны оба блока: `-- migrate:up` и непустой `-- migrate:down`.
- Применённую миграцию не правят, исправление - новой миграцией. Изменение схемы - PR сюда
  и в `dejaview-docs` (`contracts/db-schema.sql`).
- Применить новые миграции - повторить команду из «Быстрого старта» в `dejaview-infra`:
  применяются только ещё не применённые.

Полный сброс БД - **удаляет все данные**:

```sh
cd dejaview-infra
docker compose down
docker volume rm dejaview_pg-data
docker compose --profile infra --profile app up -d postgres db-migrate
```

## Зависимости C++

`conanfile.txt` - список библиотек, `conan.lock` - их точные версии. После изменения
`conanfile.txt` обновить `conan.lock` командами из шапки `conanfile.txt`.

## Docker-образ

| Стадия | Содержимое |
|---|---|
| `toolchain` | компилятор, CMake, Ninja, Conan |
| `deps` | библиотеки из `conanfile.txt` |
| `dev` | + clangd, gdb, clang-format, gcovr, git - для Dev Container |
| `build` | сборка и модульные тесты |
| `runtime` | итоговый образ: только бинарник, запуск не от root, порт 8081 |

## Частые проблемы

| Симптом | Решение |
|---|---|
| `Cannot connect to the Docker daemon` | запустить Docker Desktop |
| `port is already allocated` | порт 8081 занят: остановить процесс на нём или поменять левый порт в `compose.yaml` |
| `env file .../.env not found` | `cp .env.example .env` |
| `/health` → 503 | БД не запущена: команда из «Быстрого старта» в `dejaview-infra` |
| `Ошибка конфигурации: ...` при старте | неверная переменная в `.env`, см. «Конфигурация» |
| `db-migrate` завершился с ошибкой | `docker compose logs db-migrate` в `dejaview-infra` |

## Структура кода

```
src/
  main.cpp          точка входа: конфигурация → БД → маршруты → run()
  config.hpp/.cpp   настройки из переменных окружения
  health.hpp/.cpp   GET /health
tests/              модульные тесты
db/migrations/      миграции dbmate
```

- Код делится по разделам предметной области, как теги в `api/openapi.yaml`: `movies`, `search`,
  `auth`. Один раздел - пара файлов `<раздел>.hpp/.cpp`; разросся - папка `src/<раздел>/`.
- Внутри раздела три вида кода, обычными функциями:
  - обработчик: разбирает HTTP-запрос, вызывает логику, собирает ответ по контракту;
  - логика: проверки и правила на обычных типах C++, без Drogon - тестируется без БД и HTTP;
  - SQL: запросы с параметрами `$1, $2`, без склейки строк.
- Все маршруты регистрируются в `main.cpp` одним списком - его легко сверять с контрактом.
- Зависимости (`DbClient`, клиенты TMDB и ML) передаются параметрами, без глобальных переменных.
- Файлы, классы и интерфейсы появляются вместе с кодом, которому нужны, а не заранее.

## Ссылки

- Задачи и требования: https://ai.nsu.ru/projects/dejaview
- Архитектура, контракты, соглашения: https://github.com/dejaview-nsu/dejaview-docs
- Организация: https://github.com/dejaview-nsu

## Как работаем

- Ветка от `main`: `feat/<номер задачи>-<кратко>`, нейминг в `conventions.md`
- Изменения через Pull Request с ревью, прямой push в `main` закрыт
- Ревьюер назначается автоматически по CODEOWNERS
- Время трекается в Redmine, в задачу, а не в требование
