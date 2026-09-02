// App.cpp
// CornerstoneDesigner
//
// Embedded 循环 + LoadLayout 骨架 + RegisterAction 桥接（阶段一）。

#include "Designer/App.h"

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
#else
    getWindows   = reinterpret_cast<void*(*)(int*)>(dlsym(dll, "SDL_GetWindows"));
    setWindowSize = reinterpret_cast<void*(*)(void*, int, int)>(dlsym(dll, "SDL_SetWindowSize"));
    getWindowSize = reinterpret_cast<int(*)(void*, int*, int*)>(dlsym(dll, "SDL_GetWindowSize"));
#endif
    loaded = getWindows && getWindowSize;
    return loaded;
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
    }

    m_lastWinW = w;
    m_lastWinH = h;
}

bool App::init() {
    m_ui = UICornerstone::UICornerstone::Create(
        UICornerstone::UICornerstone::Config{}
            .WithBackend("sdl3")
            .WithResourceRoot(exeRelative("assets"))
            .WithWindow("CornerstoneDesigner", 1200, 800)
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

    while (!m_ui->IsQuitRequested() && !m_exitRequested) {
        m_ui->ProcessEvents();
        syncRootToWindow();

        if (autoMode && Clock::now() >= deadline) {
            std::printf("[AUTO] done, %d frames\n", frame);
            break;
        }

        auto now = Clock::now();
        double delta = std::chrono::duration<double>(now - last).count();
        last = now;

        m_ui->Update(delta);
        m_ui->Clear();
        m_ui->Render();
        m_ui->Present();

        if (frame == 0) {
            std::printf("[App] skeleton running (1200x800)\n");
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
