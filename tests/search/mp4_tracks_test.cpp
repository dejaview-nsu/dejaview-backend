#include "search/mp4_tracks.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include "samples.hpp"

namespace
{

constexpr std::uint32_t kLargeSizeMarker = 1;
constexpr std::size_t kHdlrMinPayload = 12;
constexpr std::uint64_t kMaxUint64 = 0xFFFFFFFFFFFFFFFFULL;

std::string be32(std::uint32_t value)
{
    std::string out(4, '\0');
    for (std::size_t i = 0; i < 4; ++i)
    {
        out[i] = static_cast<char>((value >> (24 - 8 * i)) & 0xFF);
    }
    return out;
}

std::string be64(std::uint64_t value)
{
    std::string out(8, '\0');
    for (std::size_t i = 0; i < 8; ++i)
    {
        out[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xFF);
    }
    return out;
}

std::uint32_t sizeOf(std::size_t value) { return static_cast<std::uint32_t>(value); }

// Коробка с произвольно заявленным size32: нужна для заведомо битых размеров
std::string rawBox(std::uint32_t size, std::string_view type, std::string_view payload)
{
    return be32(size) + std::string(type) + std::string(payload);
}

std::string box(std::string_view type, std::string_view payload = {})
{
    return rawBox(sizeOf(8 + payload.size()), type, payload);
}

// size32 == 1, затем 64-битный размер; declared подменяет честный размер
std::string largeBoxDeclared(std::string_view type, std::string_view payload,
                             std::uint64_t declared)
{
    return be32(kLargeSizeMarker) + std::string(type) + be64(declared) + std::string(payload);
}

std::string largeBox(std::string_view type, std::string_view payload)
{
    return largeBoxDeclared(type, payload, 16 + payload.size());
}

// size32 == 0: коробка тянется до конца охватывающего диапазона
std::string zeroSizeBox(std::string_view type, std::string_view payload = {})
{
    return rawBox(0, type, payload);
}

std::string hdlrPayload(std::string_view handler, std::size_t total = kHdlrMinPayload)
{
    std::string payload = be32(0) + be32(0) + std::string(handler);
    payload.resize(total, '\0');
    return payload;
}

std::string hdlr(std::string_view handler) { return box("hdlr", hdlrPayload(handler)); }

std::string mdia(std::string_view handler) { return box("mdia", hdlr(handler)); }

std::string trak(std::string_view handler) { return box("trak", mdia(handler)); }

std::string moov(std::string_view payload) { return box("moov", payload); }

const std::string kFtyp = box("ftyp", std::string("isom\0\0\0\0isom", 12));
const std::string kMdat = box("mdat", std::string(32, 'x'));

std::string file(std::string_view moovBox) { return kFtyp + std::string(moovBox); }

}  // namespace

TEST(IsAudioOnlyMp4Test, AcceptsSingleSoundTrack)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, AcceptsMdatAfterMoov)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(trak("soun"))) + kMdat));
}

TEST(IsAudioOnlyMp4Test, AcceptsMdatBeforeMoov)
{
    EXPECT_TRUE(isAudioOnlyMp4(kFtyp + kMdat + moov(trak("soun"))));
}

TEST(IsAudioOnlyMp4Test, RejectsSoundPlusVideoTracks)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("soun") + trak("vide")))));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("vide") + trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, RejectsVideoOnly)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("vide")))));
}

TEST(IsAudioOnlyMp4Test, IgnoresOtherHandlers)
{
    for (const char *handler : {"text", "subt", "meta", "hint"})
    {
        SCOPED_TRACE(handler);
        EXPECT_TRUE(isAudioOnlyMp4(file(moov(trak("soun") + trak(handler)))));
    }
}

TEST(IsAudioOnlyMp4Test, RejectsTrackWithOnlyOtherHandler)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("text")))));
}

TEST(IsAudioOnlyMp4Test, RejectsMoovWithoutTracks)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(""))));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("mvhd", std::string(20, '\0'))))));
}

TEST(IsAudioOnlyMp4Test, RejectsMissingMoov) { EXPECT_FALSE(isAudioOnlyMp4(kFtyp + kMdat)); }

TEST(IsAudioOnlyMp4Test, RejectsTwoMoov)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("soun")) + moov(trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, RejectsEmptyContent) { EXPECT_FALSE(isAudioOnlyMp4("")); }

