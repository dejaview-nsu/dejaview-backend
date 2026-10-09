#pragma once

#include <cstddef>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

// Образцы форматов из tests/data/search/; путь задаёт CMake (DEJAVIEW_TEST_DATA_DIR)

// Отсутствующий образец - сбой теста, а не пропуск
inline std::string readSample(const std::string &name)
{
    const std::string path = std::string(DEJAVIEW_TEST_DATA_DIR) + "/search/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("sample not found: " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Образец, дополненный до total байт: заголовок остаётся настоящим, размер - нужным
inline std::string padded(std::string sample, std::size_t total)
{
    sample.resize(total, 'x');
    return sample;
}
