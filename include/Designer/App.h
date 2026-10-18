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
    void setOpenPathArg(const std::string& p) { m_openPathArg = p; }   // 命令行路径参数（启动即打开；长路径不经对话框）
    void shutdown();

private:
    void registerActions();
    bool loadSkeleton();
    void syncRootToWindow();            // 检测窗口尺寸变化 → root SetRect 触发布局重排
    void loadSettings();                // 启动：读 designer_settings.json（对话框记忆目录）
    void saveSettings() const;          // 变更即写（对话框选择后）
    void setupCanvas();                 // canvasArea → CanvasPane（子视口 + 网格覆盖层）
    void bindToolbar();                 // 画布工具栏控件 → CanvasPane 交互接线
    void refreshMenuStates();           // 菜单可用态（撤销/重做；引擎按 item id 注册后生效）
    void togglePreview();               // 预览：独立窗体（引擎多实例）打开/刷新（工具栏/菜单 F5）
    void openPreviewWindow();           // 创建预览实例 + 加载当前布局快照
    void closePreviewWindow();          // 销毁预览实例（同步按钮标题）
    void pumpPreviewWindow(double delta);// 主循环内泵预览实例（含窗体关闭回收）
    void runPreviewChecks();              // auto=3 预览校验（Present 前像素捕获）
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
        void  (*setWindowTitle)(void*, const char*) = nullptr;   // SDL_SetWindowTitle（F5.4 窗口标题）
        unsigned (*getMouseState)(float*, float*) = nullptr;   // SDL_GetMouseState
        void*    (*getMouseFocus)() = nullptr;                 // SDL_GetMouseFocus
        void*    (*getKeyboardFocus)() = nullptr;              // SDL_GetKeyboardFocus
        void*    mainWindow = nullptr;                         // 主窗口 SDL_Window*（焦点守卫；须来自 windowHandle()）
        const unsigned char* (*getKeyboardState)(int*) = nullptr;  // SDL_GetKeyboardState
        void* (*createSystemCursor)(int) = nullptr;   // SDL_CreateSystemCursor
        void* (*setCursor)(void*) = nullptr;          // SDL_SetCursor（nullptr=恢复缺省）
        void  (*destroyCursor)(void*) = nullptr;      // SDL_DestroyCursor
        void* crossCursor = nullptr;                  // 拖放十字光标（懒创建缓存）
        void* mainWin = nullptr;                      // 主窗口句柄（启动时捕获——预览实例窗口勿混入）
        bool load();
        void unload();
        void* windowHandle();       // 主窗口（mainWin；未捕获回退 SDL_GetWindows[0]）
        bool pollMouse(float& x, float& y);   // 返回左键是否按下；填充窗口坐标
        bool pollDeleteKey();                 // Delete 键按下沿（每帧调一次）
        bool pollF5Key();                     // F5 键按下沿（预览切换；每帧调一次）
        bool pollUndoKey();                   // Ctrl+Z 按下沿（撤销；每帧调一次）
        bool pollRedoKey();                   // Ctrl+Y 按下沿（重做；每帧调一次）
        bool pollCtrl();                      // Ctrl 按住态（多选修饰键；每帧调一次）
        bool pollMiddle(float& x, float& y);  // 中键状态 + 窗口坐标（画布平移）
        bool pollSpace();                     // 空格按住态（空格+左键拖拽平移）
    };
    SdlApi m_sdl;

    std::unique_ptr<UICornerstone::UICornerstone> m_ui;  // 主实例（设计器自身 UI）
    std::unique_ptr<UICornerstone::UICornerstone> m_previewUi;  // 预览独立窗体（引擎多实例）
    std::unique_ptr<CanvasPane> m_canvas;                // 中央画布（子视口 + 网格覆盖层）
    bool m_exitRequested = false;
    bool m_zoomComboSync = false;          // 缩放档下拉程序化同步中（抑制回调）
    bool m_canvasComboSync = false;        // 画布尺寸档下拉程序化同步中（抑制回调）
    Control m_canvasSizeDlg;               // 自定义画布尺寸对话框（主实例，懒建复用）
    Control m_dlgCanvasW, m_dlgCanvasH;    // 对话框宽/高 NUD
    // 资源插入/修改对话框（懒建复用）：名称 EditBox + 路径显示 + 选择目录按钮
    Control m_resDlg, m_dlgResName, m_dlgResPathEdit;
    std::string m_dlgResPath;
    bool m_resDlgEditMode = false;            // 对话框当前模式（复用——回调按成员分派）
    bool m_previewCheckPending = false;       // auto=3：待执行预览校验（泵内 Present 前）
    bool m_menuUndoEnabled = true;            // 菜单"撤销"当前可用态（仅变化时写引擎）
    bool m_menuRedoEnabled = true;            // 菜单"重做"当前可用态
    std::string m_lastFileDir;                // 文件对话框记忆目录（UTF-8；settings 持久化）
    std::string m_lastFolderDir;              // 目录对话框记忆目录（UTF-8）
    // F5.4 最近文件（≤10；settings 持久化）+ 窗口标题（项目名 + 未保存 *）
    std::vector<std::string> m_recentFiles;   // 最近项目路径（UTF-8；[0]=最近）
    Control m_recentPanel;                    // 自持"最近文件"子菜单面板（P0-98 动态：Remove/Add 复用，零泄漏）
    std::vector<Control> m_recentItems;       // 当前动态项（刷新时逐个移除重建）
    std::string m_pendingDropPath;            // OS 拖入待打开路径（P0-100：泵内仅轻量记录，泵后处理）
    bool m_dirty = false;                     // 未保存标记（编辑提交/撤销/重做置位；保存/打开/新建清零）
    void refreshRecentMenu();                 // 动态重建"最近文件"子菜单（无预留空槽——仅实际项）
    void recordRecentFile(const std::string& path, bool persist);   // 去重提升 + 上限；persist=false 免写盘（自测）
    void openRecentFile(size_t idx);          // 最近项打开（失效路径清理 + 状态栏提示）
    void updateWindowTitle();                 // 窗口标题：项目名 + 未保存 *
    void openResourceDialog(bool editMode);   // 插入（editMode=false）/修改（true，选中行回填）
    std::string m_lastError;
    std::string m_openPathArg;             // 命令行待打开项目路径（启动时打开）
    int m_lastWinW = 0, m_lastWinH = 0;    // 上次已同步的窗口尺寸
};

}  // namespace Designer
