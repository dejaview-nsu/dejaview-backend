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
- GoogleTest, gcovr, clang-format 20
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

`docker compose up --build` собирает образ (стадии описаны в `Dockerfile`), прогоняет модульные
тесты (упавший тест останавливает сборку) и запускает сервер. Остановить - `Ctrl+C`; в фоне -
добавить `-d`, остановить - `docker compose down`. Первая сборка ~5 минут (Conan собирает Drogon),
дальше - секунды.

Проверка: `curl localhost:8081/health` → `{"database":"ok","status":"ok"}`, код 200.
503 - БД недоступна, ответ не дольше 2 с.

Если что-то не так - [частые проблемы](docs/devcontainer.md#частые-проблемы).

## Конфигурация

Только переменные окружения. Локально - файл `.env`: в git и в Docker-образ он не попадает.

| Переменная | Обязательная | По умолчанию | Назначение |
|---|---|---|---|
| `PORT` | нет | `8081` | порт HTTP-сервера |
| `DATABASE_URL` | да | - | `postgres://user:password@host:5432/db?sslmode=disable` |
| `APP_URL` | да | - | адрес frontend для ссылок в письмах, например `https://dejaview.ru` |
| `SMARTCAPTCHA_SERVER_KEY` | в продакшене | - | серверный ключ Yandex SmartCaptcha; не задан - CAPTCHA при входе отключена |
| `SMTP_URL` | да | - | почтовый сервер: `smtps://smtp.example.ru:465` (TLS), локально `smtp://host.docker.internal:1025` (Mailpit) |
| `SMTP_FROM` | да | - | адрес отправителя писем: `noreply@dejaview.ru` |
| `SMTP_USER`, `SMTP_PASSWORD` | нет | - | учётные данные SMTP; не заданы - без авторизации |
| `API_URL` | нет | `APP_URL` | публичный адрес backend для redirect_uri OIDC: `{API_URL}/api/v1/auth/oidc/<провайдер>/callback` |
| `OIDC_YANDEX_CLIENT_ID`, `OIDC_YANDEX_CLIENT_SECRET` | нет | - | приложение на oauth.yandex.ru; не заданы - вход через Яндекс выключен |
| `OIDC_GOOGLE_CLIENT_ID`, `OIDC_GOOGLE_CLIENT_SECRET` | нет | - | клиент Web application в console.cloud.google.com; не заданы - вход через Google выключен |
| `OIDC_VK_CLIENT_ID` | нет | - | ID приложения в кабинете VK ID; не задан - вход через VK выключен |

При некорректном значении сервер не стартует: пишет причину в лог (без пароля), код выхода 1.

## Документация

- [Разработка в Dev Container](docs/devcontainer.md): сборка, отладка, тесты, частые проблемы
- [База данных и миграции](docs/database.md): новая миграция, сброс БД
- [Код и тесты](docs/code.md): структура `src/`, правила для кода и тестов

## Ссылки

- Задачи и требования: https://ai.nsu.ru/projects/dejaview
- Архитектура, контракты, соглашения: https://github.com/dejaview-nsu/dejaview-docs
- Организация: https://github.com/dejaview-nsu

## Как работаем

- Ветка от `main`: `feat/<номер задачи>-<кратко>`, нейминг в `conventions.md`
- Изменения через Pull Request с ревью, прямой push в `main` закрыт
- Ревьюер назначается автоматически по CODEOWNERS
- Время трекается в Redmine, в задачу, а не в требование
