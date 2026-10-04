#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

// Проверка токена Yandex SmartCaptcha из виджета на форме входа (#17149 п. 2.5): запрос к
// smartcaptcha.yandexcloud.net/validate с серверным ключом.
// Если сам сервис недоступен (сеть, таймаут, не 200), проверка считается пройденной - так
// советует документация SmartCaptcha: сбой у Яндекса не должен закрывать вход, а от подбора всё
// равно защищает блокировка после 5 неудачных попыток.
drogon::Task<bool> verifyCaptcha(std::string serverKey, std::string token, std::string ip);
