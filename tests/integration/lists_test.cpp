#include "lists.hpp"
#include "integration/db_test.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace drogon;

namespace
{
using Cookies = std::map<std::string, std::string>;
using Ids = std::vector<std::int64_t>;

const Cookies kIvan{{"dv_session", "ivan-session"}};
}  // namespace

class ListsTest : public DbTest
{
  protected:
    void SetUp() override
    {
        DbTest::SetUp();
        if (IsSkipped())
        {
            return;  // нет TEST_DATABASE_URL - db пустой
        }
        ivan = createUser("ivan", "ivan@example.com");
        db->execSqlSync(
            "INSERT INTO sessions (token_hash, user_id, expires_at) "
            "VALUES (sha256('ivan-session'), $1, now() + interval '1 hour')",
            ivan);
        addMovie(157336, "Интерстеллар");
    }

    std::int64_t ivan = 0;

    void addMovie(std::int64_t movieId, const std::string &title)
    {
        db->execSqlSync("INSERT INTO movies (movie_id, title, original_title) VALUES ($1, $2, $2)",
                        movieId, title);
    }

    // Фильм в списке пользователя, добавлен minutesAgo минут назад - для порядка без ожидания
    void inList(std::int64_t userId, const std::string &listType, std::int64_t movieId,
                int minutesAgo)
    {
        db->execSqlSync(
            "INSERT INTO user_movie_lists (user_id, list_type, movie_id, added_at) "
            "VALUES ($1, $2, $3, now() - $4::int * interval '1 minute')",
            userId, listType, movieId, std::to_string(minutesAgo));
    }

    HttpResponsePtr put(const std::string &listType, const std::string &movieId,
                        const Cookies &cookies = kIvan)
    {
        return run(addToListHandler(
            db, request(Put, "/api/v1/users/me/lists/" + listType + "/" + movieId, {}, cookies),
            listType, movieId));
    }

    HttpResponsePtr remove(const std::string &listType, const std::string &movieId,
                           const Cookies &cookies = kIvan)
    {
        return run(removeFromListHandler(
            db, request(Delete, "/api/v1/users/me/lists/" + listType + "/" + movieId, {}, cookies),
            listType, movieId));
    }

    HttpResponsePtr state(const std::string &movieId, const Cookies &cookies = kIvan)
    {
        return run(movieListsStateHandler(
            db, request(Get, "/api/v1/users/me/movies/" + movieId, {}, cookies), movieId));
    }

    HttpResponsePtr list(const std::string &listType,
                         const std::map<std::string, std::string> &params = {},
                         const Cookies &cookies = kIvan)
    {
        auto req = request(Get, "/api/v1/users/me/lists/" + listType, {}, cookies);
        for (const auto &[name, value] : params)
        {
            req->setParameter(name, value);
        }
        return run(listMoviesHandler(db, req, listType));
    }

    // movie_id фильмов страницы по порядку
    static Ids ids(const HttpResponsePtr &response)
    {
        const Json::Value json = body(response);
        Ids result;
        for (const auto &movie : json["movies"])
        {
            result.push_back(movie["movie_id"].asInt64());
        }
        return result;
    }

    static std::int64_t total(const HttpResponsePtr &response)
    {
        return body(response)["total"].asInt64();
    }
};

// #17026 п. 1, #17027 п. 1: фильм в списке, добавление - в истории
TEST_F(ListsTest, AddPutsMovieInList)
{
    const auto response = put("want_to_watch", "157336");
    EXPECT_EQ(response->statusCode(), k204NoContent);
    EXPECT_TRUE(response->body().empty());
    EXPECT_EQ(ids(list("want_to_watch")), (Ids{157336}));
    EXPECT_EQ(scalar("SELECT action || ' ' || movie_id FROM user_actions"),
              "want_to_watch_add 157336");
}

// Повторное добавление: дубля нет, дата добавления прежняя, в историю не пишется
TEST_F(ListsTest, RepeatedAddChangesNothing)
{
    ASSERT_EQ(put("watched", "157336")->statusCode(), k204NoContent);
    db->execSqlSync("UPDATE user_movie_lists SET added_at = '2026-01-01 00:00:00+00'");

    EXPECT_EQ(put("watched", "157336")->statusCode(), k204NoContent);
    EXPECT_EQ(scalar("SELECT count(*) FROM user_movie_lists"), "1");
    EXPECT_EQ(scalar("SELECT added_at = '2026-01-01 00:00:00+00' FROM user_movie_lists"), "t");
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "1");
}

