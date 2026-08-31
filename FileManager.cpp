#include "FileManager.h"

#include "DirectoryDiff.h"
#include "DuplicateCleaner.h"
#include "FileSearch.h"
#include "FileSystemUtils.h"
#include "Localization.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

/**
 * @brief 从标准输入读取一整行文本，可选地先显示提示语。
 *
 * 设计：所有交互输入统一走此函数，避免格式化输入留下换行符影响下一次读取。
 * 关键变量：prompt 为可选提示文本；line 保存用户原始输入。
 */
std::string ReadLine(const char* prompt = nullptr) {
    if (prompt != nullptr) {
        std::cout << prompt << std::flush;
    }
    std::string line;
    std::getline(std::cin, line);
    return line;
}

/**
 * @brief 删除字符串首尾空白字符，保留中间内容不变。
 *
 * 设计：路径、菜单和确认输入都先规范化，提升粘贴输入的容错性。
 * 关键变量：notSpace 定义非空白判定；text 是原地裁剪并返回的字符串。
 */
std::string Trim(std::string text) {
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

/**
 * @brief 提示用户输入路径，去除首尾空白和一对可选引号后转换为 Unicode 路径。
 *
 * 设计：兼容资源管理器“复制为路径”带来的双引号，并集中保证 UTF-8 控制台输入可访问中文文件名。
 * 关键变量：text 保存裁剪后的输入；language 和两个提示参数选择界面语言。
 */
fs::path ReadPath(Language language, const char* chinesePrompt, const char* englishPrompt) {
    std::string text = Trim(ReadLine(Tr(language, chinesePrompt, englishPrompt)));
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
    }
    return PathFromUtf8(text);
}

/**
 * @brief 在程序结束前等待用户确认，防止双击运行时控制台立即关闭。
 *
 * 设计：读取一整行而非按键，既可在交互终端工作，也可在自动化输入中安全结束。
 * 关键变量：ignored 仅用于接收用户按下 Enter 前输入的内容。
 */
void Pause(Language language) {
    std::cout << "\n" << Tr(language,
        "按 Enter 键退出程序……",
        "Press Enter to exit...") << std::flush;
    std::string ignored;
    std::getline(std::cin, ignored);
}

/**
 * @brief 完成文件搜索的交互式参数收集，并调用搜索模块。
 *
 * 设计：本函数只负责菜单和输入校验，实际递归、多线程搜索与复制由 RunFileSearch 处理。
 * 关键变量：root 是搜索根目录；modeText 决定 SearchMode；options 汇集搜索和复制选项。
 */
bool RunSearchFlow(Language language) {
    const fs::path root = ReadPath(
        language,
        "请输入要搜索的目录路径：",
        "Enter the directory path to search: ");
    if (root.empty() || !IsDirectory(root)) {
        std::cout << Tr(language, "错误：输入的路径不是有效目录。\n", "Error: the path is not a valid directory.\n");
        return false;
    }

    std::cout << "\n" << Tr(language, "请选择查找方式：\n", "Select search mode:\n");
    std::cout << "1. " << Tr(language, "按文件名查找", "Search by file name") << '\n';
    std::cout << "2. " << Tr(language, "按文件内容查找", "Search by file content") << '\n';
    std::cout << "3. " << Tr(language, "按文件名或文件内容查找", "Search by file name or content") << '\n';
    const std::string modeText = Trim(ReadLine("> "));

    SearchOptions options;
    if (!modeText.empty() && modeText.front() == '1') {
        options.mode = SearchMode::FileName;
    } else if (!modeText.empty() && modeText.front() == '3') {
        options.mode = SearchMode::FileNameOrContent;
    } else {
        options.mode = SearchMode::FileContent;
    }

    options.target = ReadLine(Tr(language,
        "请输入要查找的文件名片段或内容：",
        "Enter the file-name fragment or content to find: "));
    if (options.target.empty()) {
        std::cout << Tr(language, "错误：查找内容不能为空。\n", "Error: the search target cannot be empty.\n");
        return false;
    }

    options.copyMatches = IsYesAnswer(ReadLine(Tr(language,
        "是否将找到的文件复制到结果目录？(y/n)：",
        "Copy matching files to a result directory? (y/n): ")));
    if (options.copyMatches) {
        options.preserveDirectoryStructure = IsYesAnswer(ReadLine(Tr(language,
            "是否保持源目录结构？(y=保持 / n=平铺)：",
            "Preserve source directory structure? (y=preserve / n=flatten): ")));
    }

    return RunFileSearch(language, root, options);
}

/**
 * @brief 完成重复文件清理的交互式确认，并调用回收站清理模块。
 *
 * 设计：在执行会改变磁盘状态的操作前明确说明规则并要求确认；具体二进制比较和回收站调用不放在 UI 层。
 * 关键变量：root 是待清理目录；用户确认结果决定是否调用 RunDuplicateCleaner。
 */
