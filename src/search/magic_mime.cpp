#include "search/magic_mime.hpp"

#include <magic.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{

// Только MIME-тип. NO_CHECK_BUILTIN отключает встроенные проверки (текст, json, csv, tar,
// сжатие, ELF): они не нужны для шести форматов поиска, а на чужих данных сканируют до
// мегабайт и запускают распаковщики в дочернем процессе. Правила из базы остаются.
constexpr int kFlags = MAGIC_MIME_TYPE | MAGIC_NO_CHECK_BUILTIN;

using Cookie = std::unique_ptr<std::remove_pointer_t<magic_t>, decltype(&magic_close)>;

Cookie openCookie()
{
    Cookie cookie(magic_open(kFlags), magic_close);
    if (!cookie)
    {
        throw std::runtime_error("libmagic: magic_open failed");
    }
    // nullptr - база по умолчанию из системы (пакет libmagic-mgc)
    if (magic_load(cookie.get(), nullptr) != 0)
    {
        throw std::runtime_error(std::string("libmagic: cannot load database: ") +
                                 magic_error(cookie.get()));
    }
    return cookie;
}

}  // namespace

std::optional<std::string> magicMime(std::string_view content)
{
    // Один cookie на процесс: libmagic не потокобезопасна, а глобальное состояние (таблицы
    // при загрузке базы, путь по умолчанию) общее для всех cookie. Инициализация статика
    // потокобезопасна и загружает базу один раз. Одного замка достаточно: на файлах разбор занимает
    // микросекунды, в худшем случае десятки мс, а вызов ML - секунды.
    static const Cookie cookie = openCookie();
    static std::mutex mutex;
    const std::scoped_lock lock(mutex);

    // Строка из magic_buffer лежит в буфере cookie и затирается следующим вызовом, поэтому
    // копия делается под замком
    const char *mime = magic_buffer(cookie.get(), content.data(), content.size());
    if (mime == nullptr)
    {
        return std::nullopt;
    }
    return mime;
}
