#include "search/search.hpp"
#include "auth/session.hpp"
#include "integration/db_test.hpp"
#include "search/samples.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace drogon;

namespace
{

const std::string kImagePath = "/v1/search/image";
const std::string kVideoPath = "/v1/search/video";
const std::string kApiImage = "/api/v1/search/image";
const std::string kApiVideo = "/api/v1/search/video";
const std::string kFoundBody = R"({"movie_ids":[603]})";

const std::string kPng = readSample("image.png");
const std::string kMp4 = readSample("video.mp4");
const std::string kMov = readSample("video.mov");
const std::string kGif = readSample("reject.gif");

constexpr std::string_view kImageRequired =
    "Невозможно выполнить поиск по изображению: изображение не загружено";
constexpr std::string_view kVideoRequired =
    "Невозможно выполнить поиск по видеофрагменту: видеофрагмент не загружен";
constexpr std::string_view kUnavailable = "Поиск временно недоступен. Попробуйте позже";

struct Part
{
    std::string name;
    std::optional<std::string> filename;  // nullopt - текстовая часть, "" - filename=""
    std::string bytes;
    std::string type = "application/octet-stream";
};

class SearchTest : public DbTest
{
  protected:
    MlConfig config;

    void SetUp() override
    {
        DbTest::SetUp();
        config.url = fakeUrl;
        config.maxConcurrent = 1;
    }

    std::shared_ptr<MlService> service() { return std::make_shared<MlService>(config); }

    static HttpRequestPtr multipart(const std::string &path, const std::vector<Part> &parts,
                                    const std::map<std::string, std::string> &cookies = {})
    {
        const std::string boundary = "----dvboundary";
        std::string raw;
        for (const Part &part : parts)
        {
            raw +=
                "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + part.name + "\"";
            if (!part.filename)
            {
                raw += "\r\n\r\n";
            }
            else
            {
                raw += "; filename=\"" + *part.filename + "\"\r\nContent-Type: " + part.type +
                       "\r\n\r\n";
            }
            raw += part.bytes + "\r\n";
        }
        raw += "--" + boundary + "--\r\n";

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath(path);
        req->addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        req->setBody(std::move(raw));
        for (const auto &[name, value] : cookies)
        {
            req->addCookie(name, value);
        }
        return req;
    }

    std::map<std::string, std::string> signedIn()
    {
        const auto userId = createUser("viewer", "viewer@example.com");
        const auto session = run(createSession(db, request(Post, "/"), userId));
        return {{"dv_session", session.token}};
    }

    HttpResponsePtr search(MediaKind kind, const HttpRequestPtr &req,
                           const std::shared_ptr<MlService> &ml)
    {
        return run(mediaSearchHandler(db, ml, kind, req));
    }

    HttpResponsePtr searchImage(const std::vector<Part> &parts)
    {
        return search(MediaKind::Image, multipart(kApiImage, parts, signedIn()), service());
    }

    HttpResponsePtr searchVideo(const std::vector<Part> &parts)
    {
        return search(MediaKind::Video, multipart(kApiVideo, parts, signedIn()), service());
    }

    static Part file(const std::string &name, const std::string &bytes,
                     const std::string &type = "application/octet-stream")
    {
        return {"file", name, bytes, type};
    }

    void insertMovie(int movieId, const std::string &columns = "", const std::string &values = "")
    {
        db->execSqlSync("INSERT INTO movies (movie_id, title, original_title" + columns +
                        ") VALUES (" + std::to_string(movieId) + ", 'Матрица', 'The Matrix'" +
                        values + ")");
    }

    void insertFullMovie(int movieId)
    {
        insertMovie(movieId, ", release_date, poster_path, overview",
                    ", '1999-03-31', '/p.jpg', 'Хакер узнаёт правду'");
    }

    static void expectError(const HttpResponsePtr &response, HttpStatusCode status,
                            const std::string &code, std::string_view message, bool fileField)
    {
        EXPECT_EQ(response->statusCode(), status);
        const Json::Value json = body(response);
        EXPECT_EQ(json["code"].asString(), code);
        EXPECT_EQ(json["message"].asString(), std::string(message));
        if (fileField)
        {
            EXPECT_EQ(json["field"].asString(), "file");
        }
        else
        {
            EXPECT_TRUE(json["field"].isNull());
        }
    }

    static void expectEmptyResults(const HttpResponsePtr &response)
    {
        EXPECT_EQ(response->statusCode(), k200OK);
        const Json::Value json = body(response);
        ASSERT_TRUE(json["results"].isArray());
        EXPECT_EQ(json["results"].size(), 0U);
    }
};

TEST_F(SearchTest, RejectsMissingSessionBeforeCallingMl)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto response =
        search(MediaKind::Image, multipart(kApiImage, {file("a.png", kPng)}), service());

    EXPECT_EQ(response->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(response)["code"].asString(), "SESSION_REQUIRED");
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);
}

