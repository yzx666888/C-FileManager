#include "FileSearch.h"

#include "FileSystemUtils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// 内容搜索每个工作线程一次读取 4 MiB，以较少的 I/O 调用处理大文件。
constexpr std::size_t kSearchBufferSize = 4 * 1024 * 1024;
// 限制搜索线程数量，避免机械硬盘或网络目录因过度并发反而变慢。
constexpr unsigned int kMaximumWorkers = 8;

/**
 * @brief 返回搜索模式的本地化显示名称。
 *
 * 设计：菜单、日志共用此转换，避免两处文字含义不一致。
 * 关键变量：mode 决定名称分支；language 决定中文或英文文本。
 */
const char* SearchModeName(Language language, SearchMode mode) {
    switch (mode) {
    case SearchMode::FileName:
        return Tr(language, "文件名", "File name");
    case SearchMode::FileContent:
        return Tr(language, "文件内容", "File content");
    case SearchMode::FileNameOrContent:
        return Tr(language, "文件名 + 文件内容", "File name + file content");
    }
    return "";
}

/**
 * @brief 判断文件名的 UTF-8 表示是否包含目标文本。
 *
 * 设计：只比较文件名，不把父目录名误算为命中内容。
 * 关键变量：path.filename() 取得末级名称；target 是用户输入的匹配片段。
 */
bool FileNameMatches(const fs::path& path, const std::string& target) {
    return PathToUtf8(path.filename()).find(target) != std::string::npos;
}

/**
 * @brief 在文件二进制内容中搜索 UTF-8 字节序列，并处理跨读取块的匹配。
 *
 * 设计：每轮保留上一块末尾 overlapSize 个字节到 buffer 开头，
 * 因而目标串即使跨越 4 MiB 边界也不会漏检。文件无法读取时通过 readError 区分失败和未命中。
 * 关键变量：carry 是保留字节数；bytesRead 是本轮新增字节；available 是可搜索总长度；buffer 是复用的读取缓存。
 */
bool FileContentMatches(const fs::path& path, const std::string& target, bool& readError) {
    readError = false;
    if (target.empty()) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        readError = true;
        return false;
    }

    const std::size_t overlapSize = target.size() > 1 ? target.size() - 1 : 0;
    std::vector<char> buffer(kSearchBufferSize + overlapSize);
    std::size_t carry = 0;

    while (input) {
        input.read(
            buffer.data() + carry,
            static_cast<std::streamsize>(kSearchBufferSize));
        const std::size_t bytesRead = static_cast<std::size_t>(input.gcount());
        const std::size_t available = carry + bytesRead;

        if (available >= target.size()) {
            const auto found = std::search(
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(available),
                target.begin(),
                target.end());
            if (found != buffer.begin() + static_cast<std::ptrdiff_t>(available)) {
                return true;
            }
        }

        if (bytesRead == 0) {
            break;
        }

        carry = std::min(overlapSize, available);
        if (carry > 0) {
            std::move(
                buffer.begin() + static_cast<std::ptrdiff_t>(available - carry),
                buffer.begin() + static_cast<std::ptrdiff_t>(available),
                buffer.begin());
        }
    }

    if (input.bad()) {
        readError = true;
    }
    return false;
}

} // namespace

/**
 * @brief 执行一次完整的递归文件搜索，并可选择把命中文件复制到结果目录。
 *
 * 设计：先收集文件以获得总大小，再使用受限线程池并发搜索；所有输出、日志和复制操作
 * 通过 outputMutex 串行化，保证控制台文字、唯一文件名和日志记录不会相互交错。
 * 关键变量：enumeration 保存待检文件和扫描错误；totalBytes/processedBytes 用于进度；
 * nextIndex 是工作线程领取任务的原子索引；matchCount/readErrorCount 是统计计数；
 * resultDirectory/logPath 分别是可选复制根目录和结果日志位置。
 */
