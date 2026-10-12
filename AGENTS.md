# AGENTS.md - CornerstoneDesigner

## 交互语言

始终使用中文进行交互。

## 每次会话必读

### [编码规范 →](design/guidelines/coding.md)
编码前思考、简洁优先、精准修改、目标驱动执行。**每次编码前必须通读。**

### [文件编码](design/guidelines/coding.md)
所有源代码文件（.h/.cpp）必须保存为 **UTF-8 with BOM** 编码格式。不要使用 UTF-8 without BOM 或其他编码。

### [临时文件](design/guidelines/coding.md)
测试临时文件与中间产物必须放入 **`Temp/`** 目录（`.gitignore` 已忽略，不会入库）。**禁止直接在项目根目录或源码目录下创建临时文件。**

### [设计规则 →](design/guidelines/design-rules.md)
位置数据存储规则、魔鬼数字规范。

### [设计文档规范 →](design/guidelines/design-docs.md)
架构决策原则、设计文档编写规范。

## 参考文档（按需阅读）

| 主题 | 文档 |
|------|------|
| 构建与测试 | [build.md](design/guidelines/build.md) |
| RGBA8888 像素格式（关键） | [pixel-format.md](design/guidelines/pixel-format.md) |
| 子模块依赖 | [dependencies.md](design/guidelines/dependencies.md) |
| Session 历史记录 | [history.md](design/guidelines/history.md) |
| 测试用例规范 | [testing.md](design/guidelines/testing.md) |
| 瀑布流程设计文档 | [design/01_需求说明文档.md](design/01_需求说明文档.md) → [05_实施编码文档.md](design/05_实施编码文档.md)（按顺序阅读） |

## 快速链接

- [开发指南索引](design/guidelines/README.md) — 全部规范文档目录

## 项目信息

- 基于 C++17 的 UI 设计器，Dogfooding：设计器自身是 UICornerstone 应用（SDL3 后端）
- UI 方案：**C++ Binding + JSON**（静态骨架 JSON 声明式 + 动态区域 Binding 命令式），见 `design/02_设计规格文档.md`
- 仓库布局：`design/` 设计文档、`include/Designer/` 公共头、`src/` 实现、`layouts/` 设计器自身 JSON 布局、`build_scripts/` 构建脚本、`subModules/UICornerstone/` 子模块
- 构建：`git clone --recursive` 后 `build_scripts\build.bat sdl3`（详见 [build.md](design/guidelines/build.md)）

## 引擎仓库规则（铁律）

- **引擎项目文件夹（`D:\GitSpace\UIControls`）为只读目录，不可更改引擎的任何文件**（源码、头文件、文档、schema、构建产物等一律禁止写入）。
- 引擎侧需求/缺陷必须写入配合修改清单（`Temp/UICornerstone_配合修改清单_第三批.md`），由引擎方决定实现方式并实施；设计器侧只能读取/同步引擎交付的产物（如 `release/` 下 DLL）。

## 提交与推送规则

- 提交（commit）和推送（push）必须经过用户的明确指示，**未经同意不得自行 push**，也不得自行执行 git add/commit。
- 用户未明确要求时，代码修改仅限于工作区内的文件变更，不创建任何提交。

## 提交前验证

1. `cmake --build build --config Debug` — 必须编译通过、零警告
2. 运行任何存在的测试
3. 检查子模块：`git submodule status`
