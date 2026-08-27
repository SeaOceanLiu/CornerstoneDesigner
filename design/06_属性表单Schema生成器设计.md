# 属性表单生成器设计（基于 Schema）

> 设计目标：从 UICornerstone v1.1.1 的 `declarative-ui.schema.json` 自动生成控件属性编辑表单，无需硬编码每种控件的属性列表。

## 1. Schema 与 PropertyNames.h 的命名映射

### 1.1 核心规则

| 层面 | 命名规范 | 示例 | 用途 |
|------|---------|------|------|
| **Schema JSON** | camelCase | `selectedIndex`, `checkState` | 声明式布局文件（JSON）|
| **Binding API** | kebab-case | `selected-index`, `check-state` | 命令式代码（`Control::SetXxx`）|
| **转换公式** | `camel` → `kebab` | `selectedIndex` → `selected-index` | `replace(/([A-Z])/g, '-$1').toLowerCase()` |

### 1.2 转换验证（全部通过）

```javascript
const toKebab = s => s.replace(/([A-Z])/g, '-$1').toLowerCase();
// selectedIndex → selected-index  ✓
// checkState → check-state        ✓
// fontResource → font-resource    ✓
// maxVisibleItems → max-visible-items  ✓
// defaultExpand → default-expand  ✓
```

### 1.3 特殊情况处理

| Schema 键名 | Binding 键名 | 说明 |
|-------------|-------------|------|
| `caption` | `"caption"` | 一致 |
| `text` | `"text"` | 一致（Label 的 `caption` 别名） |
| `visible` | `"visible"` | 一致 |
| `enabled` | `"enabled"` | 一致 |
| `rect` | 无直接映射 | 矩形用 4 个 NumericUpDown（x/y/w/h） |
| `colors` | `SetColor`/`SetStateColor` | 结构化对象，需特殊处理 |
| `items`（数组） | `SetString("items", json)` | 字符串化 JSON 数组 |

## 2. 表单控件映射表

### 2.1 按 Schema 类型映射

| Schema 类型 | Binding 读取方法 | 表单控件 | 备注 |
|-------------|----------------|---------|------|
| `string` | `GetString(prop)` | `EditBox`（单行）/ `TextArea`（多行） | 颜色字段例外：用 `ColorPicker` |
| `boolean` | `GetBool(prop)` | `CheckBox` | 直接映射 |
| `integer` | `GetInt(prop)` | `NumericUpDown`（step=1） | 有限范围时用 `ComboBox` |
| `number` | `GetFloat(prop)` | `NumericUpDown`（step=0.1） | 范围由 `x-default` 参考 |
| `enum`（有枚举值） | `GetEnum(prop)` / `GetInt` | `ComboBox`（预填充枚举选项） | Schema 中 `enum` 数组即选项列表 |
| `color`（引用 color def） | `GetColor(prop)` | `ColorPicker` | Schema 中 `$ref: "#/$defs/color"` |
| `array` | 无通用 getter | `ListView`（列/行编辑）/ `EditBox`（JSON 文本） | 复杂类型暂用 JSON 文本编辑 |
| `object` | 无通用 getter | 折叠面板展开子属性 | 如 `rect` → 4 个 NumericUpDown |

### 2.2 特殊字段处理

| 字段 | 处理策略 |
|------|---------|
| `type`（只读） | 显示标签，禁用编辑 |
| `id` | 只读显示（或 EditBox 允许修改） |
| `rect` | 展开为 x/y/w/h 四个 NumericUpDown |
| `colors` | 展开为状态色组（normal/hover/pressed/disabled）各一个 ColorPicker |
| `items`（数组对象） | ListView 编辑（每行一个 item，含 label/value/disabled 列） |
| `children` | 树形编辑器（或隐藏，由控件树管理） |
| `events` | 事件绑定面板（独立 Tab） |

## 3. 实现方案

### 3.1 Schema 加载

```cpp
// SchemaPropertyFormGenerator.h
class SchemaPropertyFormGenerator {
public:
    // 从 schema JSON 文件加载控件类型定义（一次加载，缓存）
    bool loadSchema(const std::string& schemaPath);
    
    // 获取控件类型的属性列表（返回 JSON 数组，每项含 name/type/enum/...）
    nlohmann::json getPropertyList(const std::string& controlType) const;
    
    // camelCase → kebab-case 转换（供 Binding API 调用）
    static std::string camelToKebab(const std::string& camel);
    
    // 获取属性的默认值
    std::string getDefaultValue(const std::string& controlType, const std::string& propName) const;
    
    // 获取属性的枚举值（如有）
    std::vector<std::string> getEnumValues(const std::string& controlType, const std::string& propName) const;

private:
    nlohmann::json m_schema;                    // 完整 schema
    mutable std::map<std::string, nlohmann::json> m_typeCache; // 缓存已解析的类型定义
};
```

### 3.2 属性表单动态构建

```cpp
// PropertyPanel 使用示例
void PropertyPanel::updateForControl(ControlModel* c) {
    if (!c) return;
    
    auto gen = m_schemaGenerator.loadSchema("subModules/UICornerstone/tools/schema/declarative-ui.schema.json");
    auto props = gen.getPropertyList(c->props.type);
    
    // 清除旧表单
    destroyFormWidgets();
    
    // 按属性列表生成表单行
    for (const auto& propDef : props) {
        const std::string& name = propDef["name"];
        const std::string& type = propDef["type"];
        const std::string& bindingKey = SchemaPropertyFormGenerator::camelToKebab(name);
        
        // 跳过只读字段（type/id）
        if (name == "type" || name == "id") continue;
        
        // 根据类型创建对应表单控件
        Control label = m_ui->CreateLabel(name, 14.0f, ...);
        Control editor = createEditorForType(type, bindingKey, ...);
        
        // 绑定值变化回调：编辑 → Binding API 写回
        editor.SetCallback("value-changed", [this, bindingKey](const Event&) {
            writeBack(bindingKey, editor);
        });
        
        m_formRows.push_back({label, editor});
    }
}
```

