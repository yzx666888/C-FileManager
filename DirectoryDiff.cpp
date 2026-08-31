#include "DirectoryDiff.h"
//引入本模块自己的声明，例如 DirectoryDiffOptions 和 RunDirectoryDiff(...)。
//双引号表示优先从项目目录查找

#include "FileSystemUtils.h"
//引入公共文件操作工具，例如目录扫描、二进制比较、复制文件、UTF-8 路径转换等。DirectoryDiff.cpp 依靠它来比较 A/B 文件并复制到 C

#include <cstddef>
//C++ 标准库的基础类型定义，主要提供 std::size_t、std::ptrdiff_t 等。
//这里用于文件数量等无符号计数。
#include <fstream>
//文件读写流,这里的 std::ofstream 用它创建差异提取日志文件。
#include <iostream>
//控制台输入输出流,这里的 std::cout 用于显示扫描、复制、统计和错误信息。
#include <string>

namespace {
/*
namespace（命名空间）是 C++ 用来给名字“分组”和避免重名的机制。
例如：
namespace FileTools {
    void CopyFile();
}
调用时写：
FileTools::CopyFile();
即 namespace名::内部函数名
这里的 :: 表示“属于哪个命名空间”。

比如你的程序和某个库都定义了 CopyFile()，可以分别放在不同命名空间：
namespace MyFileManager {
    void CopyFile();
}

namespace OtherLibrary {
    void CopyFile();
}
调用时就不会混淆：
MyFileManager::CopyFile();
OtherLibrary::CopyFile();


匿名命名空间：
没有名字的命名空间，例如
namespace {
    void HelperFunction() {
    }

    int internalValue = 0;
}

作用是：让其中的函数、变量、类型只能在当前这个 .cpp 文件里使用。

例如 DirectoryDiff.cpp：
namespace {
    bool ValidateDirectories(...) {
        // 只给 DirectoryDiff.cpp 内部使用
    }
}
其他文件即使写：
ValidateDirectories(...);
也无法调用，因为这个名字不会暴露到项目其他编译单元。
它的意义是把“内部辅助实现”隐藏起来：
- 对外公开的函数写在 .h 中，例如 RunDirectoryDiff(...)。
- 只为本文件服务的辅助函数放在匿名命名空间中，例如路径校验、日志写入。
- 可以避免不同 .cpp 文件里的辅助函数重名。
它和 static 修饰的全局函数用途接近：
static void HelperFunction();
但现代 C++ 通常更推荐匿名命名空间，因为它不仅能限制函数，也能限制变量、结构体、常量等名字。
*/
struct DirectoryDiffStats {
    // identical：A/B 同路径且内容相同；added/updated：已从 B 成功复制；removed：仅 A 存在；failed：任意处理失败。
    std::size_t identical = 0;
    std::size_t added = 0;
    std::size_t updated = 0;
    std::size_t removed = 0;
    std::size_t failed = 0;
    std::uintmax_t copiedBytes = 0;
};

/**
 * @brief 校验目录差异任务的 A、B、C 关系，阻止危险或含义不明确的组合。
 *
 * 设计：A 与 B 必须是互不包含的有效目录；C 不能等于或位于输入目录内，
 * 从而避免扫描自身输出、覆盖源文件或构造循环。具体“C 必须为空”由后续函数检查。
 * 关键变量：options 保存三个输入路径；error 返回本地化拒绝原因；language 选择错误文字语言。
 */
bool ValidateDirectories(
    Language language,
    const DirectoryDiffOptions& options,
    std::string& error) {
    if (!IsDirectory(options.oldDirectory)) {
        error = Tr(language,
            "目录 A（旧版本）不是有效目录。",
            "Directory A (old version) is not a valid directory.");
        return false;
    }
    if (!IsDirectory(options.newDirectory)) {
        error = Tr(language,
            "目录 B（新版本）不是有效目录。",
            "Directory B (new version) is not a valid directory.");
        return false;
    }
    if (AreSamePath(options.oldDirectory, options.newDirectory)) {
        error = Tr(language,
            "目录 A 和目录 B 不能是同一个目录。",
            "Directories A and B must be different.");
        return false;
    }
    if (IsSameOrSubPath(options.oldDirectory, options.newDirectory) ||
        IsSameOrSubPath(options.newDirectory, options.oldDirectory)) {
        error = Tr(language,
            "目录 A 和目录 B 不能互相包含。",
            "Directories A and B must not contain one another.");
        return false;
    }
    if (IsSameOrSubPath(options.outputDirectory, options.oldDirectory) ||
        IsSameOrSubPath(options.outputDirectory, options.newDirectory)) {
        error = Tr(language,
            "输出目录 C 不能等于或位于目录 A/B 内部。",
            "Output directory C must not be the same as or inside directory A/B.");
        return false;
    }
    return true;
}

/**
 * @brief 向差异日志写入一条统一格式的文件处理记录。
 *
 * 设计：日志流不可用时静默跳过，不能因此中断实际文件提取；相对路径使日志可直接对应 C 中布局。
 * 关键变量：category 是事件类型；relativePath 是相对 A/B 的路径；detail 保存可选失败详情。
 */
void LogEntry(
    std::ofstream& log,
    const char* category,
    const fs::path& relativePath,
    const std::string& detail = {}) {
    if (!log) {
        return;
    }
    log << '[' << category << "] " << PathToUtf8(relativePath);
    if (!detail.empty()) {
        log << ": " << detail;
    }
    log << '\n';
}

} // namespace