TEST(IsAudioOnlyMp4Test, RejectsTruncatedHeaders)
{
    const std::string whole = file(moov(trak("soun")));
    for (std::size_t length = 1; length <= 7; ++length)
    {
        SCOPED_TRACE(length);
        EXPECT_FALSE(isAudioOnlyMp4(whole.substr(0, length)));
    }
}

TEST(IsAudioOnlyMp4Test, RejectsStrayBytesAfterLastBox)
{
    const std::string whole = file(moov(trak("soun")));
    for (std::size_t stray = 1; stray <= 7; ++stray)
    {
        SCOPED_TRACE(stray);
        EXPECT_FALSE(isAudioOnlyMp4(whole + std::string(stray, '\0')));
    }
    EXPECT_FALSE(isAudioOnlyMp4(whole + std::string(4, 'x')));
}

TEST(IsAudioOnlyMp4Test, RejectsStrayBytesInsideMoov)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("soun") + std::string(3, '\0')))));
}

TEST(IsAudioOnlyMp4Test, RejectsStrayBytesInsideTrakAndMdia)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", mdia("soun") + std::string(5, '\0'))))));
    EXPECT_FALSE(
        isAudioOnlyMp4(file(moov(box("trak", box("mdia", hdlr("soun") + std::string(5, '\0')))))));
}

TEST(IsAudioOnlyMp4Test, RejectsEveryTruncationOfValidFile)
{
    const std::string validPart = file(moov(trak("soun")));
    const std::string whole = validPart + kMdat;
    for (std::size_t length = 0; length < whole.size(); ++length)
    {
        SCOPED_TRACE(length);
        EXPECT_EQ(isAudioOnlyMp4(whole.substr(0, length)), length == validPart.size());
    }
}

TEST(IsAudioOnlyMp4Test, AcceptsZeroSizeLastTopLevelBox)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(trak("soun"))) + zeroSizeBox("mdat", "payload")));
}

TEST(IsAudioOnlyMp4Test, AcceptsZeroSizeMoov)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(zeroSizeBox("moov", trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, AcceptsZeroSizeTrakInsideMoov)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(zeroSizeBox("trak", mdia("soun"))))));
}

TEST(IsAudioOnlyMp4Test, ZeroSizeMdatSwallowsMoovThatFollowsIt)
{
    EXPECT_FALSE(isAudioOnlyMp4(kFtyp + zeroSizeBox("mdat", moov(trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, AcceptsLargeSizeBoxes)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(largeBox("moov", trak("soun")))));
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(trak("soun"))) + largeBox("mdat", "payload")));
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(largeBox("trak", mdia("soun"))))));
}

TEST(IsAudioOnlyMp4Test, AcceptsLargeSizeBoxWithEmptyPayload)
{
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(trak("soun"))) + largeBox("free", "")));
}

TEST(IsAudioOnlyMp4Test, RejectsLargeSizeBelowSixteen)
{
    for (const std::uint64_t declared : {0ULL, 1ULL, 8ULL, 15ULL})
    {
        SCOPED_TRACE(declared);
        EXPECT_FALSE(isAudioOnlyMp4(file(largeBoxDeclared("moov", trak("soun"), declared))));
    }
}

TEST(IsAudioOnlyMp4Test, RejectsLargeSizeBeyondRemaining)
{
    const std::string payload = trak("soun");
    const std::uint64_t real = 16 + payload.size();
    EXPECT_FALSE(isAudioOnlyMp4(file(largeBoxDeclared("moov", payload, real + 1))));
    EXPECT_FALSE(isAudioOnlyMp4(file(largeBoxDeclared("moov", payload, 1ULL << 32))));
    EXPECT_FALSE(isAudioOnlyMp4(file(largeBoxDeclared("moov", payload, 1ULL << 63))));
}

TEST(IsAudioOnlyMp4Test, RejectsLargeSizeShorterThanRealLength)
{
    const std::string payload = trak("soun");
    EXPECT_FALSE(isAudioOnlyMp4(file(largeBoxDeclared("moov", payload, 16 + payload.size() - 1))));
}

