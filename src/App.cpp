// App.cpp
// CornerstoneDesigner
//
// Embedded 循环 + LoadLayout 骨架 + RegisterAction 桥接（阶段一）。

#include "Designer/App.h"

#include "Designer/CanvasPane.h"
#include "Designer/ControlModel.h"

#include "UICornerstone.h"
#include "Control.h"
#include "Event.h"
#include "PropertyNames.h"

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#undef CreateDialog   // 避免 Win32 宏（CreateDialogA/W）污染引擎 API 符号
#endif

namespace {

// exe 同目录 + 相对路径（运行期依赖与 exe 同目录，见 CMake POST_BUILD）
std::string exeDirPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos);
    int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
#else
    return ".";
#endif
}

std::string exeRelative(const std::string& rel) {
    return exeDirPath() + "/" + rel;
}

}  // namespace

namespace Designer {

App::App() = default;
App::~App() = default;

// ───────────────────────── SDL3 动态解析（窗口 API）─────────────────────────

bool App::SdlApi::load() {
#ifdef _WIN32
    dll = LoadLibraryA("SDL3.dll");
#else
    dll = dlopen("SDL3.dll", RTLD_LAZY);
#endif
    if (!dll) return false;
#ifdef _WIN32
    getWindows   = reinterpret_cast<void*(*)(int*)>(GetProcAddress((HMODULE)dll, "SDL_GetWindows"));
    setWindowSize = reinterpret_cast<void*(*)(void*, int, int)>(GetProcAddress((HMODULE)dll, "SDL_SetWindowSize"));
    getWindowSize = reinterpret_cast<int(*)(void*, int*, int*)>(GetProcAddress((HMODULE)dll, "SDL_GetWindowSize"));
    getMouseState = reinterpret_cast<unsigned(*)(float*, float*)>(GetProcAddress((HMODULE)dll, "SDL_GetMouseState"));
    getKeyboardState = reinterpret_cast<const unsigned char*(*)(int*)>(GetProcAddress((HMODULE)dll, "SDL_GetKeyboardState"));
    createSystemCursor = reinterpret_cast<void*(*)(int)>(GetProcAddress((HMODULE)dll, "SDL_CreateSystemCursor"));
    setCursor = reinterpret_cast<void*(*)(void*)>(GetProcAddress((HMODULE)dll, "SDL_SetCursor"));
    destroyCursor = reinterpret_cast<void(*)(void*)>(GetProcAddress((HMODULE)dll, "SDL_DestroyCursor"));
#else
    getWindows   = reinterpret_cast<void*(*)(int*)>(dlsym(dll, "SDL_GetWindows"));
    setWindowSize = reinterpret_cast<void*(*)(void*, int, int)>(dlsym(dll, "SDL_SetWindowSize"));
    getWindowSize = reinterpret_cast<int(*)(void*, int*, int*)>(dlsym(dll, "SDL_GetWindowSize"));
    getMouseState = reinterpret_cast<unsigned(*)(float*, float*)>(dlsym(dll, "SDL_GetMouseState"));
    getKeyboardState = reinterpret_cast<const unsigned char*(*)(int*)>(dlsym(dll, "SDL_GetKeyboardState"));
    createSystemCursor = reinterpret_cast<void*(*)(int)>(dlsym(dll, "SDL_CreateSystemCursor"));
    setCursor = reinterpret_cast<void*(*)(void*)>(dlsym(dll, "SDL_SetCursor"));
    destroyCursor = reinterpret_cast<void(*)(void*)>(dlsym(dll, "SDL_DestroyCursor"));
#endif
    loaded = getWindows && getWindowSize;
    return loaded;
}

// 左键状态 + 窗口内坐标（SDL_GetMouseState 相对当前焦点窗口，与引擎事件坐标同系）
bool App::SdlApi::pollMouse(float& x, float& y) {
    if (!getMouseState) return false;
    float mx = 0, my = 0;
    unsigned buttons = getMouseState(&mx, &my);
    x = mx; y = my;
    return (buttons & 0x01u) != 0;   // SDL_BUTTON_LMASK
}

// Delete 键按下沿（SDL_SCANCODE_DELETE = 76；SDL_GetKeyboardState 返回常驻数组，直接索引）
bool App::SdlApi::pollDeleteKey() {
    static bool wasDown = false;
    if (!getKeyboardState) return false;
    int numkeys = 0;
    const unsigned char* state = getKeyboardState(&numkeys);
    if (!state || numkeys <= 76) return false;
    const bool down = state[76] != 0;
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

// F5 键按下沿（SDL_SCANCODE_F5 = 62；预览切换）
bool App::SdlApi::pollF5Key() {
    static bool wasDown = false;
    if (!getKeyboardState) return false;
    int numkeys = 0;
    const unsigned char* state = getKeyboardState(&numkeys);
    if (!state || numkeys <= 62) return false;
    const bool down = state[62] != 0;
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

// Ctrl+Z / Ctrl+Y 按下沿（F2.5 撤销/重做快捷键；每帧调一次保持边沿状态）
bool App::SdlApi::pollUndoKey() {
    static bool wasDown = false;
    if (!getKeyboardState) return false;
    int numkeys = 0;
    const unsigned char* state = getKeyboardState(&numkeys);
    if (!state || numkeys <= 29) return false;
    const bool ctrl = numkeys > 228 && (state[224] != 0 || state[228] != 0);   // LCTRL/RCTRL
    const bool down = ctrl && state[29] != 0;   // SDL_SCANCODE_Z=29
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

bool App::SdlApi::pollRedoKey() {
    static bool wasDown = false;
    if (!getKeyboardState) return false;
    int numkeys = 0;
    const unsigned char* state = getKeyboardState(&numkeys);
    if (!state || numkeys <= 28) return false;
    const bool ctrl = numkeys > 228 && (state[224] != 0 || state[228] != 0);
    const bool down = ctrl && state[28] != 0;   // SDL_SCANCODE_Y=28
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

// 中键状态 + 窗口坐标（画布平移）
bool App::SdlApi::pollMiddle(float& x, float& y) {
    if (!getMouseState) return false;
    float mx = 0, my = 0;
    unsigned buttons = getMouseState(&mx, &my);
    x = mx; y = my;
    return (buttons & 0x02u) != 0;   // SDL_BUTTON_MMASK（中键；0x04 是右键）
}

// 空格按住态（SDL_SCANCODE_SPACE = 44；空格+左键拖拽平移）
bool App::SdlApi::pollSpace() {
    if (!getKeyboardState) return false;
    int numkeys = 0;
    const unsigned char* state = getKeyboardState(&numkeys);
    if (!state || numkeys <= 44) return false;
    return state[44] != 0;
}

// Ctrl 按住态（SDL_SCANCODE_LCTRL = 224 / RCTRL = 228；多选修饰键）
bool App::SdlApi::pollCtrl() {
    if (!getKeyboardState) return false;
    int numkeys = 0;
    const unsigned char* state = getKeyboardState(&numkeys);
    if (!state || numkeys <= 228) return false;
    return state[224] != 0 || state[228] != 0;
}

void App::SdlApi::unload() {
#ifdef _WIN32
    if (dll) FreeLibrary((HMODULE)dll);
#else
    if (dll) dlclose(dll);
#endif
    dll = nullptr;
    loaded = false;
}

void* App::SdlApi::windowHandle() {
    if (mainWin) return mainWin;   // 主窗口固定（预览等多实例窗口创建后 [0] 可能变化）
    int count = 0;
    void* arr = getWindows ? getWindows(&count) : nullptr;
    if (count > 0 && arr && ((void**)arr)[0]) return ((void**)arr)[0];
    return nullptr;
}

// 窗口尺寸变化 → root.SetRect(0,0,w,h)。
// root 为 v-flow 布局容器（menuBar/middleArea/statusBar）：menu-bar/status-bar 挂入 v-flow
// 时自动切换手动定位参与布局引擎重排；middleArea 内 h-flow + splitter(引擎模式) 由引擎
// 整链重排（面板 + splitter 链式移动，无需应用侧手动同步 splitter 位置）。
void App::syncRootToWindow() {
    if (!m_sdl.loaded || !m_sdl.getWindowSize || !m_ui) return;
    void* win = m_sdl.windowHandle();
    if (!win) return;
    int w = 0, h = 0;
    m_sdl.getWindowSize(win, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (w == m_lastWinW && h == m_lastWinH) return;

    auto root = m_ui->FindControl("root");
    if (root.IsValid()) {
        root.SetRect(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        std::printf("[App] window resized to %dx%d, root->SetRect\n", w, h);
    }

    m_lastWinW = w;
    m_lastWinH = h;
}

bool App::init() {
    m_ui = UICornerstone::UICornerstone::Create(
        UICornerstone::UICornerstone::Config{}
            .WithBackend("sdl3")
            .WithResourceRoot(exeRelative("assets"))
            .WithWindow("CornerstoneDesigner", 1920, 1080)
            .WithWindowFlags(UIWindowFlags::Resizable));
    if (!m_ui) {
        std::printf("[App] Create failed\n");
        return false;
    }
    // 动作注册必须先于布局解析：引擎在 LoadLayout 解析期将 instance->actions 注入
    // parser handlers 绑定菜单项 onClick——先解析后注册会导致所有菜单项无回调（静默失效）
    registerActions();
    if (!loadSkeleton()) {
        std::printf("[App] LoadLayout failed: %s\n", m_lastError.c_str());
        return false;
    }
    m_sdl.load();
    if (!m_sdl.loaded) {
        std::printf("[App] SDL3.dll 未解析（窗口 resize 同步降级）\n");
    }
    setupCanvas();
    bindToolbar();
    if (m_canvas) {
        m_canvas->SetMouseSource([this](float& x, float& y) { return m_sdl.pollMouse(x, y); });
        m_canvas->SetKeySource([this] { return m_sdl.pollDeleteKey(); });
        m_canvas->SetCtrlSource([this] { return m_sdl.pollCtrl(); });   // P2 多选修饰键
        m_canvas->SetMiddleSource([this](float& x, float& y) { return m_sdl.pollMiddle(x, y); });
        m_canvas->SetSpaceSource([this] { return m_sdl.pollSpace(); });
        m_canvas->SetZoomChangedFn([this] { syncZoomCombo(); });   // 缩放变化（含滚轮）同步档位
        m_canvas->SetCanvasTree(m_ui->FindControl("tvTree"));
        m_canvas->SetStatusTarget(m_ui->FindControl("statusBar"));   // P1-b 放置目标提示
        m_canvas->SetPropertyPanel(m_ui->FindControl("propertyPanel"));   // Delete 归属判定
        m_canvas->LoadSchema(exeRelative("schema/declarative-ui.schema.json"));
        m_canvas->SetDynamicPropsContainer(m_ui->FindControl("propDynamic"));
        // 滚轮双订阅：动态区内 propDynamic 消费；面板其他区域由 tabProperty 兜底
        // （最近订阅者语义——内层无订阅 Panel 透传，修复后由本层消费）
        m_ui->FindControl("tabProperty").SetCallback(
            PropertyNames::kEventMouseWheel, [this](const Event& e) {
                m_canvas->ScrollDynamicBy(e.GetValueChanged());
            });
        m_canvas->SetToolboxTree(m_ui->FindControl("tvToolbox"));
        // 资源面板：列表 + 插入/修改/删除按钮（弹窗收集名称 + 系统目录对话框选路径）
        m_canvas->SetResourcePanel(m_ui->FindControl("lvResource"));
        if (auto b = m_ui->FindControl("btnResInsert"); b.IsValid())
            b.SetCallback(PropertyNames::kEventClick,
                          [this](const Event&) { openResourceDialog(false); });
        if (auto b = m_ui->FindControl("btnResEdit"); b.IsValid())
            b.SetCallback(PropertyNames::kEventClick,
                          [this](const Event&) { openResourceDialog(true); });
        if (auto b = m_ui->FindControl("btnResRemove"); b.IsValid())
            b.SetCallback(PropertyNames::kEventClick,
                          [this](const Event&) { if (m_canvas) m_canvas->RemoveSelectedResourceProvider(); });
        m_canvas->SetEventPanel(m_ui->FindControl("ev_type"),
                                m_ui->FindControl("ev_handler"),
                                m_ui->FindControl("ev_suggest"),
                                m_ui->FindControl("ev_add"),
                                m_ui->FindControl("ev_err"),
                                m_ui->FindControl("tabEvent"));
        m_canvas->SetPropertyControls(m_ui->FindControl("prop_x"),
                                      m_ui->FindControl("prop_y"),
                                      m_ui->FindControl("prop_w"),
                                      m_ui->FindControl("prop_h"),
                                      m_ui->FindControl("prop_caption"),
                                      m_ui->FindControl("prop_color"),
                                      m_ui->FindControl("prop_visible"),
                                      m_ui->FindControl("prop_enabled"));
    }

    // 初始化基准尺寸（同时捕获主窗口句柄——仅此一窗时 [0] 即主窗）
    if (m_sdl.loaded && m_sdl.getWindowSize) {
        void* win = m_sdl.windowHandle();
        if (win) {
            m_sdl.mainWin = win;
            m_sdl.getWindowSize(win, &m_lastWinW, &m_lastWinH);
        }
    }
    return true;
}

bool App::loadSkeleton() {
    if (!m_ui->LoadLayoutFromFile(exeRelative("main_layout.json"))) {
        m_lastError = m_ui->GetLastError();
        return false;
    }
    return true;
}

void App::registerActions() {
    // 全部 JSON 事件动作先注册为占位（未实现的动作给出提示），后续阶段逐一接线。
    auto placeholder = [this](Control ctl) {
        (void)ctl;
        std::printf("[App] action not implemented yet\n");
    };
    // 菜单浮层项点击：标记画布跳过本帧（弹层显示在画布上方，SDL 轮询须同步跳过）
    auto guard = [this](std::function<void(Control)> fn) {
        return [this, fn](Control ctl) {
            if (m_canvas) m_canvas->SuppressCanvasClickThisFrame();
            fn(ctl);
        };
    };
    const char* actions[] = {
        "onMenuNew", "onMenuOpen", "onMenuSave", "onMenuSaveAs",
        "onMenuImportJson", "onMenuExportJson", "onMenuExportCode",
        "onMenuRecent", "onMenuExit",
        "onMenuUndo", "onMenuRedo", "onMenuCopy", "onMenuPaste", "onMenuDelete",
        "onMenuFront", "onMenuBack", "onMenuMoveUp", "onMenuMoveDown",
        "onMenuZoom25", "onMenuZoom50", "onMenuZoom75", "onMenuZoom100",
        "onMenuZoom150", "onMenuZoom200",
        "onMenuToggleGrid", "onMenuToggleSnap", "onMenuPreview",
        "onMenuAbout",
    };
    // Win32 原生文件对话框（03 §13.2 薄层；无 MFC——宽字符 → UTF-8 路径）
    auto fileDialog = [](bool save, const wchar_t* filter, const wchar_t* defExt,
                         const wchar_t* title) -> std::string {
        wchar_t buf[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = defExt;
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                    (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        if (!ok) return "";
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string out(len > 0 ? (size_t)(len - 1) : 0, '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
        return out;
    };
    static const wchar_t* kCsdFilter =
        L"CornerstoneDesigner 项目 (*.csd)\0*.csd\0所有文件 (*.*)\0*.*\0";
    static const wchar_t* kJsonFilter =
        L"UICornerstone 布局 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    for (const char* name : actions) {
        if (std::string(name) == "onMenuExit") {
            m_ui->RegisterAction(name, guard([this](Control) { m_exitRequested = true; }));
        } else if (std::string(name) == "onMenuNew") {
            m_ui->RegisterAction(name, guard([this](Control) {
                if (m_canvas) m_canvas->NewProject();
            }));
        } else if (std::string(name) == "onMenuOpen") {
            m_ui->RegisterAction(name, guard([this, fileDialog](Control) {
                if (!m_canvas) return;
                const std::string p = fileDialog(false, kCsdFilter, L"csd", L"打开项目");
                if (!p.empty()) m_canvas->OpenProject(p);
            }));
        } else if (std::string(name) == "onMenuSave") {
            m_ui->RegisterAction(name, guard([this, fileDialog](Control) {
                if (!m_canvas) return;
                std::string p = m_canvas->ProjectPath();
                if (p.empty())
                    p = fileDialog(true, kCsdFilter, L"csd", L"保存项目");
                if (!p.empty()) m_canvas->SaveProject(p);
            }));
        } else if (std::string(name) == "onMenuSaveAs") {
            m_ui->RegisterAction(name, guard([this, fileDialog](Control) {
                if (!m_canvas) return;
                const std::string p = fileDialog(true, kCsdFilter, L"csd", L"另存为");
                if (!p.empty()) m_canvas->SaveProject(p);
            }));
        } else if (std::string(name) == "onMenuImportJson") {
            m_ui->RegisterAction(name, guard([this, fileDialog](Control) {
                if (!m_canvas) return;
                const std::string p = fileDialog(false, kJsonFilter, L"json", L"导入 JSON 布局");
                if (!p.empty()) m_canvas->ImportLayout(p);
            }));
        } else if (std::string(name) == "onMenuExportJson") {
            m_ui->RegisterAction(name, guard([this, fileDialog](Control) {
                if (!m_canvas) return;
                const std::string p = fileDialog(true, kJsonFilter, L"json", L"导出 JSON 布局");
                if (!p.empty()) m_canvas->ExportLayout(p);
            }));
        } else if (std::string(name) == "onMenuAbout") {
            m_ui->RegisterAction(name, guard([this](Control) {
                std::printf("[App] CornerstoneDesigner v0.1 (stage 1 skeleton)\n");
            }));
        } else if (std::string(name) == "onMenuPreview") {
            m_ui->RegisterAction(name, guard([this](Control) { togglePreview(); }));   // 预览模式 F5
        } else if (std::string(name) == "onMenuUndo") {
            m_ui->RegisterAction(name, guard([this](Control) { if (m_canvas) m_canvas->Undo(); }));
        } else if (std::string(name) == "onMenuRedo") {
            m_ui->RegisterAction(name, guard([this](Control) { if (m_canvas) m_canvas->Redo(); }));
        } else if (std::string(name) == "onMenuDelete") {
            m_ui->RegisterAction(name, guard([this](Control) { if (m_canvas) m_canvas->deleteSelected(); }));
        } else if (std::string(name).rfind("onMenuZoom", 0) == 0) {
            // 视图菜单缩放档（onMenuZoomNN → NN%）——与工具栏档位同路径
            const float z = std::stof(std::string(name).substr(10)) / 100.0f;
            m_ui->RegisterAction(name, guard([this, z](Control) {
                if (m_canvas) m_canvas->SetZoom(z);
            }));
        } else {
            m_ui->RegisterAction(name, guard(placeholder));
        }
    }
}

void App::setupCanvas() {
    static constexpr float kToolbarH = 40.0f;
    auto area = m_ui->FindControl("canvasArea");
    if (!area.IsValid()) {
        std::printf("[App] canvasArea not found\n");
        return;
    }
    m_canvas = std::make_unique<CanvasPane>();
    if (!m_canvas->Create(*m_ui, area, kToolbarH, exeRelative("assets"))) {
        std::printf("[App] CanvasPane.Create failed\n");
        m_canvas.reset();
        return;
    }
    // 拖放光标：工具拖放中 → 十字（懒创建缓存）；结束恢复系统缺省
    m_canvas->SetDragCursorFn([this](bool on) {
        if (!m_sdl.setCursor) return;
        if (on && !m_sdl.crossCursor && m_sdl.createSystemCursor)
            m_sdl.crossCursor = m_sdl.createSystemCursor(3);   // SDL_SYSTEM_CURSOR_CROSSHAIR
        m_sdl.setCursor(on ? m_sdl.crossCursor : nullptr);
    });
    m_canvas->SetClickHandler([this](const std::string& id, float cx, float cy) {
        (void)cx; (void)cy;
        std::printf("[App] canvas click node=%s\n", id.c_str());
    });
}

void App::bindToolbar() {
    if (!m_canvas) return;

    auto gridCtl = m_ui->FindControl("tb_grid");
    if (gridCtl.IsValid()) {
        gridCtl.SetBool(PropertyNames::kTriState, false);   // 两态开关
        gridCtl.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
            if (m_canvas->IsPreviewMode()) return;   // 预览态：网格为编辑辅助，忽略
            m_canvas->SetGridVisible(e.GetCheckState() != 0);
        });
    }

    // 网格间距档位（10-200px——拖动/吸附随 SetGridCellSize 同步瓦片重建）
    auto gcCtl = m_ui->FindControl("cb_gridcell");
    if (gcCtl.IsValid()) {
        gcCtl.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event&) {
            m_canvas->SuppressCanvasClickThisFrame();   // 弹层项点击不误触画布
            auto gc = m_ui->FindControl("cb_gridcell");
            const int idx = gc.GetInt(PropertyNames::kSelectedIndex);
            static const float kCells[] = { 10, 20, 25, 40, 50, 100, 200 };
            if (idx >= 0 && idx < 7) m_canvas->SetGridCellSize(kCells[idx]);
        });
    }

    auto snapCtl = m_ui->FindControl("tb_snap");
    if (snapCtl.IsValid()) {
        snapCtl.SetBool(PropertyNames::kTriState, false);   // 两态开关
        snapCtl.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
            m_canvas->SetSnapEnabled(e.GetCheckState() != 0);   // 吸附到网格
        });
    }

    auto alignCtl = m_ui->FindControl("tb_alignctl");
    if (alignCtl.IsValid()) {
        alignCtl.SetBool(PropertyNames::kTriState, false);   // 两态开关
        alignCtl.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
            m_canvas->SetAlignEnabled(e.GetCheckState() != 0);   // 吸附到控件（智能对齐）
        });
    }

    // P2 多选：对齐命令下拉（0-5 左/水平居中/右/顶/垂直居中/底；≥2 启用；执行后清选中）
    auto alignCmd = m_ui->FindControl("tb_align");
    if (alignCmd.IsValid()) {
        m_canvas->SetAlignCommandControl(alignCmd);
        alignCmd.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event& e) {
            const int idx = e.GetSelectedIndex();
            if (idx >= 0 && idx < 10) {   // 0-5 对齐六模式 + 6 等宽 + 7 等高 + 8/9 水平/垂直分布
                m_canvas->SuppressCanvasClickThisFrame();   // 弹层项点击不误触画布
                m_canvas->AlignSelection(idx);
            }
        });
    }

    auto zoomCtl = m_ui->FindControl("tb_zoom");
    if (zoomCtl.IsValid()) {
        zoomCtl.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event& e) {
            if (m_zoomComboSync) return;   // 程序化同步（±/1:1 后）不触发
            m_canvas->SuppressCanvasClickThisFrame();   // 弹层项点击不误触画布
            static const float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            int idx = e.GetSelectedIndex();
            if (idx >= 0 && idx < 6) m_canvas->SetZoom(kZooms[idx]);
        });
    }

    auto zoomIn = m_ui->FindControl("tb_zoom_in");
    if (zoomIn.IsValid()) {
        zoomIn.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            // 按档位步进（非 ±0.25）——下拉恒命中：取大于当前的最小档
            static const float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            const float z = m_canvas->GetZoom();
            float nz = kZooms[5];
            for (int i = 0; i < 6; ++i)
                if (kZooms[i] > z + 0.001f) { nz = kZooms[i]; break; }
            m_canvas->SetZoom(nz);   // 档位下拉经 SetZoomChangedFn 回调同步
        });
    }

    auto zoomOut = m_ui->FindControl("tb_zoom_out");
    if (zoomOut.IsValid()) {
        zoomOut.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            // 按档位步进：取小于当前的最大档
            static const float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            const float z = m_canvas->GetZoom();
            float nz = kZooms[0];
            for (int i = 5; i >= 0; --i)
                if (kZooms[i] < z - 0.001f) { nz = kZooms[i]; break; }
            m_canvas->SetZoom(nz);   // 档位下拉经 SetZoomChangedFn 回调同步
        });
    }

    auto zoom100 = m_ui->FindControl("tb_zoom_100");
    if (zoom100.IsValid()) {
        zoom100.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            m_canvas->SetZoom(1.0f);   // 1:1（实际大小）——重置为 100%
        });
    }

    // 设计画布尺寸（基准窗体）档位下拉（含"自定义…"→ 对话框）
    auto canvasCtl = m_ui->FindControl("cb_canvas");
    if (canvasCtl.IsValid()) {
        canvasCtl.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event& e) {
            if (m_canvasComboSync) return;   // 程序化同步不触发
            const int idx = e.GetSelectedIndex();
            if (idx == 4) { openCanvasSizeDialog(); return; }   // 自定义…
            if (idx >= 0 && idx < 4) {
                m_canvas->SuppressCanvasClickThisFrame();   // 弹层项点击不误触画布
                static const float kSizes[4][2] = { {1024, 768}, {1280, 800}, {1440, 900}, {1920, 1080} };
                m_canvas->SetCanvasSize(kSizes[idx][0], kSizes[idx][1]);
            }
        });
        syncCanvasCombo();
    }

    auto preview = m_ui->FindControl("tb_preview");
    if (preview.IsValid()) {
        preview.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            togglePreview();
        });
    }

    syncZoomCombo();   // 初始档位显示（100%）
}

