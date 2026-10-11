#include "search/rules.hpp"

#include <array>

#include "search/magic_mime.hpp"

namespace
{

constexpr std::array<std::string_view, 3> kImageTypes{"image/jpeg", "image/png", "image/webp"};
constexpr std::array<std::string_view, 3> kVideoTypes{"video/mp4", "video/quicktime", "video/webm"};

}  // namespace

std::optional<std::string_view> detectMediaType(MediaKind kind, std::string_view content)
{
    const auto detected = magicMime(content);
    if (!detected)
    {
        return std::nullopt;
    }
    // Возвращаем строку из списка, не из libmagic: она живёт дольше вызова
    for (const std::string_view allowed : kind == MediaKind::Image ? kImageTypes : kVideoTypes)
    {
        if (*detected == allowed)
        {
            return allowed;
        }
    }
    return std::nullopt;
}

std::expected<Upload, SearchError> checkUpload(MediaKind kind,
                                               std::span<const std::string_view> files)
{
    std::size_t count = 0;
    std::string_view content;
    for (const std::string_view file : files)
    {
        if (!file.empty())
        {
            ++count;
            content = file;
        }
    }
    if (count == 0)
    {
        return std::unexpected(SearchError::FileRequired);
    }
    if (count > 1)
    {
        return std::unexpected(SearchError::TooManyFiles);
    }
    const std::size_t limit = kind == MediaKind::Image ? kMaxImageBytes : kMaxVideoBytes;
    if (content.size() > limit)
    {
        return std::unexpected(SearchError::FileTooLarge);
    }
    const auto mime = detectMediaType(kind, content);
    if (!mime)
    {
        return std::unexpected(SearchError::UnsupportedFormat);
    }
    return Upload{*mime, content};
}
