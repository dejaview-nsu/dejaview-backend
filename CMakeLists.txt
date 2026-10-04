cmake_minimum_required(VERSION 3.28)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
project(dejaview-backend LANGUAGES CXX)

# compile_commands.json: по нему clangd узнаёт флаги компиляции и пути к заголовкам
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Drogon CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(PostgreSQL CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

# HTML-шаблоны писем встраиваются в программу: CMake вставляет содержимое файла в строковый
# литерал R"html(...)html" (src/email/templates.cpp.in). CMAKE_CONFIGURE_DEPENDS - правка
# шаблона сама перезапускает cmake при следующей сборке
set(EMAIL_CONFIRM_TEMPLATE ${CMAKE_SOURCE_DIR}/src/email/templates/email_confirm.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EMAIL_CONFIRM_TEMPLATE})
file(READ ${EMAIL_CONFIRM_TEMPLATE} EMAIL_CONFIRM_HTML)
configure_file(src/email/templates.cpp.in ${CMAKE_BINARY_DIR}/generated/email_templates.cpp
               @ONLY)

# Весь код приложения, кроме main.cpp: его подключают и сервер, и тесты
add_library(dejaview-core STATIC
    src/background.cpp
    src/config.cpp
    src/email/message.cpp
    src/email/outbox.cpp
    ${CMAKE_BINARY_DIR}/generated/email_templates.cpp
    src/error_response.cpp
    src/health.cpp
    src/auth/captcha.cpp
    src/auth/crypto.cpp
    src/auth/http_common.cpp
    src/auth/login.cpp
    src/auth/login_rules.cpp
    src/auth/oidc.cpp
    src/auth/oidc_profile.cpp
    src/auth/registration.cpp
    src/auth/security_log.cpp
    src/auth/session.cpp
    src/auth/validation.cpp)
target_compile_features(dejaview-core PUBLIC cxx_std_23)
target_include_directories(dejaview-core PUBLIC src)
target_link_libraries(dejaview-core PUBLIC Drogon::Drogon libsodium::libsodium CURL::libcurl)

add_executable(dejaview-backend src/main.cpp)
target_link_libraries(dejaview-backend PRIVATE dejaview-core)

# Модульные тесты. Собрать и запустить одной командой: cmake --build build --target check
enable_testing()
add_executable(dejaview-tests
    tests/config_test.cpp
    tests/error_response_test.cpp
    tests/auth/crypto_test.cpp
    tests/auth/login_rules_test.cpp
    tests/auth/oidc_profile_test.cpp
    tests/auth/session_test.cpp
    tests/auth/validation_test.cpp
    tests/email/message_test.cpp
    # интеграционные: с БД из TEST_DATABASE_URL, без неё пропускаются (tests/integration/db_test.hpp)
    tests/integration/db_test.cpp
    tests/integration/background_test.cpp
    tests/integration/guest_test.cpp
    tests/integration/login_test.cpp
    tests/integration/oidc_test.cpp
    tests/integration/registration_test.cpp)
target_include_directories(dejaview-tests PRIVATE tests)
# миграции интеграционные тесты применяют сами - через libpq, как dbmate
target_compile_definitions(dejaview-tests PRIVATE
    DEJAVIEW_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations")
target_link_libraries(dejaview-tests PRIVATE dejaview-core PostgreSQL::PostgreSQL GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(dejaview-tests)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS dejaview-tests)

# Покрытие тестами (#17098 п. 1) - только в Clang-сборке: gcov из GCC не видит тел корутин
# (Task<...>), а это почти все обработчики. Как собрать - docs/code.md. В Debug код приложения
# собирается с --coverage и при работе записывает, какие строки выполнились. Отчёт:
# cmake --build build-coverage --target coverage, по строкам - build-coverage/coverage/index.html
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(dejaview-core PRIVATE $<$<CONFIG:Debug>:--coverage>)
    target_link_options(dejaview-core PUBLIC $<$<CONFIG:Debug>:--coverage>)
    # gcovr читает счётчики Clang через llvm-cov той же версии, что и компилятор
    string(REGEX MATCH "^[0-9]+" CLANG_MAJOR ${CMAKE_CXX_COMPILER_VERSION})
    set(GCOV "llvm-cov-${CLANG_MAJOR} gcov")
    # Пороги - #17098: весь src/ - 70% (п. 1.1), авторизация src/auth/ - 80% (п. 1.2). Ниже -
    # команда завершается ошибкой. Интеграционные тесты нужны для обоих: задать TEST_DATABASE_URL.
    add_custom_target(coverage
        # счётчики копятся между запусками (и от ручного запуска сервера) - обнуляем
        COMMAND find . -name "*.gcda" -delete
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E make_directory coverage
        COMMAND gcovr --root ${CMAKE_SOURCE_DIR} --filter ${CMAKE_SOURCE_DIR}/src/
                --gcov-executable ${GCOV} --txt --html-details coverage/index.html
                --fail-under-line 70 .
        COMMAND gcovr --root ${CMAKE_SOURCE_DIR} --filter ${CMAKE_SOURCE_DIR}/src/auth/
                --gcov-executable ${GCOV} --txt-summary --fail-under-line 80 .
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS dejaview-tests
        VERBATIM)
else()
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E echo "Покрытие считается в Clang-сборке: см. docs/code.md"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()
