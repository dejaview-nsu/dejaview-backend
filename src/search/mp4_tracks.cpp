#include "search/mp4_tracks.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace
{

constexpr std::size_t kBoxHeaderBytes = 8;  // size32 + type
constexpr std::size_t kTypeBytes = 4;
constexpr std::size_t kLargeSizeBytes = 8;
constexpr std::size_t kUuidBytes = 16;
constexpr std::size_t kFullBoxHeaderBytes = 4;  // version + flags
constexpr std::size_t kPreDefinedBytes = 4;
constexpr std::size_t kHdlrMinPayloadBytes = kFullBoxHeaderBytes + kPreDefinedBytes + kTypeBytes;

constexpr std::uint32_t kSizeLargeMarker = 1;  // реальный размер в 64 битах после типа
constexpr std::uint32_t kSizeToEnd = 0;        // коробка тянется до конца охватывающего диапазона

constexpr std::string_view kMoov = "moov";
constexpr std::string_view kTrak = "trak";
constexpr std::string_view kMdia = "mdia";
constexpr std::string_view kHdlr = "hdlr";
constexpr std::string_view kUuid = "uuid";
constexpr std::string_view kSound = "soun";
constexpr std::string_view kVideo = "vide";

struct Box
{
    std::string_view type;
    std::string_view payload;
};

// Читает все байты bytes (не больше 8) как big-endian; длину проверяет вызывающий
std::uint64_t bigEndian(std::string_view bytes)
{
    std::uint64_t value = 0;
    for (const char byte : bytes)
    {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

// Снимает с начала range одну коробку; nullopt - заголовок не помещается или размер неверен
std::optional<Box> takeBox(std::string_view &range)
{
    if (range.size() < kBoxHeaderBytes)
    {
        return std::nullopt;
    }
    const std::uint64_t size32 = bigEndian(range.substr(0, kBoxHeaderBytes - kTypeBytes));
    const std::string_view type = range.substr(kBoxHeaderBytes - kTypeBytes, kTypeBytes);

    std::size_t headerBytes = kBoxHeaderBytes;
    std::uint64_t size = size32;
    if (size32 == kSizeLargeMarker)
    {
        headerBytes += kLargeSizeBytes;
        if (range.size() < headerBytes)
        {
            return std::nullopt;
        }
        size = bigEndian(range.substr(kBoxHeaderBytes, kLargeSizeBytes));
    }
    else if (size32 == kSizeToEnd)
    {
        size = range.size();
    }
    if (type == kUuid)
    {
        headerBytes += kUuidBytes;
    }
    if (size < headerBytes || size > range.size())
    {
        return std::nullopt;
    }
    const Box box{type, range.substr(headerBytes, size - headerBytes)};
    range.remove_prefix(size);
    return box;
}

// Содержимое единственной коробки type среди детей range; весь range обходится до конца,
// чтобы битый хвост не прошёл. Нет такой, больше одной или структура битая -> nullopt
std::optional<std::string_view> onlyChild(std::string_view range, std::string_view type)
{
    std::optional<std::string_view> found;
    while (!range.empty())
    {
        const std::optional<Box> box = takeBox(range);
        if (!box)
        {
            return std::nullopt;
        }
        if (box->type != type)
        {
            continue;
        }
        if (found)
        {
            return std::nullopt;
        }
        found = box->payload;
    }
    return found;
}

std::optional<std::string_view> handlerType(std::string_view trakPayload)
{
    const std::optional<std::string_view> mdia = onlyChild(trakPayload, kMdia);
    const std::optional<std::string_view> hdlr = mdia ? onlyChild(*mdia, kHdlr) : std::nullopt;
    if (!hdlr || hdlr->size() < kHdlrMinPayloadBytes)
    {
        return std::nullopt;
    }
    return hdlr->substr(kFullBoxHeaderBytes + kPreDefinedBytes, kTypeBytes);
}

}  // namespace

bool isAudioOnlyMp4(std::string_view content)
{
    const std::optional<std::string_view> moov = onlyChild(content, kMoov);
    if (!moov)
    {
        return false;
    }
    std::string_view tracks = *moov;
    std::size_t sound = 0;
    std::size_t video = 0;
    while (!tracks.empty())
    {
        const std::optional<Box> box = takeBox(tracks);
        if (!box)
        {
            return false;
        }
        if (box->type != kTrak)
        {
            continue;
        }
        const std::optional<std::string_view> handler = handlerType(box->payload);
        if (!handler)
        {
            return false;
        }
        sound += *handler == kSound;
        video += *handler == kVideo;
    }
    return sound > 0 && video == 0;
}
