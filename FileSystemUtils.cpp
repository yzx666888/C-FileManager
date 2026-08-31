#include "FileSystemUtils.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

// 二进制比较每次读取 1 MiB，兼顾大文件效率与内存占用。
constexpr std::size_t kCompareBufferSize = 1024 * 1024;

/**
 * @brief 将单个路径组件转换为小写，用于 Windows 下不区分大小写的路径比较。
 *
 * 设计：按字符原地转换，避免额外创建第二个字符串；text 同时充当输入和返回结果。
 * 关键变量：text 为当前正在规范化的路径组件。
 */
std::wstring LowerPathComponent(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

/**
 * @brief 尽可能取得可比较的规范化绝对路径。
 *
 * 设计：优先使用 weakly_canonical 解析已存在部分；失败后退回 absolute，
 * 最后仍保留原路径，保证安全校验不会因不存在的输出目录而中断。
 * 关键变量：ec 保存非抛出式文件系统错误；normalized 保存逐级退化后的结果。
 */
fs::path NormalizedAbsolutePath(const fs::path& path) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = fs::absolute(path, ec);
        if (ec) {
            normalized = path;
        }
    }
    return normalized.lexically_normal();
}

/**
 * @brief 在调用者提供错误接收器时写入错误文本。
 *
 * 设计：允许 error 参数为空，使成功路径和不关心错误详情的调用无需临时字符串。
 * 关键变量：output 是可选输出指针；message 是待传递的 UTF-8 错误说明。
 */
void SetError(std::string* output, const std::string& message) {
    if (output != nullptr) {
        *output = message;
    }
}

} // namespace

/**
 * @brief 将控制台输入的 UTF-8 文本转换为 Windows 原生宽字符文本。
 *
 * 设计：先向 Win32 查询 required 所需宽字符数，再一次性分配 result，
 * 并开启 MB_ERR_INVALID_CHARS 拒绝非法 UTF-8，避免中文路径被静默损坏。
 * 关键变量：required 是目标长度；result 是转换后的宽字符串。
 */
std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

/**
 * @brief 将 Windows 原生宽字符文本转换为用于界面和日志的 UTF-8。
 *
 * 设计：采用与 Utf8ToWide 对称的两阶段转换；转换失败时返回空串，
 * 让上层仍能继续处理文件系统操作而不输出错误编码。
 * 关键变量：required 是 UTF-8 字节数；result 保存最终字节序列。
 */
std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
    return result;
}

/**
 * @brief 从 UTF-8 路径文本构造 std::filesystem::path。
 *
 * 设计：集中调用 Utf8ToWide，确保所有 Windows 文件操作都使用 Unicode 路径。
 * 关键变量：text 是来自控制台或日志逻辑的 UTF-8 路径。
 */
fs::path PathFromUtf8(const std::string& text) {
    return fs::path(Utf8ToWide(text));
}

/**
 * @brief 将文件系统路径转换为 UTF-8，供控制台与日志输出。
 *
 * 设计：filesystem 在 Windows 中保留宽字符 native 表示，统一经 WideToUtf8 输出。
 * 关键变量：path 是待展示或写入日志的原生路径。
 */
std::string PathToUtf8(const fs::path& path) {
    return WideToUtf8(path.native());
}

/**
 * @brief 生成精确到秒的时间戳，用于避免结果日志和目录名称冲突。
 *
 * 设计：使用 localtime_s 提供线程安全的本地时间转换，再按固定格式序列化。
 * 关键变量：now 保存当前时间点；localTime 为分解后的本地时间；stream 组装结果。
 */
std::string MakeTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return stream.str();
}

/**
 * @brief 判断路径是否为可访问目录，不抛出文件系统异常。
 *
 * 设计：所有目录入口均通过 error_code 检查，调用者只需处理布尔结果。
 * 关键变量：ec 保存 is_directory 的查询错误。
 */
bool IsDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

/**
 * @brief 检查已有目录是否为空，并在失败时返回原因。
 *
 * 设计：目录差异输出必须是空目录，避免陈旧文件混入本次更新包。
 * 关键变量：empty 是目录是否为空；error 向调用者返回查询失败原因。
 */
