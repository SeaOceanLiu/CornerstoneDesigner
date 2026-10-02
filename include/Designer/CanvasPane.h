// CanvasPane.h
// CornerstoneDesigner
//
// 中央画布：canvasArea 内工具栏下方创建**子视口**（承载被设计布局内容）。
// 纸面+网格画在**子视口内部**（最初设计）：
//   - 网格 = 单元瓦片纹理（白底 + #303030 线）+ scale-type=tile 平铺铺满视口；
//   - resize/splitter 只更新视口与瓦片 Image 的 rect（零纹理重建）；
//   - 仅缩放档/gridCell 变化时重建小瓦片（几 KB）。
// 事件回退（UICornerstone v1.1.1）：子视口内未被消费的事件回退 owner 树，
// splitter 拖拽等 owner 交互不受影响。主循环须显式驱动子视口
// ProcessEvents/Update/Render（见 App::run）。
// 交互：双击工具箱项 → 视口内放置控件；点击控件选中（选中框=Shape 多图元）、
// 拖动移动（网格吸附）；鼠标源由 App 注入（SDL 轮询）。

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Control.h"
#include "ControlModel.h"
#include "UICornerstoneAPI.h"

class Event;

namespace UICornerstone {
class UICornerstone;
}

namespace Designer {

class CanvasPane {
public:
    CanvasPane();
    ~CanvasPane();
    CanvasPane(const CanvasPane&) = delete;
    CanvasPane& operator=(const CanvasPane&) = delete;

    // 创建：须在主布局 LoadLayout 后调用（canvasArea 已存在）。
    // toolbarH：canvasArea 顶部工具栏高度，子视口从该位置下方开始。
    // resourceRoot：assets 资源根（用于把字体等资源注册进内存 provider，供视口内控件解析）。
    bool Create(UICornerstone::UICornerstone& ui, Control canvasArea, float toolbarH,
                const std::string& resourceRoot);

    void Destroy();

    // 布局轮询（App 每帧调用）：canvasArea 矩形变化时同步视口与网格 rect（零纹理重建）。
    void TickLayout(float deltaSeconds);

    // 交互轮询（App 每帧调用）：放置/选中/拖动（鼠标源由 App 注入）。
    void TickInput();

    // 子视口驱动（App 主循环调用；v1.1.1 事件回退要求驱动其事件循环）
    void ProcessViewportEvents();
    void UpdateViewport(float deltaSeconds);
    void RenderViewport();
    UICornerstone::UICornerstone* Viewport() const { return m_viewport.get(); }

    // 工具箱联动：双击工具箱项 → 视口内放置对应控件
    void SetToolboxTree(Control tree);
    void SetDragCursorFn(std::function<void(bool)> fn) { m_dragCursorFn = std::move(fn); }
    // 鼠标源（App 注入 SDL 轮询；返回左键是否按下，填充窗口坐标）
    using MouseSourceFn = std::function<bool(float&, float&)>;
    void SetMouseSource(MouseSourceFn src) { m_mouseSource = std::move(src); }
    using KeySourceFn = std::function<bool()>;   // 返回 Delete 键按下沿（每帧轮询一次）
    void SetKeySource(KeySourceFn src) { m_keySource = std::move(src); }

    // 右侧控件树（tvTree）：画布控件增删/选中双向同步。
    void SetCanvasTree(Control tree);
    void syncTreeAdd(const std::string& parentId, const std::string& id,
                     const std::string& label);   // 层级挂接（parentId 空=顶层）
    void syncTreeRemove(const std::string& id);
    void syncTreeSelection();

    // 删除选中控件：销毁视口控件 + 移除模型 + 取消选中（清空属性面板/隐藏选中框）。
    void deleteSelected();
    // 滚轮滚动动态区（mouse-wheel 事件载荷 scrollY；propDynamic 与 tabProperty 双订阅）
    void ScrollDynamicBy(float scrollY);

    std::set<size_t> m_fixedWidthRows;   // 固定宽行（ColorPicker 120，宽度同步跳过）
    float m_seenDynW = -1.0f;            // 动态区容器宽观测（splitter 拖动后同步滚动条/行宽）
    void syncDynamicWidth();             // 容器宽变化 → 滚动条/行宽同步

    // 属性面板联动（App 注入；选中回填面板，面板变更回写控件）
    void SetPropertyControls(Control x, Control y, Control w, Control h,
                             Control caption, Control color,
                             Control visible, Control enabled);

    // ── Schema 驱动动态属性区 ──
    // 加载引擎 declarative-ui.schema.json（$defs 按控件类型提供属性清单+类型）。
    bool LoadSchema(const std::string& path);
    // propDynamic 容器（主实例 v-flow panel）接线；选中控件时按 schema 重建行。
    void SetDynamicPropsContainer(Control box);
    void rebuildDynamicRows();

