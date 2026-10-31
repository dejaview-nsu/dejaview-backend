# Frontend: подключение к backend и проверка

Для разработчиков SPA: как ходить в API авторизации, запустить backend у себя и проверить экраны.
Поля, коды ошибок, тексты и правила - контракт `dejaview-docs/api/openapi.yaml`, теги Auth и OIDC.
Здесь - то, чего в контракте нет.

## Что готово

Регистрация, подтверждение email, повторная отправка письма, вход по паролю с CAPTCHA и
блокировкой, текущая сессия, выход, вход и регистрация через Яндекс, Google и VK ID,
восстановление пароля по ссылке из письма, смена пароля из профиля, карточка фильма: из каталога,
а фильм не из каталога backend получает из TMDB.

## Как ходить в API

- **Только относительные адреса** `/api/v1/...` с того же origin, что и страница. CORS backend не
  отдаёт: запрос из JavaScript на другой порт или домен браузер не пропустит. В продакшене
  `/api/v1/` проксирует nginx ([deployment.md](deployment.md#nginx-и-домен)), локально - Vite:

  ```ts
  // vite.config.ts
  export default defineConfig({
    server: {
      // frontend в Docker - http://host.docker.internal:8081
      proxy: { '/api': 'http://localhost:8081' },
    },
  });
  ```

- **Сессия - cookie `dv_session` с `HttpOnly`.** JavaScript её не видит, браузер отправляет сам:
  `fetch` на тот же origin шлёт cookie по умолчанию. Кто вошёл, SPA узнаёт при загрузке из
  `GET /api/v1/auth/session`: 200 - пользователь, 401 - гость. 401 на любом защищённом запросе
  (`SESSION_REQUIRED`, `SESSION_EXPIRED`) - показать вход, недействительную cookie backend сотрёт
  сам.
- **Cookie с флагом `Secure`.** Chrome и Firefox принимают её и на `http://localhost`, Safari может
  не сохранить - локально лучше Chrome или Firefox.
- **Ошибки** - JSON `{"code", "message", "field"}`. `message` - готовый текст для пользователя,
  `field` - какое поле подсветить (есть не всегда). Решения принимать по `code`, не по тексту:
  например, 429 бывает `AUTH_LOGIN_LOCKED` (блокировка входа) и `RATE_LIMITED` (лимит запросов
  nginx, только в продакшене).
- **Вход через провайдера - переход браузера, не `fetch`**: кнопка - обычная ссылка
  `<a href="/api/v1/auth/oidc/yandex/start">` (`yandex`, `google`, `vk`). Провайдер, для которого в
  backend нет ключей, выключен: переход сразу вернёт `result=error`.

## Экраны и запросы

| Экран | Запрос | Что обработать |
|---|---|---|
| Загрузка SPA, шапка | `GET /auth/session` | 200 - пользователь, 401 - гость |
| Меню пользователя → «Выйти» | `POST /auth/logout` | 204 - cookie стёрта, показать гостя на главной; 401 - сессия уже истекла, это тоже выход: окно входа не открывать |
| Регистрация | `POST /auth/register` | 201 → экран «Проверьте почту» с таймером `resend_after` секунд; 409 - email или имя заняты |
| «Проверьте почту» | `POST /auth/resend-confirmation` с `login` | 202 с `resend_after` и для несуществующего адреса: есть ли он, не раскрывается |
| `/confirm-email?token=...` - ссылка из письма | `POST /auth/confirm-email` с `token` | 200 - сессия создана, пользователь вошёл; 410 - ссылка истекла или уже использована → «Отправить новую ссылку»: `resend-confirmation` с тем же `token` |
| Вход → «Забыли пароль?» | `POST /auth/password-reset/request` с `login` | 202 всегда, есть учётная запись или нет → «Если учётная запись с таким email существует, мы отправили ссылку...»; 400 - ошибка поля `login`. Новое письмо - не чаще раза в 60 с, повтор раньше ничего не отправит: кнопку можно блокировать на минуту |
| `/reset-password?token=...` - ссылка из письма, маршрут задаёт backend | `POST /auth/password-reset/check` с `token` | 204 → форма «Новый пароль»; 410 `AUTH_RESET_LINK_EXPIRED` - ссылка истекла, заменена новой или использована → сообщение из ответа и кнопка «Запросить повторно» → форма запроса |
| Форма «Новый пароль» | `POST /auth/password-reset/complete` с `token` и `new_password` | 200 - пароль изменён, прежние сессии завершены, пользователь уже вошёл (cookie в ответе) → главная и «Пароль успешно изменён»; 400 с `field: new_password` - правила или совпадение с текущим, ссылка не израсходована; 410 - как при проверке ссылки |
| `/forgot-password` - кнопка «Восстановить пароль» в письме «Пароль изменён», маршрут задаёт backend | - | форма запроса восстановления, как по «Забыли пароль?» |
| Профиль → «Безопасность» → «Сменить пароль» | `PUT /users/me/password` с `current_password` и `new_password` | При `has_password: false` из `GET /auth/session` поля «Текущий пароль» нет, `current_password` не отправлять. 204 - пароль изменён, другие сессии завершены → «Пароль успешно изменён»; 400 с `field: current_password` - пустой или неверный (`AUTH_CURRENT_PASSWORD_INVALID`), с `field: new_password` - правила или совпадение с текущим |
| Вход | `POST /auth/login` | `captcha_required: true` или 403 `AUTH_CAPTCHA_REQUIRED` → виджет CAPTCHA, затем та же форма с `captcha_token`; 429 - блокировка, через сколько секунд - заголовок `Retry-After`; 403 `AUTH_EMAIL_NOT_CONFIRMED` → «Отправить новую ссылку» с тем же `login` |
| Страница фильма | `GET /movies/{movie_id}`, гостю тоже | 200 - карточка `MovieCard`: пустые поля - `null` и `[]`, блок с ними не показывать; 404 `MOVIE_NOT_FOUND` - сообщение вместо карточки; 503 `MOVIE_UNAVAILABLE` - фильма нет в каталоге, а TMDB недоступен: сообщение из ответа, можно повторить позже |
| `/auth/oidc?provider=...&result=...` - возврат от провайдера | по `result` | `success` - вошёл; `registration_required` → `GET /auth/oidc/pending` → форма с `suggested_username` и годом рождения → `POST /auth/oidc/complete`; `link_required` → `pending` даёт `email` → обычный вход с паролем, провайдер привяжется сам; `account_blocked`, `error` - сообщение. 410 `AUTH_OIDC_EXPIRED` - 30 минут прошли, войти заново |

## CAPTCHA

Виджет Yandex SmartCaptcha. Ключ клиента - при сборке frontend, ключ сервера - в
`SMARTCAPTCHA_SERVER_KEY` в `.env` backend. Это пара от одной капчи в консоли Yandex Cloud
(SmartCaptcha → капча): ключ сервера к ключу клиента от другой капчи проверку не пройдёт.

```html
<script src="https://smartcaptcha.yandexcloud.net/captcha.js?render=onload&onload=onCaptchaLoaded" defer></script>
```

```js
let widgetId = null;
let captchaToken = null;

// Ответ на вход с captcha_required: true или AUTH_CAPTCHA_REQUIRED
function showCaptcha() {
  if (widgetId !== null) {
    window.smartCaptcha.reset(widgetId);  // токен одноразовый: на каждую попытку новый
    return;
  }
  widgetId = window.smartCaptcha.render(document.getElementById('captcha'), {
    sitekey: '<ключ клиента>',
    hl: 'ru',
    callback: (token) => { captchaToken = token; },  // отправить форму снова с captcha_token
  });
}
```

Для React есть готовый компонент `@yandex/smart-captcha`. Без `SMARTCAPTCHA_SERVER_KEY` CAPTCHA в
backend выключена: `captcha_required` всегда `false`, работает только блокировка. Виджет не
появляется на `localhost` - проверить список сайтов в настройках капчи.

## Локальный запуск

1. PostgreSQL с миграциями из `dejaview-infra` и backend из `dejaview-backend` - по
   [README](../README.md#быстрый-старт). Backend - на http://localhost:8081, проверка:
   `curl localhost:8081/health`.
2. В `.env` backend `APP_URL` - адрес frontend: на него ведут ссылки из писем и возврат от
   провайдера. Frontend из `dejaview-infra` - `http://localhost:57437`, Vite на компьютере -
   `http://localhost:5173`. После правки `.env` - перезапустить backend.
3. Почта: `docker compose up -d mailpit` в `dejaview-backend`, письма - на http://localhost:8025
   ([email.md](email.md)).
4. Вход через провайдеров - только с приложениями у Яндекса, Google и VK и их ключами в `.env`
   backend. VK требует HTTPS-адрес, нужен туннель: [oidc.md](oidc.md). Страницы после провайдера
   можно проверить и без него - см. ниже.

## Как проверить экраны

Состояние удобно менять прямо в БД: `psql postgres://dejaview:dejaview_local_only@localhost:57432/dejaview`
или любой клиент PostgreSQL.

| Что проверить | Как получить |
|---|---|
| Письмо и ссылка подтверждения | зарегистрироваться, письмо - в Mailpit |
| Истёкшая ссылка подтверждения | `UPDATE auth_tokens SET created_at = now() - interval '25 hours', expires_at = now() - interval '1 hour' WHERE purpose = 'email_confirm';` |
| Письмо и ссылка сброса пароля | «Забыли пароль?» с именем или email существующего пользователя, письмо - в Mailpit |
| Истёкшая ссылка сброса | `UPDATE auth_tokens SET created_at = now() - interval '2 hours', expires_at = now() - interval '1 hour' WHERE purpose = 'password_reset';` |
| Письмо «Пароль изменён» | задать новый пароль по ссылке сброса, письмо - в Mailpit |
| Вход до подтверждения email | зарегистрироваться и не переходить по ссылке |
| CAPTCHA | ключи в `.env` backend и frontend, 3 неверных пароля подряд |
| Блокировка входа | 5 неверных паролей подряд; снять: `UPDATE users SET failed_login_count = 0, locked_until = NULL;` |
| Заблокированная учётная запись | `UPDATE users SET status = 'blocked' WHERE username = '<имя>';` - вход: `AUTH_ACCOUNT_BLOCKED`, открытая сессия: 401 `SESSION_EXPIRED` |
| Сессия истекла | `DELETE FROM sessions;` |
| Карточка фильма | в локальной БД каталога нет - фильм вставить, ниже |

**Страницы OIDC без провайдера.** Незавершённый вход, каким его оставляет возврат от провайдера:

```sql
INSERT INTO oidc_pending (token_hash, provider, state, code_verifier, subject, email,
                          display_name, expires_at)
VALUES (sha256('dev-pending'), 'yandex', 's', 'v', 'dev-sub-1', 'ivan@yandex.ru',
        'Иван Петров', now() + interval '30 minutes');
```

Затем в консоли браузера на странице frontend - cookie с этим токеном, и открыть страницу исхода:

```js
document.cookie = 'dv_oidc=dev-pending; path=/api/v1/auth';
location.href = '/auth/oidc?provider=yandex&result=registration_required';
```

Для `link_required` - в `INSERT` добавить `link_user_id` существующего пользователя и его email.

**Фильм для карточки.** Каталог собирает `scripts/catalog` в `dejaview-ml`, локально его можно
заменить одной строкой:

```sql
INSERT INTO movies (movie_id, title, original_title, release_date, age_rating, poster_path,
                    runtime_min, overview, genres, countries, crew, actors)
VALUES (157336, 'Интерстеллар', 'Interstellar', '2014-11-05', '12+',
        '/gEU2QniE6E77NI6lCU6MxlNBvIx.jpg', 169,
        'Группа исследователей отправляется через червоточину.',
        '[{"id": 878, "name": "фантастика"}]', '[{"code": "US", "name": "США"}]',
        '[{"person_id": 525, "name": "Кристофер Нолан", "role": "director"}]',
        '[{"person_id": 3895, "name": "Майкл Кейн", "character": "профессор Брэнд",
           "profile_path": null}]');
```

Карточка - `GET /api/v1/movies/157336`, фильм без данных - тот же `INSERT` только с `movie_id`,
`title` и `original_title`. Фильм не из каталога backend берёт из TMDB, если в его `.env` задан
`TMDB_ACCESS_TOKEN`; без ключа такой фильм получает 503.

## Примеры запросов

- Коллекция Bruno - `bruno/` в этом репозитории: живые тела запросов и ответов, папка `auth`
  проходит весь путь от регистрации до выхода ([devcontainer.md](devcontainer.md#запросы-к-api-bruno)).
- curl - [auth.md](auth.md#сессия-для-локальной-проверки).

## Ещё документы

- [auth.md](auth.md) - как устроена авторизация на backend.
- [oidc.md](oidc.md) - приложения у провайдеров, туннель для VK, частые ошибки.
- [email.md](email.md) - Mailpit и настоящая отправка писем.
- [deployment.md](deployment.md) - продакшен: один домен, nginx, ключ клиента SmartCaptcha.
