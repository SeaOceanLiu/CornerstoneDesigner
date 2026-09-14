// App.h
// CornerstoneDesigner
//
// 应用入口：Embedded 模式自持主循环，加载设计器自身 JSON 骨架并桥接菜单动作。

#pragma once

#include <memory>
#include <string>
namespace UICornerstone {
class UICornerstone;
}
class Control;

namespace Designer {

class CanvasPane;

class App {
public:
    App();
    ~App();                          // 析构在 App.cpp 定义（unique_ptr<UICornerstone> 需完整类型）
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
    void setupCanvas();                 // canvasArea → CanvasPane（子视口 + 网格覆盖层）
    void bindToolbar();                 // 画布工具栏控件 → CanvasPane 交互接线

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
    std::unique_ptr<CanvasPane> m_canvas;                // 中央画布（子视口 + 网格覆盖层）
    bool m_exitRequested = false;
    std::string m_lastError;
    int m_lastWinW = 0, m_lastWinH = 0;    // 上次已同步的窗口尺寸
};

}  // namespace Designer
