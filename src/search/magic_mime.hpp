#pragma once

#include <optional>
#include <string>
#include <string_view>

// MIME-тип содержимого по libmagic. nullopt - libmagic не смогла разобрать вход
// (например, сработал её внутренний лимит). Потокобезопасна (вызовы libmagic идут под одним
// замком).
[[nodiscard]] std::optional<std::string> magicMime(std::string_view content);
