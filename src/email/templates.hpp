#pragma once

#include <string_view>

// HTML-шаблоны писем из src/email/templates/*.html. CMake встраивает файлы в программу при
// сборке (templates.cpp.in): их не нужно копировать в Docker-образ и искать на диске.
extern const std::string_view kEmailConfirmHtml;
extern const std::string_view kPasswordResetHtml;