// 缩放档下拉同步：命中档位→选中；非档位（±/1:1 步进值）→清空（实际值由状态栏右段显示）。
void App::syncZoomCombo() {
    if (!m_canvas) return;
    auto zc = m_ui->FindControl("tb_zoom");
    if (!zc.IsValid()) return;
    static const float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    const float z = m_canvas->GetZoom();
    int idx = -1;
    for (int i = 0; i < 6; ++i)
        if (z > kZooms[i] - 0.001f && z < kZooms[i] + 0.001f) { idx = i; break; }
    m_zoomComboSync = true;
    zc.SetInt(PropertyNames::kSelectedIndex, idx);
    m_zoomComboSync = false;
}

// 画布尺寸档下拉同步：命中预设→选中；非预设（自定义）→清空。
void App::syncCanvasCombo() {
    if (!m_canvas) return;
    auto cb = m_ui->FindControl("cb_canvas");
    if (!cb.IsValid()) return;
    static const float kSizes[4][2] = { {1024, 768}, {1280, 800}, {1440, 900}, {1920, 1080} };
    const float w = m_canvas->GetCanvasW(), h = m_canvas->GetCanvasH();
    int idx = -1;
    for (int i = 0; i < 4; ++i)
        if (kSizes[i][0] == w && kSizes[i][1] == h) { idx = i; break; }
    m_canvasComboSync = true;
    cb.SetInt(PropertyNames::kSelectedIndex, idx);
    m_canvasComboSync = false;
}

