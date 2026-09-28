#pragma once
// CornerstoneDesigner 全局共享常量（design/guidelines/design-rules.md §2）。
// 仅放跨模块共享常量；模块内部常量放各模块匿名 namespace 的 constexpr。

namespace Designer::ConstDef {

inline constexpr float kSnapThreshold = 8.0f;   // 画布拖拽网格吸附的最小单元格（像素）

}  // namespace Designer::ConstDef
