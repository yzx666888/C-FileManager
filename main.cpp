#include "FileManager.h"

/**
 * @brief 控制台程序的最小入口，只把生命周期控制权交给文件管理器界面层。
 *
 * 设计：将 main 保持为无业务逻辑的稳定入口，便于以后替换界面或编写测试驱动。
 * 关键变量：无；退出码直接沿用 RunFileManager 的执行结果。
 */
int main() {
    return RunFileManager();
}