TEST_F(SearchTest, ChecksSessionBeforeFile)
{
    const auto response = search(MediaKind::Image, multipart(kApiImage, {}), service());

    EXPECT_EQ(response->statusCode(), k401Unauthorized);
    EXPECT_EQ(body(response)["code"].asString(), "SESSION_REQUIRED");
}

TEST_F(SearchTest, RejectsJsonBodyAsMissingFile)
{
    Json::Value json;
    json["file"] = "x";
    auto req = request(Post, kApiImage, json, signedIn());

    expectError(search(MediaKind::Image, req, service()), k400BadRequest, "FILE_REQUIRED",
                kImageRequired, true);
}

TEST_F(SearchTest, RejectsMultipartWithOnlyTextPartNamedFile)
{
    expectError(searchImage({{"file", std::nullopt, "text"}}), k400BadRequest, "FILE_REQUIRED",
                kImageRequired, true);
}

TEST_F(SearchTest, RejectsFilePartWithEmptyFilename)
{
    expectError(searchImage({{"file", "", kPng}}), k400BadRequest, "FILE_REQUIRED", kImageRequired,
                true);
}

TEST_F(SearchTest, RejectsMultipartWithoutParts)
{
    expectError(searchImage({}), k400BadRequest, "FILE_REQUIRED", kImageRequired, true);
}

TEST_F(SearchTest, VideoRequiresFileWithVideoMessage)
{
    expectError(searchVideo({}), k400BadRequest, "FILE_REQUIRED", kVideoRequired, true);
}

TEST_F(SearchTest, RejectsTwoFiles)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto response = searchImage({file("a.png", kPng), file("b.png", kPng)});

    EXPECT_EQ(response->statusCode(), k400BadRequest);
    EXPECT_EQ(body(response)["code"].asString(), "TOO_MANY_FILES");
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);
}

TEST_F(SearchTest, RejectsImageAboveTenMebibytes)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto response = searchImage({file("a.png", padded(kPng, kMaxImageBytes + 1))});

    expectError(response, k413RequestEntityTooLarge, "FILE_TOO_LARGE",
                "Невозможно выполнить поиск по изображению: размер файла больше 10 МБ", true);
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);
}

TEST_F(SearchTest, RejectsVideoAboveFiftyMebibytes)
{
    fakeReply(kVideoPath, k200OK, kFoundBody);
    const auto response = searchVideo({file("a.mp4", padded(kMp4, kMaxVideoBytes + 1))});

    expectError(response, k413RequestEntityTooLarge, "FILE_TOO_LARGE",
                "Невозможно выполнить поиск по видеофрагменту: размер файла больше 50 МБ", true);
    EXPECT_EQ(fakeReceived(kVideoPath), nullptr);
}

TEST_F(SearchTest, DetectsFormatByContentNotByNameOrType)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto response = searchImage({file("frame.jpg", kGif, "image/jpeg")});

    EXPECT_EQ(response->statusCode(), k415UnsupportedMediaType);
    EXPECT_EQ(body(response)["code"].asString(), "UNSUPPORTED_FORMAT");
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);
}

TEST_F(SearchTest, ForwardsExactBytesWithDetectedTypeDespiteName)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto response = searchImage({file("frame.gif", kPng, "image/gif")});

    EXPECT_EQ(response->statusCode(), k200OK);
    const auto received = fakeReceived(kImagePath);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->getHeader("content-type"), "image/png");
    EXPECT_EQ(std::string(received->body()), kPng);
}

TEST_F(SearchTest, RejectsVideoSentToImageEndpoint)
{
    expectError(searchImage({file("a.mp4", kMp4)}), k415UnsupportedMediaType, "UNSUPPORTED_FORMAT",
                "Невозможно выполнить поиск по изображению: поддерживаются только JPG, JPEG, "
                "PNG и WEBP",
                true);
}

TEST_F(SearchTest, RejectsImageSentToVideoEndpoint)
{
    expectError(searchVideo({file("a.png", kPng)}), k415UnsupportedMediaType, "UNSUPPORTED_FORMAT",
                "Невозможно выполнить поиск по видеофрагменту: поддерживаются только MP4, MOV и "
                "WEBM",
                true);
}

TEST_F(SearchTest, MapsMlCorruptedFileTo422)
{
    fakeReply(kImagePath, k422UnprocessableEntity, R"({"code":"FILE_CORRUPTED"})");

    expectError(searchImage({file("a.png", kPng)}), k422UnprocessableEntity, "FILE_CORRUPTED",
                "Невозможно выполнить поиск по изображению: файл поврежден или не может быть "
                "обработан",
                true);
}

TEST_F(SearchTest, MapsMlDurationTooLongToVideoTooLong)
{
    fakeReply(kVideoPath, k422UnprocessableEntity, R"({"code":"DURATION_TOO_LONG"})");

    expectError(searchVideo({file("a.mp4", kMp4)}), k422UnprocessableEntity, "VIDEO_TOO_LONG",
                "Невозможно выполнить поиск по видеофрагменту: видео длиннее 30 секунд", true);
}