TEST(IsAudioOnlyMp4Test, RejectsMaxUint64LargeSizeWithoutOverflow)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(largeBoxDeclared("moov", trak("soun"), kMaxUint64))));
    EXPECT_FALSE(
        isAudioOnlyMp4(file(moov(trak("soun"))) + largeBoxDeclared("mdat", "x", kMaxUint64)));
    // Значения, при которых size + смещение переполнило бы uint64
    for (const std::uint64_t delta : {1ULL, 7ULL, 8ULL, 15ULL, 16ULL, 24ULL})
    {
        SCOPED_TRACE(delta);
        EXPECT_FALSE(
            isAudioOnlyMp4(file(largeBoxDeclared("moov", trak("soun"), kMaxUint64 - delta))));
    }
}

TEST(IsAudioOnlyMp4Test, RejectsLargeSizeMarkerWithoutRoomForLargeSize)
{
    for (std::size_t extra = 0; extra < 8; ++extra)
    {
        SCOPED_TRACE(extra);
        EXPECT_FALSE(
            isAudioOnlyMp4(kFtyp + be32(kLargeSizeMarker) + "moov" + std::string(extra, '\0')));
    }
}

TEST(IsAudioOnlyMp4Test, RejectsSizeBetweenTwoAndSeven)
{
    for (const std::uint32_t size : {2U, 3U, 4U, 5U, 6U, 7U})
    {
        SCOPED_TRACE(size);
        EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("soun"))) + rawBox(size, "free", "")));
        EXPECT_FALSE(isAudioOnlyMp4(file(moov(rawBox(size, "free", "") + trak("soun")))));
    }
}

TEST(IsAudioOnlyMp4Test, AcceptsEmptyEightByteBoxBetweenOthers)
{
    EXPECT_TRUE(
        isAudioOnlyMp4(kFtyp + box("free") + moov(box("free") + trak("soun")) + box("free")));
}

TEST(IsAudioOnlyMp4Test, RejectsSizeBeyondEndOfContent)
{
    const std::string payload = trak("soun");
    EXPECT_FALSE(isAudioOnlyMp4(file(rawBox(1000, "moov", payload))));
    EXPECT_FALSE(isAudioOnlyMp4(file(rawBox(sizeOf(8 + payload.size() + 1), "moov", payload))));
    EXPECT_FALSE(isAudioOnlyMp4(file(rawBox(0xFFFFFFFFU, "moov", payload))));
}

TEST(IsAudioOnlyMp4Test, RejectsTrakLargerThanMoovPayload)
{
    // Размер trak укладывается в файл (за moov идёт mdat), но выходит за границу moov
    const std::string inner = mdia("soun");
    const std::string oversized = rawBox(sizeOf(8 + inner.size() + 4), "trak", inner);
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(oversized)) + kMdat));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(oversized))));
}

TEST(IsAudioOnlyMp4Test, RejectsMdiaAndHdlrLargerThanParent)
{
    const std::string h = hdlr("soun");
    const std::string oversizedMdia = rawBox(sizeOf(8 + h.size() + 4), "mdia", h);
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", oversizedMdia) + kMdat))));
    const std::string oversizedHdlr =
        rawBox(sizeOf(8 + kHdlrMinPayload + 4), "hdlr", hdlrPayload("soun"));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", oversizedHdlr)) + kMdat))));
}

TEST(IsAudioOnlyMp4Test, RejectsUuidBoxWithSizeBelowTwentyFour)
{
    for (std::uint32_t size = 8; size <= 23; ++size)
    {
        SCOPED_TRACE(size);
        const std::string uuid = rawBox(size, "uuid", std::string(size - 8, '\0'));
        EXPECT_FALSE(isAudioOnlyMp4(file(moov(trak("soun"))) + uuid));
        EXPECT_FALSE(isAudioOnlyMp4(file(moov(uuid + trak("soun")))));
    }
}

