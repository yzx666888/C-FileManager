#pragma once

#include "Localization.h"

#include <filesystem>

bool RunDuplicateCleaner(Language language, const std::filesystem::path& root);