// 自定义画布尺寸对话框：懒建一次（CreateDialog 自动打开→立即关闭准备复用；
// Popup::setBool(kVisible) 即 open/close），确定后应用 SetCanvasSize。
void App::openCanvasSizeDialog() {
    if (!m_canvas) return;
    if (!m_canvasSizeDlg.IsValid()) {
        m_canvasSizeDlg = m_ui->CreateDialog(u8"确定", u8"取消", 0, 0, 320, 190);
        if (!m_canvasSizeDlg.IsValid()) return;
        auto lbW = m_ui->CreateLabel(u8"宽", 16.0f, 20, 24, 40, 26);
        m_dlgCanvasW = m_ui->CreateNumericUpDown(70, 24, 140, 26);
        auto lbH = m_ui->CreateLabel(u8"高", 16.0f, 20, 64, 40, 26);
        m_dlgCanvasH = m_ui->CreateNumericUpDown(70, 64, 140, 26);
        m_dlgCanvasW.SetFloat(PropertyNames::kRangeMin, 320.0f);
        m_dlgCanvasW.SetFloat(PropertyNames::kRangeMax, 3840.0f);
        m_dlgCanvasH.SetFloat(PropertyNames::kRangeMin, 240.0f);
        m_dlgCanvasH.SetFloat(PropertyNames::kRangeMax, 2160.0f);
        m_canvasSizeDlg.AddChild(lbW);
        m_canvasSizeDlg.AddChild(m_dlgCanvasW);
        m_canvasSizeDlg.AddChild(lbH);
        m_canvasSizeDlg.AddChild(m_dlgCanvasH);
        m_canvasSizeDlg.SetCallback(PropertyNames::kEventClose, [this](const Event& e) {
            if (e.GetCloseResult() == 1 && m_canvas)   // Confirmed
                m_canvas->SetCanvasSize(m_dlgCanvasW.GetFloat(PropertyNames::kValue),
                                        m_dlgCanvasH.GetFloat(PropertyNames::kValue));
            syncCanvasCombo();
        });
        m_canvasSizeDlg.SetBool(PropertyNames::kVisible, 0);   // 首开即关（准备复用）
    }
    m_dlgCanvasW.SetFloat(PropertyNames::kValue, m_canvas->GetCanvasW());
    m_dlgCanvasH.SetFloat(PropertyNames::kValue, m_canvas->GetCanvasH());
    m_canvasSizeDlg.SetBool(PropertyNames::kVisible, 1);
}

