// App.h
// CornerstoneDesigner
//
// 应用入口：Embedded 模式自持主循环，加载设计器自身 JSON 骨架并桥接菜单动作。

#pragma once

#include <memory>
#include <string>
#include "Control.h"
namespace UICornerstone {
class UICornerstone;
}

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
    void togglePreview();               // 设计↔预览切换（工具栏按钮/菜单 F5；同步按钮标题）
    void syncZoomCombo();               // 缩放档下拉同步（命中档位选中/非档位清空）
    void syncCanvasCombo();             // 画布尺寸档下拉同步（命中预设选中/自定义清空）
    void openCanvasSizeDialog();        // 自定义画布尺寸对话框（懒建复用：SetBool visible 开关）

    // SDL3.dll 动态解析（UICS 未暴露窗口尺寸查询；窗口 resize 需要主动同步 root）
    struct SdlApi {
        bool loaded = false;
        void* dll = nullptr;
        void* (*getWindows)(int*) = nullptr;
        void* (*setWindowSize)(void*, int, int) = nullptr;
        int   (*getWindowSize)(void*, int*, int*) = nullptr;
        unsigned (*getMouseState)(float*, float*) = nullptr;   // SDL_GetMouseState
        const unsigned char* (*getKeyboardState)(int*) = nullptr;  // SDL_GetKeyboardState
        void* (*createSystemCursor)(int) = nullptr;   // SDL_CreateSystemCursor
        void* (*setCursor)(void*) = nullptr;          // SDL_SetCursor（nullptr=恢复缺省）
        void  (*destroyCursor)(void*) = nullptr;      // SDL_DestroyCursor
        void* crossCursor = nullptr;                  // 拖放十字光标（懒创建缓存）
        bool load();
        void unload();
        void* windowHandle();       // SDL_GetWindows[0]
        bool pollMouse(float& x, float& y);   // 返回左键是否按下；填充窗口坐标
        bool pollDeleteKey();                 // Delete 键按下沿（每帧调一次）
        bool pollF5Key();                     // F5 键按下沿（预览切换；每帧调一次）
        bool pollCtrl();                      // Ctrl 按住态（多选修饰键；每帧调一次）
        bool pollMiddle(float& x, float& y);  // 中键状态 + 窗口坐标（画布平移）
        bool pollSpace();                     // 空格按住态（空格+左键拖拽平移）
    };
    SdlApi m_sdl;

    std::unique_ptr<UICornerstone::UICornerstone> m_ui;  // 主实例（设计器自身 UI；预览为其内子视口）
    std::unique_ptr<CanvasPane> m_canvas;                // 中央画布（子视口 + 网格覆盖层）
    bool m_exitRequested = false;
    bool m_zoomComboSync = false;          // 缩放档下拉程序化同步中（抑制回调）
    bool m_canvasComboSync = false;        // 画布尺寸档下拉程序化同步中（抑制回调）
    Control m_canvasSizeDlg;               // 自定义画布尺寸对话框（主实例，懒建复用）
    Control m_dlgCanvasW, m_dlgCanvasH;    // 对话框宽/高 NUD
    std::string m_lastError;
    int m_lastWinW = 0, m_lastWinH = 0;    // 上次已同步的窗口尺寸
};

}  // namespace Designer
