// main.cpp
// CornerstoneDesigner
//
// 入口：解析 backend=/auto= 参数，进入 App 主循环。

#include "Designer/App.h"

#include "UICornerstone.h"   // 完整类型：栈对象析构需成员可析构（MSVC）

#include <cstdio>

int main(int argc, char* argv[]) {
    int autoSeconds = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "auto=", 5) == 0) {
            autoSeconds = atoi(argv[i] + 5);
        }
    }

    Designer::App app;
    if (!app.init()) {
        std::printf("CornerstoneDesigner init failed\n");
        return 1;
    }
    int code = app.run(autoSeconds);
    app.shutdown();
    return code;
}
