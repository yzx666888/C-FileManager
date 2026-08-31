#include "Localization.h"

#include <algorithm>
#include <cctype>

/**
 * @brief 根据当前语言从一对中英文文本中选择应显示的文本。
 *
 * 设计：返回字符串字面量指针，不分配内存；各模块通过此函数保持界面语言一致。
 * 关键变量：language 决定选择分支；chinese 和 english 是同一语义的两种文本。
 */
const char* Tr(Language language, const char* chinese, const char* english) {
    return language == Language::English ? english : chinese;
}

/**
 * @brief 判断用户文本是否表示“确认”。
 *
 * 设计：先删除所有空白字符，再接受 y、Y、1 和“是”，使菜单确认可容忍首尾空格或粘贴换行。
 * 关键变量：normalized 是去除空白后的输入副本，避免修改调用者原始文本。
 */
bool IsYesAnswer(const std::string& text) {
    std::string normalized = text;
    normalized.erase(
        std::remove_if(normalized.begin(), normalized.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; }),
        normalized.end());

    return normalized == "y" || normalized == "Y" || normalized == "1" || normalized == "是";
}