bool RunDuplicateFlow(Language language) {
    const fs::path root = ReadPath(
        language,
        "请输入要清理重复文件的目录路径：",
        "Enter the directory path for duplicate cleanup: ");
    if (root.empty() || !IsDirectory(root)) {
        std::cout << Tr(language, "错误：输入的路径不是有效目录。\n", "Error: the path is not a valid directory.\n");
        return false;
    }

    std::cout << Tr(language,
        "程序只会把后续发现的二进制完全相同文件移入回收站，并保留每组的第一个文件。\n",
        "Only later binary-identical files are moved to the Recycle Bin; the first file in each group is kept.\n");
    if (!IsYesAnswer(ReadLine(Tr(language, "确认继续？(y/n)：", "Continue? (y/n): ")))) {
        std::cout << Tr(language, "已取消重复文件清理。\n", "Duplicate cleanup canceled.\n");
        return true;
    }

    return RunDuplicateCleaner(language, root);
}

/**
 * @brief 收集 A（旧版）、B（新版）、C（输出）并发起差异文件提取。
 *
 * 设计：在调用核心模块前展示 A/B/C 的含义和最终路径，再要求确认，
 * 使用户清楚 C 只接收来自 B 的新增或二进制不同文件。
 * 关键变量：options 汇集三个目录；其路径空值在界面层提前拒绝。
 */
bool RunDirectoryDiffFlow(Language language) {
    std::cout << "\n" << Tr(language,
        "目录 A 是旧版本，目录 B 是新版本；目录 C 只输出 B 中新增或二进制不同的文件。\n",
        "Directory A is the old version and B is the new version; C receives only added or binary-different files from B.\n");

    DirectoryDiffOptions options;
    options.oldDirectory = ReadPath(language,
        "请输入目录 A（旧版本）：",
        "Enter directory A (old version): ");
    options.newDirectory = ReadPath(language,
        "请输入目录 B（新版本）：",
        "Enter directory B (new version): ");
    options.outputDirectory = ReadPath(language,
        "请输入目录 C（差异文件输出目录，必须为空或不存在）：",
        "Enter directory C (difference output; must be empty or absent): ");

    if (options.oldDirectory.empty() || options.newDirectory.empty() || options.outputDirectory.empty()) {
        std::cout << Tr(language, "错误：目录路径不能为空。\n", "Error: directory paths cannot be empty.\n");
        return false;
    }

    std::cout << "\nA: " << PathToUtf8(options.oldDirectory)
              << "\nB: " << PathToUtf8(options.newDirectory)
              << "\nC: " << PathToUtf8(options.outputDirectory) << '\n';
    if (!IsYesAnswer(ReadLine(Tr(language,
        "确认开始二进制比较并复制差异文件？(y/n)：",
        "Start binary comparison and copy differing files? (y/n): ")))) {
        std::cout << Tr(language, "已取消差异提取。\n", "Difference extraction canceled.\n");
        return true;
    }

    return RunDirectoryDiff(language, options);
}

} // namespace

/**
 * @brief 初始化控制台并调度一次用户选择的文件管理功能。
 *
 * 设计：此函数是界面层入口，负责 UTF-8 控制台、语言选择、主菜单、统一失败提示和退出暂停；
 * 具体搜索、清理、差异比较均委托给独立模块。
 * 关键变量：languageChoice 决定语言；action 决定功能分支；succeeded 保存最终退出状态。
 */
int RunFileManager() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    std::cout << "请选择语言 / Select language:\n";
    std::cout << "1. 中文\n2. English\n";
    const std::string languageChoice = Trim(ReadLine("> "));
    const Language language = (!languageChoice.empty() && languageChoice.front() == '2')
        ? Language::English
        : Language::Chinese;

    std::cout << "\n" << Tr(language, "请选择功能：\n", "Select function:\n");
    std::cout << "1. " << Tr(language, "搜索文件", "Search files") << '\n';
    std::cout << "2. " << Tr(language, "清理二进制重复文件", "Remove binary-identical duplicate files") << '\n';
    std::cout << "3. " << Tr(language, "提取新版目录中的新增/更新文件", "Extract added/updated files from a new directory") << '\n';
    const std::string action = Trim(ReadLine("> "));

    bool succeeded = false;
    if (!action.empty() && action.front() == '2') {
        succeeded = RunDuplicateFlow(language);
    } else if (!action.empty() && action.front() == '3') {
        succeeded = RunDirectoryDiffFlow(language);
    } else {
        succeeded = RunSearchFlow(language);
    }

    if (!succeeded) {
        std::cout << Tr(language,
            "操作未完成，请检查上方错误信息。\n",
            "The operation did not complete; review the error above.\n");
    }
    Pause(language);
    return succeeded ? 0 : 1;
}