TEST(IsAudioOnlyMp4Test, AcceptsWellFormedUuidBoxAndSkipsIt)
{
    const std::string uuid = box("uuid", std::string(16, 'u'));
    ASSERT_EQ(uuid.size(), 24U);
    EXPECT_TRUE(isAudioOnlyMp4(kFtyp + uuid + moov(trak("soun"))));
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(uuid + trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, AcceptsUuidBoxWithLargeSize)
{
    EXPECT_TRUE(
        isAudioOnlyMp4(kFtyp + largeBox("uuid", std::string(16, 'u')) + moov(trak("soun"))));
}

TEST(IsAudioOnlyMp4Test, RejectsLargeSizeUuidBoxWithoutRoomForExtendedType)
{
    EXPECT_FALSE(
        isAudioOnlyMp4(kFtyp + largeBox("uuid", std::string(15, 'u')) + moov(trak("soun"))));
}

TEST(IsAudioOnlyMp4Test, RejectsHdlrPayloadShorterThanTwelveBytes)
{
    for (std::size_t length = 0; length < kHdlrMinPayload; ++length)
    {
        SCOPED_TRACE(length);
        const std::string shortHdlr = box("hdlr", hdlrPayload("soun").substr(0, length));
        EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", shortHdlr))))));
    }
}

TEST(IsAudioOnlyMp4Test, AcceptsHdlrPayloadOfExactlyTwelveBytes)
{
    const std::string exact = box("hdlr", hdlrPayload("soun", kHdlrMinPayload));
    ASSERT_EQ(exact.size(), 8 + kHdlrMinPayload);
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", exact))))));
}

TEST(IsAudioOnlyMp4Test, AcceptsHdlrPayloadWithNameField)
{
    const std::string longer = box("hdlr", hdlrPayload("soun", 40));
    EXPECT_TRUE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", longer))))));
}

TEST(IsAudioOnlyMp4Test, RejectsTrakWithoutMdia)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("tkhd", std::string(84, '\0')))))));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak")))));
}

TEST(IsAudioOnlyMp4Test, RejectsMdiaWithoutHdlr)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", box("mdhd", "xxxx")))))));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("mdia"))))));
}

TEST(IsAudioOnlyMp4Test, RejectsMdiaWithTwoHdlr)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", hdlr("soun") + hdlr("soun")))))));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", box("mdia", hdlr("soun") + hdlr("vide")))))));
}

TEST(IsAudioOnlyMp4Test, RejectsTrakWithTwoMdia)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", mdia("soun") + mdia("soun"))))));
}

TEST(IsAudioOnlyMp4Test, RejectsTrakNestedInTrakWithoutMdia)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", trak("soun"))))));
}

TEST(IsAudioOnlyMp4Test, SkipsMoovAndTrakNestedElsewhere)
{
    // Вложенные moov/trak вне пути moov -> trak -> mdia -> hdlr не учитываются
    EXPECT_TRUE(
        isAudioOnlyMp4(file(moov(box("udta", moov(trak("vide")) + trak("vide")) + trak("soun")))));
    EXPECT_TRUE(isAudioOnlyMp4(kFtyp + box("free", moov(trak("vide"))) + moov(trak("soun"))));
}

TEST(IsAudioOnlyMp4Test, IgnoresHdlrOutsideMdia)
{
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", hdlr("soun"))))));
}

TEST(IsAudioOnlyMp4Test, RejectsDeeplyNestedTrakChainWithoutCrash)
{
    std::string nested = box("free");
    for (int i = 0; i < 10000; ++i)
    {
        nested = box("trak", nested);
    }
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(nested))));
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(nested + trak("soun")))));
}

TEST(IsAudioOnlyMp4Test, RejectsDeeplyNestedMdiaChainWithoutCrash)
{
    std::string nested = box("free");
    for (int i = 0; i < 10000; ++i)
    {
        nested = box("mdia", nested);
    }
    EXPECT_FALSE(isAudioOnlyMp4(file(moov(box("trak", nested)))));
}

TEST(IsAudioOnlyMp4Test, RejectsRealVideoAndImageSamples)
{
    EXPECT_FALSE(isAudioOnlyMp4(readSample("video.mp4")));
    EXPECT_FALSE(isAudioOnlyMp4(readSample("video.mov")));
    EXPECT_FALSE(isAudioOnlyMp4(readSample("image.png")));
}

TEST(IsAudioOnlyMp4Test, AcceptsRealAudioOnlySamples)
{
    EXPECT_TRUE(isAudioOnlyMp4(readSample("audio.m4a")));
    EXPECT_TRUE(isAudioOnlyMp4(readSample("audio-isom.m4a")));
}

TEST(IsAudioOnlyMp4Test, RejectsRealVideoWithSoundSample)
{
    EXPECT_FALSE(isAudioOnlyMp4(readSample("reject-av.mp4")));
}

TEST(IsAudioOnlyMp4Test, RejectsRandomBytes)
{
    std::mt19937 generator(12345);
    std::string noise(1U << 20, '\0');
    for (char &byte : noise)
    {
        byte = static_cast<char>(generator() & 0xFF);
    }
    EXPECT_FALSE(isAudioOnlyMp4(noise));
}
