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
#include <unordered_map>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    using CtrlSourceFn = std::function<bool()>;  // Ctrl 按住态（每帧轮询一次；多选修饰键）
    void SetCtrlSource(CtrlSourceFn src) { m_ctrlSource = std::move(src); }
    using MiddleSourceFn = std::function<bool(float&, float&)>;  // 中键按下 + 窗口坐标
    void SetMiddleSource(MiddleSourceFn src) { m_middleSource = std::move(src); }
    using SpaceSourceFn = std::function<bool()>; // 空格按住态（空格+左键拖拽平移）
    void SetSpaceSource(SpaceSourceFn src) { m_spaceSource = std::move(src); }

    // 右侧控件树（tvTree）：画布控件增删/选中双向同步。
    void SetCanvasTree(Control tree);
    // P0-105：控件树拖拽父子（引擎内置换序 → reordered 回调回写模型/画布）
    void onTreeReordered(const Event& e);           // 仅记录参数（泵内不重入 ✗）
    void applyTreeReorder(const std::string& srcId, int hintKind,
                          const std::string& hintTarget);   // 下一帧处理
    bool m_treeReorderPending = false;
    std::string m_treeReorderSrc, m_treeReorderHintTarget;
    int m_treeReorderHintKind = 0;
    // P0-105：控件树右键动作
    void treeDelete();
    void treeMoveInParent(int delta);
    void treeMoveToRoot();
    void syncZOrder(const std::string& parentId);   // 引擎 z 序 = 树序（树底=最前 ✓，与新建/载入一致 ✗）
    bool engineParentOf(const std::string& modelParentId, Control& outParent,
                        float& outOriginX, float& outOriginY);
    std::string benchTreeNodeLabel() const;   // 控件树画布根节点标签（Bench 控件 id）
    void selectTreeRow(const std::string& id);   // P0-107：右键行同步选中（模型+树）
    void applyStatePreview();         // P0-114：应用当前选中控件的记忆态 + 回显组合框 ✓
    void clearAllStatePreviews();     // P0-114：全部恢复真实态（预览模式/重建 ✗）
    int StatePreviewModeOf(const std::string& id) const;
    std::unordered_map<std::string, int> m_statePreviewById;   // P0-114：每控件记忆（0/缺省=跟随 ✗）
    std::function<void(int)> m_statePreviewUiFn;               // P0-114：回显组合框（选中变化 ✗）
    void ensureBenchId();                     // P0-112：bench id 预置/自愈（唯一 ✗）
    void syncTreeAdd(const std::string& parentId, const std::string& id,
                     const std::string& label);   // 层级挂接（parentId 空=顶层）
    void syncTreeRemove(const std::string& id);
    void syncTreeRename(const std::string& id, const std::string& label);   // 树节点标签更新
    void syncTreeSelection();
    // 控件 id 重命名（用户需求：代码引用）——模型/引用（子 parentId、tabs page-id、
    // 放置目标、选中集）/引擎 id 注册/锚定父 child-id/控件树 全链更新。
    // false = 无效（空/重复/含空白/保留键）——未做任何修改。
    bool renameControlId(const std::string& oldId, std::string newId);
    // 控件 id 行编辑完成再提交（防逐键改名致树节点反复重建——500ms 空闲/回车/切换选中）
    void commitPendingIdRename();

    // ── 事件面板（F4：右栏"事件"Tab）──
    // 绑定行：事件类型 ComboBox（按控件类型注册表过滤）+ handler EditBox + 建议命名/添加；
    // 已绑定列表：控件 id + 事件名 + handler + 删除（动态行——选中变化时重建）；
    // 持久化 extended["events"] = { "onClick": "handlerName" }（引擎布局 JSON 1:1）；
    // 冲突/不合规：C 标识符校验 + handler 唯一（ev_err 红字提示，拒绝添加）。
    void SetEventPanel(Control typeCombo, Control handlerEdit, Control suggestBtn,
                       Control addBtn, Control errLabel, Control listHost);
    void refreshEventPanel();      // 选中变化 / 绑定增删后刷新（下拉 + 列表）
    bool addEventBinding(const std::string& ctrlId, const std::string& jsonEvent,
                         const std::string& handler);
    bool removeEventBinding(const std::string& ctrlId, const std::string& jsonEvent);
    bool isValidIdentifier(const std::string& name) const;      // C 标识符规范
    bool isHandlerNameUnique(const std::string& name) const;    // 全体绑定中唯一
    std::string suggestHandlerName(const std::string& ctrlId, const std::string& jsonEvent) const;
    struct EventBinding { std::string controlId, jsonEvent, handler; };
    std::vector<EventBinding> collectEventBindings() const;     // 先序遍历全树（导出用）
    // 画布设置对话框（P0-84 绕过：对话框输入通道已证可用——id/窗口标题）
    void ShowBenchSettingsDialog();

    // P1-b 放置目标提示：主实例 statusBar（statusHint 段显示当前目标容器）。
    void SetStatusTarget(Control statusBar);
    // 属性面板容器（Delete 键归属判定：面板内编辑时画布不删控件——SDL 轮询不感知引擎焦点）。
    void SetPropertyPanel(Control panel) { m_propPanel = panel; }

    // P2 多选：对齐命令下拉（tb_align，选中 ≥2 启用）+ 执行
    // （0-5：左/水平居中/右/顶/垂直居中/底；作用于选择集全局包围盒）。
    void SetAlignCommandControl(Control combo);
    void AlignSelection(int mode);
    // 引擎浮层消费点击标记（App 在工具栏下拉/菜单项回调中调用）：本帧画布交互跳过，
    // 防弹层项点击落到画布（SDL 轮询独立于引擎路由）误清选择。
    void SuppressCanvasClickThisFrame() { m_suppressCanvasClick = true; }

    // P1 预览模式（设计↔预览切换）：清选择/去网格线/关设计器交互（控件交互由引擎
    // 子视口事件驱动）；退出恢复网格与放置目标提示。
    void SetPreviewMode(bool on);
    void SetStatePreview(int mode);   // P0-114：设计态状态预览（0=跟随 1=常态 2=悬停 3=按下 4=禁用 ✗）
    void SetStatePreviewUiCallback(std::function<void(int)> fn) { m_statePreviewUiFn = std::move(fn); }
    bool IsPreviewMode() const { return m_previewMode; }

    // 删除选中控件：销毁视口控件 + 移除模型 + 取消选中（清空属性面板/隐藏选中框）。
    void deleteSelected();
    // F2.5 撤销/重做（快照式：模型序列化入栈；同键短时编辑合并为一步）
    static constexpr size_t kMaxUndoSteps = 50;     // 需求 F2.5：至少 50 步
    void commitUndo(const std::string& coalesceKey = "");   // 操作后记录（加载/恢复期挂起）
    bool Undo();
    bool Redo();
    bool CanUndo() const { return m_undoPos > 0; }
    bool CanRedo() const { return m_undoPos + 1 < m_undoStack.size(); }
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
    // 缩放变化通知（App 接线：同步缩放档下拉）——所有缩放路径（含滚轮）统一触发
    using ZoomChangedFn = std::function<void()>;
    void SetZoomChangedFn(ZoomChangedFn fn) { m_zoomChangedFn = std::move(fn); }
    // 文档脏标记通知（App 接线：窗口标题"未保存 *"）——编辑提交/撤销/重做时触发
    using DirtyChangedFn = std::function<void()>;
    void SetDirtyChangedFn(DirtyChangedFn fn) { m_dirtyFn = std::move(fn); }

    // 设计画布尺寸（基准窗体，缺省 1280×800）：bench 固定为设计尺寸（不随视口/缩放漂移），
    // 纸面=设计尺寸区域；根级锚定（P0-61）/导出以此为基准。
    void SetCanvasSize(float w, float h);
    float GetCanvasW() const { return m_designW; }
    float GetCanvasH() const { return m_designH; }

    // 画布（Bench）编辑（用户需求）：空白处单击选中画布 → 属性栏可改 id/标题/
    // 尺寸/缩放模式/背景色（alpha=0 透明——游戏场景 UI 叠加透出）。缩放模式与
    // 背景随导出 viewport 携带（预览生效）；标题经 .csd metadata（预览窗口标题）。
    bool IsBenchSelected() const { return m_benchSelected; }
    const std::string& BenchWindowTitle() const { return m_benchTitle; }
    const std::string& BenchId() const { return m_benchId; }

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

    // ── 项目/布局 IO（菜单：新建/打开/保存/另存为/导入 JSON/导出 JSON）──
    bool SaveProject(const std::string& path);      // 写 .csd（metadata + layout 整包）
    bool OpenProject(const std::string& path);      // 读 .csd → 重建画布（清空前置）
    bool ExportLayout(const std::string& path);     // 写引擎兼容布局 JSON（F7.1）
    bool moveTabPage(const std::string& tabsId, int from, int to);   // 页序调整（拖动排序内核）
    bool ExportCode(const std::string& dir);        // F4.4/F7.2/F7.3：可编译 C 宿主工程导出
    bool ExportCodeCpp(const std::string& dir);     // F7.4：可编译 C++（C++ Binding）宿主工程导出
    bool ExportReport(const std::string& dir, int shotW = 0, int shotH = 0);   // F7.3：项目报告（report.md + screenshot.png；shotW/H>0 表示截图已由调用方（预览实例）写入）
    bool WriteScreenshotPng(const std::string& path, const uint8_t* px, int w, int h);   // 轻量 PNG 写出（报告截图共用编码器）
    void SetShareDir(const std::string& dir) { m_shareDir = dir; }   // 设计器 exe 目录（sdk 头/DLL/assets 分发源）
    bool ImportLayout(const std::string& path);     // 读引擎布局 JSON → 重建画布
    void NewProject();                              // 清空画布（新建）
    nlohmann::json serializeLayoutJson() const;     // 模型树 → 引擎兼容布局 JSON（保存/导出共用）
    bool buildFromLayoutJson(const nlohmann::json& layout, bool useViewportSize);  // 布局 JSON → 画布
    void clearCanvas();                             // 清空全部画布控件/模型/树节点
    const std::string& ProjectPath() const { return m_projectPath; }
    void SetProjectPath(const std::string& p) { m_projectPath = p; }
    void SetStatusHint(const std::string& text) { setStatusHint(text); }   // 状态栏提示（App 用）
    bool SelfTestIo(const std::string& dir);        // auto=2 自测：放置→保存→打开→导出→导入往返
    void SelfTestPlaceSample();                     // auto=3 冒烟：放置少量样例控件（预览窗口测试）
    std::string SelfTestIdOfType(const std::string& type) const;   // auto=3：样例控件 id（预览实例校验）

    // ── 资源面板（布局级 resourceProviders：插入/删除按钮 + 名称弹窗 + 目录对话框）──
    void SetResourcePanel(Control list);            // lvResource 接线（App）
    bool AddResourceProvider(const std::string& name, const std::string& path);   // 唯一校验
    bool UpdateSelectedResourceProvider(const std::string& name, const std::string& path);  // 修改选中行（唯一校验排除自身）
    bool SelectedResourceProvider(std::string& name, std::string& path) const;    // 选中行回填（App 弹窗）
    void RemoveSelectedResourceProvider();          // 删除列表选中行
    void refreshResourceList();                     // 列表重填（名称/路径）
    void mountResourceProviders();                  // 设计态挂载（LoadLayout 顶层 resourceProviders）
    const std::vector<std::pair<std::string, std::string>>& ResourceProviders() const { return m_resourceProviders; }

