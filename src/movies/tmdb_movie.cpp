#include "movies/tmdb_movie.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace
{
constexpr std::int64_t kTheatricalRelease = 3;
constexpr std::size_t kMaxActors = 30;
constexpr std::int64_t kMaxRuntimeMin = 32767;  // runtime_min в movies - smallint

// Роли съёмочной группы в порядке показа
constexpr std::array<std::string_view, 4> kCrewRoles{"director", "writer", "composer", "producer"};

// Должность TMDB -> роль карточки
std::string_view crewRole(std::string_view job)
{
    if (job == "Director")
    {
        return "director";
    }
    if (job == "Screenplay" || job == "Writer")
    {
        return "writer";
    }
    if (job == "Original Music Composer" || job == "Music")
    {
        return "composer";
    }
    return job == "Producer" ? "producer" : "";
}

// Ответу TMDB доверять нельзя: поле может отсутствовать или быть другого типа. Не тот тип -
// как отсутствие, без исключений jsoncpp (scripts/catalog/json_values.py)
const Json::Value &field(const Json::Value &object, const char *name)
{
    static const Json::Value kNull;
    return object.isObject() ? object[name] : kNull;
}

const Json::Value &asList(const Json::Value &value)
{
    static const Json::Value kEmpty(Json::arrayValue);
    return value.isArray() ? value : kEmpty;
}

// Целое число JSON: true и 18.0 - нет
std::optional<std::int64_t> asInt(const Json::Value &value)
{
    const bool integer = value.type() == Json::intValue || value.type() == Json::uintValue;
    return integer && value.isInt64() ? std::optional(value.asInt64()) : std::nullopt;
}

std::optional<std::string> cleanText(const Json::Value &value)
{
    if (!value.isString())
    {
        return std::nullopt;
    }
    const std::string text = value.asString();
    constexpr std::string_view kSpaces = " \t\n\r\f\v";
    const auto first = text.find_first_not_of(kSpaces);
    if (first == std::string::npos)
    {
        return std::nullopt;
    }
    return text.substr(first, text.find_last_not_of(kSpaces) - first + 1);
}

bool isText(const Json::Value &value, std::string_view text)
{
    return value.isString() && value.asString() == text;
}

// Путь картинки на CDN TMDB вида /abc.jpg, URL из него собирает backend
std::optional<std::string> tmdbPath(const Json::Value &value)
{
    auto path = cleanText(value);
    return path && path->starts_with('/') ? path : std::nullopt;
}

Json::Value nullable(const std::optional<std::string> &value)
{
    return value ? Json::Value(*value) : Json::Value();
}

// Без перевода TMDB кладёт в title оригинальное название, поэтому смотрим translations
std::optional<std::string> russianTitle(const Json::Value &details)
{
    if (isText(field(details, "original_language"), "ru"))
    {
        return cleanText(field(details, "original_title"));
    }
    // Перевод на русский, для России - предпочтительнее
    const Json::Value *russian = nullptr;
    for (const auto &item : asList(field(field(details, "translations"), "translations")))
    {
        if (isText(field(item, "iso_639_1"), "ru") &&
            (russian == nullptr || (!isText(field(*russian, "iso_3166_1"), "RU") &&
                                    isText(field(item, "iso_3166_1"), "RU"))))
        {
            russian = &item;
        }
    }
    return russian ? cleanText(field(field(*russian, "data"), "title")) : std::nullopt;
}

// «16» -> «16+»; не российский формат - nullopt
std::optional<std::string> russianAgeRating(const Json::Value &certification)
{
    auto rating = cleanText(certification);
    if (!rating)
    {
        return std::nullopt;
    }
    if (!rating->ends_with('+'))
    {
        *rating += '+';
    }
    constexpr std::array<std::string_view, 5> kRatings{"0+", "6+", "12+", "16+", "18+"};
    return std::ranges::contains(kRatings, *rating) ? rating : std::nullopt;
}

// Рейтинг из релизов в России: сначала кинотеатральный, затем остальные по порядку
std::optional<std::string> ageRating(const Json::Value &details)
{
    const Json::Value *russia = nullptr;
    for (const auto &country : asList(field(field(details, "release_dates"), "results")))
    {
        if (isText(field(country, "iso_3166_1"), "RU"))
        {
            russia = &country;
            break;
        }
    }
    if (russia == nullptr)
    {
        return std::nullopt;
    }
    const Json::Value &releases = asList(field(*russia, "release_dates"));
    for (const bool theatrical : {true, false})
    {
        for (const auto &release : releases)
        {
            if ((asInt(field(release, "type")) == kTheatricalRelease) != theatrical)
            {
                continue;
            }
            if (auto rating = russianAgeRating(field(release, "certification")))
            {
                return rating;
            }
        }
    }
    return std::nullopt;
}

// Строго YYYY-MM-DD и существующая дата: иначе PostgreSQL отклонит запись
std::optional<std::string> releaseDate(const Json::Value &value)
{
    auto text = cleanText(value);
    if (!text || text->size() != 10 || (*text)[4] != '-' || (*text)[7] != '-')
    {
        return std::nullopt;
    }
    const auto number = [&](std::size_t from, std::size_t length) -> std::optional<int>
    {
        int result = 0;
        for (const char c : text->substr(from, length))
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
            result = result * 10 + (c - '0');
        }
        return result;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    if (!year || !month || !day ||
        !std::chrono::year_month_day{std::chrono::year{*year},
                                     std::chrono::month{static_cast<unsigned>(*month)},
                                     std::chrono::day{static_cast<unsigned>(*day)}}
             .ok())
    {
        return std::nullopt;
    }
    return text;
}

// 0 в TMDB означает «неизвестно»
std::optional<int> runtime(const Json::Value &value)
{
    const auto minutes = asInt(value);
    return minutes && *minutes > 0 && *minutes <= kMaxRuntimeMin
               ? std::optional(static_cast<int>(*minutes))
               : std::nullopt;
}

Json::Value genres(const Json::Value &items)
{
    Json::Value result(Json::arrayValue);
    std::set<std::int64_t> seen;
    for (const auto &item : asList(items))
    {
        const auto id = asInt(field(item, "id"));
        const auto name = cleanText(field(item, "name"));
        if (id && name && seen.insert(*id).second)
        {
            Json::Value genre;
            genre["id"] = Json::Int64(*id);
            genre["name"] = *name;
            result.append(genre);
        }
    }
    return result;
}

std::optional<std::string> countryCode(const Json::Value &item)
{
    auto code = cleanText(field(item, "iso_3166_1"));
    if (!code || code->size() != 2)
    {
        return std::nullopt;
    }
    std::ranges::transform(*code, code->begin(),
                           [](char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; });
    return code;
}

// В production_countries названия английские: русское - из countryNames, если оно есть
Json::Value countries(const Json::Value &details, const CountryNames &countryNames)
{
    Json::Value result(Json::arrayValue);
    std::set<std::string> seen;
    for (const auto &item : asList(field(details, "production_countries")))
    {
        const auto code = countryCode(item);
        if (!code || seen.contains(*code))
        {
            continue;
        }
        const auto russian = countryNames.find(*code);
        const auto name =
            russian != countryNames.end() ? russian->second : cleanText(field(item, "name"));
        if (name)
        {
            seen.insert(*code);
            Json::Value country;
            country["code"] = *code;
            country["name"] = *name;
            result.append(country);
        }
    }
    return result;
}

struct Person
{
    std::int64_t id;
    std::string name;
};

std::optional<Person> person(const Json::Value &member)
{
    const auto id = asInt(field(member, "id"));
    auto name = cleanText(field(member, "name"));
    return id && name ? std::optional(Person{*id, std::move(*name)}) : std::nullopt;
}

// Роли по порядку kCrewRoles; человек с двумя должностями одной роли попадает в неё один раз
Json::Value crew(const Json::Value &members)
{
    Json::Value result(Json::arrayValue);
    for (const std::string_view role : kCrewRoles)
    {
        std::set<std::int64_t> seen;
        for (const auto &member : asList(members))
        {
            const Json::Value &job = field(member, "job");
            const auto who = person(member);
            if (job.isString() && crewRole(job.asString()) == role && who &&
                seen.insert(who->id).second)
            {
                Json::Value item;
                item["person_id"] = Json::Int64(who->id);
                item["name"] = who->name;
                item["role"] = std::string(role);
                result.append(item);
            }
        }
    }
    return result;
}

// В порядке order из TMDB (без order - в конец), без повторов, не больше kMaxActors
Json::Value actors(const Json::Value &cast)
{
    std::vector<const Json::Value *> members;
    for (const auto &member : asList(cast))
    {
        members.push_back(&member);
    }
    const auto order = [](const Json::Value *member)
    { return asInt(field(*member, "order")).value_or(std::numeric_limits<std::int64_t>::max()); };
    std::ranges::stable_sort(members, {}, order);

    Json::Value result(Json::arrayValue);
    std::set<std::int64_t> seen;
    for (const Json::Value *member : members)
    {
        const auto who = person(*member);
        if (result.size() == kMaxActors || !who || !seen.insert(who->id).second)
        {
            continue;
        }
        Json::Value actor;
        actor["person_id"] = Json::Int64(who->id);
        actor["name"] = who->name;
        actor["character"] = nullable(cleanText(field(*member, "character")));
        actor["profile_path"] = nullable(tmdbPath(field(*member, "profile_path")));
        result.append(actor);
    }
    return result;
}
}  // namespace

