#include "integration/db_test.hpp"
#include "movies/card.hpp"

#include <json/reader.h>

#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

using namespace drogon;

namespace
{
Json::Value parse(const std::string &text)
{
    Json::Value json;
    std::istringstream stream(text);
    Json::parseFromStream(Json::CharReaderBuilder(), stream, &json, nullptr);
    return json;
}

// Ответ TMDB /movie/157336?append_to_response=credits,release_dates,translations, сокращённый
const std::string kInterstellar = R"({
    "id": 157336, "adult": false, "original_language": "en", "original_title": "Interstellar",
    "title": "Интерстеллар", "overview": "Исследователи отправляются через червоточину.",
    "poster_path": "/poster-157336.jpg", "release_date": "2014-11-05", "runtime": 169,
    "genres": [{"id": 878, "name": "фантастика"}],
    "production_countries": [{"iso_3166_1": "US", "name": "United States of America"}],
    "credits": {
        "cast": [{"id": 3895, "name": "Майкл Кейн", "character": "профессор Брэнд", "order": 3,
                  "profile_path": "/profile-3895.jpg"}],
        "crew": [{"id": 525, "name": "Кристофер Нолан", "job": "Director"}]},
    "release_dates": {"results": [{"iso_3166_1": "RU",
                                   "release_dates": [{"certification": "12+", "type": 3}]}]},
    "translations": {"translations": [{"iso_639_1": "ru", "iso_3166_1": "RU",
                                       "data": {"title": "Интерстеллар"}}]}
})";

const std::string kCountries =
    R"([{"iso_3166_1": "US", "english_name": "United States of America", "native_name": "США"}])";

// Та же карточка, что получится из kInterstellar
const std::string kInterstellarCard = R"({
    "movie_id": 157336, "title": "Интерстеллар", "original_title": "Interstellar", "year": 2014,
    "age_rating": "12+", "poster_url": "https://image.tmdb.org/t/p/w500/poster-157336.jpg",
    "runtime_min": 169, "overview": "Исследователи отправляются через червоточину.",
    "genres": [{"id": 878, "name": "фантастика"}], "countries": [{"code": "US", "name": "США"}],
    "crew": [{"person_id": 525, "name": "Кристофер Нолан", "role": "director"}],
    "actors": [{"person_id": 3895, "name": "Майкл Кейн", "character": "профессор Брэнд",
                "photo_url": "https://image.tmdb.org/t/p/w185/profile-3895.jpg"}]
})";

// Интерстеллар под другим movie_id: для фильмов, которых нет в кеше
std::string interstellarAs(std::int64_t movieId)
{
    Json::Value details = parse(kInterstellar);
    details["id"] = Json::Int64(movieId);
    return details.toStyledString();
}
}  // namespace

class MovieCardTest : public DbTest
{
  protected:
    // TMDB - фейковый сервер; у каждого теста свой выключатель и пустой кеш стран
    void SetUp() override
    {
        DbTest::SetUp();
        tmdb = std::make_shared<TmdbService>(
            TmdbSettings{.accessToken = "test-token", .url = fakeUrl});
        fakeReply("/3/configuration/countries", k200OK, kCountries);
    }

    std::shared_ptr<TmdbService> tmdb;

    HttpResponsePtr card(const std::string &movieId)
    {
        return run(movieCardHandler(db, tmdb, movieId));
    }

    static void expectError(const HttpResponsePtr &response, HttpStatusCode status,
                            const std::string &code)
    {
        EXPECT_EQ(response->statusCode(), status);
        EXPECT_EQ(body(response)["code"].asString(), code);
    }
};

