#include "DuplicateCleaner.h"

#include "FileSystemUtils.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

/**
 * @brief 将单个文件送入 Windows 回收站，而非直接永久删除。
 *
 * 设计：使用 SHFileOperationW 和 FOF_ALLOWUNDO 保留恢复机会；pFrom 必须是双空字符结尾的路径列表。
 * 关键变量：absolutePath 是绝对路径；from 是符合 Win32 协议的双空结尾缓冲区；
 * operation 保存回收站请求；result 和 error 分别记录 API 结果与失败说明。
 */
bool MoveFileToRecycleBin(const fs::path& path, std::string& error) {
    const fs::path absolutePath = fs::absolute(path);
    std::wstring from = absolutePath.native();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int result = SHFileOperationW(&operation);
    if (result == 0 && !operation.fAnyOperationsAborted) {
        return true;
    }

    error = "SHFileOperationW failed with code " + std::to_string(result);
    return false;
}

} // namespace

/**
 * @brief 递归找出二进制完全相同的文件，并把每组中后发现的副本移入回收站。
 *
 * 设计：先按文件大小分组，只有同大小组才进行昂贵的逐字节比较；每组的 representatives
 * 保存已保留的代表文件，确保不因仅同名或同大小而误删。所有实际删除均走回收站函数。
 * 关键变量：enumeration 是扫描结果；groups 按大小聚合候选；representatives 是每组保留文件；
 * recycledCount/recycleFailureCount/compareErrorCount/recycledBytes 用于最终统计与日志。
 */
bool RunDuplicateCleaner(Language language, const fs::path& root) {
    if (!IsDirectory(root)) {
        std::cout << Tr(language,
            "错误：重复文件清理路径不是有效目录。\n",
            "Error: the duplicate-cleanup path is not a valid directory.\n");
        return false;
    }

    const fs::path logPath = fs::current_path() /
        PathFromUtf8("duplicate_remove_results_" + MakeTimestamp() + ".txt");
    std::ofstream log(logPath, std::ios::binary);
    if (log) {
        log << Tr(language, "重复文件清理目录：", "Duplicate cleanup directory: ")
            << PathToUtf8(root) << '\n';
        log << Tr(language,
            "安全规则：每组保留第一次发现的文件，后续二进制完全相同的文件移入回收站。\n\n",
            "Safety rule: keep the first file in each group and move later binary-identical files to the Recycle Bin.\n\n");
    }

    std::cout << Tr(language,
        "正在收集目录中的文件……\n",
        "Collecting files under the directory...\n");
    FileEnumerationResult enumeration = EnumerateRegularFiles(root);
    std::cout << Tr(language, "已收集文件：", "Files collected: ")
              << enumeration.files.size() << '\n';

    // 以文件大小作为快速筛选键，大小不同的文件无需再读取内容比较。
    std::map<std::uintmax_t, std::vector<fs::path>> groups;
    for (const FileEntry& entry : enumeration.files) {
        groups[entry.size].push_back(entry.path);
    }

    // 这些统计分别表示成功回收、回收失败、比较失败及成功回收的总字节数。
    std::size_t recycledCount = 0;
    std::size_t recycleFailureCount = 0;
    std::size_t compareErrorCount = 0;
    std::uintmax_t recycledBytes = 0;

    for (const auto& [fileSize, paths] : groups) {
        if (paths.size() < 2) {
            continue;
        }

        // 当前大小组中已确认保留的文件；新文件只需与这些代表逐一比较。
        std::vector<fs::path> representatives;
        for (const fs::path& candidate : paths) {
            // duplicateFound 为真时 candidate 已与一个代表相同，不能再加入代表列表。
            bool duplicateFound = false;

            for (const fs::path& kept : representatives) {
                std::string compareError;
                const BinaryComparison comparison =
                    CompareFilesBinary(kept, candidate, &compareError);
                if (comparison == BinaryComparison::Error) {
                    ++compareErrorCount;
                    if (log) {
                        log << "[COMPARE FAILED] " << PathToUtf8(kept)
                            << " <-> " << PathToUtf8(candidate)
                            << ": " << compareError << '\n';
                    }
                    continue;
                }
                if (comparison != BinaryComparison::Equal) {
                    continue;
                }

                duplicateFound = true;
                std::string recycleError;
                std::cout << Tr(language, "发现重复文件：\n", "Duplicate file found:\n")
                          << "  " << PathToUtf8(kept) << "\n  "
                          << PathToUtf8(candidate) << '\n';

                if (MoveFileToRecycleBin(candidate, recycleError)) {
                    ++recycledCount;
                    recycledBytes += fileSize;
                    if (log) {
                        log << "[KEEP] " << PathToUtf8(kept) << '\n';
                        log << "[RECYCLE] " << PathToUtf8(candidate) << "\n\n";
                    }
                } else {
                    ++recycleFailureCount;
                    if (log) {
                        log << "[KEEP] " << PathToUtf8(kept) << '\n';
                        log << "[RECYCLE FAILED] " << PathToUtf8(candidate)
                            << ": " << recycleError << "\n\n";
                    }
                }
                break;
            }

            if (!duplicateFound) {
                representatives.push_back(candidate);
            }
        }
    }

    std::cout << Tr(language, "重复文件清理完成。\n", "Duplicate cleanup completed.\n");
    std::cout << Tr(language, "移入回收站：", "Moved to Recycle Bin: ") << recycledCount << '\n';
    std::cout << Tr(language, "回收站操作失败：", "Recycle Bin failures: ")
              << recycleFailureCount << '\n';
    std::cout << Tr(language, "比较失败：", "Comparison failures: ")
              << compareErrorCount << '\n';
    std::cout << Tr(language, "已回收大小：", "Recycled size: ")
              << static_cast<double>(recycledBytes) / (1024.0 * 1024.0) << " MB\n";
    std::cout << Tr(language, "日志文件：", "Log file: ") << PathToUtf8(logPath) << '\n';

    if (log) {
        log << "\n" << Tr(language, "移入回收站：", "Moved to Recycle Bin: ") << recycledCount << '\n';
        log << Tr(language, "回收站操作失败：", "Recycle Bin failures: ") << recycleFailureCount << '\n';
        log << Tr(language, "比较失败：", "Comparison failures: ") << compareErrorCount << '\n';
    }
    return recycleFailureCount == 0 && compareErrorCount == 0;
}
