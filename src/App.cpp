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
#else
    getWindows   = reinterpret_cast<void*(*)(int*)>(dlsym(dll, "SDL_GetWindows"));
    setWindowSize = reinterpret_cast<void*(*)(void*, int, int)>(dlsym(dll, "SDL_SetWindowSize"));
    getWindowSize = reinterpret_cast<int(*)(void*, int*, int*)>(dlsym(dll, "SDL_GetWindowSize"));
    getMouseState = reinterpret_cast<unsigned(*)(float*, float*)>(dlsym(dll, "SDL_GetMouseState"));
    getKeyboardState = reinterpret_cast<const unsigned char*(*)(int*)>(dlsym(dll, "SDL_GetKeyboardState"));
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
        // [诊断] middleArea 各面板引擎重排后的实际 rect（ratio 语义定位）
        auto ma = m_ui->FindControl("middleArea");
        if (ma.IsValid()) {
            const char* ids[] = { "toolboxPanel", "canvasArea", "propertyPanel" };
            for (const char* id : ids) {
                auto p = m_ui->FindControl(id);
                if (p.IsValid())
                    std::printf("[DBG-layout] %s rect=(%g,%g,%g,%g)\n", id,
                                p.GetRect().x, p.GetRect().y,
                                p.GetRect().w, p.GetRect().h);
            }
            std::fflush(stdout);
        }
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
    if (!loadSkeleton()) {
        std::printf("[App] LoadLayout failed: %s\n", m_lastError.c_str());
        return false;
    }
    m_sdl.load();
    if (!m_sdl.loaded) {
        std::printf("[App] SDL3.dll 未解析（窗口 resize 同步降级）\n");
    }
    registerActions();
    setupCanvas();
    bindToolbar();
    if (m_canvas) {
        m_canvas->SetMouseSource([this](float& x, float& y) { return m_sdl.pollMouse(x, y); });
        m_canvas->SetKeySource([this] { return m_sdl.pollDeleteKey(); });
        m_canvas->SetCanvasTree(m_ui->FindControl("tvTree"));
        m_canvas->LoadSchema(exeRelative("schema/declarative-ui.schema.json"));
        m_canvas->SetDynamicPropsContainer(m_ui->FindControl("propDynamic"));
        // 滚轮双订阅：动态区内 propDynamic 消费；面板其他区域由 tabProperty 兜底
        // （最近订阅者语义——内层无订阅 Panel 透传，修复后由本层消费）
        m_ui->FindControl("tabProperty").SetCallback(
            PropertyNames::kEventMouseWheel, [this](const Event& e) {
                m_canvas->ScrollDynamicBy(e.GetValueChanged());
            });
        m_canvas->SetToolboxTree(m_ui->FindControl("tvToolbox"));
        m_canvas->SetPropertyControls(m_ui->FindControl("prop_x"),
                                      m_ui->FindControl("prop_y"),
                                      m_ui->FindControl("prop_w"),
                                      m_ui->FindControl("prop_h"),
                                      m_ui->FindControl("prop_caption"),
                                      m_ui->FindControl("prop_color"),
                                      m_ui->FindControl("prop_visible"),
                                      m_ui->FindControl("prop_enabled"));
    }

    // 初始化基准尺寸
    if (m_sdl.loaded && m_sdl.getWindowSize) {
        void* win = m_sdl.windowHandle();
        if (win) m_sdl.getWindowSize(win, &m_lastWinW, &m_lastWinH);
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
    for (const char* name : actions) {
        if (std::string(name) == "onMenuExit") {
            m_ui->RegisterAction(name, [this](Control) { m_exitRequested = true; });
        } else if (std::string(name) == "onMenuAbout") {
            m_ui->RegisterAction(name, [this](Control) {
                std::printf("[App] CornerstoneDesigner v0.1 (stage 1 skeleton)\n");
            });
        } else {
            m_ui->RegisterAction(name, placeholder);
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
            m_canvas->SetGridVisible(e.GetCheckState() != 0);
        });
    }

    auto snapCtl = m_ui->FindControl("tb_snap");
    if (snapCtl.IsValid()) {
        snapCtl.SetBool(PropertyNames::kTriState, false);   // 两态开关
        snapCtl.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
            m_canvas->SetSnapEnabled(e.GetCheckState() != 0);
        });
    }

    auto zoomCtl = m_ui->FindControl("tb_zoom");
    if (zoomCtl.IsValid()) {
        zoomCtl.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event& e) {
            static const float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            int idx = e.GetSelectedIndex();
            if (idx >= 0 && idx < 6) m_canvas->SetZoom(kZooms[idx]);
        });
    }

    auto zoomIn = m_ui->FindControl("tb_zoom_in");
    if (zoomIn.IsValid()) {
        zoomIn.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            float z = m_canvas->GetZoom();
            m_canvas->SetZoom(z < 2.0f ? z + 0.25f : 2.0f);
        });
    }

    auto zoomOut = m_ui->FindControl("tb_zoom_out");
    if (zoomOut.IsValid()) {
        zoomOut.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            float z = m_canvas->GetZoom();
            m_canvas->SetZoom(z > 0.25f ? z - 0.25f : 0.25f);
        });
    }

    auto zoom100 = m_ui->FindControl("tb_zoom_100");
    if (zoom100.IsValid()) {
        zoom100.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            m_canvas->SetZoom(1.0f);
        });
    }

    auto preview = m_ui->FindControl("tb_preview");
    if (preview.IsValid()) {
        preview.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            std::printf("[App] preview toggle not implemented yet\n");
        });
    }
}

int App::run(int autoSeconds) {
    std::chrono::steady_clock::time_point deadline;
    bool autoMode = autoSeconds > 0;
    if (autoMode) {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(autoSeconds);
        std::printf("[App] auto=%d seconds\n", autoSeconds);
    }

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    int frame = 0;

    {   // [诊断] 引擎初始重排后的 middleArea 面板实际 rect（ratio 语义定位，一次性）
        const char* ids[] = { "toolboxPanel", "canvasArea", "propertyPanel" };
        for (const char* id : ids) {
            auto p = m_ui->FindControl(id);
            if (p.IsValid())
                std::printf("[DBG-layout] %s rect=(%g,%g,%g,%g)\n", id,
                            p.GetRect().x, p.GetRect().y,
                            p.GetRect().w, p.GetRect().h);
        }
        std::fflush(stdout);
    }

    while (!m_ui->IsQuitRequested() && !m_exitRequested) {
        m_ui->ProcessEvents();
        if (m_canvas) m_canvas->ProcessViewportEvents();   // 子视口事件循环（未消费事件回退 owner）
        syncRootToWindow();

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
        m_ui->Present();

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
