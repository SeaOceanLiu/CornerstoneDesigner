# Dependencies (Submodules)

CornerstoneDesigner 仅直接依赖一个子模块：**UICornerstone**。UICornerstone 自身的依赖（SDL3/SDL3_ttf/SDL3_image/json/assets/libs/SFML/raylib）随其递归获取，设计器不直接引用。

| Submodule | Path | Source | 用途 |
| --------- | ---- | ------ | ---- |
| UICornerstone | `subModules/UICornerstone` | SeaOceanLiu/UICornerstone | UI 框架 + C++ Binding + LayoutParser + 全部控件 + 构建脚本 |

## 集成方式

设计器经 **C++ Binding** 使用 UICornerstone，依赖基线为 **v1.0.0**（tag commit `202346a5`）：

- 编译期：链接 `UICornerstoneBinding.lib`（静态，纯动态加载封装，零链接期依赖）
- 运行期：`LoadLibrary` 动态加载 `UICornerstone.dll`（核心）+ `UIBackend_sdl3.dll`（后端）+ `SDL3.dll`/`SDL3_ttf.dll`/`SDL3_image.dll` + `assets/` 资源目录
- 上述运行依赖由 CMake POST_BUILD 自动拷贝到设计器输出目录（参照 UICornerstone `binding/CMakeLists.txt` 的拷贝模式）

### 两种获取方式

| 方式 | 说明 |
| ---- | ---- |
| **子模块源码**（默认） | `git clone --recursive` + `subModules\UICornerstone\build_scripts\build.bat sdl3`，产出核心/后端 DLL |
| **v1.0.0 发布包** | `UICornerstone_full_v1.0.0.rar`（GitHub Releases）：解压即得核心/三后端 DLL + SDL3/SFML/raylib 运行库 + `assets/` + `binding/` 源码 + `binding/CMakeLists.example.txt`（**官方独立集成样例**：`set(UICORNERSTONE_CORE_OUTPUT "")` + `add_subdirectory(binding)` + 链接 `UICornerstoneBinding`，与设计器 CMake 结构一致） |

## 更新子模块

```batch
git submodule update --init --recursive
```

## 注意

- **不得修改 `subModules/UICornerstone/` 内的文件**；需要 UICornerstone 侧能力变更时，在 UICornerstone 仓库提交后更新本仓库 submodule 指针
- UICornerstone 自身子模块依赖清单见其 `design/guidelines/dependencies.md`
- 设计器的资产（字体/图片）与 UICornerstone 测试资产同源（HarmonyOS Sans 等），通过 `assets/` 管理
