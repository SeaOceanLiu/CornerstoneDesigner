// main.cpp
// CornerstoneDesigner
//
// 入口：解析 backend=/auto= 参数，进入 App 主循环。

#include "Designer/App.h"

#include "UICornerstone.h"   // 完整类型：栈对象析构需成员可析构（MSVC）

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif


#ifdef _WIN32
// Release 版本使用 Windows GUI 子系统，避免打开 Console 窗口；
// 运行日志追加到 exe 同目录的 designer.log。
static void redirect_release_log() {
#ifdef NDEBUG
    wchar_t exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring path(exePath);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    path.resize(slash + 1);
    path += L"designer.log";
    FILE* f = nullptr;
    if (_wfreopen_s(&f, path.c_str(), L"a", stdout) != 0 || !f) return;
    _wfreopen_s(&f, path.c_str(), L"a", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout, "\n--- CornerstoneDesigner build %s %s ---\n", __DATE__, __TIME__);
    std::fflush(stdout);
#endif
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    redirect_release_log();
#endif
    int autoSeconds = 0;
    std::string openPath;
    bool startupHidden = false;
    bool startupProgressStdio = false;
    std::string startupProgressFile;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--startup-hidden") == 0) {
            startupHidden = true;
        } else if (strcmp(argv[i], "--startup-progress=stdio") == 0) {
            startupProgressStdio = true;
        } else if (strncmp(argv[i], "--startup-progress-file=", 24) == 0) {
            startupProgressFile = argv[i] + 24;
        } else if (strncmp(argv[i], "auto=", 5) == 0) {
            autoSeconds = atoi(argv[i] + 5);
        } else if (strchr(argv[i], '=') == nullptr && openPath.empty()) {
            openPath = argv[i];   // 非 name=value 参数视为待打开项目路径（支持超长路径——不经文件对话框）
        }
    }

    Designer::App app;
    app.setStartupOptions(startupHidden, startupProgressStdio);
    app.setStartupProgressFile(startupProgressFile);
    if (!app.init()) {
        std::printf("CornerstoneDesigner init failed\n");
        return 1;
    }
    app.setOpenPathArg(openPath);
    int code = app.run(autoSeconds);
    app.shutdown();
    return code;
}