    // 缩放档（0.25~2.0）。改变 z → 重建网格瓦片（小纹理）。
    void SetZoom(float zoom);
    float GetZoom() const { return m_zoom; }

    void SetGridVisible(bool visible);
    bool IsGridVisible() const { return m_gridVisible; }
    void SetGridCellSize(float px);
    void SetSnapEnabled(bool snap) { m_snapEnabled = snap; }      // 吸附到网格
    bool IsSnapEnabled() const { return m_snapEnabled; }
    void SetAlignEnabled(bool on) { m_alignEnabled = on; }        // 吸附到控件（智能对齐）
    bool IsAlignEnabled() const { return m_alignEnabled; }

    // 内容：渲染 ControlModel 树为画布控件（交互阶段接入）。
    void SetContent(std::unique_ptr<ControlModel> root);
    void Rebuild();

    // 坐标互转（窗口坐标 ↔ 画布逻辑坐标）。
    bool ScreenToCanvas(float sx, float sy, float& cx, float& cy) const;
    bool CanvasToScreen(float cx, float cy, float& sx, float& sy) const;

    // 画布尺寸（画布逻辑坐标）与纸面窗口区域。
    SRect CanvasRect() const;
    SRect ViewportRect() const;

    // 放置控件查询：按 id 取逻辑 rect（供 App/探针使用）。
    bool GetPlacedRect(const std::string& id, SRect& out) const;
    // 放置控件查询：按 id 取控件 rect（引擎 getRect=逻辑值，bench scale 不改 rect；
    // 与 GetPlacedRect 等价，供迁移回归比对）；未找到返回 false。
    bool GetPlacedViewRect(const std::string& id, SRect& out) const;
    // 放置控件查询：按 id 取当前逻辑字号（供 App/探针使用）；未找到返回 false。
    bool GetPlacedFontPx(const std::string& id, float& out) const;
    // 放置控件写入：按 id 设置像素字号（writeFontPx 双路）；未找到返回 false。
    bool SetPlacedFontPx(const std::string& id, float px);
    // 放置控件查询：按 id 取当前背景色（供 App/探针使用）；未找到返回 false。
    bool GetPlacedColor(const std::string& id, UIColor& out) const;

    // 画布控件点击（画布坐标）。
    using ClickHandler = std::function<void(const std::string& id, float cx, float cy)>;
    void SetClickHandler(ClickHandler handler) { m_clickHandler = std::move(handler); }

private:
    // 画布内容（被设计控件，z 序 = vector 序）
    struct PlacedControl {
        std::unique_ptr<ControlModel> model;
        Control ctl;
        std::string parentId;        // 容器子控件：父容器 id（空=画布根）
    };

    void refreshLayout();          // 同步视口与网格 Image rect（零重建）
    void registerGridTile();       // 生成瓦片 PNG → RegisterResource（含首次 provider 触发）
    void createGridImage();        // 在子视口内创建网格 Image（scale-type=tile）
    void rebuildGridTile();        // registerGridTile + createGridImage（缩放档变化）
    void registerFonts();          // 字体注册进内存 provider（视口内 Label 解析字体必需）
    void styleControl(const std::string& toolboxId, Control ctl);   // 放置控件默认样式（白纸上可见）

    void tickInput();              // 交互轮询：放置/选中/拖动
    void placeControl(const std::string& toolboxId, float cx, float cy, bool fromDrag = false);
    Control createViewportControl(const std::string& toolboxId, float x, float y,
                                  float w, float h, ControlModel& model) const;
    bool hitTest(float cx, float cy, std::string& outId);   // 非 const：子控件命中需 placedById（非 const）
    ControlModel* findModel(const std::string& id);
    PlacedControl* placedById(const std::string& id);
    // P1-a：控件画布全局原点（沿 parentId 链累加局部 rect；WinFrame 加 ClientPanel 内容偏移）
    void globalOriginOf(const PlacedControl& pc, float& gx, float& gy);
    // 智能对齐（x/y 为选中控件局部坐标，引用修正）：命中轴吸附+参考线更新；返回命中轴
    void applyAlignSnap(float& x, float& y, float w, float h, bool& hitX, bool& hitY);
    void selectItem(const std::string& id);
    void ensureSelHandle(Control target);   // 引擎 HandleControl 附加（选中框+8向手柄+光标）
    void detachSelHandle();                 // 分离并隐藏
    static int rectFilterThunk(UIControlHandle target, float* x, float* y,
                               float* w, float* h, void* userData);
    int filterRect(float& x, float& y, float& w, float& h);   // 网格吸附（HandleControl 拖拽几何过滤）
    void fillPropertyPanel();      // 选中变化 → 属性面板回填
    void applyModelRect(ControlModel* m);   // 模型 rect → 视口控件 rect + 选中框

