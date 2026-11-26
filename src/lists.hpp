#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <expected>
#include <string>
#include <string_view>

// Списки профиля «Хочу посмотреть», «Просмотрено» и «Мои оценки» (#17030 п. 2.2–2.4).
// Владелец списка определяется по сессии.

enum class ListSort
{
    Newest,     // по дате добавления, сначала новые
    Oldest,     // по дате добавления, сначала старые
    TitleAsc,   // по названию, А-Я
    TitleDesc,  // по названию, Я-А
    Best,       // по средней оценке, сначала лучшие
    Worst       // по средней оценке, сначала худшие
};

struct ListQuery
{
    ListSort sort = ListSort::Newest;
    std::string q;  // часть названия без пробелов по краям, пустая - весь список
    int page = 1;
};

// list_type из пути: want_to_watch или watched.
bool isListType(std::string_view text);

// Параметры sort, q и page из адреса, пустой - значение по умолчанию. Ошибка - имя неверного
// параметра для field в ответе 400.
std::expected<ListQuery, std::string_view> parseListQuery(std::string_view sort, std::string_view q,
                                                          std::string_view page);

// GET /users/me/lists/{list_type}: {movies, total}, по 24 фильма на странице.
drogon::Task<drogon::HttpResponsePtr> listMoviesHandler(drogon::orm::DbClientPtr db,
                                                        drogon::HttpRequestPtr req,
                                                        std::string listType);

// GET /users/me/ratings: {movies, total}, только фильмы с оценкой пользователя.
drogon::Task<drogon::HttpResponsePtr> listRatingsHandler(drogon::orm::DbClientPtr db,
                                                         drogon::HttpRequestPtr req);

// PUT /users/me/lists/{list_type}/{movie_id}: 204 и при повторе, фильма нет в movies - 404.
drogon::Task<drogon::HttpResponsePtr> addToListHandler(drogon::orm::DbClientPtr db,
                                                       drogon::HttpRequestPtr req,
                                                       std::string listType, std::string movieId);

// DELETE /users/me/lists/{list_type}/{movie_id}: 204, в том числе если фильма в списке не было.
drogon::Task<drogon::HttpResponsePtr> removeFromListHandler(drogon::orm::DbClientPtr db,
                                                            drogon::HttpRequestPtr req,
                                                            std::string listType,
                                                            std::string movieId);

// GET /users/me/movies/{movie_id}: состояние списков и оценка пользователя на странице фильма.
drogon::Task<drogon::HttpResponsePtr> movieListsStateHandler(drogon::orm::DbClientPtr db,
                                                             drogon::HttpRequestPtr req,
                                                             std::string movieId);
