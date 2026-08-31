#pragma once

#include "Localization.h"

#include <filesystem>
#include <string>

enum class SearchMode {
    FileName,
    FileContent,
    FileNameOrContent
};

struct SearchOptions {
    SearchMode mode = SearchMode::FileContent;
    std::string target;
    bool copyMatches = false;
    bool preserveDirectoryStructure = false;
};

bool RunFileSearch(
    Language language,
    const std::filesystem::path& root,
    const SearchOptions& options);

