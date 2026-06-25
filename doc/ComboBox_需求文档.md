# ComboBox 控件需求文档

> 本文档定义 UICornerstone 新增控件 ComboBox 的需求，输入到 UICornerstone 项目进行独立实现。

## 1. 背景与动机

CornerstoneDesigner 的属性编辑器中需要让用户从预定义选项中选择值（如 orientation 选择 Horizontal/Vertical、textAlign 选择 Left/Center/Right）。同时设计器自身的 Toolbox 面板也可用 ComboBox 做控件分类筛选。

**需求来源**：CornerstoneDesigner F3 属性编辑器中的枚举/选择类型属性。通用 UI 模式，几乎所有应用都需要。

## 2. 功能需求

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F1 | 下拉列表展示 | P0 | 点击展开下拉列表，显示所有选项 |
| F2 | 选中高亮 | P0 | 当前选中的选项在列表中高亮 |
| F3 | 关闭后显示选中值 | P0 | 关闭时在输入框中显示当前选中项的标签 |
| F4 | 键盘导航 | P1 | 展开后 ↑/↓ 移动高亮，Enter 选中，Esc 关闭 |
| F5 | 搜索/过滤 | P2 | 输入文字过滤选项列表 |
| F6 | 禁用项 | P2 | 部分选项可标记为 disabled |

## 3. 数据结构

```cpp
struct ComboBoxItem {
    std::string label;    // 显示文本
    std::string value;    // 实际值
    bool disabled = false;
};

class ComboBox : public ControlImpl {
public:
    // 选项列表
    void setItems(const std::vector<ComboBoxItem>& items);
    void clearItems();

    // 选中
    void setSelectedIndex(int index);
    int getSelectedIndex() const;
    void setSelectedValue(const std::string& value);
    std::string getSelectedValue() const;

    // 事件
    void setOnSelectionChangedCallback(
        std::function<void(int index, const std::string& value)> callback);
};
```

## 4. 事件

| 事件 | 触发时机 | 回调参数 |
|------|---------|---------|
| onSelectionChanged | 选中项改变时 | 选中项的 index 和 value |

## 5. JSON 布局格式

```json
{
  "type": "ComboBox",
  "id": "orientationPicker",
  "rect": { "x": 10, "y": 10, "w": 150, "h": 24 },
  "selectedIndex": 0,
  "items": [
    { "label": "Horizontal", "value": "horizontal" },
    { "label": "Vertical", "value": "vertical" }
  ],
  "events": { "onSelectionChanged": "onOrientChanged" }
}
```

## 6. 渲染规范

### 6.1 闭合状态

```
┌──────────────────┬─┐
│ Horizontal       │▼│
└──────────────────┴─┘
```

- 左侧：显示当前选中项的 label
- 右侧：下拉箭头 ▼，点击展开列表

### 6.2 展开状态

```
┌──────────────────┬─┐
│ Horizontal       │▲│
├────────────────────┤
│ Horizontal         │  ← 高亮
│ Vertical           │
└────────────────────┘
```

- 列表浮动于其他控件之上（z-order 提升）
- 列表背景色与 Button 的 normal 背景一致
- 高亮行背景色与 Button 的 hover 背景一致
- 列表高度自适应：最多显示 6 项，超出时出现滚动条
- 点击非高亮行 -> 选中该项 -> 列表关闭
- 点击高亮行或外部区域 -> 列表关闭，选中值不变

## 7. 边界与约束

- items 为空时点击下拉不显示列表（或显示"无选项"提示）
- selectedIndex 为 -1 表示无选中，闭合时显示 placeholder
- 所有 item 的 label 会在闭合时截断（...）以适配控件宽度

---

## 本阶段测试内容

| 测试项 | 方法 | 通过标准 |
|--------|------|---------|
| 渲染 | 闭合/展开/选中项 | 显示正确 |
| 选中 | 点击选项 -> 选中 -> 闭合 | 选中项正确，高亮显示 |
| 键盘 | ↑↓ Enter Esc | 导航正常 |
| 空数据 | 空 items | 不崩溃，下拉列表不显示 |
| 长文本 | 超长 label | 截断显示 ... |
| 事件 | onSelectionChanged | 选中变化时触发 |
| JSON 解析 | 全参数/缺省/items | 解析正确 |
