# NumericUpDown 控件需求文档

> 本文档定义 UICornerstone 新增控件 NumericUpDown 的需求，输入到 UICornerstone 项目进行独立实现。

## 1. 背景与动机

UICornerstone 的 PropertyGrid（属性编辑器）中需要编辑数值属性（如坐标、尺寸、字体大小、缩放等），需要一个带步进按钮的数值微调控件。

**需求来源**：CornerstoneDesigner F3.4。该控件通用性极强，任何需要数值输入的场景均可复用。

## 2. 功能需求

| 编号 | 需求 | 优先级 | 说明 |
|------|------|--------|------|
| F1 | 数值显示与编辑 | P0 | 显示当前数值，支持直接键盘输入 |
| F2 | 步进增/减按钮 | P0 | 上箭头+1，下箭头-1。长按连续步进 |
| F3 | 范围限制 | P0 | min/max 限制输入范围，越界自动截断 |
| F4 | 步长配置 | P0 | step 可配置（默认 1，可设为 0.1、10 等） |
| F5 | 小数精度 | P1 | decimals 控制小数位数（默认 0） |
| F6 | 初始值和占位符 | P1 | 可设默认值，空值时显示 placeholder |
| F7 | 只读模式 | P2 | 禁止编辑但可显示值 |

## 3. 数据结构

```cpp
class NumericUpDown : public ControlImpl {
public:
    // 值
    void setValue(double val);
    double getValue() const;

    // 范围
    void setRange(double min, double max);
    std::pair<double, double> getRange() const;

    // 步长
    void setStep(double step);
    double getStep() const;

    // 小数位数
    void setDecimals(int n);  // 0-6
    int getDecimals() const;

    // 事件
    void setOnValueChangedCallback(
        std::function<void(double newValue)> callback);
};
```

## 4. 事件

| 事件 | 触发时机 | 回调参数 |
|------|---------|---------|
| onValueChanged | 值改变时（按钮点击或编辑完成） | 新数值 |

## 5. JSON 布局格式

```json
{
  "type": "NumericUpDown",
  "id": "posX",
  "rect": { "x": 10, "y": 10, "w": 120, "h": 24 },
  "value": 100,
  "range": { "min": 0, "max": 4096 },
  "step": 1,
  "decimals": 0,
  "events": { "onValueChanged": "onPosXChanged" }
}
```

## 6. 渲染规范

```
┌─────────────┬─┐
│  100        │▲│
│             │▼│
└─────────────┴─┘
```

- 左侧：EditBox 风格的数值输入区
- 右侧：两个三角形箭头按钮（上/下），垂直排列，各占一半高度
- 按钮：hover 变色，press 立即步进
- 长按：首次 500ms 延迟后，每 100ms 步进一次

## 7. 边界与约束

- 空值时取 `range.min`
- 输入非数字字符时忽略
- 范围可为无穷大（`std::numeric_limits<double>::infinity()`）

---

## 本阶段测试内容

| 测试项 | 方法 | 通过标准 |
|--------|------|---------|
| 显示与编辑 | 初始化/键盘输入 | 值正确显示，输入有效 |
| 步进按钮 | 单击/长按 | 单步+1/-1，长按连续 |
| 范围限制 | 设限后输入越界值 | 自动截断到 min/max |
| 步长 | step=0.1/10/100 | 步进幅度正确 |
| 小数精度 | decimals=2 时输入 3.14159 | 显示 3.14 |
| 事件 | onValueChanged | 每次值变化触发 |
| JSON 解析 | 全参数/缺省参数 | 解析正确 |
