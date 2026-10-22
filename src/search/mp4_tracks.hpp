#pragma once

#include <string_view>

// Разбор ISO/IEC 14496-12 (ISO BMFF) для чужих байтов: audio-only MP4 принимается как M4A.
// Битая или неоднозначная структура -> false.
// true: в единственном moov есть хотя бы одна дорожка soun и нет дорожек vide
[[nodiscard]] bool isAudioOnlyMp4(std::string_view content);