bool RunFileSearch(
    Language language,
    const fs::path& root,
    const SearchOptions& options) {
    if (!IsDirectory(root)) {
        std::cout << Tr(language,
            "错误：搜索路径不是有效目录。\n",
            "Error: the search path is not a valid directory.\n");
        return false;
    }

    std::cout << Tr(language,
        "正在扫描目录并统计文件……\n",
        "Scanning the directory and collecting file information...\n");
    FileEnumerationResult enumeration = EnumerateRegularFiles(root);

    std::uintmax_t totalBytes = 0;
    for (const FileEntry& file : enumeration.files) {
        totalBytes += file.size;
    }

    fs::path resultDirectory;
    if (options.copyMatches) {
        const std::string prefix = language == Language::English ? "SearchResults_" : "查找结果_";
        resultDirectory = fs::current_path() / PathFromUtf8(prefix + MakeTimestamp());
        std::error_code ec;
        fs::create_directories(resultDirectory, ec);
        if (ec) {
            std::cout << Tr(language,
                "错误：无法创建搜索结果目录：",
                "Error: failed to create the search result directory: ")
                      << ec.message() << '\n';
            return false;
        }
    }

    const fs::path logPath = options.copyMatches
        ? resultDirectory / L"search_results.txt"
        : fs::current_path() / PathFromUtf8("search_results_" + MakeTimestamp() + ".txt");
    std::ofstream log(logPath, std::ios::binary);
    if (log) {
        log << Tr(language, "搜索目录：", "Search directory: ") << PathToUtf8(root) << '\n';
        log << Tr(language, "搜索方式：", "Search mode: ") << SearchModeName(language, options.mode) << '\n';
        log << Tr(language, "搜索内容：", "Search target: ") << options.target << "\n\n";
    }

    std::cout << Tr(language, "文件数量：", "Files: ") << enumeration.files.size() << '\n';
    std::cout << Tr(language, "总大小：", "Total size: ")
              << static_cast<double>(totalBytes) / (1024.0 * 1024.0) << " MB\n";
    if (!enumeration.errors.empty()) {
        std::cout << Tr(language, "扫描警告数量：", "Scan warnings: ")
                  << enumeration.errors.size() << '\n';
    }

    // 多线程通过此索引领取不同文件，避免重复扫描。
    std::atomic<std::size_t> nextIndex{ 0 };
    // 已完成文件的总字节数，仅用于显示整体进度。
    std::atomic<std::uintmax_t> processedBytes{ 0 };
    // 命中数和读取失败数可由多个工作线程同时更新。
    std::atomic<std::size_t> matchCount{ 0 };
    std::atomic<std::size_t> readErrorCount{ 0 };
    // 仅在 outputMutex 保护下访问，保证进度输出单调递增。
    int lastPercent = -1;
    std::mutex outputMutex;

    // 根据硬件并发度选线程数，并限制到文件数和 kMaximumWorkers 范围内。
    unsigned int workerCount = std::thread::hardware_concurrency();
    workerCount = std::max(1U, std::min(kMaximumWorkers, workerCount == 0 ? 1U : workerCount));
    workerCount = std::min(workerCount,
        static_cast<unsigned int>(std::max<std::size_t>(1, enumeration.files.size())));

    // 每个工作线程循环领取文件、匹配、必要时复制，并安全地更新共享统计数据。
    auto worker = [&]() {
        while (true) {
            const std::size_t index = nextIndex.fetch_add(1);
            if (index >= enumeration.files.size()) {
                return;
            }

            // file 是当前线程独占处理的文件记录；matched/readError 分别记录匹配和读取状态。
            const FileEntry& file = enumeration.files[index];
            bool matched = false;
            bool readError = false;

            if (options.mode == SearchMode::FileName || options.mode == SearchMode::FileNameOrContent) {
                matched = FileNameMatches(file.path, options.target);
            }
            if (!matched &&
                (options.mode == SearchMode::FileContent || options.mode == SearchMode::FileNameOrContent)) {
                matched = FileContentMatches(file.path, options.target, readError);
            }

            if (readError) {
                readErrorCount.fetch_add(1);
            }

            if (matched) {
                std::lock_guard<std::mutex> lock(outputMutex);
                ++matchCount;
                // copyError 只在复制失败时写入日志；destination 为空表示不复制或复制失败。
                std::string copyError;
                fs::path destination;

                if (options.copyMatches) {
                    if (options.preserveDirectoryStructure) {
                        std::error_code relativeError;
                        const fs::path relative = fs::relative(file.path, root, relativeError);
                        if (!relativeError) {
                            destination = resultDirectory / relative;
                        }
                    } else {
                        destination = MakeUniqueDestination(resultDirectory / file.path.filename());
                    }

                    if (!destination.empty() &&
                        !CopyFileCreatingParents(file.path, destination, copyError)) {
                        destination.clear();
                    }
                }

                std::cout << "\n" << Tr(language, "找到：", "Found: ")
                          << PathToUtf8(file.path) << '\n';
                if (log) {
                    log << PathToUtf8(file.path);
                    if (!destination.empty()) {
                        log << " -> " << PathToUtf8(destination);
                    } else if (!copyError.empty()) {
                        log << " -> [COPY FAILED] " << copyError;
                    }
                    log << '\n';
                }
            }

            // completed 是本次累加后的总进度，用于计算百分比。
            const std::uintmax_t completed = processedBytes.fetch_add(file.size) + file.size;
            const int percent = totalBytes == 0
                ? 100
                : static_cast<int>((completed * 100) / totalBytes);
            {
                std::lock_guard<std::mutex> lock(outputMutex);
                if (percent > lastPercent) {
                    lastPercent = percent;
                std::cout << "\r" << Tr(language, "进度：", "Progress: ")
                          << std::min(100, percent) << "%   " << std::flush;
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    for (std::thread& thread : workers) {
        thread.join();
    }

    std::cout << "\n" << Tr(language, "搜索完成。匹配文件：", "Search completed. Matching files: ")
              << matchCount.load() << '\n';
    if (readErrorCount.load() != 0) {
        std::cout << Tr(language, "读取失败：", "Read failures: ")
                  << readErrorCount.load() << '\n';
    }
    std::cout << Tr(language, "结果清单：", "Result list: ") << PathToUtf8(logPath) << '\n';
    if (options.copyMatches) {
        std::cout << Tr(language, "复制结果目录：", "Copied results directory: ")
                  << PathToUtf8(resultDirectory) << '\n';
    }
    return true;
}
