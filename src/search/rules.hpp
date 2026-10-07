#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

// Чистая логика поиска по медиа: проверки загруженного файла в порядке контракта
// и таблица текстов ошибок

enum class MediaKind
{
    Image,
    Video
};

enum class SearchError
{
    FileRequired,
    TooManyFiles,
    FileTooLarge,
    UnsupportedFormat,
    VideoTooLong,
    FileCorrupted,
    Unavailable,
    Busy
};

// В контракте 1 МБ = 1 048 576 байт, границы включительно
constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kMaxImageBytes = 10 * kMiB;
constexpr std::size_t kMaxVideoBytes = 50 * kMiB;

struct ErrorReply
{
    int status;
    std::string_view code;
    std::string_view message;
    bool fileField;
};

// Для Image + VideoTooLong возвращает ответ Unavailable: ML такую ошибку для картинки не даёт
[[nodiscard]] ErrorReply errorReply(MediaKind kind, SearchError error);

// Формат определяется по содержимому, не по расширению. MIME: image/jpeg, image/png,
// image/webp, video/mp4, video/quicktime, video/webm. Тип чужого вида (kind) не принимается.
[[nodiscard]] std::optional<std::string_view> detectMediaType(MediaKind kind,
                                                              std::string_view content);

struct Upload
{
    std::string_view mime;
    std::string_view content;
};

// files - содержимое всех файловых частей; пустые части игнорируются (браузер шлёт такую,
// если файл не выбран). Порядок проверок: число файлов, размер, формат.
[[nodiscard]] std::expected<Upload, SearchError> checkUpload(
    MediaKind kind, std::span<const std::string_view> files);
