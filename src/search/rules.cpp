#include "search/rules.hpp"

#include <array>
#include <span>
#include <stdexcept>
#include <utility>

#include "search/magic_mime.hpp"
#include "search/mp4_tracks.hpp"

namespace
{

// detected - ответ libmagic, sent - что backend отправляет в ML (backend-ml.md)
struct MimeRule
{
    std::string_view detected;
    std::string_view sent;
};

constexpr std::array<MimeRule, 3> kImageTypes{
    {{"image/jpeg", "image/jpeg"}, {"image/png", "image/png"}, {"image/webp", "image/webp"}}};
constexpr std::array<MimeRule, 3> kVideoTypes{{{"video/mp4", "video/mp4"},
                                               {"video/quicktime", "video/quicktime"},
                                               {"video/webm", "video/webm"}}};
// Все варианты M4A уходят в ML как audio/mp4
constexpr std::array<MimeRule, 5> kAudioTypes{{{"audio/mpeg", "audio/mpeg"},
                                               {"audio/x-wav", "audio/wav"},
                                               {"audio/ogg", "audio/ogg"},
                                               {"audio/x-m4a", "audio/mp4"},
                                               {"audio/mp4", "audio/mp4"}}};

// M4A с брендом isom/mp42 libmagic называет video/mp4: аудио, только если видеодорожек нет
constexpr MimeRule kAudioOnlyMp4{"video/mp4", "audio/mp4"};

std::span<const MimeRule> rulesFor(MediaKind kind)
{
    switch (kind)
    {
        case MediaKind::Image:
            return kImageTypes;
        case MediaKind::Video:
            return kVideoTypes;
        case MediaKind::Audio:
            return kAudioTypes;
        case MediaKind::Text:
            return {};
    }
    std::unreachable();
}

std::size_t limitFor(MediaKind kind)
{
    switch (kind)
    {
        case MediaKind::Image:
            return kMaxImageBytes;
        case MediaKind::Video:
            return kMaxVideoBytes;
        case MediaKind::Audio:
            return kMaxAudioBytes;
        case MediaKind::Text:
            break;
    }
    std::unreachable();
}

}  // namespace

std::optional<std::string_view> detectMediaType(MediaKind kind, std::string_view content)
{
    const auto detected = magicMime(content);
    if (!detected)
    {
        return std::nullopt;
    }
    // Возвращаем строку из таблицы, не из libmagic: она живёт дольше вызова
    for (const MimeRule &rule : rulesFor(kind))
    {
        if (*detected == rule.detected)
        {
            return rule.sent;
        }
    }
    if (kind == MediaKind::Audio && *detected == kAudioOnlyMp4.detected && isAudioOnlyMp4(content))
    {
        return kAudioOnlyMp4.sent;
    }
    return std::nullopt;
}

std::expected<Upload, SearchError> checkUpload(MediaKind kind,
                                               std::span<const std::string_view> files)
{
    if (kind == MediaKind::Text)
    {
        throw std::invalid_argument("checkUpload: у текстового поиска нет файла");
    }
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
    if (content.size() > limitFor(kind))
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