// #17026 п. 2, #17027 п. 2: удаление; удалить отсутствующий - тоже 204, без записи в историю
TEST_F(ListsTest, RemoveDeletesMovieFromList)
{
    ASSERT_EQ(put("want_to_watch", "157336")->statusCode(), k204NoContent);

    const auto response = remove("want_to_watch", "157336");
    EXPECT_EQ(response->statusCode(), k204NoContent);
    EXPECT_TRUE(response->body().empty());
    EXPECT_EQ(total(list("want_to_watch")), 0);
    EXPECT_EQ(scalar("SELECT string_agg(action, ' ' ORDER BY action_id) FROM user_actions"),
              "want_to_watch_add want_to_watch_remove");

    EXPECT_EQ(remove("want_to_watch", "157336")->statusCode(), k204NoContent);
    EXPECT_EQ(remove("watched", "603")->statusCode(), k204NoContent);  // 603 нет и в movies
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "2");
}

// Списки независимы: фильм может быть в обоих, состояние - для чекбоксов на странице фильма
TEST_F(ListsTest, StateShowsBothListsIndependently)
{
    const auto checkboxes = [this](const std::string &movieId)
    {
        const auto response = state(movieId);
        EXPECT_EQ(response->statusCode(), k200OK) << movieId;
        const Json::Value json = body(response);
        EXPECT_EQ(json.size(), 2U);  // ровно поля UserMovieState
        return std::pair{json["want_to_watch"].asBool(), json["watched"].asBool()};
    };
    EXPECT_EQ(checkboxes("157336"), std::pair(false, false));

    put("watched", "157336");
    EXPECT_EQ(checkboxes("157336"), std::pair(false, true));
    put("want_to_watch", "157336");
    EXPECT_EQ(checkboxes("157336"), std::pair(true, true));
    remove("watched", "157336");
    EXPECT_EQ(checkboxes("157336"), std::pair(true, false));

    // списки другого пользователя не видны; фильма нет в movies - тоже 200
    inList(createUser("petr", "petr@example.com"), "watched", 157336, 1);
    EXPECT_EQ(checkboxes("157336"), std::pair(true, false));
    EXPECT_EQ(checkboxes("603"), std::pair(false, false));
}

// Запрос гостя: 401, ничего не меняется (#17026 п. 3, #17027 п. 3)
TEST_F(ListsTest, GuestGetsUnauthorized)
{
    for (const auto &[cookies, code] :
         {std::pair{Cookies{}, "SESSION_REQUIRED"},
          std::pair{Cookies{{"dv_session", "stale"}}, "SESSION_EXPIRED"}})
    {
        for (const auto &response :
             {put("want_to_watch", "157336", cookies), remove("want_to_watch", "157336", cookies),
              state("157336", cookies), list("want_to_watch", {}, cookies)})
        {
            EXPECT_EQ(response->statusCode(), k401Unauthorized) << code;
            EXPECT_EQ(body(response)["code"].asString(), code);
        }
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM user_movie_lists"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM user_actions"), "0");
}

TEST_F(ListsTest, MissingMovieGivesNotFound)
{
    for (const auto &response : {put("want_to_watch", "603"), put("want_to_watch", "abc"),
                                 remove("watched", "0"), state("-1")})
    {
        EXPECT_EQ(response->statusCode(), k404NotFound);
        EXPECT_EQ(body(response)["code"].asString(), "MOVIE_NOT_FOUND");
    }
    EXPECT_EQ(scalar("SELECT count(*) FROM user_movie_lists"), "0");
}

TEST_F(ListsTest, InvalidParameterGivesBadRequest)
{
    const auto expectField = [](const HttpResponsePtr &response, const std::string &field)
    {
        EXPECT_EQ(response->statusCode(), k400BadRequest) << field;
        EXPECT_EQ(body(response)["code"].asString(), "INVALID_PARAMETER") << field;
        EXPECT_EQ(body(response)["message"].asString(), "Некорректный запрос") << field;
        EXPECT_EQ(body(response)["field"].asString(), field);
    };
    expectField(put("ratings", "157336"), "list_type");
    expectField(remove("Watched", "157336"), "list_type");
    expectField(list("ratings"), "list_type");
    expectField(list("watched", {{"sort", "rating"}}), "sort");
    expectField(list("watched", {{"page", "0"}}), "page");
}

