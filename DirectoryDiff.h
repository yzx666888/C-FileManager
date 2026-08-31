#pragma once

#include "Localization.h"

#include <filesystem>

struct DirectoryDiffOptions {
    std::filesystem::path oldDirectory;
    std::filesystem::path newDirectory;
    std::filesystem::path outputDirectory;
};

bool RunDirectoryDiff(Language language, const DirectoryDiffOptions& options);