// 资源插入/修改对话框（懒建复用）+ 系统目录选择（SHBrowseForFolderW）
void App::openResourceDialog(bool editMode) {
    if (!m_canvas) return;
    std::string initName, initPath;
    if (editMode && !m_canvas->SelectedResourceProvider(initName, initPath)) {
        m_canvas->SetStatusHint(u8"请先在资源列表中选择要修改的条目");
        return;
    }
    m_resDlgEditMode = editMode;
    if (!m_resDlg.IsValid()) {
        m_resDlg = m_ui->CreateDialog(u8"确定", u8"取消", 0, 0, 360, 240);
        if (!m_resDlg.IsValid()) return;
        auto lbN = m_ui->CreateLabel(u8"名称", 16.0f, 20, 24, 50, 26);
        m_dlgResName = m_ui->CreateEditBox(80, 24, 240, 26);
        auto lbP = m_ui->CreateLabel(u8"路径", 16.0f, 20, 64, 50, 26);
        // 路径显示：只读 EditBox（P0-83：长路径内部滚动不越界、内容不可误改、选择/复制保留）
        m_dlgResPathEdit = m_ui->CreateEditBox(80, 64, 240, 26);
        // 两种形态并存：文件（name → 单文件）/ 目录（name → 目录，引用 provider:name/相对路径）
        auto setPath = [this](const std::string& out) {
            if (out.empty()) return;
            m_dlgResPath = out;
            if (m_dlgResPathEdit.IsValid())
                m_dlgResPathEdit.SetString(PropertyNames::kTextContent, out);
        };
        if (m_dlgResPathEdit.IsValid())
            m_dlgResPathEdit.SetBool(PropertyNames::kReadOnly, true);   // P0-83：真只读（选择/复制/滚动保留）
        auto btnFile = m_ui->CreateButton(u8"选择文件…", 80, 104, 120, 28);
        btnFile.SetCallback(PropertyNames::kEventClick, [setPath](const Event&) {
            wchar_t buf[MAX_PATH] = L"";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"选择资源文件";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return;
            const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string out(len > 0 ? (size_t)(len - 1) : 0, '\0');
            if (len > 0)
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
            setPath(out);
        });
        auto btnDir = m_ui->CreateButton(u8"选择目录…", 210, 104, 120, 28);
        btnDir.SetCallback(PropertyNames::kEventClick, [setPath](const Event&) {
            BROWSEINFOW bi{};
            bi.hwndOwner = GetActiveWindow();
            bi.lpszTitle = L"选择资源目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (!pidl) return;
            wchar_t wpath[MAX_PATH] = L"";
            SHGetPathFromIDListW(pidl, wpath);
            ILFree(pidl);
            const int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
            std::string out(len > 0 ? (size_t)(len - 1) : 0, '\0');
            if (len > 0)
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out.data(), len, nullptr, nullptr);
            setPath(out);
        });
        m_resDlg.AddChild(lbN);
        m_resDlg.AddChild(m_dlgResName);
        m_resDlg.AddChild(lbP);
        m_resDlg.AddChild(m_dlgResPathEdit);
        m_resDlg.AddChild(btnFile);
        m_resDlg.AddChild(btnDir);
        m_resDlg.SetCallback(PropertyNames::kEventClose, [this](const Event& e) {
            if (e.GetCloseResult() == 1 && m_canvas) {   // Confirmed
                const std::string name =
                    m_dlgResName.IsValid() ? m_dlgResName.GetString(PropertyNames::kTextContent) : "";
                if (m_resDlgEditMode) m_canvas->UpdateSelectedResourceProvider(name, m_dlgResPath);
                else                  m_canvas->AddResourceProvider(name, m_dlgResPath);
            }
        });
        m_resDlg.SetBool(PropertyNames::kVisible, 0);   // 首开即关（准备复用）
    }
    m_dlgResPath = editMode ? initPath : "";
    m_dlgResName.SetString(PropertyNames::kTextContent, editMode ? initName : "");
    m_dlgResPathEdit.SetString(PropertyNames::kTextContent,
                               m_dlgResPath.empty() ? u8"（未选择）" : m_dlgResPath);
    m_resDlg.SetBool(PropertyNames::kVisible, 1);
}