std::string_view rejectionReason(MovieRejection rejection)
{
    switch (rejection)
    {
        case MovieRejection::WrongId:
            return "TMDB прислал другой фильм";
        case MovieRejection::Adult:
            return "фильм для взрослых";
        case MovieRejection::NoPoster:
            return "нет постера";
        case MovieRejection::NoRussianTitle:
            return "нет русского названия";
        case MovieRejection::NoRussianOverview:
            return "нет русского описания";
    }
    return "";
}

std::expected<TmdbMovie, MovieRejection> parseTmdbMovie(const Json::Value &details,
                                                        std::int64_t movieId,
                                                        const CountryNames &countryNames)
{
    if (asInt(field(details, "id")) != movieId)
    {
        return std::unexpected(MovieRejection::WrongId);
    }
    const Json::Value &adult = field(details, "adult");
    if (adult.isBool() && adult.asBool())
    {
        return std::unexpected(MovieRejection::Adult);
    }
    auto poster = tmdbPath(field(details, "poster_path"));
    if (!poster)
    {
        return std::unexpected(MovieRejection::NoPoster);
    }
    auto title = russianTitle(details);
    if (!title)
    {
        return std::unexpected(MovieRejection::NoRussianTitle);
    }
    auto overview = cleanText(field(details, "overview"));
    if (!overview)
    {
        return std::unexpected(MovieRejection::NoRussianOverview);
    }

    const Json::Value &credits = field(details, "credits");
    return TmdbMovie{
        .movieId = movieId,
        .title = *title,
        .originalTitle = cleanText(field(details, "original_title")).value_or(*title),
        .releaseDate = releaseDate(field(details, "release_date")),
        .ageRating = ageRating(details),
        .posterPath = std::move(*poster),
        .runtimeMin = runtime(field(details, "runtime")),
        .overview = std::move(*overview),
        .genres = genres(field(details, "genres")),
        .countries = countries(details, countryNames),
        .crew = crew(field(credits, "crew")),
        .actors = actors(field(credits, "cast")),
    };
}

CountryNames parseCountryNames(const Json::Value &countries)
{
    CountryNames names;
    for (const auto &item : asList(countries))
    {
        const auto code = countryCode(item);
        const auto name = cleanText(field(item, "native_name"));
        if (code && name)
        {
            names[*code] = *name;
        }
    }
    return names;
}
