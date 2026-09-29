-- migrate:up
-- Первая миграция: схема из dejaview-docs/contracts/db-schema.sql (#17627) без изменений.
-- Применённую миграцию не правят: изменение схемы - новой миграцией (dbmate new <имя>).
--
-- Соглашения: имена в snake_case, время в timestamptz, перечисления - text + CHECK,
-- пустое значение - NULL (скалярное поле) или '[]' (список), пустых строк нет.

-- ============================================================================
-- Каталог фильмов: кеш TMDB (#17393 п. 2, #17241, состав полей - #17640)
-- ============================================================================

CREATE TABLE movies (
    movie_id        bigint PRIMARY KEY CHECK (movie_id > 0),       -- id фильма в TMDB (#17241)
    title           text COLLATE "ru-RU-x-icu" NOT NULL CHECK (title <> ''),  -- ru-RU, блок 3
    original_title  text NOT NULL CHECK (original_title <> ''),    -- блок 3
    release_date    date,                                          -- год для блока 3
    age_rating      text CHECK (age_rating IN ('0+', '6+', '12+', '16+', '18+')),  -- блок 3
    poster_path     text CHECK (poster_path LIKE '/%'),            -- путь на CDN TMDB, блок 1
    runtime_min     smallint CHECK (runtime_min > 0),              -- блок 8
    overview        text CHECK (overview <> ''),                   -- блок 9
    genres          jsonb NOT NULL DEFAULT '[]' CHECK (jsonb_typeof(genres) = 'array'),     -- блок 8
    countries       jsonb NOT NULL DEFAULT '[]' CHECK (jsonb_typeof(countries) = 'array'),  -- блок 8
    crew            jsonb NOT NULL DEFAULT '[]' CHECK (jsonb_typeof(crew) = 'array'),       -- блок 8
    actors          jsonb NOT NULL DEFAULT '[]' CHECK (jsonb_typeof(actors) = 'array'),     -- блок 10
    cached_at       timestamptz NOT NULL DEFAULT now(),            -- когда метаданные получены из TMDB
    indexed_at      timestamptz                                    -- когда векторы записаны в Qdrant (#17650)
);

-- ============================================================================
-- Учетные записи и доступ (#17148, #17149, #17150, #17151, #17094)
-- ============================================================================

CREATE TABLE users (
    user_id             bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username            text NOT NULL CHECK (username ~ '^[A-Za-z0-9_]{3,30}$'),  -- #17148 п. 2.1
    email               text NOT NULL CHECK (char_length(email) <= 254 AND email LIKE '_%@_%'),
    -- Обязателен всегда: учетная запись через OIDC создается на шаге «Завершение регистрации»,
    -- когда год уже введен (#17148 п. 3.2 шаг 10). Условие монотонно по времени, поэтому
    -- переоценка при restore безопасна.
    birth_year          smallint NOT NULL CHECK (birth_year >= 1900
                                                 AND birth_year <= extract(year FROM current_date)),
    -- Только PHC-строка Argon2id (#17094 п. 2.2). NULL - аккаунт без пароля, вход через OIDC (#17150 п. 3.1).
    password_hash       text CHECK (password_hash LIKE '$argon2id$%'),
    status              text NOT NULL DEFAULT 'unconfirmed'
                        CHECK (status IN ('unconfirmed', 'active', 'blocked')),  -- #17148 п. 2.5, #17149 п. 2.4
    failed_login_count  smallint NOT NULL DEFAULT 0 CHECK (failed_login_count >= 0),  -- #17149 п. 2.5
    locked_until        timestamptz,                               -- блокировка на 15 мин (#17094 п. 3.3)
    avatar_key          text,                                      -- ключ объекта в MinIO (#17030 п. 2.1.1)
    created_at          timestamptz NOT NULL DEFAULT now()         -- дата регистрации (#17030 п. 2.1.4)
);

-- Уникальность без учета регистра: «Ivan» и «ivan» - одно имя.
CREATE UNIQUE INDEX users_username_key ON users (lower(username));
CREATE UNIQUE INDEX users_email_key ON users (lower(email));

CREATE TABLE oidc_accounts (
    provider    text NOT NULL CHECK (provider IN ('google', 'yandex', 'vk')),  -- #17148 п. 3.1
    subject     text NOT NULL CHECK (subject <> ''),               -- id пользователя у провайдера (claim sub)
    user_id     bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    linked_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (provider, subject),
    UNIQUE (user_id, provider)
);

-- Незавершенный вход через OIDC: от ухода к провайдеру до входа или завершения регистрации.
-- В cookie dv_oidc - случайный токен, в БД - его SHA-256. Срок 30 минут (#17629, тег OIDC).
CREATE TABLE oidc_pending (
    token_hash      bytea PRIMARY KEY CHECK (octet_length(token_hash) = 32),
    provider        text NOT NULL CHECK (provider IN ('google', 'yandex', 'vk')),
    state           text NOT NULL CHECK (state <> ''),             -- OAuth2 state
    code_verifier   text NOT NULL CHECK (code_verifier <> ''),     -- PKCE
    subject         text,                                          -- sub провайдера, после callback
    email           text,                                          -- email из профиля провайдера
    display_name    text,                                          -- имя из профиля, основа имени (#17148 п. 3.2 шаг 7)
    link_user_id    bigint REFERENCES users ON DELETE CASCADE,     -- исход link_required: чья запись
    created_at      timestamptz NOT NULL DEFAULT now(),
    expires_at      timestamptz NOT NULL,
    CHECK (expires_at > created_at AND expires_at <= created_at + interval '30 minutes')
);
CREATE INDEX oidc_pending_expires_idx ON oidc_pending (expires_at);

-- Серверные сессии. В cookie dv_session - случайный токен, в БД - его SHA-256.
CREATE TABLE sessions (
    token_hash  bytea PRIMARY KEY CHECK (octet_length(token_hash) = 32),
    user_id     bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL,
    CHECK (expires_at > created_at AND expires_at <= created_at + interval '24 hours')  -- #17094 п. 1.3
);

CREATE INDEX sessions_user_id_idx ON sessions (user_id);        -- выход везде при смене пароля (#17150 п. 2.5)
CREATE INDEX sessions_expires_at_idx ON sessions (expires_at);  -- очистка по таймеру

-- Ссылки из писем: подтверждение email (24 ч, #17148 п. 2.5) и сброс пароля (1 ч, #17150 п. 1.2).
-- Одна строка на пользователя и назначение: новая ссылка заменяет старую, использованная удаляется.
CREATE TABLE auth_tokens (
    token_hash  bytea PRIMARY KEY CHECK (octet_length(token_hash) = 32),
    user_id     bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    purpose     text NOT NULL CHECK (purpose IN ('email_confirm', 'password_reset')),
    created_at  timestamptz NOT NULL DEFAULT now(),                -- от нее же отсчет 60 с до повторной отправки
    expires_at  timestamptz NOT NULL,
    UNIQUE (user_id, purpose),
    CHECK (expires_at > created_at AND expires_at <= created_at
           + CASE purpose WHEN 'email_confirm' THEN interval '24 hours' ELSE interval '1 hour' END)
);

CREATE INDEX auth_tokens_expires_at_idx ON auth_tokens (expires_at);

-- Очередь исходящих писем. Фоновый поток каждого экземпляра backend выбирает строки через
-- FOR UPDATE SKIP LOCKED, отправленное письмо удаляется. Адрес берется из users.email.
CREATE TABLE email_outbox (
    email_id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id          bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    kind             text NOT NULL CHECK (kind IN ('email_confirm', 'password_reset', 'password_changed')),
    payload          jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(payload) = 'object'),  -- параметры шаблона
    status           text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'failed')),
    attempts         smallint NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    next_attempt_at  timestamptz NOT NULL DEFAULT now(),
    last_error       text,
    created_at       timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX email_outbox_pending_idx ON email_outbox (next_attempt_at) WHERE status = 'pending';

-- Журнал событий безопасности (#17094 п. 5.1), хранится 30 дней (п. 5.2).
CREATE TABLE security_events (
    event_id     bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    occurred_at  timestamptz NOT NULL DEFAULT now(),
    event_type   text NOT NULL CHECK (event_type IN
                     ('login_success', 'login_failure', 'logout', 'password_change', 'access_denied')),
    user_id      bigint REFERENCES users ON DELETE SET NULL,       -- NULL: логин не найден или профиль удален
    ip           inet,
    details      jsonb CHECK (jsonb_typeof(details) = 'object')    -- причина отказа, способ входа, путь
);

CREATE INDEX security_events_occurred_at_idx ON security_events (occurred_at);
CREATE INDEX security_events_user_id_idx ON security_events (user_id, occurred_at);

-- ============================================================================
-- Пользовательские данные (#17026, #17027, #17028, #17029, #17030)
-- ============================================================================

-- Списки «Хочу посмотреть» и «Просмотрено». Фильм может быть в обоих списках сразу.
CREATE TABLE user_movie_lists (
    user_id    bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    list_type  text NOT NULL CHECK (list_type IN ('want_to_watch', 'watched')),
    movie_id   bigint NOT NULL REFERENCES movies ON DELETE CASCADE,
    added_at   timestamptz NOT NULL DEFAULT now(),                 -- сортировка по дате добавления (#17030 п. 2.4.6)
    PRIMARY KEY (user_id, list_type, movie_id)
);

CREATE INDEX user_movie_lists_added_at_idx ON user_movie_lists (user_id, list_type, added_at);

-- Числовые оценки 1-10 (#17028). После удаления профиля оценка остается, user_id = NULL (#17030 п. 3).
CREATE TABLE ratings (
    rating_id   bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     bigint REFERENCES users ON DELETE SET NULL,
    movie_id    bigint NOT NULL REFERENCES movies ON DELETE CASCADE,
    score       smallint NOT NULL CHECK (score BETWEEN 1 AND 10),
    created_at  timestamptz NOT NULL DEFAULT now(),
    UNIQUE (user_id, movie_id)                                     -- NULL не мешает: NULLS DISTINCT
);

CREATE INDEX ratings_movie_id_idx ON ratings (movie_id) INCLUDE (score);  -- средняя и число оценок фильма

-- Отзывы (#17029). Вердикт - обязательное поле отзыва, отдельно от отзыва не существует.
-- Дата редактирования не хранится, только отметка «Редактирован» (#17029 п. 3).
CREATE TABLE reviews (
    review_id   bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     bigint REFERENCES users ON DELETE SET NULL,        -- NULL: «Удаленный пользователь» (#17030 п. 3)
    movie_id    bigint NOT NULL REFERENCES movies ON DELETE CASCADE,
    verdict     text NOT NULL CHECK (verdict IN ('positive', 'neutral', 'negative')),  -- #17029 п. 1.1
    title       text CHECK (char_length(title) BETWEEN 1 AND 150),                     -- #17029 п. 1.2
    body        text NOT NULL CHECK (char_length(body) BETWEEN 300 AND 10000),         -- #17029 п. 1.3
    is_edited   boolean NOT NULL DEFAULT false,
    created_at  timestamptz NOT NULL DEFAULT now(),
    UNIQUE (user_id, movie_id)                                     -- один отзыв на фильм (#17029 п. 2)
);

CREATE INDEX reviews_movie_id_idx ON reviews (movie_id, created_at);  -- «Самые новые» / «Самые старые» (#17239)

-- Реакции на отзывы: 1 - «палец вверх», -1 - «палец вниз» (#17029 п. 5).
-- sum(reaction) по отзыву дает «+N» / «-N» из #17239. Реакцию на свой отзыв запрещает backend.
CREATE TABLE review_reactions (
    review_id   bigint NOT NULL REFERENCES reviews ON DELETE CASCADE,
    user_id     bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    reaction    smallint NOT NULL CHECK (reaction IN (1, -1)),
    created_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (review_id, user_id)
);

CREATE INDEX review_reactions_user_id_idx ON review_reactions (user_id);

-- История действий пользователя (#17393 п. 2, чтение за 2 с - #17093 п. 2.9).
-- Пишется backend в той же транзакции, что и само действие. Состав - допущение, см. db-schema.md.
CREATE TABLE user_actions (
    action_id    bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id      bigint NOT NULL REFERENCES users ON DELETE CASCADE,
    action       text NOT NULL CHECK (action IN (
                     'want_to_watch_add', 'want_to_watch_remove', 'watched_add', 'watched_remove',
                     'rating_set', 'rating_remove', 'review_create', 'review_edit', 'review_delete',
                     'reaction_set', 'reaction_remove')),
    movie_id     bigint NOT NULL REFERENCES movies ON DELETE CASCADE,
    occurred_at  timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX user_actions_user_id_idx ON user_actions (user_id, occurred_at);

-- migrate:down
DROP TABLE user_actions;
DROP TABLE review_reactions;
DROP TABLE reviews;
DROP TABLE ratings;
DROP TABLE user_movie_lists;
DROP TABLE security_events;
DROP TABLE email_outbox;
DROP TABLE auth_tokens;
DROP TABLE sessions;
DROP TABLE oidc_pending;
DROP TABLE oidc_accounts;
DROP TABLE users;
DROP TABLE movies;