/**
 * @brief 将新目录 B 与旧目录 A 做递归二进制比较，把新增和已更新文件复制到目录 C。
 *
 * 设计：以 B 的文件为主视角；B 中不存在于 A 的文件视为新增，存在但 CompareFilesBinary
 * 判定不同的文件视为更新，二者均保留相对路径复制到 C。随后单独扫描 A 统计删除项，
 * 但按更新包语义不把删除项写入 C。扫描、比较和复制错误累积到 failed 并记录日志。
 * 关键变量：validationError 承接校验失败原因；newFiles/oldFiles 是两边扫描结果；
 * stats 汇总相同、新增、更新、删除和失败数量；relative 用于映射同路径文件；
 * oldFile/outputFile 是 A 中对照文件和 C 中目标文件；shouldCopy/category 决定当前 B 文件的处理方式。
 */
bool RunDirectoryDiff(Language language, const DirectoryDiffOptions& options) {
    std::string validationError;
    if (!ValidateDirectories(language, options, validationError)) {
        std::cout << Tr(language, "错误：", "Error: ") << validationError << '\n';
        return false;
    }

    if (!PrepareEmptyOutputDirectory(options.outputDirectory, validationError)) {
        std::cout << Tr(language, "错误：无法使用输出目录 C：", "Error: cannot use output directory C: ")
                  << validationError << '\n';
        return false;
    }

    const fs::path logPath = fs::current_path() /
        PathFromUtf8("directory_diff_results_" + MakeTimestamp() + ".txt");
    std::ofstream log(logPath, std::ios::binary);
    if (log) {
        log << Tr(language, "目录 A（旧版本）：", "Directory A (old version): ")
            << PathToUtf8(options.oldDirectory) << '\n';
        log << Tr(language, "目录 B（新版本）：", "Directory B (new version): ")
            << PathToUtf8(options.newDirectory) << '\n';
        log << Tr(language, "目录 C（差异输出）：", "Directory C (difference output): ")
            << PathToUtf8(options.outputDirectory) << "\n\n";
    }

    std::cout << Tr(language,
        "正在扫描目录 B，并与目录 A 逐文件进行二进制比较……\n",
        "Scanning directory B and comparing each file with directory A...\n");

    // stats 在整个任务期间累计，可同时用于控制台摘要和日志摘要。
    DirectoryDiffStats stats;
    FileEnumerationResult newFiles = EnumerateRegularFiles(options.newDirectory);
    stats.failed += newFiles.errors.size();
    for (const std::string& scanError : newFiles.errors) {
        if (log) {
            log << "[SCAN FAILED] " << scanError << '\n';
        }
    }

    for (const FileEntry& newFile : newFiles.files) {
        std::error_code ec;
        const fs::path relative = fs::relative(newFile.path, options.newDirectory, ec);
        if (ec || relative.empty()) {
            ++stats.failed;
            LogEntry(log, "RELATIVE PATH FAILED", newFile.path, ec.message());
            continue;
        }

        // oldFile 用于内容对照；outputFile 保持 B 相对目录结构写入 C。
        const fs::path oldFile = options.oldDirectory / relative;
        const fs::path outputFile = options.outputDirectory / relative;
        // shouldCopy 为真时才执行写入；category 用于区分新增、更新和类型变化日志。
        bool shouldCopy = false;
        const char* category = nullptr;

        ec.clear();
        const bool oldExists = fs::exists(oldFile, ec);
        if (ec) {
            ++stats.failed;
            LogEntry(log, "STATUS FAILED", relative, ec.message());
            continue;
        }

        if (!oldExists) {
            shouldCopy = true;
            category = "ADDED";
        } else {
            ec.clear();
            if (!fs::is_regular_file(oldFile, ec) || ec) {
                shouldCopy = true;
                category = "UPDATED TYPE";
            } else {
                std::string compareError;
                const BinaryComparison comparison =
                    CompareFilesBinary(oldFile, newFile.path, &compareError);
                if (comparison == BinaryComparison::Error) {
                    ++stats.failed;
                    LogEntry(log, "COMPARE FAILED", relative, compareError);
                    continue;
                }
                if (comparison == BinaryComparison::Equal) {
                    ++stats.identical;
                    LogEntry(log, "IDENTICAL", relative);
                    continue;
                }
                shouldCopy = true;
                category = "UPDATED";
            }
        }

        if (shouldCopy) {
            std::string copyError;
            if (!CopyFileCreatingParents(newFile.path, outputFile, copyError)) {
                ++stats.failed;
                LogEntry(log, "COPY FAILED", relative, copyError);
                continue;
            }

            if (std::string(category) == "ADDED") {
                ++stats.added;
            } else {
                ++stats.updated;
            }
            stats.copiedBytes += newFile.size;
            LogEntry(log, category, relative);
            std::cout << "[" << category << "] " << PathToUtf8(relative) << '\n';
        }
    }

    std::cout << Tr(language,
        "正在检查仅存在于目录 A 的已删除文件……\n",
        "Checking for removed files that exist only in directory A...\n");
    FileEnumerationResult oldFiles = EnumerateRegularFiles(options.oldDirectory);
    stats.failed += oldFiles.errors.size();
    for (const std::string& scanError : oldFiles.errors) {
        if (log) {
            log << "[SCAN FAILED] " << scanError << '\n';
        }
    }

    for (const FileEntry& oldFile : oldFiles.files) {
        std::error_code ec;
        const fs::path relative = fs::relative(oldFile.path, options.oldDirectory, ec);
        if (ec || relative.empty()) {
            ++stats.failed;
            LogEntry(log, "RELATIVE PATH FAILED", oldFile.path, ec.message());
            continue;
        }

        const fs::path correspondingNewFile = options.newDirectory / relative;
        ec.clear();
        if (!fs::exists(correspondingNewFile, ec) && !ec) {
            ++stats.removed;
            LogEntry(log, "REMOVED (NOT COPIED)", relative);
        } else if (ec) {
            ++stats.failed;
            LogEntry(log, "STATUS FAILED", relative, ec.message());
        }
    }

    std::cout << "\n" << Tr(language, "差异提取完成。\n", "Difference extraction completed.\n");
    std::cout << Tr(language, "二进制相同：", "Binary-identical: ") << stats.identical << '\n';
    std::cout << Tr(language, "新增并已复制：", "Added and copied: ") << stats.added << '\n';
    std::cout << Tr(language, "更新并已复制：", "Updated and copied: ") << stats.updated << '\n';
    std::cout << Tr(language, "新版已删除（未复制）：", "Removed in new version (not copied): ")
              << stats.removed << '\n';
    std::cout << Tr(language, "失败：", "Failures: ") << stats.failed << '\n';
    std::cout << Tr(language, "复制总大小：", "Total copied size: ")
              << static_cast<double>(stats.copiedBytes) / (1024.0 * 1024.0) << " MB\n";
    std::cout << Tr(language, "输出目录：", "Output directory: ")
              << PathToUtf8(options.outputDirectory) << '\n';
    std::cout << Tr(language, "日志文件：", "Log file: ") << PathToUtf8(logPath) << '\n';

    if (log) {
        log << "\n" << Tr(language, "二进制相同：", "Binary-identical: ") << stats.identical << '\n';
        log << Tr(language, "新增并已复制：", "Added and copied: ") << stats.added << '\n';
        log << Tr(language, "更新并已复制：", "Updated and copied: ") << stats.updated << '\n';
        log << Tr(language, "新版已删除（未复制）：", "Removed in new version (not copied): ")
            << stats.removed << '\n';
        log << Tr(language, "失败：", "Failures: ") << stats.failed << '\n';
    }

    return stats.failed == 0;
}