// 菜单可用态：撤销/重做随 CanvasPane 栈状态置灰（仅变化时写；依赖引擎把带 id 的
// 菜单项注册进 id 表——FindControl 可达；清单 P0-87。未注册前 FindControl 无效 = 无操作）
void App::refreshMenuStates() {
    if (!m_canvas || !m_ui) return;
    const bool canUndo = m_canvas->CanUndo();
    const bool canRedo = m_canvas->CanRedo();
    if (canUndo != m_menuUndoEnabled) {
        Control c = m_ui->FindControl("miUndo");
        if (c.IsValid()) {
            c.SetBool(PropertyNames::kEnabled, canUndo);
            m_menuUndoEnabled = canUndo;
        }
    }
    if (canRedo != m_menuRedoEnabled) {
        Control c = m_ui->FindControl("miRedo");
        if (c.IsValid()) {
            c.SetBool(PropertyNames::kEnabled, canRedo);
            m_menuRedoEnabled = canRedo;
        }
    }
}

// 预览：**独立窗体**呈现（引擎多实例）——未打开则创建并加载当前布局快照；
// 已打开则重建刷新（引擎 LoadLayout 不清旧控件——重建最稳）。窗体关闭由
// pumpPreviewWindow 检测回收。
void App::togglePreview() {
    if (!m_canvas) return;
    if (!m_previewUi) {
        openPreviewWindow();
        return;
    }
    closePreviewWindow();
    openPreviewWindow();
}

