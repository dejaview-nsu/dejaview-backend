cmake_minimum_required(VERSION 3.28)
project(dejaview-backend LANGUAGES CXX)

# compile_commands.json: по нему clangd узнаёт флаги компиляции и пути к заголовкам
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(Drogon CONFIG REQUIRED)

add_executable(dejaview-backend src/main.cpp src/config.cpp src/health.cpp)
target_compile_features(dejaview-backend PRIVATE cxx_std_23)
target_link_libraries(dejaview-backend PRIVATE Drogon::Drogon)
