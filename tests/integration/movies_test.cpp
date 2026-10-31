#include "movies.hpp"
#include "integration/db_test.hpp"

#include <json/reader.h>

#include <sstream>

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
}  // namespace

class MovieCardTest : public DbTest
{
  protected:
    HttpResponsePtr card(const std::string &movieId) { return run(movieCardHandler(db, movieId)); }
};

// Примеры full и empty из контракта (GET /movies/{movie_id})
TEST_F(MovieCardTest, FullCardMatchesMoviesRow)
{
    db->execSqlSync(
        "INSERT INTO movies (movie_id, title, original_title, release_date, age_rating, "
        "poster_path, runtime_min, overview, genres, countries, crew) VALUES (157336, "
        "'Интерстеллар', 'Interstellar', '2014-11-05', '12+', '/poster-157336.jpg', 169, "
        "'Группа исследователей отправляется через червоточину искать планету для человечества.', "
        "'[{\"id\": 12, \"name\": \"приключения\"}, {\"id\": 878, \"name\": \"фантастика\"}]', "
        "'[{\"code\": \"US\", \"name\": \"США\"}, {\"code\": \"GB\", \"name\": "
        "\"Великобритания\"}]', "
        "'[{\"person_id\": 525, \"name\": \"Кристофер Нолан\", \"role\": \"director\"}, "
        "{\"person_id\": 525, \"name\": \"Кристофер Нолан\", \"role\": \"writer\"}]')");

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
                 {"person_id": 525, "name": "Кристофер Нолан", "role": "writer"}]
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
        "overview": null, "genres": [], "countries": [], "crew": []
    })"));
}

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
