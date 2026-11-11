// Launcher.cpp
// CornerstoneLauncher：独立进程启动器 + 代码创建的加载界面。
// 只负责显示阶段进度；主窗口由 CornerstoneDesigner 自己通过 UICorstone::Create 创建。

#include "UICornerstone.h"
#include "Control.h"
#include "PropertyNames.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::string exeDirPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos);
    int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
#else
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    std::string path(buf, (size_t)n);
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
#endif
}

std::string exeRelative(const std::string& rel) {
    return exeDirPath() + "/" + rel;
}

#ifdef _WIN32
static void ensureLauncherConsole() {
    // Launcher 为 WIN32 子系统；从 Explorer 启动时没有 Console。附着调用方 Console，
    // 没有则创建，并把 stdio 指到 CONOUT$/CONIN$。子进程继承该 Console 后日志可持续输出。
    if (!GetConsoleWindow()) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
}
#endif

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n - 1 : 0), 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}
static std::wstring quoteArg(const std::string& s) {
    std::wstring w = widen(s);
    std::wstring out;
    out.push_back(L'"');
    out += w;
    out.push_back(L'"');
    out.push_back(L' ');
    return out;
}
#endif

void launcherLog(const std::string& text) {
    std::ofstream f(std::filesystem::u8path(exeRelative("launcher.log")), std::ios::app);
    if (f) { f << text; f.put(10); }
}

struct Stage {
    int progress = 0;
    std::string text;
};

Stage stageFor(const std::string& phase) {
    if (phase == "init") return {5, u8"正在初始化…"};
    if (phase == "layout_start") return {20, u8"正在加载布局…"};
    if (phase == "layout_done") return {75, u8"正在初始化界面…"};
    if (phase == "canvas") return {80, u8"正在准备画布…"};
    if (phase == "recent") return {85, u8"正在加载最近项目…"};
    if (phase == "theme_start") return {90, u8"正在应用主题…"};
    if (phase == "theme_done") return {95, u8"正在收尾…"};
    if (phase == "ready") return {100, u8"启动完成"};
    if (phase == "error") return {0, u8"启动失败"};
    return {0, u8"正在启动…"};
}

struct LauncherUi {
    std::unique_ptr<UICornerstone::UICornerstone> ui;
    Control panel;
    Control title;
    Control status;
    Control bar;
    std::string statusBase = u8"正在启动…";
    int progress = 0;
    bool ready = false;
    bool failed = false;

    bool create() {
        ui = UICornerstone::UICornerstone::Create(
            UICornerstone::UICornerstone::Config{}
                .WithBackend("sdl3")
                .WithResourceRoot(exeRelative("assets"))
                .WithWindow("CornerstoneDesigner", 640, 360)
                .WithWindowFlags(UIWindowFlags::Borderless | UIWindowFlags::Hidden |
                                 UIWindowFlags::NoShadow | UIWindowFlags::Utility));
        if (!ui) return false;
        ui->SetViewportBackgroundColor(0x1B, 0x1F, 0x27, 255);

        const UIColor bg{0x1B, 0x1F, 0x27, 255};
        const UIColor panelColor{0x25, 0x2B, 0x35, 255};
        const UIColor text{0xDC, 0xE5, 0xF2, 255};
        const UIColor muted{0x9A, 0xA7, 0xB8, 255};
        const UIColor accent{0x4C, 0x8D, 0xFF, 255};

        UIRect vp = ui->GetViewport();
        float ww = 0, wh = 0;
        ui->GetWindowSize(ww, wh);
        float vw = ww > 0 ? ww : (vp.w > 0 ? vp.w : 640.0f);
        float vh = wh > 0 ? wh : (vp.h > 0 ? vp.h : 360.0f);
        launcherLog("layout window=" + std::to_string((int)ww) + "x" + std::to_string((int)wh) +
                    " viewport=" + std::to_string((int)vp.w) + "x" + std::to_string((int)vp.h));
        const float barW = (std::min)(440.0f, vw * 0.70f);

        panel = ui->CreatePanel(0, 0, vw, vh);
        panel.SetColor(PropertyNames::kBackground, bg);
        panel.SetBool(PropertyNames::kBorderVisible, false);

        title = ui->CreateLabel("CornerstoneDesigner", 30, 0, vh * 0.30f, vw, 40);
        title.SetStateColor(PropertyNames::kText, UIStateColor{text, text, text, text});
        title.SetString(PropertyNames::kCaption, "CornerstoneDesigner");
        title.SetEnum(PropertyNames::kAlign, PropertyNames::kAlignCenter);

        status = ui->CreateLabel(statusBase, 16, 0, vh * 0.49f, vw, 28);
        status.SetStateColor(PropertyNames::kText, UIStateColor{muted, muted, muted, muted});
        status.SetEnum(PropertyNames::kAlign, PropertyNames::kAlignCenter);

        bar = ui->CreateProgressBar((vw - barW) * 0.5f, vh * 0.64f, barW, 16);
        bar.SetFloat(PropertyNames::kValue, 0.0f);
        bar.SetFloat(PropertyNames::kRangeMax, 100.0f);
        bar.SetColor(PropertyNames::kProgress, accent);
        bar.SetColor(PropertyNames::kBackground, panelColor);
        bar.SetColor(PropertyNames::kText, muted);
        ui->SetWindowAlwaysOnTop(true);
        ui->SetWindowVisible(true);
        ui->SetWindowForeground();
        return true;
    }

