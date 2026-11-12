# 构建与测试

## 克隆（含子模块）

```batch
git clone --recursive git@github.com:SeaOceanLiu/CornerstoneDesigner.git
```

已有仓库若子模块为空：

```batch
git submodule update --init --recursive
```

## 依赖：UICornerstone 发布子模块

设计器通过 **C++ Binding（纯动态加载）** 使用 UICornerstone，运行依赖核心 DLL 与后端 DLL（不链接核心静态库）。

`subModules/UICornerstone` 是 **UICornerstone-release** 发布目录，包含：

```text
UICornerstone.dll
UIBackend_sdl3.dll
SDL3*.dll
assets/
binding/
tools/
```

升级引擎时，将新的 release 目录同步到 `subModules/UICornerstone`（DLL、后端、assets、binding、tools 一并更新），不需要在本仓库内编译 UICornerstone。

## 构建设计器

```batch
build_scripts\build.bat sdl3
```

- 输出目录：`build\{backend}\Debug\`（不是 `build\Debug`）
- 构建脚本自动调用 VsDevCmd.bat，不要从普通 cmd.exe 运行
- 当前验证后端为 SDL3；SFML/Raylib 不在设计器运行环境验证范围

## 运行

```batch
cd build\sdl3\Debug
CornerstoneLauncher.exe
```

或直接运行 `CornerstoneDesigner.exe`（跳过启动界面）。

## 测试

- 设计器回归：`CornerstoneDesigner.exe auto=2`、`auto=3`
- 布局校验：`subModules\UICornerstone\tools\validate_layout.exe --strict layouts\main_layout.json`
- 测试前后可运行 `build_scripts\cleanup_processes.bat` 清理残留进程
- 测试规范详见 [testing.md](testing.md)

## 发布包

```batch
build_scripts\make_release.bat
```

输出到项目根目录 `release/`（已加入 `.gitignore`），包含 Release exe、运行时 DLL、`assets/`、`themes/`、`schema/`、`sdk/`、用户手册、README、LICENSE，以及官方 `vc_redist.x64.exe` + `install_vc_runtime.bat` + `VC_Runtime_README_zh-CN.txt`；不包含日志、`designer_settings.json` 或 `Temp/`。

Release 使用 MSVC 动态运行库；目标机器需要 **Microsoft Visual C++ 2015–2022 x64 运行库**。建议随包提供微软官方 `VC_redist.x64.exe`，不要只复制单个运行库 DLL。

Release 版 Designer/Launcher 使用 Windows GUI 子系统，不打开新的 Console 窗口；Designer 日志追加到 exe 同目录 `designer.log`，Launcher 日志写入 `launcher.log`。Debug 版仍保留 Console 便于开发。

## 提交前验证（按顺序）

1. `cmake --build build/sdl3 --config Debug` — 必须编译通过、零警告
2. 运行 `CornerstoneDesigner.exe auto=2`、`auto=3` 和 strict 布局校验
3. 检查子模块：`git submodule status` — 指向正确 commit

## 重要说明

- 构建脚本会自动调用 `VsDevCmd.bat`（注意路径），不要从普通 cmd.exe 运行
- `NOMINMAX` 必须出现在任何 `#include` 之前（SDL3 头传递引入 windows.h，否则 `std::min/std::max` 冲突）
- 仅使用 SDL3 后端（设计器默认且唯一验证后端）；SFML/Raylib 后端不在本阶段验证范围
