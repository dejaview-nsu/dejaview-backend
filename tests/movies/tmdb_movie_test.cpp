#include "movies/tmdb_movie.hpp"

#include <gtest/gtest.h>
#include <json/reader.h>

#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

// Случаи - из scripts/catalog/tests/test_parser.py в dejaview-ml: правила у backend
// и каталога общие

namespace
{
Json::Value parse(const std::string &text)
{
    Json::Value json;
    std::istringstream stream(text);
    EXPECT_TRUE(Json::parseFromStream(Json::CharReaderBuilder(), stream, &json, nullptr)) << text;
    return json;
}

// По форме - настоящий ответ /movie/550?append_to_response=credits,release_dates,translations
Json::Value fightClub()
{
    return parse(R"({
        "id": 550, "adult": false, "title": "Бойцовский клуб", "original_title": "Fight Club",
        "original_language": "en",
        "overview": "Сотрудник страховой компании страдает хронической бессонницей.",
        "poster_path": "/pB8BM7pdSp6B6Ih7QZ4DrQ3PmJK.jpg", "release_date": "1999-10-15",
        "runtime": 139,
        "genres": [{"id": 18, "name": "драма"}, {"id": 53, "name": "триллер"}],
        "production_countries": [{"iso_3166_1": "US", "name": "United States of America"},
                                 {"iso_3166_1": "DE", "name": "Germany"}],
        "credits": {
            "cast": [{"id": 287, "name": "Brad Pitt", "character": "Tyler Durden", "order": 1},
                     {"id": 819, "name": "Edward Norton", "character": "The Narrator", "order": 0,
                      "profile_path": "/8nytsqL59SFJTVYVrN72k6qkGgJ.jpg"}],
            "crew": [{"id": 7467, "name": "David Fincher", "job": "Director"},
                     {"id": 7468, "name": "Jim Uhls", "job": "Screenplay"},
                     {"id": 7469, "name": "Chuck Palahniuk", "job": "Novel"},
                     {"id": 1060, "name": "Dust Brothers", "job": "Original Music Composer"},
                     {"id": 7474, "name": "Art Linson", "job": "Producer"},
                     {"id": 7475, "name": "Arnon Milchan", "job": "Executive Producer"}]
        },
        "release_dates": {"results": [
            {"iso_3166_1": "US", "release_dates": [{"certification": "R", "type": 3}]},
            {"iso_3166_1": "RU", "release_dates": [{"certification": "", "type": 4},
                                                   {"certification": "18+", "type": 3}]}
        ]},
        "translations": {"translations": [
            {"iso_3166_1": "US", "iso_639_1": "en", "data": {"title": "", "overview": "A ticking"}},
            {"iso_3166_1": "RU", "iso_639_1": "ru", "data": {"title": "Бойцовский клуб"}}
        ]}
    })");
}

// Как /configuration/countries?language=ru-RU: у части стран русского названия нет
const CountryNames kCountryNames{{"US", "США"}};

TmdbMovie movieFrom(const Json::Value &details)
{
    auto movie = parseTmdbMovie(details, 550, kCountryNames);
    EXPECT_TRUE(movie) << (movie ? "" : rejectionReason(movie.error()));
    return movie.value_or(TmdbMovie{});
}

// Фильм с одним изменённым полем ответа TMDB
TmdbMovie movieWith(const char *name, const Json::Value &value)
{
    Json::Value details = fightClub();
    details[name] = value;
    return movieFrom(details);
}

std::optional<std::string> ratingOf(const std::string &releases)
{
    return movieWith("release_dates", parse(R"({"results": [{"iso_3166_1": "RU",
                                               "release_dates": )" +
                                            releases + "}]}"))
        .ageRating;
}
}  // namespace

