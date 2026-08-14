# Session 历史记录

本文件按时间顺序记录每个开发 Session 的决策、变更与待办，供后续 Session 回溯（遵循 UICornerstone 的 history.md 惯例）。

## 格式

每次 Session 结束后追加一条：

| 日期 | Session | 主题 | 关键决策 | 变更文件 |
| ---- | ------- | ---- | -------- | -------- |
| YYYY-MM-DD | N | 一句话主题 | 决策要点 | 文件列表 |

## 记录

| 日期 | Session | 主题 | 关键决策 | 变更文件 |
| ---- | ------- | ---- | -------- | -------- |
| 2026-08-14 | 1 | 设计规格 v2 + 规范体系建立 | 技术方案升级为 C++ Binding + JSON；AGENTS.md 与 design/guidelines 规范参照 UICornerstone 体系建立 | doc/02_设计规格文档.md、AGENTS.md、design/guidelines/*.md、doc/03_详细设计文档.md、doc/04_实施编码文档.md |
| 2026-08-14 | 2 | UICornerstone v1.0.0 依赖基线确认 | v1.0.0（tag commit `202346a5`，即 main HEAD）完整满足设计器需求：全部控件 + Binding（含新增内存资源/任意字体 API）+ 视口缩放 + 官方独立集成样例；依赖基线由 main 锁定为 v1.0.0，发布包为备用获取方式 | doc/02_设计规格文档.md、design/guidelines/build.md、design/guidelines/dependencies.md |
| 2026-08-14 | 3 | UI 界面概念设计 | 三栏式深色专业布局（菜单栏/左栏工具箱+控件树/中央画布/右栏属性+事件/状态栏）；设计态（网格、缩放、对齐吸附、HandleControl 控点、多选）与预览态（子视口 LoadLayout 实时渲染）双模式；控件类型注册表驱动工具箱/F3 动态表单/导出三处复用；三联动交互流；概念图以 SVG 矢量图呈现（doc/assets/ui-concept-main.svg） | doc/05_概念设计文档.md、doc/assets/ui-concept-main.svg |
| 2026-08-14 | 4 | 概念设计可行性评估 | 对照 v1.0.0 头文件与 HandleControl 源码逐区域核实：≥90% 界面能力现成（含 HandleControl 内置虚线框+8 控点+缩放拖拽）；仅 5 处业务层薄层需自研（Tab 切换、网格/对齐线内存纹理、文件对话框、分组折叠、右键菜单 P2）；3 条非阻塞 UICS 增强建议（handle 视觉属性、onContextMenu、tree item icon） | doc/05_概念设计文档.md §13 |
| 2026-08-14 | 5 | 设计文档按概念设计同步 | 修正 05 右栏坐标笔误（252→240）；03 §9 main_layout.json 原型定稿：补全四组菜单（含视图缩放/网格/对齐/预览快捷键）、增加 rightSplitter、状态栏改三段式 Panel+3 Label、左栏 Tab 与画布工具栏/内存纹理落地说明；04 阶段一/阶段四同步薄层组件任务；坐标 1200=240+6+708+6+240；JSON 语法对照 v1.0.0 test_layout.json 验证可行 | doc/05_概念设计文档.md、doc/03_详细设计文档.md、doc/04_实施编码文档.md |
| 2026-08-14 | 6 | 文档图全面 SVG 化 | 按视觉图规范将全部 ASCII/Mermaid 图替换为 SVG（12 张）：设计文档链 5 张（layer-stack 分层、architecture 架构、property-writeback 回写管线、preview-flow 预览流程、three-linkage 三联动）；控件需求线框 6 张（colorpicker×2、combobox×2、numericupdown、splitter）；05 主窗口概念图沿用；02 目录树为文件清单保留代码块 | doc/assets/*.svg（12 张）、doc/02/03/05、ColorPicker/ComboBox/NumericUpDown/Splitter_需求文档.md |
| 2026-08-14 | 7 | 控件注册表排序 + LuotiAni 基础托管需求 | 02 §5 注册表按控件名 A–Z 排序；需求补充：Animation（LuotiAni）支持放置/rect/资源路径编辑（`luotiAni` 字段 .jsonc 字符串，v1.0.0 实测），内容级编辑仍不做——同步 01 §7 边界、02 §5/§9、05 §4.1/§6.1/§13.2 | doc/01、02、05 及 history |
| 2026-08-14 | 8 | 资源面板 + 组件系统需求 | 澄清 v1.0.0 无 ResourceLoader 控件（资源机制=顶层 resourceProviders 挂载 + 内存资源 + provider: 引用）→ 设计器提供布局级资源面板（P1）；组件系统 v1.0.0 引擎级支持（components/props/template/{{ }}/_comp_，test_layout_advanced 实测含嵌套）→ 设计器 P2 落地组件编辑器（模板+props 表单）+ 工具箱"组件"动态分组 + 引用交互（双击进入编辑/整体块编辑/props 传值）；01 §7 补两条需求，02 §5/§9 同步，05 新增 §4.2 资源面板、§4.3 组件管理 | doc/01、02、05 |
| 2026-08-14 | 9 | 主界面概念图同步 + 布局功能补遗 | ui-concept-main.svg 同步：左栏三 Tab（工具箱/控件树/资源）、高级控件组加 Animation、新增"组件（动态）"分组（Greeter/FramedGreeter）；布局功能补遗：① 对齐/分布命令（多选 ≥2：左/中/右、顶/中/底、等宽/等高、水平/垂直分布，业务层包围盒计算入 Undo）② UICS 布局引擎属性编辑（anchor 9+5、anchorOffset、margin、padding/gap、layout h-flow/v-flow/grid、columns/rows/flowWeight、matchParentRect、sizeRatio）——同步 02 M3/M4、03 §4.1、04 阶段四、05 §5/§6.1/§13.2 → 主界面图二次同步：画布工具栏加"对齐/分布"下拉命令组（含"选中 ≥2 个控件时可用"示意）、右栏布局分组加锚点/布局模式行，05 §2 图注同步 | doc/assets/ui-concept-main.svg、doc/01/02/03/04/05 |
| 2026-08-14 | 10 | 文档顺序重排 | 概念设计前移：05_概念设计 → 03_概念设计，原 03 详细设计 → 04、原 04 实施编码 → 05（01 需求、02 规格不动）；同步更新全部交叉引用（04 §4.1/修订记录、05 修订记录/阶段一、03 §2 坐标自洽注、02 目录树注释）与 AGENTS.md 链接；history 历史记录保留当时编号原貌 | doc/03_概念设计文档.md、doc/04_详细设计文档.md、doc/05_实施编码文档.md、doc/02、AGENTS.md |
