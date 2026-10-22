// Фаззинг разбора MP4 из чужих байтов (libFuzzer + ASan/UBSan): любой вход должен давать
// true/false, без чтения за границей, переполнений и зависаний. Как запустить - docs/code.md
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "search/mp4_tracks.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    static_cast<void>(isAudioOnlyMp4(std::string_view(reinterpret_cast<const char *>(data), size)));
    return 0;
}
