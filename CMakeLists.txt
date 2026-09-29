cmake_minimum_required(VERSION 3.28)
project(dejaview-backend LANGUAGES CXX)

find_package(Drogon CONFIG REQUIRED)

add_executable(dejaview-backend src/main.cpp src/config.cpp)
target_compile_features(dejaview-backend PRIVATE cxx_std_23)
target_link_libraries(dejaview-backend PRIVATE Drogon::Drogon)
