cmake_minimum_required(VERSION 3.28)
project(dejaview-backend LANGUAGES CXX)

# compile_commands.json: по нему clangd узнаёт флаги компиляции и пути к заголовкам
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(Drogon CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# Весь код приложения, кроме main.cpp: его подключают и сервер, и тесты
add_library(dejaview-core STATIC src/config.cpp src/health.cpp)
target_compile_features(dejaview-core PUBLIC cxx_std_23)
target_include_directories(dejaview-core PUBLIC src)
target_link_libraries(dejaview-core PUBLIC Drogon::Drogon)

add_executable(dejaview-backend src/main.cpp)
target_link_libraries(dejaview-backend PRIVATE dejaview-core)

# Модульные тесты. Собрать и запустить одной командой: cmake --build build --target check
enable_testing()
add_executable(dejaview-tests tests/config_test.cpp)
target_link_libraries(dejaview-tests PRIVATE dejaview-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(dejaview-tests)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS dejaview-tests)