// 创建预览实例（独立窗口；尺寸=设计画布）+ 加载当前布局快照（含资源挂载点）
void App::openPreviewWindow() {
    if (!m_canvas || !m_ui) return;
    const int w = (int)m_canvas->GetCanvasW();
    const int h = (int)m_canvas->GetCanvasH();
    // 画布（Bench）窗口标题（用户设置非空时采用——设计产物窗口语义）
    std::string title = m_canvas->BenchWindowTitle();
    if (title.empty()) title = u8"预览 - CornerstoneDesigner";
    m_previewUi = UICornerstone::UICornerstone::Create(
        UICornerstone::UICornerstone::Config{}
            .WithBackend("sdl3")
            .WithResourceRoot(exeRelative("assets"))
            .WithWindow(title, w, h)
            .WithWindowFlags(UIWindowFlags::Resizable));
    if (!m_previewUi) {
        m_canvas->SetStatusHint(u8"预览窗口创建失败");
        std::printf("[App] preview instance create FAILED\n");
        return;
    }
    const nlohmann::json layout = m_canvas->serializeLayoutJson();
    const bool hasControls = layout.contains("controls") &&
                             layout["controls"].is_array() && !layout["controls"].empty();
    if (hasControls && !m_previewUi->LoadLayout(layout.dump())) {   // 空设计 → 空预览窗（引擎空布局返回 0）
        m_canvas->SetStatusHint(u8"预览布局加载失败");
        std::printf("[App] preview LoadLayout FAILED: %s\n", m_previewUi->GetLastError().c_str());
        closePreviewWindow();
        return;
    }
    if (auto pb = m_ui->FindControl("tb_preview"); pb.IsValid())
        pb.SetString(PropertyNames::kCaption, u8"刷新预览");
    m_canvas->SetStatusHint(u8"预览窗口已打开（再次点击刷新；关闭窗口退出）");
    std::printf("[App] preview window opened (%dx%d)\n", w, h);
}