// Примеры full и empty из контракта (GET /movies/{movie_id})
TEST_F(MovieCardTest, FullCardMatchesMoviesRow)
{
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title, release_date, age_rating, "
        "poster_path, runtime_min, overview, genres, countries, crew, actors) VALUES (157336, "
        "'Интерстеллар', 'Interstellar', '2014-11-05', '12+', '/poster-157336.jpg', 169, "
        "'Группа исследователей отправляется через червоточину искать планету для человечества.', "
        "'[{\"id\": 12, \"name\": \"приключения\"}, {\"id\": 878, \"name\": \"фантастика\"}]', "
        "'[{\"code\": \"US\", \"name\": \"США\"}, {\"code\": \"GB\", \"name\": "
        "\"Великобритания\"}]', "
        "'[{\"person_id\": 525, \"name\": \"Кристофер Нолан\", \"role\": \"director\"}, "
        "{\"person_id\": 525, \"name\": \"Кристофер Нолан\", \"role\": \"writer\"}]', "
        "'[{\"person_id\": 3895, \"name\": \"Майкл Кейн\", \"character\": \"профессор Брэнд\", "
        "\"profile_path\": \"/profile-3895.jpg\"}, {\"person_id\": 10297, \"name\": \"Мэттью "
        "Макконахи\", \"character\": null, \"profile_path\": null}]')");

    const auto response = card("157336");
    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(body(response), parse(R"({
        "movie_id": 157336,
        "title": "Интерстеллар",
        "original_title": "Interstellar",
        "year": 2014,
        "age_rating": "12+",
        "poster_url": "https://image.tmdb.org/t/p/w500/poster-157336.jpg",
        "runtime_min": 169,
        "overview": "Группа исследователей отправляется через червоточину искать планету для человечества.",
        "genres": [{"id": 12, "name": "приключения"}, {"id": 878, "name": "фантастика"}],
        "countries": [{"code": "US", "name": "США"}, {"code": "GB", "name": "Великобритания"}],
        "crew": [{"person_id": 525, "name": "Кристофер Нолан", "role": "director"},
                 {"person_id": 525, "name": "Кристофер Нолан", "role": "writer"}],
        "actors": [{"person_id": 3895, "name": "Майкл Кейн", "character": "профессор Брэнд",
                    "photo_url": "https://image.tmdb.org/t/p/w185/profile-3895.jpg"},
                   {"person_id": 10297, "name": "Мэттью Макконахи", "character": null,
                    "photo_url": null}]
    })"));
}

TEST_F(MovieCardTest, EmptyFieldsAreNullAndEmptyLists)
{
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title) "
        "VALUES (1000001, 'Фильм без данных', 'Film Without Data')");

    const auto response = card("1000001");
    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(body(response), parse(R"({
        "movie_id": 1000001, "title": "Фильм без данных", "original_title": "Film Without Data",
        "year": null, "age_rating": null, "poster_url": null, "runtime_min": null,
        "overview": null, "genres": [], "countries": [], "crew": [], "actors": []
    })"));
}

// 603 нет ни в movies, ни в фейковом TMDB: он отвечает 404 на всё, что не задано
TEST_F(MovieCardTest, UnknownOrInvalidIdIsNotFound)
{
    for (const char *movieId : {"603", "abc", "0", "-1", "99999999999999999999"})
    {
        const auto response = card(movieId);
        EXPECT_EQ(response->statusCode(), k404NotFound) << movieId;
        EXPECT_EQ(body(response)["code"].asString(), "MOVIE_NOT_FOUND") << movieId;
        EXPECT_EQ(body(response)["message"].asString(), "Фильм не найден") << movieId;
    }
}

// #18843: фильма нет в movies - он берётся из TMDB, сохраняется, и дальше идёт из кеша
TEST_F(MovieCardTest, FetchesMissingMovieFromTmdbAndCachesIt)
{
    fakeReply("/3/movie/157336", k200OK, kInterstellar);
    const auto response = card("157336");
    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(body(response), parse(kInterstellarCard));
    EXPECT_EQ(scalar("SELECT title || ' ' || (cached_at > now() - interval '1 minute') || ' ' || "
                     "(indexed_at IS NULL) FROM movies"),
              "Интерстеллар true true");

    // запрос - как у сборки каталога (#17640)
    const auto request = fakeReceived("/3/movie/157336");
    ASSERT_TRUE(request);
    EXPECT_EQ(request->getHeader("authorization"), "Bearer test-token");
    EXPECT_EQ(request->getParameter("language"), "ru-RU");
    EXPECT_EQ(request->getParameter("append_to_response"), "credits,release_dates,translations");

    // фильм из кеша отдаётся и при недоступном TMDB
    fakeReply("/3/movie/157336", k500InternalServerError, "{}");
    const auto cached = card("157336");
    EXPECT_EQ(cached->statusCode(), k200OK);
    EXPECT_EQ(body(cached), parse(kInterstellarCard));
}

