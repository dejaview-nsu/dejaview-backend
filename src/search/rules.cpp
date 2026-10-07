#include "search/rules.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace
{

constexpr std::string_view kJpegSignature = "\xFF\xD8\xFF";
constexpr std::string_view kPngSignature = "\x89PNG\r\n\x1a\n";
constexpr std::string_view kRiffSignature = "RIFF";
constexpr std::string_view kWebpTag = "WEBP";
constexpr std::string_view kFtypTag = "ftyp";
constexpr std::string_view kQuickTimeBrand = "qt  ";
constexpr std::string_view kEbmlSignature = "\x1A\x45\xDF\xA3";
constexpr std::uint32_t kDocTypeId = 0x4282;
constexpr std::string_view kWebmDocType = "webm";

constexpr std::size_t kRiffTagOffset = 8;
constexpr std::size_t kFtypTagOffset = 4;
constexpr std::size_t kBrandOffset = 8;
constexpr std::size_t kBrandLength = 4;
constexpr std::size_t kFtypMinBytes = 12;
constexpr std::size_t kMaxEbmlIdBytes = 4;

// Бренды ISO BMFF, которые не видео: изображения (AVIF/HEIC) и аудио
constexpr std::array<std::string_view, 13> kNonVideoBrands{"avif", "avis", "heic", "heix", "heim",
                                                           "heis", "hevc", "hevx", "mif1", "msf1",
                                                           "M4A ", "M4B ", "M4P "};

struct Vint
{
    std::uint64_t value;
    std::size_t length;
    bool unknownSize;
};

// Целое переменной длины EBML: число ведущих нулей первого байта задаёт длину (1-8 байт).
// ID хранится вместе с маркерным битом, размер - без него.
std::optional<Vint> readVint(std::string_view content, std::size_t offset, bool keepMarker)
{
    if (offset >= content.size())
    {
        return std::nullopt;
    }
    const auto first = static_cast<std::uint8_t>(content[offset]);
    if (first == 0)
    {
        return std::nullopt;
    }
    const std::size_t length = static_cast<std::size_t>(std::countl_zero(first)) + 1;
    if (length > content.size() - offset)
    {
        return std::nullopt;
    }
    std::uint64_t value = keepMarker ? first : first & (0xFFu >> length);
    for (std::size_t i = 1; i < length; ++i)
    {
        value = (value << 8) | static_cast<std::uint8_t>(content[offset + i]);
    }
    const std::uint64_t allOnes = (std::uint64_t{1} << (7 * length)) - 1;
    return Vint{value, length, !keepMarker && value == allOnes};
}

std::optional<std::string_view> detectImage(std::string_view content)
{
    if (content.starts_with(kJpegSignature))
    {
        return "image/jpeg";
    }
    if (content.starts_with(kPngSignature))
    {
        return "image/png";
    }
    if (content.starts_with(kRiffSignature) && content.size() >= kRiffTagOffset + kWebpTag.size() &&
        content.substr(kRiffTagOffset, kWebpTag.size()) == kWebpTag)
    {
        return "image/webp";
    }
    return std::nullopt;
}

std::optional<std::string_view> detectIsoBmff(std::string_view content)
{
    if (content.size() < kFtypMinBytes ||
        content.substr(kFtypTagOffset, kFtypTag.size()) != kFtypTag)
    {
        return std::nullopt;
    }
    const std::string_view brand = content.substr(kBrandOffset, kBrandLength);
    if (brand == kQuickTimeBrand)
    {
        return "video/quicktime";
    }
    for (const std::string_view other : kNonVideoBrands)
    {
        if (brand == other)
        {
            return std::nullopt;
        }
    }
    return "video/mp4";
}

std::string_view trimTrailingNuls(std::string_view value)
{
    while (!value.empty() && value.back() == '\0')
    {
        value.remove_suffix(1);
    }
    return value;
}

// Ищет DocType среди детей EBML-заголовка. Matroska с тем же сигнатурой - не WebM.
std::optional<std::string_view> detectWebm(std::string_view content)
{
    if (!content.starts_with(kEbmlSignature))
    {
        return std::nullopt;
    }
    const auto headerSize = readVint(content, kEbmlSignature.size(), false);
    if (!headerSize)
    {
        return std::nullopt;
    }
    const std::size_t bodyStart = kEbmlSignature.size() + headerSize->length;
    if (!headerSize->unknownSize && headerSize->value > content.size() - bodyStart)
    {
        return std::nullopt;
    }
    const std::size_t end =
        headerSize->unknownSize ? content.size() : bodyStart + headerSize->value;

    std::size_t offset = bodyStart;
    while (offset < end)
    {
        const auto id = readVint(content, offset, true);
        if (!id || id->length > kMaxEbmlIdBytes)
        {
            return std::nullopt;
        }
        const auto size = readVint(content, offset + id->length, false);
        if (!size || size->unknownSize)
        {
            return std::nullopt;
        }
        const std::size_t dataStart = offset + id->length + size->length;
        if (dataStart > end || size->value > end - dataStart)
        {
            return std::nullopt;
        }
        if (id->value == kDocTypeId)
        {
            const std::string_view docType =
                trimTrailingNuls(content.substr(dataStart, size->value));
            if (docType == kWebmDocType)
            {
                return "video/webm";
            }
            return std::nullopt;
        }
        offset = dataStart + size->value;
    }
    return std::nullopt;
}

std::optional<std::string_view> detectVideo(std::string_view content)
{
    if (const auto mp4OrMov = detectIsoBmff(content))
    {
        return mp4OrMov;
    }
    return detectWebm(content);
}

struct ErrorRow
{
    SearchError error;
    int status;
    std::string_view code;
    std::string_view imageMessage;
    std::string_view videoMessage;
    bool fileField;
};

constexpr std::string_view kUnavailable = "Поиск временно недоступен. Попробуйте позже";

// Тексты дословно из openapi.yaml
constexpr std::array<ErrorRow, 8> kErrorRows{{
    {SearchError::FileRequired, 400, "FILE_REQUIRED",
     "Невозможно выполнить поиск по изображению: изображение не загружено",
     "Невозможно выполнить поиск по видеофрагменту: видеофрагмент не загружен", true},
    {SearchError::TooManyFiles, 400, "TOO_MANY_FILES",
     "Невозможно выполнить поиск по изображению: загружено больше одного файла",
     "Невозможно выполнить поиск по видеофрагменту: загружено больше одного файла", true},
    {SearchError::FileTooLarge, 413, "FILE_TOO_LARGE",
     "Невозможно выполнить поиск по изображению: размер файла больше 10 МБ",
     "Невозможно выполнить поиск по видеофрагменту: размер файла больше 50 МБ", true},
    {SearchError::UnsupportedFormat, 415, "UNSUPPORTED_FORMAT",
     "Невозможно выполнить поиск по изображению: поддерживаются только JPG, JPEG, PNG и WEBP",
     "Невозможно выполнить поиск по видеофрагменту: поддерживаются только MP4, MOV и WEBM", true},
    {SearchError::VideoTooLong, 422, "VIDEO_TOO_LONG", "",
     "Невозможно выполнить поиск по видеофрагменту: видео длиннее 30 секунд", true},
    {SearchError::FileCorrupted, 422, "FILE_CORRUPTED",
     "Невозможно выполнить поиск по изображению: файл поврежден или не может быть обработан",
     "Невозможно выполнить поиск по видеофрагменту: файл поврежден или не может быть обработан",
     true},
    {SearchError::Unavailable, 503, "SEARCH_UNAVAILABLE", kUnavailable, kUnavailable, false},
    {SearchError::Busy, 503, "SERVER_BUSY", "Сервис перегружен. Попробуйте позже",
     "Сервис перегружен. Попробуйте позже", false},
}};

}  // namespace

ErrorReply errorReply(MediaKind kind, SearchError error)
{
    if (kind == MediaKind::Image && error == SearchError::VideoTooLong)
    {
        error = SearchError::Unavailable;
    }
    for (const ErrorRow &row : kErrorRows)
    {
        if (row.error == error)
        {
            return {row.status, row.code,
                    kind == MediaKind::Image ? row.imageMessage : row.videoMessage, row.fileField};
        }
    }
    return errorReply(kind, SearchError::Unavailable);
}

std::optional<std::string_view> detectMediaType(MediaKind kind, std::string_view content)
{
    return kind == MediaKind::Image ? detectImage(content) : detectVideo(content);
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
