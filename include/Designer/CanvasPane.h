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
#include <string>

#include "Control.h"

class Event;

namespace UICornerstone {
class UICornerstone;
}

namespace Designer {

struct ControlModel;
struct SRect;

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
    // 鼠标源（App 注入 SDL 轮询；返回左键是否按下，填充窗口坐标）
    using MouseSourceFn = std::function<bool(float&, float&)>;
    void SetMouseSource(MouseSourceFn src) { m_mouseSource = std::move(src); }
    // 属性面板联动（App 注入；选中回填面板，面板变更回写控件）
    void SetPropertyControls(Control x, Control y, Control w, Control h, Control caption);

    // 缩放档（0.25~2.0）。改变 z → 重建网格瓦片（小纹理）。
    void SetZoom(float zoom);
    float GetZoom() const { return m_zoom; }

    void SetGridVisible(bool visible);
    bool IsGridVisible() const { return m_gridVisible; }
    void SetGridCellSize(float px);
    void SetSnapEnabled(bool snap) { m_snapEnabled = snap; }
    bool IsSnapEnabled() const { return m_snapEnabled; }

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

    // 画布控件点击（画布坐标）。
    using ClickHandler = std::function<void(const std::string& id, float cx, float cy)>;
    void SetClickHandler(ClickHandler handler) { m_clickHandler = std::move(handler); }

private:
    // 画布内容（被设计控件，z 序 = vector 序）
    struct PlacedControl {
        std::unique_ptr<ControlModel> model;
        Control ctl;
    };

    void refreshLayout();          // 同步视口与网格 Image rect（零重建）
    void registerGridTile();       // 生成瓦片 PNG → RegisterResource（含首次 provider 触发）
    void createGridImage();        // 在子视口内创建网格 Image（scale-type=tile）
    void rebuildGridTile();        // registerGridTile + createGridImage（缩放档变化）
    void registerFonts();          // 字体注册进内存 provider（视口内 Label 解析字体必需）
    void styleControl(const std::string& toolboxId, Control ctl);   // 放置控件默认样式（白纸上可见）

    void tickInput();              // 交互轮询：放置/选中/拖动
    void placeControl(const std::string& toolboxId, float cx, float cy);
    Control createViewportControl(const std::string& toolboxId, float x, float y,
                                  float w, float h, ControlModel& model) const;
    bool hitTest(float cx, float cy, std::string& outId) const;
    ControlModel* findModel(const std::string& id);
    PlacedControl* placedById(const std::string& id);
    void selectItem(const std::string& id);
    void ensureSelBox();
    void updateSelBox();
    void fillPropertyPanel();      // 选中变化 → 属性面板回填
    void applyModelRect(ControlModel* m);   // 模型 rect → 视口控件 rect + 选中框

    std::vector<PlacedControl> m_items;
    std::string m_selectedId;
    Control m_selBox;              // 选中框（视口内 Shape 多图元）
    bool  m_propSync = false;      // 属性面板回填中（抑制回写）
    Control m_propX, m_propY, m_propW, m_propH, m_propCaption;

    // 交互状态
    bool  m_mouseWasDown = false;
    bool  m_dragging = false;
    float m_dragOffX = 0, m_dragOffY = 0;
    std::string m_dragId;
    Control m_dragCtl;
    Control m_toolboxTree;
    std::string m_lastToolId;
    std::chrono::steady_clock::time_point m_lastToolTime;
    int   m_seq = 0;
    MouseSourceFn m_mouseSource;

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
    bool  m_snapEnabled = true;

    std::unique_ptr<ControlModel> m_root;
    ClickHandler m_clickHandler;
};

}  // namespace Designer