TEST(ParseTmdbMovieTest, CollectsAllCardFields)
{
    const TmdbMovie movie = movieFrom(fightClub());
    EXPECT_EQ(movie.movieId, 550);
    EXPECT_EQ(movie.title, "Бойцовский клуб");
    EXPECT_EQ(movie.originalTitle, "Fight Club");
    EXPECT_EQ(movie.releaseDate, "1999-10-15");
    EXPECT_EQ(movie.ageRating, "18+");
    EXPECT_EQ(movie.posterPath, "/pB8BM7pdSp6B6Ih7QZ4DrQ3PmJK.jpg");
    EXPECT_EQ(movie.runtimeMin, 139);
    EXPECT_TRUE(movie.overview.starts_with("Сотрудник"));
    EXPECT_EQ(movie.genres,
              parse(R"([{"id": 18, "name": "драма"}, {"id": 53, "name": "триллер"}])"));
    EXPECT_EQ(movie.countries,
              parse(R"([{"code": "US", "name": "США"}, {"code": "DE", "name": "Germany"}])"));
    EXPECT_EQ(movie.crew, parse(R"([
        {"person_id": 7467, "name": "David Fincher", "role": "director"},
        {"person_id": 7468, "name": "Jim Uhls", "role": "writer"},
        {"person_id": 1060, "name": "Dust Brothers", "role": "composer"},
        {"person_id": 7474, "name": "Art Linson", "role": "producer"}])"));
    EXPECT_EQ(movie.actors, parse(R"([
        {"person_id": 819, "name": "Edward Norton", "character": "The Narrator",
         "profile_path": "/8nytsqL59SFJTVYVrN72k6qkGgJ.jpg"},
        {"person_id": 287, "name": "Brad Pitt", "character": "Tyler Durden",
         "profile_path": null}])"));
}

// Причина попадает в лог: у каждой должен быть текст
TEST(ParseTmdbMovieTest, EveryRejectionHasReason)
{
    for (const auto rejection :
         {MovieRejection::WrongId, MovieRejection::Adult, MovieRejection::NoPoster,
          MovieRejection::NoRussianTitle, MovieRejection::NoRussianOverview})
    {
        EXPECT_FALSE(rejectionReason(rejection).empty());
    }
}

TEST(ParseTmdbMovieTest, RejectsForeignId)
{
    EXPECT_EQ(parseTmdbMovie(fightClub(), 551, kCountryNames).error(), MovieRejection::WrongId);
}

// Каталог (#17640) такие фильмы не берёт - и карточка тоже
TEST(ParseTmdbMovieTest, RejectsWhatCatalogRejects)
{
    const std::pair<const char *, const char *> cases[] = {
        {R"({"adult": true})", "adult"},
        {R"({"poster_path": null})", "poster"},
        {R"({"poster_path": ""})", "poster"},
        {R"({"poster_path": "без-слеша.jpg"})", "poster"},
        {R"({"translations": {"translations": []}})", "title"},
        {R"({"overview": "   "})", "overview"},
        {R"({"overview": null})", "overview"},
    };
    const std::map<std::string, MovieRejection> reasons{
        {"adult", MovieRejection::Adult},
        {"poster", MovieRejection::NoPoster},
        {"title", MovieRejection::NoRussianTitle},
        {"overview", MovieRejection::NoRussianOverview}};
    for (const auto &[overrides, reason] : cases)
    {
        Json::Value details = fightClub();
        const Json::Value changes = parse(overrides);
        for (const auto &name : changes.getMemberNames())
        {
            details[name] = changes[name];
        }
        const auto movie = parseTmdbMovie(details, 550, kCountryNames);
        ASSERT_FALSE(movie) << overrides;
        EXPECT_EQ(movie.error(), reasons.at(reason)) << overrides;
    }
}

TEST(ParseTmdbMovieTest, SurvivesMissingOptionalBlocks)
{
    Json::Value details = fightClub();
    details["credits"] = Json::Value();
    details["release_dates"] = "мусор";
    details["genres"] = parse(R"([{"name": "  "}, "не словарь"])");
    details["production_countries"] = Json::Value();
    details["runtime"] = 0;
    details["release_date"] = "";
    const TmdbMovie movie = movieFrom(details);

    EXPECT_EQ(movie.ageRating, std::nullopt);
    EXPECT_EQ(movie.releaseDate, std::nullopt);
    EXPECT_EQ(movie.runtimeMin, std::nullopt);
    for (const Json::Value *list : {&movie.genres, &movie.countries, &movie.crew, &movie.actors})
    {
        EXPECT_EQ(*list, Json::Value(Json::arrayValue));
    }
}