    std::vector<PlacedControl> m_items;
    std::string m_selectedId;
    Control m_selHandle;           // 引擎 HandleControl（选中框+8向手柄+方向光标，附加 target）
    bool  m_propSync = false;      // 属性面板回填中（抑制回写）
    Control m_propX, m_propY, m_propW, m_propH, m_propCaption, m_propVisible, m_propEnabled;
    Control m_captionEditor;    // 动态区文本行编辑器（caption/title——与固定区"文本"双向同步）
    // E 方案同类型复用：行集按 schema 类型确定（同类型行集相同）——切换同类控件
    // 跳过销毁重建，仅执行各行刷值闭包（读控件当前值回填行显示）
    std::vector<std::function<void()>> m_rowRefreshers;
    std::string m_rowsBuiltSig;                 // 已建行集的类型签名（空=未建）
    std::set<std::string> m_collapsedGroups = {
        "背景色", "边框色", "字体及阴影", "常规"};   // 缺省全收起：初次创建/切换控件只建 4 个组标题行（快）
    Control m_propColor;           // 颜色（color-changed 事件回调：onOK 确认触发）
    std::string m_lastColorHex;    // 上次回填/已应用的颜色（echo 抑制）
    void onColorPicked(unsigned char r, unsigned char g, unsigned char b);

    // Schema 驱动动态属性区
    Control m_propDynamic;                       // propDynamic 容器（absolute 自管布局）
    Control m_dynScroll;                         // 容器底部滚动条（position-changed 驱动）
    std::unique_ptr<nlohmann::json> m_schema;    // 引擎 declarative-ui.schema.json
    std::vector<Control> m_dynamicRows;          // 已生成的行控件（销毁/滚动偏移用）
    std::vector<float>   m_dynamicRowY;          // 各行未滚动时的 baseY（与 rows 对齐）
    float m_dynScrollMax = 0;                    // 滚动范围（内容高-视野高，<0 归零）
    Control m_schemaTarget;                      // 当前行集对应的目标控件
    std::string m_schemaType;
    void applyDynScroll(float value);            // 滚动 → 行偏移+超界隐藏

    // 交互状态
    bool  m_mouseWasDown = false;
    bool  m_dragging = false;
    float m_dragOffX = 0, m_dragOffY = 0;
    std::string m_dragId;
    std::string m_dragParentId;   // 拖动起始的父容器 id（子控件拖动转局部用）
    std::function<void(bool)> m_dragCursorFn;   // 拖放光标回调（保留接口）
    bool m_dragCursorOn = false;
    Control m_dragGhost;          // 拖放跟随图标（鼠标旁控件小图标 ghost）
    Control m_alignV;             // 对齐辅助线（竖 1px——拖动中显示）
    Control m_alignH;             // 对齐辅助线（横 1px）
    Control m_dragCtl;

    Control m_toolboxTree;
    Control m_canvasTree;          // 控件树（主实例 tvTree）
    bool    m_treeSync = false;    // 编程式树操作中（抑制 select 回调回环）
    std::string m_lastToolId;
    std::string m_pendingPlaceTool;   // 放置待定工具 id（画布点击放置——P1-a）
    std::chrono::steady_clock::time_point m_lastToolTime;
    int   m_seq = 0;
    MouseSourceFn m_mouseSource;
    KeySourceFn   m_keySource;

    float m_seenX = 0, m_seenY = 0, m_seenW = 0, m_seenH = 0;   // 观测缓存

    UICornerstone::UICornerstone* m_ui = nullptr;
    Control m_canvasArea;
    std::unique_ptr<UICornerstone::UICornerstone> m_viewport;   // 子视口（承载纸面/网格/被设计内容）
    Control m_grid;                                             // 网格瓦片 Image（子视口内）

    // 画布区绝对坐标（窗口系）与内容区尺寸（工具栏下方）
    float m_areaX = 0, m_areaY = 0;
    float m_contentW = 0, m_contentH = 0;
    float m_toolbarH = 40.0f;
    std::string m_resourceRoot;    // assets 资源根（字体注册用）

    float m_zoom = 1.0f;
    bool  m_gridVisible = true;
    float m_gridCell = 40.0f;      // 画布逻辑坐标网格间距
    bool  m_snapEnabled = true;    // 吸附到网格
    bool  m_alignEnabled = true;   // 吸附到控件（智能对齐——独立开关）

    std::unique_ptr<ControlModel> m_root;
    ClickHandler m_clickHandler;
};

}  // namespace Designer