// #17030 п. 2.4.2: не более 24 фильмов на странице, total - для «Страница X из Y»
TEST_F(ListsTest, PagesOf24)
{
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title) "
        "SELECT g, 'Фильм ' || g, 'Movie ' || g FROM generate_series(1, 50) AS g");
    db->execSqlSync(
        "INSERT INTO user_movie_lists (user_id, list_type, movie_id, added_at) "
        "SELECT $1::bigint, 'watched', g, now() - g * interval '1 minute' "
        "FROM generate_series(1, 50) AS g",
        ivan);

    Ids expected(50);
    for (std::int64_t i = 0; i < 50; ++i)
    {
        expected[i] = i + 1;  // по умолчанию сначала последние добавленные
    }
    Ids seen;
    for (const auto &[page, size] : {std::pair{"1", 24U}, {"2", 24U}, {"3", 2U}})
    {
        const auto response = list("watched", {{"page", page}});
        EXPECT_EQ(response->statusCode(), k200OK);
        EXPECT_EQ(ids(response).size(), size) << "страница " << page;
        EXPECT_EQ(total(response), 50) << page;
        for (const auto id : ids(response))
        {
            seen.push_back(id);
        }
    }
    EXPECT_EQ(seen, expected);  // страницы идут подряд и не повторяют друг друга

    const auto beyond = list("watched", {{"page", "4"}});
    EXPECT_EQ(beyond->statusCode(), k200OK);
    EXPECT_TRUE(ids(beyond).empty());
    EXPECT_EQ(total(beyond), 50);
    EXPECT_EQ(ids(list("watched", {{"page", "2147483647"}})), Ids{});
}

// #17030 п. 2.4.6: шесть сортировок, при равенстве - от недавно добавленных к ранним
TEST_F(ListsTest, SixSortsWithNewestAddedFirstOnTies)
{
    addMovie(1, "Бойцовский клуб");
    addMovie(2, "Амели");
    addMovie(3, "Яблоко");
    addMovie(4, "Ёжик в тумане");
    addMovie(5, "Matrix");
    addMovie(6, "Амели");  // то же название, добавлен последним
    for (std::int64_t movieId = 1; movieId <= 6; ++movieId)
    {
        inList(ivan, "want_to_watch", movieId, 60 - static_cast<int>(movieId) * 10);
    }
    // средние: 1 и 4 - по 7.0, 2 - 9.0, у 3, 5, 6 оценок нет
    db->execSqlSync("INSERT INTO ratings (movie_id, score) VALUES (1, 7), (2, 9), (4, 6), (4, 8)");

    const auto sorted = [this](const std::string &sort)
    { return ids(list("want_to_watch", {{"sort", sort}})); };
    EXPECT_EQ(ids(list("want_to_watch")), (Ids{6, 5, 4, 3, 2, 1}));
    EXPECT_EQ(sorted("newest"), (Ids{6, 5, 4, 3, 2, 1}));
    EXPECT_EQ(sorted("oldest"), (Ids{1, 2, 3, 4, 5, 6}));
    // ICU ru-RU: «Ё» рядом с «Е», кириллица раньше латиницы
    EXPECT_EQ(sorted("title_asc"), (Ids{6, 2, 1, 4, 3, 5}));
    EXPECT_EQ(sorted("title_desc"), (Ids{5, 3, 4, 1, 6, 2}));
    // фильмы без оценок - после оцененных в обоих направлениях
    EXPECT_EQ(sorted("best"), (Ids{2, 4, 1, 6, 5, 3}));
    EXPECT_EQ(sorted("worst"), (Ids{4, 1, 2, 6, 5, 3}));
}

