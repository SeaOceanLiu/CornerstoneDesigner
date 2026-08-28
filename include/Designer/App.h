// App.h
// CornerstoneDesigner
//
// 应用入口：Embedded 模式自持主循环，加载设计器自身 JSON 骨架并桥接菜单动作。

#pragma once

#include <memory>
#include <string>

class Control;

namespace UICornerstone {
class UICornerstone;
}

namespace Designer {

class App {
public:
    App();                          // 构造/析构在 App.cpp 定义（unique_ptr<Control> 需完整类型）
    ~App();                         // 析构在 App.cpp 定义（unique_ptr<UICornerstone> 需完整类型）
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    bool init();
    int  run(int autoSeconds = 0);   // autoSeconds>0 时无人值守渲染指定秒数后干净退出
    void shutdown();

private:
    void registerActions();
    bool loadSkeleton();
    void syncRootToWindow();            // 检测窗口尺寸变化 → root SetRect 触发布局重排
    void setupSplitters();              // 命令式创建 Splitter（LayoutParser 解析 splitter 节点会阻塞，绕过）
    void syncSplitterLayout(int W, int H);  // 按 ratio 重算三栏 + 两分隔条位置

    // SDL3.dll 动态解析（UICS 未暴露窗口尺寸查询；窗口 resize 需要主动同步 root）
    struct SdlApi {
        bool loaded = false;
        void* dll = nullptr;
        void* (*getWindows)(int*) = nullptr;
        void* (*setWindowSize)(void*, int, int) = nullptr;
        int   (*getWindowSize)(void*, int*, int*) = nullptr;
        bool load();
        void unload();
        void* windowHandle();       // SDL_GetWindows[0]
    };
    SdlApi m_sdl;

    std::unique_ptr<UICornerstone::UICornerstone> m_ui;  // 主实例（设计器自身 UI；预览为其内子视口）
    bool m_exitRequested = false;
    std::string m_lastError;
    int m_lastWinW = 0, m_lastWinH = 0;    // 上次已同步的窗口尺寸

    // Splitter（命令式创建，关联三栏）
    std::unique_ptr<Control> m_splitter1;  // toolbox|canvas
    std::unique_ptr<Control> m_splitter2;  // canvas|property
    float m_s1Ratio = 0.20f;               // splitter1 位置（相对 middleArea 宽度）
    float m_s2Ratio = 0.80f;               // splitter2 位置
    static constexpr float kSplitterThk = 6.0f;
};

}  // namespace Designer
