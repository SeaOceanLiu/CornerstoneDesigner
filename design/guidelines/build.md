# 构建与测试

## 克隆（含子模块）

```batch
git clone --recursive git@github.com:SeaOceanLiu/CornerstoneDesigner.git
```

## 首次构建：先编译 UICornerstone（提供运行依赖 DLL）

设计器通过 **C++ Binding（纯动态加载）** 使用 UICornerstone（依赖基线 **v1.0.0**，tag commit `202346a5`），运行依赖核心 DLL 与后端 DLL（不链接核心静态库）。

方式 A（默认，子模块源码）：

```batch
cd subModules\UICornerstone
build_scripts\build.bat sdl3
```

方式 B（v1.0.0 发布包）：下载 GitHub Releases 的 `UICornerstone_full_v1.0.0.rar` 解压，核心/后端 DLL + assets + binding 源码即用（官方含 `binding/CMakeLists.example.txt` 独立集成样例）。

产出：`UICornerstone.dll` + `UIBackend_sdl3.dll` + SDL3 运行库与 assets（CMake POST_BUILD 自动拷贝到设计器输出目录）。

## 构建设计器

```batch
build_scripts\build.bat sdl3
```

- 输出目录：`build\{backend}\Debug\`（不是 `build\Debug`）
- 构建脚本自动调用 VsDevCmd.bat —— 不要从普通 cmd.exe 运行

## 运行

```batch
cd build\sdl3\Debug
CornerstoneDesigner.exe
```

## 测试

- 核心逻辑测试（无 UI 依赖的单元测试）：测试目标在 `build\{backend}\test\Debug\`
- 设计器冒烟/集成测试支持 `auto=<秒>` 无人值守参数（详见 testing.md）

## 提交前验证（按顺序）

1. `cmake --build build --config Debug` — 必须编译通过、零警告
2. 运行任何存在的测试
3. 检查子模块：`git submodule status` — 指向正确 commit

## 重要说明

- 构建脚本会自动调用 `VsDevCmd.bat`（注意路径），不要从普通 cmd.exe 运行
- `NOMINMAX` 必须出现在任何 `#include` 之前（SDL3 头传递引入 windows.h，否则 `std::min/std::max` 冲突）
- 若运行时行为与源码不符，先检查输出目录下 DLL 时间戳是否最新（库代码改动后需重新编译 UICornerstone）
- 仅使用 SDL3 后端（设计器默认且唯一验证后端）；SFML/Raylib 后端不在本阶段验证范围