void App::closePreviewWindow() {
    if (m_previewUi) {
        m_previewUi.reset();
        std::printf("[App] preview window closed\n");
    }
    if (m_ui) {
        auto pb = m_ui->FindControl("tb_preview");
        if (pb.IsValid()) pb.SetString(PropertyNames::kCaption, u8"预览");
    }
}

// 主循环内泵预览实例（多实例：各自 ProcessEvents/Update/Render/Present）；
// 窗体关闭（IsQuitRequested）→ 回收实例 + 恢复按钮标题
void App::pumpPreviewWindow(double delta) {
    if (!m_previewUi) return;
    m_previewUi->ProcessEvents();
    if (m_previewUi->IsQuitRequested()) {
        closePreviewWindow();
        return;
    }
    m_previewUi->Update(delta);
    m_previewUi->Clear();
    m_previewUi->Render();
    m_previewUi->RenderOverlays();   // P0-52①：浮层通道（菜单弹层/下拉/右键菜单——预览缺此致主菜单点击"无反应"）
    if (m_previewCheckPending) {     // auto=3 校验（Present 前——readback 读当前缓冲）
        m_previewCheckPending = false;
        runPreviewChecks();
    }
    m_previewUi->Present();
}

// auto=3：预览实例校验（颜色状态 + 像素捕获——Bench 背景 / 图片 / 动画内容）。
void App::runPreviewChecks() {
    if (!m_previewUi || !m_canvas) return;
    // StatusBar 部分对象语义：仅设 hover → normal 保持构造缺省蓝 (0,122,204)；
    // hover 须为样例绿（用户值应用）
    {
        auto sc = m_previewUi->FindControl(m_canvas->SelfTestIdOfType("status-bar"));
        bool ok = false;
        if (sc.IsValid()) {
            UIColor c = sc.GetColor(PropertyNames::kBackground);
            UIColor h = sc.GetColor(PropertyNames::kStateHover);
            ok = (c.r == 0 && c.g == 122 && c.b == 204) &&
                 (h.r == 0 && h.g == 255 && h.b == 0);
        }
        std::printf("[AUTO3] preview statusbar partial-color %s\n", ok ? "OK" : "FAIL");
    }
    // 菜单栏 pressed 色（用户问题 2：预览态生效回归——读回 m_activeBgColor）
    {
        auto mb = m_previewUi->FindControl(m_canvas->SelfTestIdOfType("menu-bar"));
        bool ok = false;
        if (mb.IsValid()) {
            UIColor c = mb.GetColor(PropertyNames::kStatePressed);
            ok = (c.r == 255 && c.g == 128 && c.b == 0);
        }
        std::printf("[AUTO3] preview menu pressed %s\n", ok ? "OK" : "FAIL");
    }
    // Bench 背景像素（样例灰 #C0C0C0FF——viewport.background 预览生效）
    {
        static std::vector<uint8_t> buf((size_t)1280 * 800 * 4);
        int w = 0, h = 0;
        bool bgOk = false;
        if (m_previewUi->CaptureBench(buf.data(), &w, &h) && w > 0 && h > 0) {
            int gray = 0;
            const size_t n = (size_t)w * h;
            const uint32_t* px = reinterpret_cast<const uint32_t*>(buf.data());
            for (size_t i = 0; i < n; ++i) {
                const uint32_t c = px[i];
                const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                if (r >= 150 && r <= 220 && g >= 150 && g <= 220 && b >= 150 && b <= 220 &&
                    std::abs(r - g) < 24 && std::abs(g - b) < 24)
                    ++gray;   // 灰主导（readback 字节序 [R,G,B,A]；alpha 不可靠不查 A）
            }
            bgOk = gray > (int)(n / 4);   // 背景占多数（控件覆盖其余）
        }
        std::printf("[AUTO3] preview bench bg %s\n", bgOk ? "OK" : "FAIL");
    }
    // 图片/动画内容像素（非透明占比——预览资源加载回归，用户问题 5）
    for (const char* tp : { "image", "animation" }) {
        auto ic = m_previewUi->FindControl(m_canvas->SelfTestIdOfType(tp));
        bool contentOk = false;
        if (ic.IsValid()) {
            static std::vector<uint8_t> cbuf((size_t)512 * 512 * 4);
            int w = 0, h = 0;
            if (m_previewUi->CaptureControl(ic, cbuf.data(), &w, &h) && w > 0 && h > 0) {
                int vis = 0;
                const size_t n = (size_t)w * h;
                const uint32_t* px = reinterpret_cast<const uint32_t*>(cbuf.data());
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t c = px[i];
                    const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                    // 非灰底像素（控件内容叠在样例灰画布上）——readback 字节序 [R,G,B,A]
                    const bool grayish = r >= 150 && r <= 220 && g >= 150 && g <= 220 &&
                                         b >= 150 && b <= 220 &&
                                         std::abs(r - g) < 24 && std::abs(g - b) < 24;
                    if (!grayish) ++vis;
                }
                contentOk = vis > (int)(n / 50);   // ≥2% 可见
                UICornerstone::UICornerstone::SavePixelsToFile(
                    cbuf.data(), w, h, std::string("Temp/preview_") + tp + ".bmp");
            }
        }
        std::printf("[AUTO3] preview %s content %s\n", tp, contentOk ? "OK" : "FAIL");
        if (std::string(tp) == "animation" && ic.IsValid())
            std::printf("[AUTO3] preview animation playing=%d loop=%d total=%d\n",
                        ic.GetBool(PropertyNames::kPlaying),
                        ic.GetBool(PropertyNames::kLoop),
                        ic.GetInt(PropertyNames::kTotalFrames));
    }
}

