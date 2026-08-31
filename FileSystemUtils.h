#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

struct FileEntry {
    fs::path path;
    std::uintmax_t size = 0;
};

struct FileEnumerationResult {
    std::vector<FileEntry> files;
    std::vector<std::string> errors;
};

enum class BinaryComparison {
    Equal,
    Different,
    Error
};

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
fs::path PathFromUtf8(const std::string& text);
std::string PathToUtf8(const fs::path& path);
std::string MakeTimestamp();

bool IsDirectory(const fs::path& path);
bool IsDirectoryEmpty(const fs::path& path, std::string& error);
bool IsSameOrSubPath(const fs::path& candidate, const fs::path& parent);
bool AreSamePath(const fs::path& left, const fs::path& right);

FileEnumerationResult EnumerateRegularFiles(const fs::path& root);
BinaryComparison CompareFilesBinary(
    const fs::path& left,
    const fs::path& right,
    std::string* error = nullptr);

bool PrepareEmptyOutputDirectory(const fs::path& output, std::string& error);
bool CopyFileCreatingParents(
    const fs::path& source,
    const fs::path& destination,
    std::string& error);
fs::path MakeUniqueDestination(const fs::path& desired);

