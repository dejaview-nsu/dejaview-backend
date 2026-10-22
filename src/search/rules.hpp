#pragma once

#include "search/errors.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

// Чистая логика поиска по медиа: проверки загруженного файла в порядке контракта

// В контракте 1 МБ = 1 048 576 байт, границы включительно
constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kMaxImageBytes = 10 * kMiB;
constexpr std::size_t kMaxVideoBytes = 50 * kMiB;
constexpr std::size_t kMaxAudioBytes = 20 * kMiB;

// Формат определяется по содержимому (libmagic), не по расширению. Возвращает MIME для ML:
// image/jpeg, image/png, image/webp, video/mp4, video/quicktime, video/webm, audio/mpeg,
// audio/wav, audio/ogg, audio/mp4. Тип чужого вида (kind) не принимается.
[[nodiscard]] std::optional<std::string_view> detectMediaType(MediaKind kind,
                                                              std::string_view content);

struct Upload
{
    std::string_view mime;
    std::string_view content;
};

// files - содержимое всех файловых частей; пустые части игнорируются (браузер шлёт такую,
// если файл не выбран). Порядок проверок: число файлов, размер, формат.
// Для MediaKind::Text бросает std::invalid_argument: у текстового поиска нет файла.
[[nodiscard]] std::expected<Upload, SearchError> checkUpload(
    MediaKind kind, std::span<const std::string_view> files);