int App::run(int autoSeconds) {
    std::chrono::steady_clock::time_point deadline;
    bool autoMode = autoSeconds > 0;
    if (autoMode) {
        // auto=3（预览冒烟）需更多帧完成像素校验（Present 前捕获额外一帧）
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::seconds(autoSeconds == 3 ? 8 : autoSeconds);
        std::printf("[App] auto=%d seconds\n", autoSeconds);
    }

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    int frame = 0;

    while (!m_ui->IsQuitRequested() && !m_exitRequested) {
        m_ui->ProcessEvents();
        if (m_sdl.pollF5Key()) togglePreview();   // 预览模式切换（菜单快捷键 F5）
        {   // F2.5 撤销/重做快捷键（Ctrl+Z / Ctrl+Y；每帧轮询保持边沿状态）
            const bool undoEdge = m_sdl.pollUndoKey();
            const bool redoEdge = m_sdl.pollRedoKey();
            if ((undoEdge || redoEdge) && !m_previewUi && m_canvas) {
                if (undoEdge) m_canvas->Undo();
                else m_canvas->Redo();
            }
        }
        refreshMenuStates();   // 菜单"撤销/重做"可用态（引擎按 item id 注册后生效——清单 P0-87）
        if (autoMode && autoSeconds == 1 && frame == 30) {
            // 工具条宽度跟随校验（canvasToolbar top-stretch）＋菜单项 id 可达性探针
            auto tb = m_ui->FindControl("canvasToolbar");
            auto ca = m_ui->FindControl("canvasArea");
            if (tb.IsValid() && ca.IsValid()) {
                const float bw = tb.GetRect().w, aw = ca.GetRect().w;
                std::printf("[AUTO1] toolbar w=%.0f canvasArea w=%.0f -> %s\n",
                            bw, aw, (std::fabs(bw - aw) < 1.0f) ? "STRETCH-OK" : "STRETCH-FAIL");
            }
            // P0-87 + F2.5 端到端：菜单项可达 + 空撤销栈应已置灰（refreshMenuStates 首帧写入）
            auto mi = m_ui->FindControl("miUndo");
            const bool miOk = mi.IsValid();
            const bool miDisabled = miOk && !mi.GetBool(PropertyNames::kEnabled);
            auto mr = m_ui->FindControl("miRedo");
            const bool mrDisabled = mr.IsValid() && !mr.GetBool(PropertyNames::kEnabled);
            std::printf("[AUTO1] miUndo valid=%d disabled=%d miRedo disabled=%d -> %s\n",
                        miOk ? 1 : 0, miDisabled ? 1 : 0, mrDisabled ? 1 : 0,
                        (miOk && miDisabled && mrDisabled) ? "MENU-STATE-OK" : "MENU-STATE-PENDING");
        }
        if (m_canvas) m_canvas->ProcessViewportEvents();   // 子视口事件循环（未消费事件回退 owner）
        syncRootToWindow();

        // auto=2：项目 IO 自测（放置→保存→打开→导出→导入往返；一次性）
        if (autoMode && autoSeconds == 2 && frame == 30 && m_canvas) {
            m_canvas->SelfTestIo("Temp");
        }
        // auto=3：预览独立窗体冒烟（放置样例 → 多实例开→泵→退出）；校验延迟到
        // 泵内 Present 前（readback 须在缓冲交换前执行）
        if (autoMode && autoSeconds == 3 && frame == 30) {
            if (m_canvas) m_canvas->SelfTestPlaceSample();
            togglePreview();
            m_previewCheckPending = (bool)m_previewUi;
            if (m_previewCheckPending) pumpPreviewWindow(0.016);   // 立即泵一帧（Present 前跑校验）
        }

        auto now = Clock::now();
        double delta = std::chrono::duration<double>(now - last).count();
        last = now;

        if (autoMode && Clock::now() >= deadline) {
            std::printf("[AUTO] done, %d frames\n", frame);
            break;
        }

        m_ui->Update(delta);
        if (m_canvas) {
            m_canvas->TickInput();                    // 画布交互（放置/选中/拖动）
            m_canvas->UpdateViewport((float)delta);
        }
        // 布局引擎在 Update 中重排，须在其后读取 canvasArea 矩形做稳定检测
        if (m_canvas) m_canvas->TickLayout((float)delta);
        m_ui->Clear();
        m_ui->Render();
        if (m_canvas) m_canvas->RenderViewport();
        m_ui->RenderOverlays();   // P0-52①：owner 浮层（下拉/菜单/弹窗）重绘于画布子视口之上
        m_ui->Present();
        pumpPreviewWindow(delta);   // 预览独立窗体（多实例泵；含关闭回收）

        if (frame == 0) {
            std::printf("[App] skeleton running (1920x1080)\n");
        }
        frame++;
    }

    std::printf("[App] exit cleanly, %d frames\n", frame);
    return 0;
}

void App::shutdown() {
    if (m_ui) m_ui->Shutdown();
}

}  // namespace Designer
