// main.cpp
// CornerstoneDesigner
//
// 入口：解析 backend=/auto= 参数，进入 App 主循环。

#include "Designer/App.h"

#include "UICornerstone.h"   // 完整类型：栈对象析构需成员可析构（MSVC）

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
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
