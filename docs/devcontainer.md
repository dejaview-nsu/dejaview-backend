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
   Письма, которые отправляет backend, видно на http://localhost:8025.

## Работа

| Действие | Как |
|---|---|
| Собрать | `Ctrl+Shift+B` (на Mac `Cmd+Shift+B`) или `cmake --build build` |
| Запустить под отладчиком | `F5`: соберёт и запустит с переменными из `.env`; точка останова - клик слева от номера строки |
| Запустить в терминале | `set -a && . ./.env && set +a && ./build/dejaview-backend` |
| Тесты | `cmake --build build --target check` или `Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) → **Tasks: Run Test Task** |
| Покрытие | `cmake --build build --target coverage`, по строкам - `build/coverage/index.html` |
| Форматирование | само при сохранении, правила в `.clang-format` |
| Добавить библиотеку | в `conanfile.txt`, затем обновить `conan.lock` командами из шапки `conanfile.txt` |
| Git | как обычно; SSH-ключи берутся из ssh-agent компьютера, в контейнер не копируются |

Порт 8081 VS Code пробрасывает сам: `curl localhost:8081/health` работает с компьютера.

После изменения `Dockerfile`, `conanfile.txt`, `conan.lock` или `.devcontainer/devcontainer.json` -
`Ctrl+Shift+P` (на Mac `Cmd+Shift+P`) → **Dev Containers: Rebuild Container**.

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
