cmake_minimum_required(VERSION 3.28)
project(dejaview-backend LANGUAGES CXX)

add_executable(dejaview-backend src/main.cpp)
target_compile_features(dejaview-backend PRIVATE cxx_std_23)
