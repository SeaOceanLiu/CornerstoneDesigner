# 测试用例规范

新功能或新模块的测试应包含以下层次，缺一不可：

| 层次 | 文件模式 | 用途 | 验收标准 |
| ---- | -------- | ---- | -------- |
| **单元测试** | `test/test_xxx.cpp` | 测试无 UI 依赖的核心逻辑：ControlModel/Serializer/UndoRedoService/LayoutEngine/CodeGen/EventManager/ProjectManager | SDL3 构建通过、全部断言通过、exit=0 |
| **集成测试** | `test/test_app.cpp` | 设计器应用级验证：启动、加载布局、画布交互注入、导出链路 | 可无人值守（`auto=<秒>`）自动退出，exit=0 |
| **兼容性测试** | `testing_scripts/` 脚本 | 导出 JSON 在 UICornerstone 中 `LoadLayout` 验证（以 `test_layout`/`test_layout_advanced` 为 Schema 基准） | 解析无报错、控件树一致 |

## 自动化测试参数规范（所有可运行测试强制）

所有测试可执行程序必须支持命令行参数 `auto=<秒>` 实现无人值守自动化，且参数支持任意顺序（不限定位置）。无法识别的参数 WARN 提示后忽略，不得报错退出。

| 参数 | 语义 | 要求 |
| ---- | ---- | ---- |
| `auto=<秒>` | 无人值守自动退出：`auto=0`/缺省 = 人工模式（窗口驻留，关闭窗口退出）；`auto=N` = N 秒后超时自动退出 | 必须支持、任意顺序 |

规则：

1. 窗口类测试：main 循环中解析任意顺序的 `auto=<秒>`，达到秒数后退出窗口循环，exit=0。
2. 无窗口循环的纯逻辑测试（单元测试）也须解析并接受该参数（不改变行为）。
3. 未知参数打印 WARN 后忽略。

## 测试脚本归档策略

1. 测试脚本（自动化回归脚本、批量运行脚本、数据准备/校验脚本等）统一归档在 `testing_scripts` 目录，不得散落在项目其他位置。
2. 应尽可能编写**可重复使用**的测试脚本（参数化输入），避免一次性脚本。
3. 后续测试**优先通过修改这些测试脚本的适用性来完成测试**（复用已有脚本、调整参数与目标），而不是新建临时脚本；确需新建时也必须归档到 `testing_scripts` 并保持可重复使用。

## 兼容性验证（N4 需求）

| 测试项 | 方法 | 通过标准 |
| ------ | ---- | -------- |
| test_layout 兼容 | 设计器导出 JSON 在 UICornerstone 的 `test_layout` 输入规格下自检 | 结构与 `layouts/test_layout.json` 完全一致 |
| test_layout_advanced 兼容 | 同上，对照 `layouts/test_layout_advanced.json` | 结构完全一致 |
| LoadLayout 端到端 | 设计器预览 = 内部 `LoadLayout(json)`，同时作为自检 | 解析无报错 |

## 执行顺序

1. 先实现**单元测试**（核心逻辑，无 UI 依赖），功能稳定后提交审核。
2. 审核通过后实现**集成测试**（应用冒烟 + auto 参数）。
3. 最后做**兼容性测试**（导出 JSON 在 UICornerstone 验证）。
4. 新控件需求（如需要 UICornerstone 侧能力）在 UICornerstone 仓库侧按其三层次规范（标准 C++ / fromsource 集成 / C ABI）补充测试。
