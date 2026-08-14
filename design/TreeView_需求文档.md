# TreeView 控件需求文档

> 本文档定义 UICornerstone 新增控件 TreeView/TreeNode 的需求，输入到 UICornerstone 项目进行独立实现。

## 1. 背景与动机

UICornerstone 目前提供 Panel、Label、Button、CheckBox 等 12+ 控件，但缺少树形层级展示控件。

**需求来源**：CornerstoneDesigner（UICornerstone 界面设计器）的 F2.2 需求——需要以树形结构展示用户正在编辑的布局的父子控件关系。该控件设计器本身使用，同时也可被其他 UICornerstone 项目复用。

## 2. 功能需求

### F1 树形数据展示

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F1.1 | 以缩进树形展示层级数据 | P0 | 子节点相对于父节点向右缩进，缩进量可配置（默认 16px） |
| F1.2 | 展开/折叠 | P0 | 展开箭头（▶/▼）点击切换子节点可见性，支持初始状态设置 |
| F1.3 | 节点选择 | P0 | 单击选中节点，高亮显示，触发 onSelect 事件 |
| F1.4 | 滚动支持 | P1 | 节点数量超出可视区域时自动出现滚动条 |

### F2 数据接口

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F2.1 | 程序化设置树数据 | P0 | `setItems(TreeNode tree)` 直接设置树数据 |
| F2.2 | 动态更新 | P1 | 增删节点后增量更新，避免全量重建 |
| F2.3 | 清除 | P1 | `clearItems()` 清除所有节点 |
| F2.4 | 按 id 查找节点 | P2 | `findNodeById(const string& id)` 返回节点引用 |

### F3 JSON 布局支持

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F3.1 | 静态树定义 | P0 | 在 JSON 布局中直接定义树结构 |
| F3.2 | 事件绑定 | P0 | onSelect 事件在 JSON 中可绑定 |

### F4 事件

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F4.1 | onSelect | P0 | 选中节点时触发，回调参数包含选中节点 id |
| F4.2 | onExpand | P1 | 展开节点时触发 |
| F4.3 | onCollapse | P1 | 折叠节点时触发 |

## 3. 数据结构

### 3.1 TreeNode

```cpp
struct TreeNode {
    std::string id;          // 唯一标识
    std::string label;       // 显示文本
    bool expanded = false;   // 初始展开状态
    bool selected = false;   // 选中状态
    int indentLevel = 0;     // 缩进层级（自动计算）
    std::vector<TreeNode> children;  // 子节点
};
```

### 3.2 TreeView 类

```cpp
class TreeView : public ControlImpl {
public:
    // 设置树数据
    void setItems(const std::vector<TreeNode>& items);
    void clearItems();

    // 节点操作
    TreeNode* findNodeById(const std::string& id);
    bool expandNode(const std::string& id);
    bool collapseNode(const std::string& id);
    bool selectNode(const std::string& id);

    // 事件回调类型
    void setOnSelectCallback(std::function<void(const std::string& nodeId)> callback);

    // 配置
    void setIndentWidth(float pixels);  // 缩进宽度，默认 16px
    float getIndentWidth() const;
};
```

## 4. 非功能需求

| 编号 | 需求 | 指标 |
|------|------|------|
| N1 | 渲染性能 | 1000+ 节点可流畅展开/折叠 |
| N2 | 最大深度 | 至少支持 20 层嵌套 |
| N3 | 颜色风格 | 遵循现有 UICornerstone 控件颜色体系 |

## 5. JSON 布局格式示例

```json
{
  "type": "TreeView",
  "id": "controlTree",
  "rect": { "x": 0, "y": 24, "w": 200, "h": 300 },
  "items": [
    {
      "id": "rootPanel",
      "label": "Root Panel",
      "expanded": true,
      "children": [
        { "id": "btn1", "label": "Button (btn1)" },
        { "id": "lbl1", "label": "Label (lbl1)" },
        {
          "id": "subPanel",
          "label": "Panel (subPanel)",
          "expanded": false,
          "children": [
            { "id": "btn2", "label": "Button (btn2)" }
          ]
        }
      ]
    }
  ],
  "events": { "onSelect": "onTreeSelect" }
}
```

## 6. 渲染规范

### 6.1 单行渲染

```
[▶/▼] [缩进] [图标/无] [Label 文本]
```

- 展开箭头：▶（折叠）/ ▼（展开），与缩进之间 4px 间距
- 缩进：每级 `indentWidth` 像素
- 选中时整行高亮（背景色变化，参照 Button hover 色系）
- 节点高度：与 Label 控件行高一致（由 font.size 决定）

### 6.2 展开/折叠行为

- 折叠后子节点不渲染、不参与命中测试
- 展开/折叠不会影响其他节点
- 展开/折叠时无需动画（P2 可添加）

### 6.3 选中行为

- 单选模式（P2 可扩展多选）
- 选中节点高亮背景（#3A80C9 或系统主题色）
- 选中时触发 onSelect 事件，回调携带 node id

## 7. 边界与约束

- 当前版本仅支持静态树数据（在 JSON 布局中定义），不实现虚拟化滚动
- 不支持拖拽排序节点（P2 功能）
- 不支持自定义节点图标（P2 功能）
- 节点 id 在同一 TreeView 实例中必须唯一

---

## 本阶段测试内容

### 单元测试

| 测试项 | 说明 | 通过标准 |
|--------|------|---------|
| 渲染 | 空树/单节点/多层嵌套（20 层） | 每层缩进正确，无重叠 |
| 展开/折叠 | 展开 -> 子节点可见 / 折叠 -> 子节点隐藏 | 状态切换正确，不影响兄弟节点 |
| 选中 | 单击选中 -> 高亮 + onSelect 触发 | 回调参数与选中节点 id 一致 |
| JSON 解析 | 静态树定义 / 空 items / 深层嵌套 | 解析结果与定义一致 |
| 滚动 | 大量节点（200+） | 滚动条出现，滚动时渲染正确 |

### 集成测试

| 测试项 | 方法 | 通过标准 |
|--------|------|---------|
| 事件绑定 | JSON 中绑定 onSelect -> 选中节点 | 回调函数被调用 |
| 动态 setItems | 初始空树 -> setItems -> 渲染 | 树正常显示 |
| 内存 | 创建/销毁 TreeView，反复设置数据 | 无内存泄漏 |