TEST(ParseTmdbMovieTest, OriginalTitleFallsBackToRussianTitle)
{
    EXPECT_EQ(movieWith("original_title", "  ").originalTitle, "Бойцовский клуб");
}

TEST(ParseTmdbMovieTest, RussianTitle)
{
    // у российского фильма русское название - оригинальное
    Json::Value russian = fightClub();
    russian["original_language"] = "ru";
    russian["original_title"] = "Брат";
    russian["translations"] = Json::Value(Json::objectValue);
    EXPECT_EQ(movieFrom(russian).title, "Брат");

    // перевода нет: title TMDB с оригинальным названием не годится
    Json::Value untranslated = fightClub();
    untranslated["title"] = "Fight Club";
    untranslated["translations"] = parse(R"({"translations": [{"iso_639_1": "ru", "data": {}}]})");
    EXPECT_EQ(parseTmdbMovie(untranslated, 550, kCountryNames).error(),
              MovieRejection::NoRussianTitle);

    EXPECT_EQ(movieWith("translations", parse(R"({"translations": [
                  {"iso_639_1": "ru", "iso_3166_1": "UA", "data": {"title": "Вариант UA"}},
                  {"iso_639_1": "ru", "iso_3166_1": "RU", "data": {"title": "Вариант RU"}}]})"))
                  .title,
              "Вариант RU");
}

TEST(ParseTmdbMovieTest, AgeRatingOnlyRussianFormat)
{
    const std::pair<const char *, std::optional<std::string>> cases[] = {
        {R"("18+")", "18+"},          {R"(" 16 ")", "16+"},       {R"("0+")", "0+"},
        {R"("PG-13")", std::nullopt}, {R"("21+")", std::nullopt}, {R"("")", std::nullopt},
        {"null", std::nullopt}};
    for (const auto &[certification, expected] : cases)
    {
        EXPECT_EQ(ratingOf(std::string(R"([{"type": 3, "certification": )") + certification + "}]"),
                  expected)
            << certification;
    }
}

TEST(ParseTmdbMovieTest, AgeRatingPrefersTheatricalReleaseInRussia)
{
    EXPECT_EQ(ratingOf(R"([{"certification": "12+", "type": 4},
                           {"certification": "16+", "type": 3}])"),
              "16+");
    EXPECT_EQ(movieWith("release_dates",
                        parse(R"({"results": [{"iso_3166_1": "US", "release_dates": []}]})"))
                  .ageRating,
              std::nullopt);
}

TEST(ParseTmdbMovieTest, ReleaseDateMustBeValidIsoDate)
{
    EXPECT_EQ(movieWith("release_date", "2001-02-03").releaseDate, "2001-02-03");
    for (const char *date :
         {R"("2001")", R"("")", "null", "20010203", R"("2001-02-30")", R"("2001-0a-03")"})
    {
        EXPECT_EQ(movieWith("release_date", parse(date)).releaseDate, std::nullopt) << date;
    }
}

TEST(ParseTmdbMovieTest, RuntimeFitsSmallint)
{
    EXPECT_EQ(movieWith("runtime", 90).runtimeMin, 90);
    EXPECT_EQ(movieWith("runtime", 32767).runtimeMin, 32767);
    for (const char *runtime : {"32768", "0", "-5", "null", "true", R"("90")", "90.0"})
    {
        EXPECT_EQ(movieWith("runtime", parse(runtime)).runtimeMin, std::nullopt) << runtime;
    }
}

TEST(ParseTmdbMovieTest, GenresKeepIdAndSkipDuplicatesAndGarbage)
{
    EXPECT_EQ(
        movieWith("genres", parse(R"([{"id": 18, "name": "драма"}, {"id": 18, "name": "драма"},
                                            {"id": 35}, {"name": "без id"}, "мусор"])"))
            .genres,
        parse(R"([{"id": 18, "name": "драма"}])"));
}