### 3.3 写回管线（PropertyChangeCommand）

```cpp
void PropertyPanel::writeBack(const std::string& bindingKey, const Control& editor) {
    if (!m_selectedControl) return;
    
    // 读取编辑控件的值
    std::string value;
    if (editor.GetType() == "edit-box") value = editor.GetString("text");
    else if (editor.GetType() == "check-box") value = editor.GetBool("checked") ? "true" : "false";
    // ... 其他类型
    
    // 构建命令（旧值快照 + 新值）
    auto cmd = std::make_unique<PropertyChangeCommand>(
        m_selectedControl,
        bindingKey,   // kebab-case key for Binding API
        oldValue,
        value
    );
    m_undoService.record(std::move(cmd));
    
    // 同步到内存模型（extended 结构）
    m_selectedControl->props.extended[bindingKey] = value;
    
    // 触发画布刷新
    m_canvas->markDirty();
}
```

## 4. 与现有代码的关系

### 4.1 复用点

- **Schema 加载**：新增 `SchemaPropertyFormGenerator` 类，独立于现有 `ControlModel`
- **Binding API 调用**：复用现有 `Control::SetString/SetBool/SetInt/SetEnum` 等方法
- **Undo 集成**：复用现有 `PropertyChangeCommand` 和 `UndoRedoService`

### 4.2 需新增的代码

1. `include/Designer/SchemaPropertyFormGenerator.h/.cpp`
   - Schema JSON 解析
   - camelCase ↔ kebab-case 转换
   - 属性列表查询（带缓存）

2. `src/PropertyPanel.cpp` 重写
   - 移除硬编码的属性表单构建逻辑
   - 改为调用 `SchemaPropertyFormGenerator.getPropertyList()`
   - 动态创建表单控件

### 4.3 不改动部分

- `ControlModel` 数据结构（保持 `extended` 承载未知属性）
- `PropertyChangeCommand` 命令结构（保持不变）
- 布局 JSON 序列化规则（ toJson 已支持 `extended` 原样输出）

## 5. Schema 与 validator 的使用说明

### 5.1 validate_layout.exe 用法

```bash
# 基本用法：校验布局文件
./validate_layout.exe layouts/main_layout.json
# 输出：[keys] PropertyNames.h 键集: 617 个
#       [summary] errors=0 warns=0 -> PASS

# 严格模式（WARN 也视为 ERROR）
./validate_layout.exe --strict layouts/main_layout.json

# 指定自定义 schema（如有更新）
./validate_layout.exe --schema=path/to/schema.json layouts/main_layout.json
```

### 5.2 Schema 验证状态

当前 Schema（`tools/schema/declarative-ui.schema.json`）存在**内部不一致**（v1.1.1 bug）：

| 问题 | 影响 | 处理方式 |
|------|------|---------|
| 部分属性名（`fontSize`, `tabs` 等）在 Schema 中定义为 camelCase，但 PropertyNames.h 中只有 kebab-case | validator 报 SCHEMA_INVALID | **忽略**，这些是 Schema 自身的 bug，不影响布局验证 |
| Schema 引用了 PropertyNames.h 中不存在的键 | 同上 | 视为 Schema 滞后于实际 API，以 PropertyNames.h 为准 |
| `combo-box.items` 定义为对象数组 `{label, value, disabled}`，但 Binding 用字符串数组 | 格式冲突 | **JSON 用对象数组**，Binding 代码需序列化为字符串 |

### 5.3 设计器使用策略

1. **生成 JSON 时**：以 Schema 为准（camelCase 键名、对象数组格式）
2. **读取/编辑 JSON 时**：自动转换 camelCase ↔ kebab-case
3. **验证 JSON 合法性**：运行时调用 `validate_layout.exe`（如有需要）
4. **PropertyNames.h 为准**：当 Schema 与 PropertyNames.h 冲突时，以 PropertyNames.h 为准

## 6. 实施计划

### Phase 1：Schema 加载器（1 天）
- 实现 `SchemaPropertyFormGenerator` 类
- 加载 `declarative-ui.schema.json`
- 实现 `getPropertyList(controlType)` 查询
- 单元测试：验证 27 种控件类型的属性列表

### Phase 2：表单动态构建（2 天）
- 重写 `PropertyPanel::buildForm()`
- 实现类型 → 表单控件映射
- 实现值读取/写回逻辑
- 集成 Undo 系统

### Phase 3：特殊字段处理（1 天）
- `rect` 展开为 4 个 NumericUpDown
- `colors` 展开为状态色组
- `items`（数组）用 ListView 编辑
- `events` 保持独立 Tab（现有逻辑）

### Phase 4：验证与测试（1 天）
- 冒烟测试覆盖所有控件类型
- 边界测试：未知类型、缺失属性、格式错误
- 集成测试：属性修改 → 画布刷新 → 导出 JSON 一致性

## 7. 风险与缓解

| 风险 | 缓解措施 |
|------|---------|
| Schema 不完整或有遗漏属性 | 以 PropertyNames.h 为补充源，缺失属性走 `extended` 手动编辑 |
| 数组类型（items/children）编辑复杂 | MVP 用 JSON 文本编辑，后续迭代为结构化编辑器 |
| 颜色字段格式复杂（状态色组） | 复用现有 `ColorPicker` 控件，状态切换 Tab |
| Schema 内部 bug（SCHEMA_INVALID） | 忽略警告，以实际 Binding API 为准 |