TEST_F(SearchTest, ReturnsCardOfFoundMovie)
{
    insertFullMovie(603);
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto response = searchImage({file("a.png", kPng)});

    EXPECT_EQ(response->statusCode(), k200OK);
    const Json::Value json = body(response);
    ASSERT_EQ(json["results"].size(), 1U);
    const Json::Value &card = json["results"][0];
    EXPECT_EQ(card["movie_id"].asInt64(), 603);
    EXPECT_EQ(card["title"].asString(), "Матрица");
    EXPECT_EQ(card["year"].asInt(), 1999);
    EXPECT_EQ(card["poster_url"].asString(), "https://image.tmdb.org/t/p/w500/p.jpg");
    EXPECT_EQ(card["overview"].asString(), "Хакер узнаёт правду");
}

TEST_F(SearchTest, ReturnsNullsForMissingOptionalFields)
{
    insertMovie(603);
    fakeReply(kImagePath, k200OK, kFoundBody);
    const Json::Value json = body(searchImage({file("a.png", kPng)}));

    ASSERT_EQ(json["results"].size(), 1U);
    const Json::Value &card = json["results"][0];
    for (const char *key : {"year", "poster_url", "overview"})
    {
        SCOPED_TRACE(key);
        EXPECT_TRUE(card.isMember(key));
        EXPECT_TRUE(card[key].isNull());
    }
}

TEST_F(SearchTest, ReturnsEmptyResultsWhenMlFindsNothing)
{
    fakeReply(kImagePath, k200OK, R"({"movie_ids":[]})");

    expectEmptyResults(searchImage({file("a.png", kPng)}));
}

TEST_F(SearchTest, ReturnsEmptyResultsWhenMovieIsNotInDatabase)
{
    fakeReply(kImagePath, k200OK, R"({"movie_ids":[999]})");

    expectEmptyResults(searchImage({file("a.png", kPng)}));
}

TEST_F(SearchTest, ReturnsOnlyMostRelevantMovie)
{
    insertFullMovie(603);
    insertFullMovie(604);
    fakeReply(kImagePath, k200OK, R"({"movie_ids":[604,603]})");
    const Json::Value json = body(searchImage({file("a.png", kPng)}));

    ASSERT_EQ(json["results"].size(), 1U);
    EXPECT_EQ(json["results"][0]["movie_id"].asInt64(), 604);
}

TEST_F(SearchTest, SendsDistinctNonEmptyRequestIdPerSearch)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto ml = service();
    const auto cookies = signedIn();
    std::set<std::string> ids;
    for (int i = 0; i < 2; ++i)
    {
        search(MediaKind::Image, multipart(kApiImage, {file("a.png", kPng)}, cookies), ml);
        const auto received = fakeReceived(kImagePath);
        ASSERT_NE(received, nullptr);
        const std::string id = received->getHeader("x-request-id");
        EXPECT_FALSE(id.empty());
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 2U);
}

TEST_F(SearchTest, ReportsUnavailableWhenMlIsUnreachable)
{
    config.url = "http://127.0.0.1:1";

    expectError(searchImage({file("a.png", kPng)}), k503ServiceUnavailable, "SEARCH_UNAVAILABLE",
                kUnavailable, false);
}

TEST_F(SearchTest, MapsMlNotReadyToUnavailable)
{
    fakeReply(kImagePath, k503ServiceUnavailable, R"({"code":"NOT_READY"})");

    expectError(searchImage({file("a.png", kPng)}), k503ServiceUnavailable, "SEARCH_UNAVAILABLE",
                kUnavailable, false);
}

TEST_F(SearchTest, MapsMlServerErrorToUnavailableNeverTo500)
{
    fakeReply(kImagePath, k500InternalServerError, "{}");

    expectError(searchImage({file("a.png", kPng)}), k503ServiceUnavailable, "SEARCH_UNAVAILABLE",
                kUnavailable, false);
}

TEST_F(SearchTest, ReportsBusyWhenAllMlSlotsAreTaken)
{
    fakeReply(kImagePath, k200OK, kFoundBody);
    const auto ml = service();
    ASSERT_TRUE(ml->slots.try_acquire());
    const auto response =
        search(MediaKind::Image, multipart(kApiImage, {file("a.png", kPng)}, signedIn()), ml);
    ml->slots.release();

    expectError(response, k503ServiceUnavailable, "SERVER_BUSY",
                "Сервис перегружен. Попробуйте позже", false);
    EXPECT_EQ(fakeReceived(kImagePath), nullptr);
}

TEST_F(SearchTest, VideoSearchSendsMovToVideoEndpointAndReturnsCard)
{
    insertFullMovie(603);
    fakeReply(kVideoPath, k200OK, kFoundBody);
    const auto response = searchVideo({file("clip.mov", kMov)});

    EXPECT_EQ(response->statusCode(), k200OK);
    EXPECT_EQ(body(response)["results"][0]["movie_id"].asInt64(), 603);
    const auto received = fakeReceived(kVideoPath);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->getHeader("content-type"), "video/quicktime");
    EXPECT_EQ(std::string(received->body()), kMov);
}

}  // namespace