TEST_F(MovieCardTest, MovieTmdbDoesNotKnowOrCatalogWouldRejectIsNotFound)
{
    fakeReply("/3/movie/603", k404NotFound, R"({"status_code": 34})");
    expectError(card("603"), k404NotFound, "MOVIE_NOT_FOUND");

    // те же правила, что у каталога: фильм для взрослых, без постера, без русского названия
    // или описания в movies не попадает
    Json::Value adult = parse(interstellarAs(604));
    adult["adult"] = true;
    fakeReply("/3/movie/604", k200OK, adult.toStyledString());
    expectError(card("604"), k404NotFound, "MOVIE_NOT_FOUND");

    EXPECT_EQ(scalar("SELECT count(*) FROM movies"), "0");
}

TEST_F(MovieCardTest, TmdbFailureGivesUnavailable)
{
    for (const auto status :
         {k500InternalServerError, k429TooManyRequests, k401Unauthorized, k403Forbidden})
    {
        tmdb = std::make_shared<TmdbService>(
            TmdbSettings{.accessToken = "test-token", .url = fakeUrl});
        fakeReply("/3/movie/157336", status, "{}");
        expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    }
    // нет соединения: на этом порту никто не слушает
    tmdb = std::make_shared<TmdbService>(
        TmdbSettings{.accessToken = "test-token", .url = "http://127.0.0.1:1"});
    expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    // нет ключа: в TMDB не ходим
    tmdb = std::make_shared<TmdbService>(TmdbSettings{.url = fakeUrl});
    fakeReply("/3/movie/157336", k200OK, kInterstellar);
    const auto noKey = card("157336");
    expectError(noKey, k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    EXPECT_EQ(body(noKey)["message"].asString(),
              "Не удалось загрузить информацию о фильме. Попробуйте позже");

    // список стран не получен - фильм не сохраняется: без русских названий данные хуже каталога
    for (const auto &[status, countries] :
         {std::pair{k500InternalServerError, "{}"}, std::pair{k200OK, R"({"не": "список"})"}})
    {
        tmdb = std::make_shared<TmdbService>(
            TmdbSettings{.accessToken = "test-token", .url = fakeUrl});
        fakeReply("/3/configuration/countries", status, countries);
        expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM movies"), "0");
}

// #17296: после серии сбоев вызовы к TMDB отключаются на время
TEST_F(MovieCardTest, CallsToTmdbStopAfterSeriesOfFailures)
{
    fakeReply("/3/movie/157336", k500InternalServerError, "{}");
    for (int failure = 0; failure < 5; ++failure)
    {
        expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    }
    // TMDB уже здоров, но вызовы отключены: в TMDB не ходим, сразу 503
    fakeReply("/3/movie/157336", k200OK, kInterstellar);
    expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    EXPECT_EQ(scalar("SELECT count(*) FROM movies"), "0");
}

// Через openFor пробный запрос: TMDB не ожил - вызовы снова отключены, ожил - возобновлены
TEST_F(MovieCardTest, CallsToTmdbResumeWhenItRecovers)
{
    using namespace std::chrono_literals;
    tmdb = std::make_shared<TmdbService>(
        TmdbSettings{.accessToken = "test-token", .url = fakeUrl, .openFor = 200ms});
    fakeReply("/3/movie/157336", k500InternalServerError, "{}");
    for (int failure = 0; failure < 5; ++failure)
    {
        expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");
    }

    std::this_thread::sleep_for(300ms);
    expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");  // проба не прошла
    fakeReply("/3/movie/157336", k200OK, kInterstellar);
    expectError(card("157336"), k503ServiceUnavailable, "MOVIE_UNAVAILABLE");  // снова отключены

    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(card("157336")->statusCode(), k200OK);  // проба прошла
    fakeReply("/3/movie/158000", k200OK, interstellarAs(158000));
    EXPECT_EQ(card("158000")->statusCode(), k200OK);  // и вызовы идут как обычно
}

TEST_F(MovieCardTest, CountryNamesAreRequestedOnce)
{
    fakeReply("/3/movie/157336", k200OK, kInterstellar);
    EXPECT_EQ(card("157336")->statusCode(), k200OK);

    // список стран больше не нужен: он в памяти
    fakeReply("/3/configuration/countries", k500InternalServerError, "{}");
    fakeReply("/3/movie/158000", k200OK, interstellarAs(158000));
    const auto second = card("158000");
    EXPECT_EQ(second->statusCode(), k200OK);
    EXPECT_EQ(body(second)["countries"], parse(R"([{"code": "US", "name": "США"}])"));
}
