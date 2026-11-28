#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <expected>
#include <string>
#include <string_view>

// Отзывы о фильме (#17029, #17239, тег Reviews в api/openapi.yaml).

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

// GET /movies/{movie_id}/reviews: до 10 отзывов и has_more, гостю тоже. Свой отзыв возвращается
// отдельно в own_review только на первой странице (#17029 п. 3).
drogon::Task<drogon::HttpResponsePtr> movieReviewsHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req,
                                                          std::string movieId);

// GET /users/me/reviews: по 24 отзыва владельца сессии с данными фильмов и общим числом совпадений.
drogon::Task<drogon::HttpResponsePtr> myReviewsHandler(drogon::orm::DbClientPtr db,
                                                       drogon::HttpRequestPtr req);

drogon::Task<drogon::HttpResponsePtr> createMovieReviewHandler(drogon::orm::DbClientPtr db,
                                                               drogon::HttpRequestPtr req,
                                                               std::string movieId);

drogon::Task<drogon::HttpResponsePtr> updateReviewHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req,
                                                          std::string reviewId);

drogon::Task<drogon::HttpResponsePtr> deleteReviewHandler(drogon::orm::DbClientPtr db,
                                                          drogon::HttpRequestPtr req,
                                                          std::string reviewId);
