#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <expected>
#include <string>
#include <string_view>

// Отзывы о фильме (#17239, тег Reviews в api/openapi.yaml). Пока только список (#18845):
// написание, редактирование и реакции - Sprint 5 (#17029).

enum class ReviewsSort
{
    Best,    // больше «палец вверх»
    Worst,   // больше «палец вниз»
    Newest,  // сначала новые
    Oldest   // сначала старые
};

struct ReviewsQuery
{
    ReviewsSort sort = ReviewsSort::Best;
    int page = 1;
};

// Параметры sort и page из адреса, пустой - значение по умолчанию. Ошибка - имя неверного
// параметра для field в ответе 400.
std::expected<ReviewsQuery, std::string_view> parseReviewsQuery(std::string_view sort,
                                                                std::string_view page);

// GET /movies/{movie_id}/reviews: страница до 10 отзывов и has_more, гостю тоже. С действующей
// сессией свой отзыв в список не входит: он показывается отдельно (#17029 п. 3).
drogon::Task<drogon::HttpResponsePtr> movieReviewsHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req,
                                                          std::string movieId);