private:
    // 画布内容（被设计控件，z 序 = vector 序）
    struct PlacedControl {
        std::unique_ptr<ControlModel> model;
        Control ctl;
        std::string parentId;        // 容器子控件：父容器 id（空=画布根）
    };

    void refreshLayout();          // 同步视口与网格 Image rect（零重建）
    void applyCanvasSize();        // 设计尺寸 → SetCanvasSize（P0-61③ 即时）+ 视口/纸面/滚动条刷新
    void selectBench();            // 选中画布（Bench）——属性面板显示画布行
    void buildBenchRows();         // 画布属性动态行（id/标题/缩放模式/背景色）
    void applyBenchBackground();   // 画布背景 → 设计器画布视口（WYSIWYG；alpha=0 透明）
    void updateViewportRect();     // 视口尺寸 = 内容区扣除滚动条边条（设计尺寸/缩放变化须重算）
    void updateGridRect();         // 纸面/网格 rect = 设计尺寸（逻辑；drawRect=设计尺寸×zoom）
    void clampPan();               // 平移钳制（纸面小于视口→居中；大于→边缘不越界）
    void applyPan();               // 平移 → 视口锚点偏移（recompute 会重置锚点，须重算增量）
    void onWheel(float scrollY);   // 滚轮：Ctrl=按档缩放（光标锚点）；普通=垂直滚动
    void SetZoomAt(float zoom, float sx, float sy);   // 缩放并保持光标下画布点屏幕位置不变
    void syncScrollBars();         // 画布滚动条：rect/范围/值/可见性（纸面超出视口时显示）
    void updateStatusPos(bool inVp, float cx, float cy);   // 状态栏鼠标画布坐标（视口外 "-"）
    void registerGridTile();       // 生成瓦片 PNG → RegisterResource（含首次 provider 触发）
    void createGridImage();        // 在子视口内创建网格 Image（scale-type=tile）
    void rebuildGridTile();        // registerGridTile + createGridImage（缩放档变化）
    void styleControl(const std::string& toolboxId, Control ctl,
                      ControlModel& model);   // 放置控件默认样式（白纸上可见；同步模型 colors）

    void tickInput();              // 交互轮询：放置/选中/拖动
    void placeControl(const std::string& toolboxId, float cx, float cy, bool fromDrag = false);
    Control createViewportControl(const std::string& toolboxId, float x, float y,
                                  float w, float h, ControlModel& model) const;
    bool hitTest(float cx, float cy, std::string& outId);   // 非 const：子控件命中需 placedById（非 const）
    ControlModel* findModel(const std::string& id);
    PlacedControl* placedById(const std::string& id);
    // P1-a：控件画布全局原点（沿 parentId 链累加局部 rect；WinFrame 加 ClientPanel 内容偏移）
    void globalOriginOf(const PlacedControl& pc, float& gx, float& gy);
    // P1-b：容器 id → 挂载父控件（WinFrame 严格 ClientPanel）+ 内容区全局 rect
    bool resolveContainer(const std::string& id, Control& outParent, SRect& outContentRect);
    void setPlaceTarget(const std::string& id);   // 更新当前放置目标 + 状态栏提示
    void refreshPlaceTargetHint();
    void setStatusHint(const std::string& text);  // 状态栏 statusHint 段写入 + 重排（P0-53 过渡）
    void writeStatusItem(const char* itemId, const std::string& text);   // 状态栏段写入 + 重排
    void refreshStatusRight();                    // 状态栏右段：缩放/控件数/网格
    // 智能对齐（x/y 为选中控件局部坐标，引用修正）：命中轴吸附+参考线更新；返回命中轴
    void applyAlignSnap(float& x, float& y, float w, float h, bool& hitX, bool& hitY);
    void selectItem(const std::string& id);
    // P2 多选：选择集维护（m_selectedId = 主选 = 末位）、次级外框、包围盒、组拖动
    void setSelection(std::vector<std::string> ids);
    void toggleSelection(const std::string& id);
    void updateSelOutlines();
    // P2 框选（橡皮筋）：空白处按下拖动 → 相交控件选择（Ctrl=并入当前选择；微小移动=空白点击）
    void beginBand(float cx, float cy, bool additive);   // 起框：重建面板置顶（z 序）+ 记录起点
    void updateBand(float cx, float cy);
    void finalizeBand(float cx, float cy);
    void selectionGlobalBBox(float& x, float& y, float& w, float& h);
    void parentGlobalOriginOf(const PlacedControl& pc, float& px, float& py);
    // P0-61：锚定目标父级——容器子控件→容器；顶层→根（bench，经 Root() 暴露）
    Control anchorParentOf(const PlacedControl& pc);
    // P0-59：引擎重排（布局切换/锚点应用）后同步全部模型 rect——防模型失配致选不中
    void syncModelsFromEngine();
    void beginGroupDrag(const std::string& primaryId);
    void applyGroupDelta(float dx, float dy);          // 引擎路径：增量
    void applyGroupFromStart(float dx, float dy);      // 设计器路径：起始快照绝对
    std::string readCaptionOf(const std::string& id);  // 多选共有属性：caption 读取（含内部 Label 直控）
    void applyCaptionTo(const std::string& id, const std::string& v);
    void writeColorSlotTo(const std::string& id, const std::string& prop,
                          const std::string& grpName, const UIColor& c);   // P2 多选：颜色槽批量写
    void writeStringPropTo(const std::string& id, const std::string& prop,
                           const std::string& v);                          // P2 多选：string 行批量写
    void applyComboItemsText(ControlModel& m, const std::string& text);    // 结构化：combo items 文本 → 模型 + 引擎同步
    void applyStatusBarItemsText(ControlModel& m, const std::string& text);  // 结构化：状态栏段
    void applyListColumnsText(ControlModel& m, const std::string& text);     // 结构化：列表列（标题|宽度）
    void applyListRowsText(ControlModel& m, const std::string& text);        // 结构化：列表行（| 分隔单元格）
    void syncListRows(ControlModel& m, int engineRowCount);                  // 列表行引擎重放（列变更后亦调用）
    void applyTreeItemsText(ControlModel& m, const std::string& text);       // 结构化：树节点（> 数量=层级）
    // 结构化模型 → 引擎同步（加载/打开路径复用；文本编辑路径由 apply*Text 调用）
    // oldIds/oldCount：引擎当前（旧）状态——编辑路径按旧移除，加载路径传空/0
    void syncComboItems(ControlModel& m);
    void syncStatusBarItems(ControlModel& m, const nlohmann::json& oldItems);
    void syncListColumns(ControlModel& m, int engineColCount);
    void syncTreeItems(ControlModel& m);
    void applyMenusText(ControlModel& m, const std::string& text);         // 结构化：menu-bar menus（> 层级/@快捷键/-- 分隔）
    void syncMenus(ControlModel& m, const nlohmann::json& oldMenus);
    void applyTabsText(ControlModel& m, const std::string& text);          // 结构化：tab-control tabs（一行一页签标题）
    void syncTabs(ControlModel& m, const nlohmann::json& oldTabs);
    void removePlacedSubtree(const std::string& rootId);                   // 销毁放置控件子树（引擎/模型/树）
    std::string resolvedParentIdOf(const std::string& targetId);
    void syncTabCurrentToPage(const std::string& pageId);   // 树选页 Panel → 切换所属 TabControl 当前页
    bool effectivelyVisible(const PlacedControl& pc);        // 自身+祖先链引擎可见位（隐藏页子树不参与命中）
    bool isTabPage(const std::string& id);                   // id 是否 Tab 页 Panel（父为 TabControl）
    bool isSelectedTabPage();                                // 选中项=Tab 页 Panel（禁手柄/禁反向同步）
    int  tabItemIndexAt(const std::string& tabId, float cx, float cy,
                        bool clampToNearest);   // P0-91① 页签项命中（局部→画布；clamp=拖放就近钳位）
    void writeTabColorTo(const std::string& id, const std::string& key, const UIColor& c);  // P0-90 页签配色
    // Tab 页删除：文本行 ↔ 旧页配对（标题优先；等行数按序兜底=就地改标题）+ 确认弹窗
    nlohmann::json pairTabsWithOld(const nlohmann::json& oldTabs,
                                   const std::vector<std::string>& lines,
                                   std::vector<std::string>& deletedOut);
    void showTabDeleteConfirm(const std::string& tabsId, const std::string& pendingText,
                              const std::string& msg);
    Control m_tabDeleteDlg;                                  // 删除页确认对话框（懒建复用）
    Control m_tabDeleteLabel;
    std::string m_pendingTabsId;                             // 待确认的 tabs 编辑（id/文本）
    std::string m_pendingTabsText;
    bool m_tabDeleteConfirmed = false;                       // 确认后放行一次（防弹窗循环）
    // 页签排序拖动（P0-91③：TabGetItemRect 判落点 + TabMovePage 迁移）
    std::string m_tabDragId;
    int m_tabDragFrom = -1;
    int m_tabDragTo = -1;
    bool m_tabDragActive = false;
    Control m_tabDragGhost;                  // 拖动光标旁小图标（拖动可见性反馈）           // 目标容器 → 实际父 id（TabControl→当前页 Panel）
    void applyShapePointsText(ControlModel& m, const std::string& text);   // 结构化：shape points（一行一点 x,y）
    void syncShapePoints(ControlModel& m);
    void applyModelToEngine(PlacedControl& pc);     // 模型属性 → 引擎控件（加载应用）
    void ensureSelHandle(Control target);   // 引擎 HandleControl 附加（选中框+8向手柄+光标）
    void detachSelHandle();                 // 分离并隐藏
    static int rectFilterThunk(UIControlHandle target, float* x, float* y,
                               float* w, float* h, void* userData);
    int filterRect(float& x, float& y, float& w, float& h);   // 网格吸附（HandleControl 拖拽几何过滤）
    void fillPropertyPanel();      // 选中变化 → 属性面板回填
    void applyModelRect(ControlModel* m);   // 模型 rect → 视口控件 rect + 选中框

    std::vector<PlacedControl> m_items;
    std::string m_selectedId;                   // 主选（= m_selIds 末位；空=未选中）
    std::string m_projectPath;                  // 当前 .csd 路径（空=未关联文件）
    std::vector<std::pair<std::string, std::string>> m_resourceProviders;   // 布局级资源挂载点
    Control m_resList;                          // 资源列表（lvResource，主实例）
    int m_resRows = 0;                          // 引擎列表当前行数（RemoveRow 越界不报——记账）
    std::vector<std::string> m_selIds;          // P2 多选：选择集
    std::vector<Control> m_selOutlines;         // P2 多选：次级选中外框池（橙 1px 描边）
    Control m_alignCombo;                       // P2 多选：tb_align（≥2 启用）
    bool m_suppressCanvasClick = false;         // 引擎浮层消费点击 → 本帧画布交互跳过
    bool m_previewMode = false;                 // P1 预览模式（设计器交互关闭）
    bool m_gridVisibleSaved = true;             // 进入预览前的网格开关（退出恢复）
    // P2 框选（橡皮筋）
    bool  m_bandActive = false;
    bool  m_bandAdditive = false;               // Ctrl 启动：并入当前选择
    float m_bandSX = 0, m_bandSY = 0;           // 起始点（画布逻辑坐标）
    Control m_bandRect;                         // 框选矩形（半透明填充+描边，视口内）
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
    std::vector<Control> m_dynamicRows;          // 本趟生效行控件（滚动偏移/宽度同步用）
    std::vector<float>   m_dynamicRowY;          // 各行未滚动时的 baseY（与 rows 对齐）
    // 动态行池（跨类型复用——隐藏不销毁，换控件时重排+刷值；仅首次创建慢）
    std::unordered_map<std::string, Control> m_rowPool;      // 行种类 → 常驻控件
    std::unordered_set<std::string> m_rowPoolUsed;           // 本趟使用标记（未用→隐藏）
    Control takePoolRow(const std::string& kind, const std::function<Control()>& create);
    void hideUnusedPoolRows();
    float m_dynScrollMax = 0;                    // 滚动范围（内容高-视野高，<0 归零）
    Control m_schemaTarget;                      // 当前行集对应的目标控件（主选——读回/回填）
    std::vector<Control> m_schemaTargets;        // P2 多选：批量写目标（全部选中）
    std::string m_schemaType;
    void applyDynScroll(float value);            // 滚动 → 行偏移+超界隐藏

    // 交互状态
    // F2.5 撤销/重做栈（m_undoStack[m_undoPos]=当前状态；coalesce 0.7s 内同键覆盖合并）
    std::vector<std::string> m_undoStack;
    size_t m_undoPos = 0;
    std::string m_undoKey;
    double m_undoTime = 0.0;
    bool m_undoSuspended = false;       // 加载/恢复期不记录
    bool m_engineRectDirty = false;     // 引擎手柄拖拽/缩放中 rect 变化（松开提交一次）
    std::string m_engineRectId;
    bool m_dragOrigValid = false;       // 设计器拖动起始 rect（未移动不记历史）
    SRect m_dragOrigRect{ 0, 0, 0, 0 };
    void restoreSnapshot(const std::string& json);
    void resetUndo();

    // F4.4/F7.2 代码导出生成器（ExportLayout 事件侧车与 ExportCode 共用）
    std::string buildEventsHeaderText() const;   // events.h（唯一 handler 声明序）
    std::string buildEventsSourceText() const;   // events.c 骨架实现
    std::string buildEventListText() const;      // event_list.json（机器可读）
    std::string buildMainCText() const;          // F7.3：宿主 main.c（LoadLibrary 显式 C ABI）
    std::string buildMainCppText() const;        // F7.4：宿主 main.cpp（C++ Binding）
    std::string buildHostCMakeText() const;      // F7.3/F7.4：生成工程 CMakeLists.txt（C 版）
    std::string buildHostCppCMakeText() const;   // F7.4：生成工程 CMakeLists.txt（C++ Binding 版）
    std::string buildHostReadmeText(bool cpp) const;   // 生成工程 README（C / C++）
    std::string buildBuildScriptText(bool release) const;   // 生成工程一键构建脚本（bat，CRLF）
    bool exportProject(const std::string& dir, bool cpp);   // 共用导出实现
    static std::string cEscape(const std::string& s);        // C 字符串转义
    bool validateEventBindings(std::string& badName) const;   // 非法标识符/重名 → false
    std::string m_shareDir;                      // 设计器 exe 目录（App 注入）

    bool  m_mouseWasDown = false;
    bool  m_dragging = false;
    float m_dragOffX = 0, m_dragOffY = 0;
    std::string m_dragId;
    std::string m_dragParentId;   // 拖动起始的父容器 id（子控件拖动转局部用）
    // P2 多选组拖动：设计器路径=起始快照；引擎 HandleControl 路径=逐帧增量（filterRect）
    struct GroupEntry { std::string id; SRect startLocal; };
    std::vector<GroupEntry> m_groupEntries;
    float m_groupStartX = 0, m_groupStartY = 0;   // 主选起始全局（设计器路径）
    float m_groupPrevX = 0, m_groupPrevY = 0;     // 主选上帧局部（引擎路径增量基准）
    bool  m_groupDrag = false;
    std::function<void(bool)> m_dragCursorFn;   // 拖放光标回调（保留接口）
    bool m_dragCursorOn = false;
    Control m_dragGhost;          // 拖放跟随图标（鼠标旁控件小图标 ghost）
    Control m_alignV;             // 对齐辅助线（竖 1px——拖动中显示）
    Control m_alignH;             // 对齐辅助线（横 1px）
    Control m_dragCtl;

    Control m_toolboxTree;
    Control m_canvasTree;          // 控件树（主实例 tvTree）
    Control m_statusTarget;        // 主实例 statusBar（放置目标提示）
    Control m_propPanel;           // 主实例 propertyPanel（Delete 归属判定）
    bool m_panelEditActive = false;   // 鼠标最后按下在属性面板内（Delete 归面板编辑器）
    std::string m_placeTargetId;   // 当前放置目标容器 id（空=画布根；双通道更新，最新者优先）
    bool    m_treeSync = false;    // 编程式树操作中（抑制 select 回调回环）
    std::string m_lastToolId;
    std::string m_pendingPlaceTool;   // 放置待定工具 id（画布点击放置——P1-a）
    std::chrono::steady_clock::time_point m_lastToolTime;
    int   m_seq = 0;
    MouseSourceFn  m_mouseSource;
    KeySourceFn    m_keySource;
    CtrlSourceFn   m_ctrlSource;
    MiddleSourceFn m_middleSource;
    SpaceSourceFn  m_spaceSource;
    // 画布滚动条（canvasArea 子级：视口右缘/下缘让条，非覆盖）
    Control m_hScroll, m_vScroll;
    float m_vpW = 0, m_vpH = 0;    // 实际视口尺寸（内容区扣除滚动条边条）
    bool  m_scrollSync = false;    // 滚动条程序化同步中（抑制回调）
    // 画布平移（中键拖拽 / 空格+左键拖拽）：逻辑偏移 + 引擎锚点增量记账
    float m_panX = 0, m_panY = 0;
    float m_panAppliedX = 0, m_panAppliedY = 0;
    bool  m_panning = false;
    bool  m_middleWasDown = false;
    float m_panStartSX = 0, m_panStartSY = 0, m_panStartX = 0, m_panStartY = 0;
    ZoomChangedFn m_zoomChangedFn;
    DirtyChangedFn m_dirtyFn;

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
    float m_designW = 1280.0f;     // 设计画布（基准窗体）尺寸
    float m_designH = 800.0f;
    // 画布（Bench）属性（导出 viewport 携带；标题入 .csd metadata）
    UIColor m_benchBg{255, 255, 255, 255}; // 画布背景色（缺省白=纸面；alpha=0 透明=透出底层）
    int m_benchScaleMode = 0;      // 0=off 1=fit 2=stretch
    std::string m_benchTitle;      // 窗口标题（预览窗 / 代码生成）
    std::string m_benchId;         // 画布 id（代码引用；viewport.id）
    bool m_benchSelected = false;  // 画布选中态（与控件选择互斥）
    Control m_benchDlg, m_benchDlgId, m_benchDlgTitle;   // 画布设置对话框（复用）
    // 事件面板句柄（App 接线）
    Control m_evType, m_evHandler, m_evSuggest, m_evAdd, m_evErr, m_evHost;
    std::vector<Control> m_evRows;    // 动态绑定行（刷新时销毁重建）
    std::vector<std::string> m_evTypeItems;   // 事件下拉值平行表（选中索引 → JSON 事件名）
    // 控件 id 行待提交状态（编辑完成再改名）
    std::string m_idRenameTarget, m_idRenameText;
    std::chrono::steady_clock::time_point m_idRenameDeadline;
    bool m_idRenamePending = false;
    bool  m_gridVisible = true;
    float m_gridCell = 40.0f;      // 画布逻辑坐标网格间距
    bool  m_snapEnabled = true;    // 吸附到网格
    bool  m_alignEnabled = true;   // 吸附到控件（智能对齐——独立开关）

    std::unique_ptr<ControlModel> m_root;
    ClickHandler m_clickHandler;
};

}  // namespace Designer