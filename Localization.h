#pragma once

#include <string>

enum class Language {
    Chinese,
    English
};

const char* Tr(Language language, const char* chinese, const char* english);
bool IsYesAnswer(const std::string& text);