TEST(ParseTmdbMovieTest, CountryNamesSkipUntranslatedAndMalformed)
{
    EXPECT_EQ(parseCountryNames(parse(R"([
                  {"iso_3166_1": "US", "english_name": "United States of America", "native_name": "США"},
                  {"iso_3166_1": "DE", "english_name": "Germany", "native_name": ""},
                  {"iso_3166_1": "SUN", "native_name": "Не код"}, "мусор"])")),
              (CountryNames{{"US", "США"}}));
}

TEST(ParseTmdbMovieTest, CountriesUseRussianNameAndFallBackToTmdbName)
{
    EXPECT_EQ(movieWith("production_countries", parse(R"([
                  {"iso_3166_1": "us", "name": "United States of America"},
                  {"iso_3166_1": "US", "name": "дубль"}, {"iso_3166_1": "SU", "name": "Soviet Union"},
                  {"iso_3166_1": "XX"}, {"name": "без кода"}])"))
                  .countries,
              parse(R"([{"code": "US", "name": "США"}, {"code": "SU", "name": "Soviet Union"}])"));
}

TEST(ParseTmdbMovieTest, CrewIsGroupedByRoleInDisplayOrder)
{
    Json::Value details = fightClub();
    details["credits"]["crew"] = parse(R"([
        {"id": 3, "name": "Продюсер", "job": "Producer"}, {"id": 1, "name": "Автор", "job": "Writer"},
        {"id": 1, "name": "Автор", "job": "Screenplay"}, {"id": 1, "name": "Автор", "job": "Director"},
        {"id": 2, "name": "Не сценарист", "job": "Novel"}, {"id": null, "name": "Без id", "job": "Director"}])");
    EXPECT_EQ(movieFrom(details).crew, parse(R"([
        {"person_id": 1, "name": "Автор", "role": "director"},
        {"person_id": 1, "name": "Автор", "role": "writer"},
        {"person_id": 3, "name": "Продюсер", "role": "producer"}])"));
}

TEST(ParseTmdbMovieTest, ActorProfilePathMustStartWithSlash)
{
    const std::pair<const char *, Json::Value> cases[] = {{"/a.jpg", "/a.jpg"},
                                                          {" /a.jpg ", "/a.jpg"},
                                                          {"a.jpg", Json::Value()},
                                                          {"", Json::Value()}};
    for (const auto &[raw, expected] : cases)
    {
        Json::Value details = fightClub();
        details["credits"]["cast"] = Json::Value(Json::arrayValue);
        Json::Value actor;
        actor["id"] = 1;
        actor["name"] = "Актёр";
        actor["profile_path"] = raw;
        details["credits"]["cast"].append(actor);
        EXPECT_EQ(movieFrom(details).actors[0]["profile_path"], expected) << raw;
    }
}

TEST(ParseTmdbMovieTest, ActorsAreDeduplicatedOrderedAndCapped)
{
    Json::Value cast(Json::arrayValue);
    for (int index = 0; index < 40; ++index)
    {
        Json::Value actor;
        actor["id"] = index;
        actor["name"] = "Актёр " + std::to_string(index);
        actor["order"] = 100 - index;
        cast.append(actor);
    }
    cast.append(parse(R"({"id": 5, "name": "Повтор", "order": -1})"));
    cast.append(parse(R"({"id": null, "name": "Без id", "order": -2})"));
    cast.append(parse(R"({"id": 999, "name": "Без порядка"})"));
    Json::Value details = fightClub();
    details["credits"]["cast"] = cast;

    const Json::Value actors = movieFrom(details).actors;
    ASSERT_EQ(actors.size(), 30U);
    EXPECT_EQ(actors[0], parse(R"({"person_id": 5, "name": "Повтор", "character": null,
                                   "profile_path": null})"));
    std::set<std::int64_t> ids;
    for (const auto &actor : actors)
    {
        ids.insert(actor["person_id"].asInt64());
    }
    EXPECT_EQ(ids.size(), 30U);
    EXPECT_FALSE(ids.contains(999));
}