    void setStage(const Stage& s) {
        progress = s.progress;
        if (bar.IsValid()) bar.SetFloat(PropertyNames::kValue, (float)progress);
        statusBase = s.text;
        if (status.IsValid()) status.SetString(PropertyNames::kCaption, statusBase);
    }

    void pump(double dt) {
        if (!ui) return;
        ui->ProcessEvents();
        ui->Update(dt);
        ui->Clear();
        ui->Render();
        ui->RenderOverlays();
        ui->Present();
    }
};

struct Win32Child {
    HANDLE process = nullptr;
    DWORD pid = 0;
    bool exited = false;
    int exitCode = 0;

    bool start(const std::vector<std::string>& args) {
        std::wstring cmd;
        for (const auto& a : args) cmd += quoteArg(a);
        std::wstring dir = widen(exeDirPath());
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                            0, nullptr, dir.c_str(), &si, &pi)) {
            std::string err = "CreateProcess failed: " + std::to_string((unsigned long)GetLastError());
            launcherLog(err);
            std::fputs("[Launcher] ", stderr); std::fputs(err.c_str(), stderr); std::fputc(10, stderr);
            return false;
        }
        process = pi.hProcess; pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        return true;
    }
    bool pollExit() {
        if (!process) return false;
        if (!exited && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            exited = true;
            DWORD code = 0; GetExitCodeProcess(process, &code); exitCode = (int)code;
        }
        return exited;
    }
    void close(bool terminate) {
        if (!process) return;
        if (terminate && !exited) TerminateProcess(process, 1);
        CloseHandle(process); process = nullptr;
    }
    ~Win32Child() { close(true); }
};
}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    ensureLauncherConsole();
#endif
    LauncherUi shell;
    if (!shell.create()) {
        launcherLog("UICorstone create failed");
        MessageBoxW(nullptr, L"Launcher 初始化失败：UICorstone 无法创建。", L"CornerstoneDesigner", MB_OK | MB_ICONERROR);
        return 1;
    }
    shell.pump(0.0);

    const std::string dir = exeDirPath();
    const std::string progressPath = exeRelative("launcher_progress.log");
    std::remove(progressPath.c_str());

    std::vector<std::string> args;
    args.push_back(dir + "/CornerstoneDesigner.exe");
    args.push_back("--startup-hidden");
    args.push_back("--startup-progress-file=" + progressPath);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--startup-hidden") == 0 ||
            strncmp(argv[i], "--startup-progress", 18) == 0)
            continue;
        args.push_back(argv[i]);
    }

    Win32Child child;
    if (!child.start(args)) {
        launcherLog("process start failed");
        shell.failed = true;
        shell.setStage({0, u8"无法启动 CornerstoneDesigner"});
        shell.pump(0.0);
        return 1;
    }

    launcherLog("process started: pid=" + std::to_string(child.pid));
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    auto last = start;
    bool timeout = false;
    std::string buffer;
    std::streamoff offset = 0;

    auto readProgress = [&]() {
        std::ifstream f(std::filesystem::u8path(progressPath), std::ios::binary);
        if (!f) return;
        f.seekg(offset);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        offset += (std::streamoff)data.size();
        buffer += data;
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            char phase[64] = {0};
            int progress = 0;
            if (std::sscanf(line.c_str(),
                            "[StartupProgress] {\"phase\":\"%63[^\"]\",\"progress\":%d}",
                            phase, &progress) == 2) {
                shell.setStage(stageFor(phase));
                if (strcmp(phase, "ready") == 0) shell.ready = true;
                if (strcmp(phase, "error") == 0) shell.failed = true;
            }
        }
    };

    while (!shell.ui->IsQuitRequested()) {
        readProgress();
        if (shell.ready || shell.failed) break;
        auto now = Clock::now();
        if (now - start > std::chrono::seconds(30)) {
            timeout = true;
            shell.failed = true;
            shell.setStage({0, u8"启动超时"});
            break;
        }
        if (child.pollExit()) {
            shell.failed = true;
            shell.setStage({0, u8"CornerstoneDesigner 已退出"});
            break;
        }
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0.1) dt = 0.1;
        shell.pump(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (timeout || shell.failed) {
        child.close(true);
        shell.pump(0.0);
        launcherLog("startup failed: " + shell.statusBase);
        return 1;
    }
    if (shell.ready) {
        launcherLog("ready: pid=" + std::to_string(child.pid));
        shell.pump(0.0);
        child.close(false);
        std::fputs("[Launcher] ready", stdout); std::fputc(10, stdout);
        shell.ui->Shutdown();
        return 0;
    }

    child.close(true);
    shell.ui->Shutdown();
    return 0;
}
