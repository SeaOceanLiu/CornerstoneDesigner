# Splitter 控件需求文档

> 本文档定义 UICornerstone 新增控件 Splitter 的需求，输入到 UICornerstone 项目进行独立实现。

## 1. 背景与动机

CornerstoneDesigner 主界面采用三栏布局（控件面板 / 画布 / 属性面板），栏与栏之间需要可拖拽的分割条来调整宽度。当前 UICornerstone 的控件只能在创建时固定 rect，运行中无法通过拖拽改变相邻控件尺寸。

**需求来源**：CornerstoneDesigner 主界面布局。通用 UI 模式，任何需要分栏调整大小的应用都需要。

## 2. 功能需求

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F1 | 拖拽分割 | P0 | 鼠标拖拽分割条，改变两侧控件的宽度/高度 |
| F2 | 方向支持 | P0 | 水平分割条（纵向分栏）和垂直分割条（横向分行） |
| F3 | 最小尺寸限制 | P0 | 两侧控件不被拖拽到小于最小宽度/高度 |
| F4 | 视觉反馈 | P0 | 拖拽时分割条变色，提示可拖拽区域 |
| F5 | 双击恢复 | P1 | 双击分割条恢复 50/50 比例 |

## 3. 数据结构和用法

Splitter 本身不是一个独立控件，而是一个**可嵌入 Panel 的行为类**，放置在两个 Panel 之间。

```cpp
class Splitter : public ControlImpl {
public:
    // 方向
    void setOrientation(bool horizontal);  // true=水平分割, false=垂直分割
    bool isHorizontal() const;

    // 关联的左右/上下控件
    void setLinkedControls(Control* first, Control* second);

    // 最小尺寸
    void setMinSize(float firstMin, float secondMin);

    // 位置比例 (0.0-1.0)
    void setSplitRatio(float ratio);
    float getSplitRatio() const;
};
```

### 与父容器的关系

```
┌──────────────────────────────┐
│         Panel (parent)        │
│  ┌──────┬────────┬─────────┐ │
│  │ Left  │ Split │  Right   │ │
│  │ Panel │ ter   │  Panel   │ │
│  │ 200px │       │  750px   │ │
│  └──────┴────────┴─────────┘ │
└──────────────────────────────┘
```

Splitter 的 rect 由父容器和左右 Panel 的尺寸自动计算。

## 4. 事件

| 事件 | 触发时机 | 回调参数 |
|------|---------|---------|
| onSplitterMoved | 拖拽结束时 | 新的 splitRatio |

## 5. JSON 布局格式

```json
{
  "type": "Panel",
  "id": "mainArea",
  "rect": { "x": 0, "y": 24, "w": 1200, "h": 776 },
  "layout": "horizontal",
  "children": [
    { "type": "Panel", "id": "leftPanel", "rect": { "x": 0, "y": 0, "w": 200, "h": 776 } },
    {
      "type": "Splitter",
      "id": "split1",
      "orientation": "vertical",
      "firstPanel": "leftPanel",
      "secondPanel": "rightPanel",
      "minFirst": 150,
      "minSecond": 300,
      "ratio": 0.21
    },
    { "type": "Panel", "id": "rightPanel", "rect": { "x": 206, "y": 0, "w": 994, "h": 776 } }
  ]
}
```

## 6. 渲染规范

### 6.1 外观

```
水平分割条（纵向分栏）：
         ↓ 可拖拽区域
  ────── ┼ ──────
         │        ← 鼠标变为 ↔ 光标
  ────── ┼ ──────
         ↑ 2-4px 宽

垂直分割条（横向分行）：
  ────────
  ────────   ← 鼠标变为 ↕ 光标
  ════════     ← 4-6px 高，比背景稍暗
  ────────
  ────────
```

- 默认颜色：比背景深 15-20%（或 #808080）
- Hover 颜色：系统主题色（#4A90D9）
- Dragging 颜色：#3A80C9
- 宽度/高度：水平 4px，垂直 6px

### 6.2 交互

- 鼠标进入：变色 + 光标样式变化
- 按下拖拽：跟随鼠标移动，实时调整两侧控件 rect
- 拖拽边界：两侧控件不得小于 minFirst/minSecond
- 释放鼠标：触发 onSplitterMoved 事件

## 7. 边界与约束

- Splitter 本身不包含子控件，只调节 linkedControls 的尺寸
- 多层嵌套支持：父容器可包含多个 Splitter 对
- 最小分割比例：任一控件不小于 min size

---

## 本阶段测试内容

| 测试项 | 方法 | 通过标准 |
|--------|------|---------|
| 拖拽功能 | 鼠标拖拽 50px | 两侧控件宽度正确变化 |
| 方向 | 水平/垂直 | 各自正确调整宽度/高度 |
| 最小尺寸 | 拖拽使左侧 < minFirst | 左侧停在 minFirst，右侧占剩余 |
| 视觉反馈 | hover / dragging | 颜色变化正确 |
| 事件 | onSplitterMoved | 拖拽结束时触发，比例正确 |
| JSON 解析 | 全参数/缺省 | 解析正确 |
| 嵌套 | 多层 Splitter | 各层独立工作不互相干扰 |