bool IsDirectoryEmpty(const fs::path& path, std::string& error) {
    std::error_code ec;
    const bool empty = fs::is_empty(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return empty;
}

/**
 * @brief 判断两个路径是否指向同一位置，兼容大小写和符号链接差异。
 *
 * 设计：路径存在时优先使用 equivalent 比较真实对象；不能查询时退回到
 * 小写后的规范化路径文本比较，仍可保护尚未创建的输出目录。
 * 关键变量：normalizedLeft/normalizedRight 是比较基准；ec 记录查询错误。
 */
bool AreSamePath(const fs::path& left, const fs::path& right) {
    const fs::path normalizedLeft = NormalizedAbsolutePath(left);
    const fs::path normalizedRight = NormalizedAbsolutePath(right);

    std::error_code ec;
    if (fs::exists(normalizedLeft, ec) && !ec) {
        ec.clear();
        if (fs::exists(normalizedRight, ec) && !ec) {
            ec.clear();
            const bool equivalent = fs::equivalent(normalizedLeft, normalizedRight, ec);
            if (!ec) {
                return equivalent;
            }
        }
    }

    return LowerPathComponent(normalizedLeft.native()) == LowerPathComponent(normalizedRight.native());
}

/**
 * @brief 判断 candidate 是否等于或位于 parent 目录内部。
 *
 * 设计：逐个比较规范化路径组件，而非简单前缀匹配，避免 C:\\A 被误判为 C:\\AB 的父目录。
 * 关键变量：candidateIt 与 parentIt 分别遍历候选路径和父路径的组件。
 */
bool IsSameOrSubPath(const fs::path& candidate, const fs::path& parent) {
    const fs::path normalizedCandidate = NormalizedAbsolutePath(candidate);
    const fs::path normalizedParent = NormalizedAbsolutePath(parent);

    auto candidateIt = normalizedCandidate.begin();
    auto parentIt = normalizedParent.begin();
    for (; parentIt != normalizedParent.end(); ++parentIt, ++candidateIt) {
        if (candidateIt == normalizedCandidate.end()) {
            return false;
        }
        if (LowerPathComponent(candidateIt->native()) != LowerPathComponent(parentIt->native())) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 递归收集目录内所有普通文件及其大小，跳过链接和无权访问项。
 *
 * 设计：使用 skip_permission_denied 保持扫描继续进行；链接目录禁用递归，
 * 防止符号链接或重解析点造成循环。错误收集到 result.errors 而非中断任务。
 * 关键变量：result 保存文件和错误；iterator/end 控制递归遍历；statusError 与 ec 分别记录条目和迭代错误。
 */
FileEnumerationResult EnumerateRegularFiles(const fs::path& root) {
    FileEnumerationResult result;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    if (ec) {
        result.errors.push_back(PathToUtf8(root) + ": " + ec.message());
        ec.clear();
    }

    while (iterator != end) {
        const fs::directory_entry entry = *iterator;

        std::error_code statusError;
        const fs::file_status symlinkStatus = entry.symlink_status(statusError);
        if (statusError) {
            result.errors.push_back(PathToUtf8(entry.path()) + ": " + statusError.message());
        } else if (fs::is_symlink(symlinkStatus)) {
            if (fs::is_directory(entry.status(statusError))) {
                iterator.disable_recursion_pending();
            }
        } else if (fs::is_regular_file(symlinkStatus)) {
            const std::uintmax_t size = entry.file_size(statusError);
            if (statusError) {
                result.errors.push_back(PathToUtf8(entry.path()) + ": " + statusError.message());
            } else {
                result.files.push_back({ entry.path(), size });
            }
        }

        iterator.increment(ec);
        if (ec) {
            result.errors.push_back(PathToUtf8(entry.path()) + ": " + ec.message());
            ec.clear();
        }
    }

    return result;
}

/**
 * @brief 以分块方式严格比较两个文件的全部二进制内容。
 *
 * 设计：先比较 leftSize/rightSize 快速排除不同文件；大小相同才打开流并用
 * 两个固定大小缓冲区逐块比较，因此不会一次读入大文件。返回三态结果，避免将读取失败误当成内容不同。
 * 关键变量：leftFile/rightFile 是两个输入流；leftBuffer/rightBuffer 是比较块；error 是可选错误输出。
 */
BinaryComparison CompareFilesBinary(
    const fs::path& left,
    const fs::path& right,
    std::string* error) {
    std::error_code ec;
    const std::uintmax_t leftSize = fs::file_size(left, ec);
    if (ec) {
        SetError(error, PathToUtf8(left) + ": " + ec.message());
        return BinaryComparison::Error;
    }

    ec.clear();
    const std::uintmax_t rightSize = fs::file_size(right, ec);
    if (ec) {
        SetError(error, PathToUtf8(right) + ": " + ec.message());
        return BinaryComparison::Error;
    }

    if (leftSize != rightSize) {
        return BinaryComparison::Different;
    }

    std::ifstream leftFile(left, std::ios::binary);
    std::ifstream rightFile(right, std::ios::binary);
    if (!leftFile || !rightFile) {
        SetError(error, "Failed to open one or both files for binary comparison.");
        return BinaryComparison::Error;
    }

    std::vector<char> leftBuffer(kCompareBufferSize);
    std::vector<char> rightBuffer(kCompareBufferSize);

    while (leftFile && rightFile) {
        leftFile.read(leftBuffer.data(), static_cast<std::streamsize>(leftBuffer.size()));
        rightFile.read(rightBuffer.data(), static_cast<std::streamsize>(rightBuffer.size()));

        const std::streamsize leftRead = leftFile.gcount();
        const std::streamsize rightRead = rightFile.gcount();
        if (leftRead != rightRead) {
            return BinaryComparison::Different;
        }
        if (leftRead == 0) {
            break;
        }
        const std::ptrdiff_t comparableBytes = static_cast<std::ptrdiff_t>(leftRead);
        if (!std::equal(
                leftBuffer.begin(),
                leftBuffer.begin() + comparableBytes,
                rightBuffer.begin())) {
            return BinaryComparison::Different;
        }
    }

    if (leftFile.bad() || rightFile.bad()) {
        SetError(error, "An I/O error occurred during binary comparison.");
        return BinaryComparison::Error;
    }
    return BinaryComparison::Equal;
}

/**
 * @brief 确保差异输出目录可安全使用：不存在则创建，存在时必须为空目录。
 *
 * 设计：禁止覆盖非目录或非空目录，确保目录 C 只包含本次提取出的文件。
 * 关键变量：ec 保存 exists/create_directories/is_empty 的错误；error 返回失败原因。
 */
bool PrepareEmptyOutputDirectory(const fs::path& output, std::string& error) {
    std::error_code ec;
    if (fs::exists(output, ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        if (!fs::is_directory(output, ec) || ec) {
            error = "The output path exists but is not a directory.";
            return false;
        }
        if (!fs::is_empty(output, ec) || ec) {
            error = ec ? ec.message() : "The output directory is not empty.";
            return false;
        }
        return true;
    }

    if (!fs::create_directories(output, ec) && ec) {
        error = ec.message();
        return false;
    }
    return true;
}

/**
 * @brief 创建目标父目录后复制文件，并尽力保留源文件的最后修改时间。
 *
 * 设计：统一封装复制步骤，让搜索复制与目录差异复制具备相同的错误处理。
 * 关键变量：parent 是 destination 的父目录；ec 保存文件系统错误；writeTime 保存可复制的时间戳。
 */
bool CopyFileCreatingParents(
    const fs::path& source,
    const fs::path& destination,
    std::string& error) {
    std::error_code ec;
    const fs::path parent = destination.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            error = PathToUtf8(parent) + ": " + ec.message();
            return false;
        }
    }

    ec.clear();
    if (!fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec) || ec) {
        error = PathToUtf8(source) + " -> " + PathToUtf8(destination) + ": " + ec.message();
        return false;
    }

    ec.clear();
    const fs::file_time_type writeTime = fs::last_write_time(source, ec);
    if (!ec) {
        fs::last_write_time(destination, writeTime, ec);
    }
    return true;
}

/**
 * @brief 为平铺复制模式生成不覆盖已有文件的唯一目标名。
 *
 * 设计：若 desired 已存在，在文件主名后追加 _1、_2 等序号，扩展名保持不变。
 * 关键变量：parent 是目标目录；stem/extension 拆分文件名；index 控制递增序号；candidate 是当前候选路径。
 */
fs::path MakeUniqueDestination(const fs::path& desired) {
    std::error_code ec;
    if (!fs::exists(desired, ec)) {
        return desired;
    }

    const fs::path parent = desired.parent_path();
    const std::wstring stem = desired.stem().native();
    const std::wstring extension = desired.extension().native();
    for (std::uint64_t index = 1; ; ++index) {
        const fs::path candidate = parent / fs::path(stem + L"_" + std::to_wstring(index) + extension);
        ec.clear();
        if (!fs::exists(candidate, ec)) {
            return candidate;
        }
    }
}