// #17030 п. 2.4.7: часть названия без учета регистра, total - сколько нашлось
TEST_F(ListsTest, SearchByTitlePartIgnoringCase)
{
    addMovie(603, "Матрица");
    addMovie(604, "Матрица: Перезагрузка");
    addMovie(605, "Ёлки");
    addMovie(606, "100% любви");
    int minutesAgo = 10;
    for (const std::int64_t movieId : {603, 604, 605, 606})
    {
        inList(ivan, "watched", movieId, minutesAgo--);
    }

    const auto found = [this](const std::string &q, const std::string &page = "1")
    { return list("watched", {{"q", q}, {"page", page}}); };
    EXPECT_EQ(ids(found("МАТР")), (Ids{604, 603}));
    EXPECT_EQ(total(found("МАТР")), 2);
    EXPECT_EQ(ids(found("ёлк")), (Ids{605}));
    EXPECT_EQ(ids(found("ЁЛКИ")), (Ids{605}));
    EXPECT_EQ(ids(found("  ица: ")), (Ids{604}));
    EXPECT_EQ(ids(found("%")), (Ids{606}));  // не шаблон LIKE
    EXPECT_EQ(ids(found("")), (Ids{606, 605, 604, 603}));

    const auto nothing = found("_");
    EXPECT_EQ(nothing->statusCode(), k200OK);
    EXPECT_EQ(body(nothing)["movies"], Json::Value(Json::arrayValue));
    EXPECT_EQ(total(nothing), 0);
    EXPECT_EQ(total(found("матр", "2")), 2);  // за последней страницей total тот же
}

// Свой список: чужие и другой список в него не попадают
TEST_F(ListsTest, ListHasOnlyOwnMoviesOfThisList)
{
    addMovie(603, "Матрица");
    addMovie(680, "Криминальное чтиво");
    inList(ivan, "want_to_watch", 157336, 3);
    inList(ivan, "watched", 680, 2);
    inList(createUser("petr", "petr@example.com"), "want_to_watch", 603, 1);

    EXPECT_EQ(ids(list("want_to_watch")), (Ids{157336}));
    EXPECT_EQ(ids(list("watched")), (Ids{680}));
    EXPECT_EQ(total(list("watched")), 1);
}

// #17030 п. 2.2.1.1: постер, название, общая оценка, оценка пользователя
TEST_F(ListsTest, TileHasContractFields)
{
    db->execSqlSync("UPDATE movies SET poster_path = '/poster.jpg' WHERE movie_id = 157336");
    addMovie(20992, "Фильм без данных");
    inList(ivan, "watched", 157336, 2);
    inList(ivan, "watched", 20992, 1);
    // средняя (9 + 8 + 8 + 9 + 9) / 5 = 8.6, последняя 9 - оценка Ивана
    db->execSqlSync(
        "INSERT INTO ratings (user_id, movie_id, score) "
        "VALUES (NULL, 157336, 9), (NULL, 157336, 8), (NULL, 157336, 8), (NULL, 157336, 9), "
        "($1::bigint, 157336, 9)",
        ivan);

    const auto response = list("watched");
    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(response->contentType(), CT_APPLICATION_JSON);
    const Json::Value json = body(response);
    EXPECT_EQ(json.size(), 2U);  // ровно поля UserListPage
    EXPECT_EQ(json["total"].asInt64(), 2);
    const Json::Value &movies = json["movies"];
    ASSERT_EQ(movies.size(), 2U);

    EXPECT_EQ(movies[0]["movie_id"].asInt64(), 20992);
    EXPECT_EQ(movies[0]["title"].asString(), "Фильм без данных");
    EXPECT_TRUE(movies[0]["poster_url"].isNull());
    EXPECT_TRUE(movies[0]["average_rating"].isNull());
    EXPECT_TRUE(movies[0]["user_rating"].isNull());
    EXPECT_EQ(movies[0].size(), 5U);  // ровно поля UserListMovie

    EXPECT_EQ(movies[1]["movie_id"].asInt64(), 157336);
    EXPECT_EQ(movies[1]["title"].asString(), "Интерстеллар");
    EXPECT_EQ(movies[1]["poster_url"].asString(), "https://image.tmdb.org/t/p/w500/poster.jpg");
    EXPECT_DOUBLE_EQ(movies[1]["average_rating"].asDouble(), 8.6);
    EXPECT_EQ(movies[1]["user_rating"].asInt(), 9);
    // в теле ровно 8.6, а не 8.5999999999999996 из double
    EXPECT_EQ(std::string(response->body()).find("8.5999"), std::string::npos);
}
