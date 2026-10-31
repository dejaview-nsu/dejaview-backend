# Код и тесты

## Структура

```
src/
  main.cpp              точка входа: конфигурация → БД → фоновые задачи → маршруты → run()
  config.hpp/.cpp       настройки из переменных окружения
  error_response.*      тело ошибки Error из контракта
  health.*              GET /health
  movies.*              GET /movies/{movie_id}: карточка фильма из movies
  background.*          фоновый поток: письма из очереди, очистка устаревшего
  auth/                 регистрация, вход, сессии, пароль, OIDC - docs/auth.md
  search/               поиск по изображению, видео, аудио и тексту: проверки файла и запроса, клиент ML, выключатель
  email/                письма: шаблоны templates/*.html, формат, отправка из очереди
tests/                  модульные тесты, раскладка как в src/
db/migrations/          миграции dbmate
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

## Тесты

- Файлы `tests/<раздел>/<модуль>_test.cpp`, GoogleTest. Новый файл добавить в
  `add_executable(dejaview-tests ...)` в `CMakeLists.txt`.
- Имя теста говорит, что проверяется: `LoadConfigTest.RejectsInvalidPort`.
- **Модульные** тесты - чистая логика, без БД и сети.
- **Интеграционные** - `tests/integration/`: обработчики вызываются напрямую с настоящим PostgreSQL,
  проверяются ответ и строки в таблицах. Класс теста наследует `DbTest` (`db_test.hpp`): перед
  каждым тестом таблицы пустые, миграции из `db/migrations` применяются сами, есть помощники
  `request`, `run`, `scalar`, `createUser`, `lastLinkToken`.
- БД интеграционных тестов - `TEST_DATABASE_URL`, сервис `postgres-test` из `compose.yaml`:
  `docker compose up -d postgres-test` на компьютере. В Dev Container переменная уже задана. Без неё
  интеграционные тесты пропускаются (Skipped), остальные идут как обычно.
- Пороги покрытия (#17098): весь `src/` - 70%, авторизация `src/auth/` и поиск `src/search/` - по 80%.
  Ниже - `coverage` завершается ошибкой; без тестовой БД эти пороги не набрать.
- **Покрытие считается только Clang-сборкой:** gcov из GCC не видит тела корутин (`Task<...>`), а
  это почти все обработчики. Отдельная сборка в `build-coverage/`, в Dev Container:
  ```sh
  CXX=clang++-20 cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
  cmake --build build-coverage --target coverage
  ```
  Отчёт по строкам - `build-coverage/coverage/index.html`. В обычной сборке (GCC) цели `coverage`
  нет смысла: она подскажет команды выше и завершится ошибкой.
- **Санитайзеры и фаззинг** - для кода, который разбирает чужие байты (загруженные файлы,
  тексты запросов). ASan и UBSan роняют тест на чтении за границей, переполнении и UB, которые
  обычная сборка пропустит молча; libFuzzer гоняет разбор MP4 (`src/search/mp4_tracks.cpp`) на
  случайных входах, начальный корпус - образцы из `tests/data/search`. Отдельная сборка в
  `build-sanitize/` (покрытия в ней нет), в Dev Container:
  ```sh
  CXX=clang++-20 cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DDEJAVIEW_SANITIZE=ON -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
  cmake --build build-sanitize --target check   # все тесты под ASan и UBSan
  cmake --build build-sanitize --target fuzz    # фаззинг 15 минут
  ```
  Найденный фаззером вход libFuzzer сохраняет в `build-sanitize/crash-*`: его байты становятся
  модульным тестом в `tests/search/mp4_tracks_test.cpp`.
- **Внешние сервисы** (SmartCaptcha, OIDC-провайдеры) в тестах заменяет фейковый HTTP-сервер в том
  же процессе: `DbTest::fakeReply` задаёт ответ на путь, `fakeReceived` - что прислал backend. Его
  адрес попадает в код через `CaptchaSettings::url` и `OidcSettings::fakeProviderUrl`. С настоящими
  сервисами вход проверяют вручную ([oidc.md](oidc.md)).
