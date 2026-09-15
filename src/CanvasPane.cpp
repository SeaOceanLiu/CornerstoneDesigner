// CanvasPane.cpp
// CornerstoneDesigner
//
// 中央画布实现（纸面+网格在子视口内部，tile 平铺）：
// - 子视口覆盖 canvasArea 内容区（工具栏下方），承载全部画布内容；
// - 网格 = 小瓦片纹理（白底 + 线）+ scale-type=tile 平铺铺满视口：
//     resize/splitter → 只 SetViewport + SetRect（零纹理重建）
//     -/+ 缩放/gridCell → 重建小瓦片（几 KB）
// - 视口创建须晚于首次 RegisterResource（内存 provider 在首次注册时才创建并挂载
//   owner；子视口初始化时是资源 provider 的指针拷贝，挂载后再创建才能解析）。

#include "Designer/CanvasPane.h"

#include "Designer/ControlModel.h"

#include "UICornerstone.h"
#include "Control.h"
#include "Event.h"
#include "PropertyNames.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {

// ── 轻量 RGBA → PNG 编码（IDAT zlib stored 封装，无第三方依赖）──
void putU32(std::vector<unsigned char>& out, unsigned v) {
    out.push_back((unsigned char)(v >> 24)); out.push_back((unsigned char)(v >> 16));
    out.push_back((unsigned char)(v >> 8));  out.push_back((unsigned char)v);
}

void putTag(std::vector<unsigned char>& out, const char* t) {
    for (int i = 0; t[i]; ++i) out.push_back((unsigned char)t[i]);
}

unsigned long crc32(unsigned long crc, const unsigned char* buf, int len) {
    static unsigned long table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            unsigned long c = (unsigned long)i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc ^= 0xffffffffUL;
    for (int i = 0; i < len; ++i) crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffUL;
}

void pngChunk(std::vector<unsigned char>& out, const char* t,
              const std::vector<unsigned char>& data) {
    putU32(out, (unsigned)data.size());
    putTag(out, t);
    size_t start = out.size();
    for (auto b : data) out.push_back(b);
    unsigned long c = crc32(0, reinterpret_cast<const unsigned char*>(t), 4);
    c = crc32(c, out.data() + start, (int)data.size());
    putU32(out, (unsigned)c);
}

// zlib stored 流封装（PNG IDAT 要求 zlib 容器：2 字节头 + stored 块 + Adler-32）
std::vector<unsigned char> zlibWrapStored(const std::vector<unsigned char>& raw) {
    std::vector<unsigned char> out;
    out.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    out.push_back(0x78);
    out.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t block = std::min<size_t>(65535, raw.size() - off);
        bool final = (off + block == raw.size());
        out.push_back(final ? 0x01 : 0x00);
        out.push_back((unsigned char)(block & 0xFF));
        out.push_back((unsigned char)((block >> 8) & 0xFF));
        out.push_back((unsigned char)(~(block & 0xFF) & 0xFF));
        out.push_back((unsigned char)(~((block >> 8) & 0xFF) & 0xFF));
        out.insert(out.end(), raw.begin() + off, raw.begin() + off + block);
        off += block;
    }
    unsigned long s1 = 1, s2 = 0;
    for (unsigned char b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    unsigned long a = (s2 << 16) | s1;
    out.push_back((unsigned char)((a >> 24) & 0xFF));
    out.push_back((unsigned char)((a >> 16) & 0xFF));
    out.push_back((unsigned char)((a >> 8) & 0xFF));
    out.push_back((unsigned char)(a & 0xFF));
    return out;
}

// 网格瓦片：tilePx×tilePx，白底 + #303030 1px 线（每 cellPx 一条，左上边界对齐）。
// 平铺后线在 0, cellPx, 2*cellPx… 处连续；瓦片右/下边缘不画线（由下一瓦片左/上边界提供）。
std::vector<unsigned char> buildGridTilePng(int tilePx, int cellPx) {
    std::vector<unsigned char> raw;
    raw.reserve((size_t)tilePx * ((size_t)tilePx * 4 + 1));
    for (int y = 0; y < tilePx; ++y) {
        raw.push_back(0);   // filter 0
        bool yLine = (y % cellPx == 0);
        for (int x = 0; x < tilePx; ++x) {
            bool line = yLine || (x % cellPx == 0);
            if (line) { raw.push_back(0x30); raw.push_back(0x30); raw.push_back(0x30); raw.push_back(0xC0); }
            else { raw.push_back(0xFF); raw.push_back(0xFF); raw.push_back(0xFF); raw.push_back(0xFF); }
        }
    }
    std::vector<unsigned char> idat = zlibWrapStored(raw);

    std::vector<unsigned char> png;
    png.reserve(idat.size() + 128);
    const unsigned char sig[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    png.insert(png.end(), sig, sig + 8);

    std::vector<unsigned char> ihdr;
    putU32(ihdr, (unsigned)tilePx); putU32(ihdr, (unsigned)tilePx);
    ihdr.push_back(8);
    ihdr.push_back(6);   // RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    pngChunk(png, "IHDR", ihdr);
    pngChunk(png, "IDAT", idat);
    pngChunk(png, "IEND", {});
    return png;
}

constexpr int kTileCells = 4;                  // 每瓦片 4×4 格（减少每帧平铺绘制调用数）
constexpr int kMaxTilePx = 512;                // 瓦片尺寸上限（防缩放档过大）

}  // namespace

namespace Designer {

CanvasPane::CanvasPane() = default;
CanvasPane::~CanvasPane() = default;

bool CanvasPane::Create(UICornerstone::UICornerstone& ui, Control canvasArea, float toolbarH,
                        const std::string& resourceRoot) {
    m_ui = &ui;
    m_canvasArea = canvasArea;
    m_toolbarH = toolbarH;
    m_resourceRoot = resourceRoot;
    if (!m_canvasArea.IsValid() || !m_ui) return false;

    // 顺序关键：先注册瓦片资源（触发 owner 内存 provider 创建并挂载），
    // 再创建子视口（视口初始化时继承 owner 的 provider 指针）。
    registerGridTile();
    registerFonts();       // 视口内 Label/Button 的字体解析依赖内存 provider
    refreshLayout();
    createGridImage();
    return true;
}

void CanvasPane::Destroy() {
    if (m_grid.IsValid()) m_grid.Destroy();
    m_grid = Control();
    m_viewport.reset();   // 子视口共享 owner 后端，此处仅释放代理
    m_ui = nullptr;
    m_root = nullptr;
}

// 子视口驱动（App 主循环显式调用；事件回退经其事件循环生效）：
void CanvasPane::ProcessViewportEvents() {
    if (m_viewport) m_viewport->ProcessEvents();
}
void CanvasPane::UpdateViewport(float deltaSeconds) {
    if (m_viewport) m_viewport->Update((double)deltaSeconds);
}
void CanvasPane::RenderViewport() {
    if (m_viewport) m_viewport->Render();
}

// 同步视口与网格 Image 的 rect（零纹理重建）。
// canvasArea.GetRect() 为相对父（middleArea）的局部坐标；窗口绝对坐标 = 父偏移 + 局部。
void CanvasPane::refreshLayout() {
    if (!m_ui || !m_canvasArea.IsValid()) return;
    UIRect area = m_canvasArea.GetRect();

    float ox = 0, oy = 0;
    Control parent = m_ui->FindControl("middleArea");
    if (parent.IsValid()) { UIRect pr = parent.GetRect(); ox = pr.x; oy = pr.y; }
    m_areaX = ox + area.x;
    m_areaY = oy + area.y;

    float top = m_toolbarH;
    m_contentW = area.w;
    m_contentH = area.h - top;
    if (m_contentH < 8.0f) m_contentH = 8.0f;

    if (!m_viewport) {
        m_viewport = m_ui->CreateViewport(m_areaX, m_areaY + top, m_contentW, m_contentH);
    } else {
        m_viewport->SetViewport(m_areaX, m_areaY + top, m_contentW, m_contentH);
    }

    // 网格 Image 铺满视口内容区（视口局部坐标 (0,0)）。
    if (m_grid.IsValid()) {
        m_grid.SetRect(0, 0, m_contentW, m_contentH);
    }
}

// 生成瓦片 PNG 并注册（首次调用触发 owner 内存 provider 创建/挂载）。
void CanvasPane::registerGridTile() {
    if (!m_ui) return;
    int cellPx = (int)std::ceil(m_gridCell * m_zoom);
    if (cellPx < 2) cellPx = 2;
    int tilePx = cellPx * kTileCells;
    while (tilePx > kMaxTilePx && kTileCells > 1) {
        cellPx = std::max(2, cellPx / 2);
        tilePx = cellPx * kTileCells;
    }

    std::vector<unsigned char> png = buildGridTilePng(tilePx, cellPx);
    if (png.empty()) return;
    if (!m_ui->RegisterResource("dc_canvas_grid", png.data(), png.size())) {
        std::printf("[CanvasPane] RegisterResource grid tile failed\n");
    }
}

// 在子视口内创建网格 Image（scale-type=tile）。重建时先销毁旧图。
void CanvasPane::createGridImage() {
    if (!m_viewport || !m_ui) return;
    if (m_grid.IsValid()) { m_grid.Destroy(); m_grid = Control(); }
    m_grid = m_viewport->CreateImage("provider:dc_canvas_grid", 0, 0, m_contentW, m_contentH);
    if (!m_grid.IsValid()) {
        std::printf("[CanvasPane] CreateImage(grid) failed\n");
        return;
    }
    m_grid.SetEnum(PropertyNames::kScaleType, PropertyNames::kScaleTypeTile);
    m_grid.SetBool(PropertyNames::kVisible, m_gridVisible);
}

// 重建网格瓦片（缩放档/gridCell 变化）。
void CanvasPane::rebuildGridTile() {
    if (!m_viewport) return;
    registerGridTile();
    createGridImage();
}

// 字体注册：视口实例的 provider 继承自 owner 的内存 provider（未命中不做文件系统
// 回退），placed 控件的字体文件须显式注册才能解析。
void CanvasPane::registerFonts() {
    if (!m_ui || m_resourceRoot.empty()) return;
    static const char* kFonts[] = {
        "fonts/HarmonyOS_Sans_SC_Regular.ttf",
        "fonts/HarmonyOS_Sans_SC_Thin.ttf",
        "fonts/Asul-Bold.ttf",
        "fonts/MapleMono-NF-CN-Regular.ttf",
        "fonts/Muyao-Softbrush.ttf",
        "fonts/Quando-Regular.ttf",
    };
    for (const char* rel : kFonts) {
        const std::string full = m_resourceRoot + "/" + rel;
        FILE* f = fopen(full.c_str(), "rb");
        if (!f) continue;   // 缺失字体跳过（引擎回落内置默认）
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            std::vector<unsigned char> buf((size_t)size);
            size_t rd = fread(buf.data(), 1, (size_t)size, f);
            if (rd == (size_t)size)
                m_ui->RegisterResource(rel, buf.data(), buf.size());
        }
        fclose(f);
    }
}

// 放置控件默认样式：白纸上可见（浅色面 + 深色字 + 描边）。
// 编程创建的控件不带主题样式：Button 默认面色透明（露出视口深底）、Hover 蓝（引擎缺省）。
void CanvasPane::styleControl(const std::string& toolboxId, Control ctl) {
    const UIStateColor face{ UIColor{0xFA, 0xFA, 0xFA, 255}, UIColor{0xE8, 0xF0, 0xFE, 255},
                             UIColor{0xD8, 0xD8, 0xD8, 255}, UIColor{0xF0, 0xF0, 0xF0, 255} };
    const UIStateColor border{ UIColor{0xB0, 0xB0, 0xB0, 255}, UIColor{0x4A, 0x90, 0xD9, 255},
                               UIColor{0x4A, 0x90, 0xD9, 255}, UIColor{0xC0, 0xC0, 0xC0, 255} };
    const UIStateColor text{ UIColor{0x33, 0x33, 0x33, 255}, UIColor{0x33, 0x33, 0x33, 255},
                             UIColor{0x33, 0x33, 0x33, 255}, UIColor{0x99, 0x99, 0x99, 255} };
    if (toolboxId == "tBtn") {
        ctl.SetStateColor(PropertyNames::kBackground, face);
        ctl.SetStateColor(PropertyNames::kBorder, border);
        ctl.SetStateColor(PropertyNames::kText, text);
    } else if (toolboxId == "tLabel" || toolboxId == "tCheck" ||
               toolboxId == "tNum" || toolboxId == "tCombo") {
        ctl.SetStateColor(PropertyNames::kText, text);
        if (toolboxId == "tCheck")
            ctl.SetStateColor(PropertyNames::kBorder, border);
    }
}

void CanvasPane::TickLayout(float deltaSeconds) {
    (void)deltaSeconds;
    if (!m_ui || !m_canvasArea.IsValid()) return;
    UIRect area = m_canvasArea.GetRect();
    bool changed = (area.x != m_seenX) || (area.y != m_seenY) ||
                   (area.w != m_seenW) || (area.h != m_seenH);
    m_seenX = area.x; m_seenY = area.y; m_seenW = area.w; m_seenH = area.h;
    if (changed) {
        refreshLayout();   // 零重建：仅同步视口与网格 rect
    }
}

void CanvasPane::SetZoom(float zoom) {
    if (zoom < 0.25f) zoom = 0.25f;
    if (zoom > 2.0f) zoom = 2.0f;
    if (zoom == m_zoom) return;
    m_zoom = zoom;
    rebuildGridTile();
}

void CanvasPane::SetGridCellSize(float px) {
    if (px < 8.0f) px = 8.0f;
    if (px == m_gridCell) return;
    m_gridCell = px;
    rebuildGridTile();
}

void CanvasPane::SetGridVisible(bool visible) {
    if (visible == m_gridVisible) return;
    m_gridVisible = visible;
    if (m_grid.IsValid()) m_grid.SetBool(PropertyNames::kVisible, m_gridVisible);
}

void CanvasPane::SetContent(std::unique_ptr<ControlModel> root) {
    m_root = std::move(root);
    Rebuild();
}

void CanvasPane::Rebuild() {
    refreshLayout();   // 控件树渲染随交互阶段接入
}

// 窗口坐标 → 画布逻辑坐标：视口内容铺满，画布逻辑坐标 = 视口内窗口像素 / z。
bool CanvasPane::ScreenToCanvas(float sx, float sy, float& cx, float& cy) const {
    if (!m_viewport) return false;
    if (m_zoom <= 0) return false;
    cx = (sx - m_areaX) / m_zoom;
    cy = (sy - (m_areaY + m_toolbarH)) / m_zoom;
    return true;
}

bool CanvasPane::CanvasToScreen(float cx, float cy, float& sx, float& sy) const {
    if (!m_viewport) return false;
    sx = (cx * m_zoom) + m_areaX;
    sy = (cy * m_zoom) + m_areaY + m_toolbarH;
    return true;
}

// 画布逻辑可视区域（内容区 / z）。
SRect CanvasPane::CanvasRect() const {
    if (m_zoom <= 0) return SRect{ 0, 0, 0, 0 };
    return SRect{ 0, 0, m_contentW / m_zoom, m_contentH / m_zoom };
}

bool CanvasPane::GetPlacedRect(const std::string& id, SRect& out) const {
    for (const auto& it : m_items) {
        if (it.model && it.model->props.id == id) {
            out = it.model->props.rect;
            return true;
        }
    }
    return false;
}

// 视口窗口区域（内容区绝对坐标，含工具栏）。
SRect CanvasPane::ViewportRect() const {
    if (!m_viewport) return SRect{ 0, 0, 0, 0 };
    return SRect{ m_areaX, m_areaY + m_toolbarH, m_contentW, m_contentH };
}

// ── 交互：放置 / 选中 / 拖动 ──

// 工具箱 id → 规格（默认逻辑尺寸 / 缺省标题 / id 前缀）。
static bool controlSpec(const std::string& tb, float& w, float& h,
                        const char*& caption, const char*& prefix) {
    struct Spec { const char* id; float w, h; const char* cap; const char* pfx; };
    static const Spec kSpecs[] = {
        { "tBtn",    120, 32,  "按钮",        "btn" },
        { "tLabel",  120, 24,  "标签",        "label" },
        { "tImage",  120, 80,  "图片",        "image" },
        { "tCheck",  120, 24,  "复选框",      "check" },
        { "tEdit",   140, 28,  "输入框",      "edit" },
        { "tArea",   180, 90,  "文本域",      "area" },
        { "tBar",    160, 18,  "进度条",      "bar" },
        { "tSlider", 140, 24,  "滑块",        "slider" },
        { "tScroll", 140, 16,  "滚动条",      "scroll" },
        { "tShape",  80,  80,  "形状",        "shape" },
        { "tPanel",  200, 150, "面板",        "panel" },
        { "tWin",    240, 160, "窗口",        "win" },
        { "tSplit",  6,   140, "分割器",      "split" },
        { "tTabs",   200, 150, "选项卡(占位)", "tabs" },
        { "tCombo",  140, 28,  "下拉框",      "combo" },
        { "tList",   160, 120, "列表",        "list" },
        { "tTree",   160, 120, "树",          "tree" },
        { "tNum",    110, 28,  "数字输入",    "num" },
        { "tColor",  80,  32,  "颜色",        "color" },
        { "tMenu",   220, 28,  "菜单",        "menu" },
        { "tStatus", 220, 28,  "状态栏(占位)", "status" },
        { "tAni",    120, 120, "动画(占位)",  "ani" },
        { "tDialog", 240, 150, "对话框(占位)", "dlg" },
        { "tCtx",    140, 80,  "右键菜单(占位)","ctx" },
        { "tImgBtn", 48,  48,  "图片按钮",    "imgbtn" },
        { "tPopup",  160, 100, "弹窗(占位)",  "popup" },
        { "tConfirm", 200, 100, "确认框(占位)", "confirm" },
    };
    for (const auto& s : kSpecs) {
        if (tb == s.id) { w = s.w; h = s.h; caption = s.cap; prefix = s.pfx; return true; }
    }
    return false;
}

// 工具箱 id → 引擎控件类型串（与布局 JSON type 一致）。
static std::string controlTypeOf(const std::string& tb) {
    static const std::pair<const char*, const char*> kMap[] = {
        { "tBtn", "button" },   { "tLabel", "label" },   { "tImage", "image" },
        { "tCheck", "check-box" }, { "tEdit", "edit-box" }, { "tArea", "text-area" },
        { "tBar", "progress-bar" }, { "tSlider", "slider" }, { "tScroll", "scroll-bar" },
        { "tShape", "shape" },  { "tPanel", "panel" },   { "tWin", "win-frame" },
        { "tSplit", "splitter" }, { "tTabs", "tab-control" }, { "tCombo", "combo-box" },
        { "tList", "list-view" }, { "tTree", "tree-view" }, { "tNum", "numeric-up-down" },
        { "tColor", "color-picker" }, { "tMenu", "menu-bar" }, { "tStatus", "status-bar" },
        { "tAni", "animation" }, { "tDialog", "dialog" }, { "tCtx", "context-menu" },
        { "tImgBtn", "image-button" }, { "tPopup", "popup" }, { "tConfirm", "confirm-popup" },
    };
    for (const auto& m : kMap)
        if (tb == m.first) return m.second;
    return "panel";
}

// 视口内按工具箱 id 创建控件（坐标为视口局部物理像素）。
Control CanvasPane::createViewportControl(const std::string& tb, float x, float y,
                                          float w, float h, ControlModel& model) const {
    const std::string cap = model.props.extended.value("caption", "");
    Control ctl;
    if (tb == "tBtn")    ctl = m_viewport->CreateButton(cap, x, y, w, h);
    else if (tb == "tLabel")  ctl = m_viewport->CreateLabel(cap, 14.0f, x, y, w, h);
    else if (tb == "tCheck")  ctl = m_viewport->CreateCheckBox(cap, x, y, w, h);
    else if (tb == "tEdit")   ctl = m_viewport->CreateEditBox(x, y, w, h);
    else if (tb == "tArea")   ctl = m_viewport->CreateTextArea(x, y, w, h);
    else if (tb == "tBar")    ctl = m_viewport->CreateProgressBar(x, y, w, h);
    else if (tb == "tSlider") ctl = m_viewport->CreateSlider(x, y, w, h, 0.0f, 100.0f, 50.0f);
    else if (tb == "tScroll") ctl = m_viewport->CreateScrollBar(x, y, w, h, 0);
    else if (tb == "tShape")  ctl = m_viewport->CreateShape(x, y, w, h);
    else if (tb == "tNum")    ctl = m_viewport->CreateNumericUpDown(x, y, w, h);
    else if (tb == "tColor")  ctl = m_viewport->CreateColorPicker(x, y, w, h, "#3478F6");
    else if (tb == "tCombo")  ctl = m_viewport->CreateComboBox(x, y, w, h);
    else if (tb == "tList")   ctl = m_viewport->CreateListView(x, y, w, h);
    else if (tb == "tTree")   ctl = m_viewport->CreateTreeView(x, y, w, h);
    else if (tb == "tSplit")  ctl = m_viewport->CreateSplitter(x, y, w, h, 0);
    else if (tb == "tMenu")   ctl = m_viewport->CreateMenuBar(x, y, w, h);
    else if (tb == "tWin" || tb == "tDialog")
        ctl = m_viewport->CreateWinFrame(cap, x, y, w, h);
    else if (tb == "tImage" || tb == "tImgBtn")
        ctl = m_viewport->CreateImage("provider:dc_canvas_grid", x, y, w, h);
    else ctl = m_viewport->CreatePanel(x, y, w, h);   // 占位类型
    return ctl;
}

void CanvasPane::placeControl(const std::string& toolboxId, float cx, float cy) {
    if (!m_viewport) return;
    float w = 120, h = 32;
    const char* caption = "";
    const char* prefix = "ctl";
    if (!controlSpec(toolboxId, w, h, caption, prefix)) return;

    // 瀑布偏移避免完全重叠
    float casc = (float)(m_seq % 8) * 16.0f;
    float x = cx - w / 2 + casc;
    float y = cy - h / 2 + casc;
    if (m_snapEnabled) {
        x = std::round(x / m_gridCell) * m_gridCell;
        y = std::round(y / m_gridCell) * m_gridCell;
    }

    auto model = std::make_unique<ControlModel>();
    model->props.type = controlTypeOf(toolboxId);
    model->props.id = std::string(prefix) + "_" + std::to_string(++m_seq);
    model->props.rect = { x, y, w, h };
    model->props.extended["caption"] = caption;

    Control ctl = createViewportControl(toolboxId, x * m_zoom, y * m_zoom,
                                        w * m_zoom, h * m_zoom, *model);
    if (!ctl.IsValid()) return;
    styleControl(toolboxId, ctl);   // 编程创建无主题样式：显式浅色面/深字/描边

    m_items.push_back({ std::move(model), ctl });
    std::printf("[Canvas] placed %s id=%s at (%.0f,%.0f)\n",
                m_items.back().model->props.type.c_str(),
                m_items.back().model->props.id.c_str(), x, y);

    // 选中新控件；选中框需重新置顶
    m_selectedId.clear();
    if (m_selBox.IsValid()) { m_selBox.Destroy(); m_selBox = Control(); }
    selectItem(m_items.back().model->props.id);
}

bool CanvasPane::hitTest(float cx, float cy, std::string& outId) const {
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it) {
        const SRect& r = it->model->props.rect;
        if (cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h) {
            outId = it->model->props.id;
            return true;
        }
    }
    return false;
}

ControlModel* CanvasPane::findModel(const std::string& id) {
    for (auto& it : m_items)
        if (it.model->props.id == id) return it.model.get();
    return nullptr;
}

void CanvasPane::selectItem(const std::string& id) {
    if (m_selectedId == id) { updateSelBox(); return; }
    m_selectedId = id;
    ensureSelBox();
    updateSelBox();
    fillPropertyPanel();
    if (m_clickHandler) m_clickHandler(id, 0, 0);
}

// 选中变化 → 属性面板回填（m_propSync 抑制回写）；未选中清零。
void CanvasPane::fillPropertyPanel() {
    if (!m_propX.IsValid()) return;
    const ControlModel* m = findModel(m_selectedId);
    m_propSync = true;
    m_propX.SetFloat(PropertyNames::kValue, m ? m->props.rect.x : 0);
    m_propY.SetFloat(PropertyNames::kValue, m ? m->props.rect.y : 0);
    m_propW.SetFloat(PropertyNames::kValue, m ? m->props.rect.w : 0);
    m_propH.SetFloat(PropertyNames::kValue, m ? m->props.rect.h : 0);
    m_propCaption.SetString(PropertyNames::kTextContent,
                            m ? m->props.extended.value("caption", "") : "");
    m_propSync = false;
}

// 模型 rect → 视口控件 rect（× zoom）+ 选中框同步。
void CanvasPane::applyModelRect(ControlModel* m) {
    if (!m) return;
    if (PlacedControl* pc = placedById(m->props.id))
        pc->ctl.SetRect(m->props.rect.x * m_zoom, m->props.rect.y * m_zoom,
                        m->props.rect.w * m_zoom, m->props.rect.h * m_zoom);
    updateSelBox();
}

void CanvasPane::SetPropertyControls(Control x, Control y, Control w, Control h, Control caption) {
    m_propX = x; m_propY = y; m_propW = w; m_propH = h; m_propCaption = caption;
    // NUD 事件载荷为 Float（GetValueChanged）；lambda 按值捕获（不可悬空）
    auto bindRect = [this](Control nud, int which) {
        nud.SetCallback(PropertyNames::kEventValueChanged, [this, which](const Event& e) {
            if (m_propSync) return;
            ControlModel* m = findModel(m_selectedId);
            if (!m) return;
            const float v = e.GetValueChanged();
            if (which == 0) m->props.rect.x = v;
            else if (which == 1) m->props.rect.y = v;
            else if (which == 2) m->props.rect.w = v;
            else m->props.rect.h = v;
            applyModelRect(m);
        });
    };
    bindRect(m_propX, 0);
    bindRect(m_propY, 1);
    bindRect(m_propW, 2);
    bindRect(m_propH, 3);
    m_propCaption.SetCallback(PropertyNames::kEventTextChanged, [this](const Event& e) {
        if (m_propSync) return;
        ControlModel* m = findModel(m_selectedId);
        if (!m) return;
        m->props.extended["caption"] = e.GetTextChanged();
        if (PlacedControl* pc = placedById(m->props.id))
            pc->ctl.SetString(PropertyNames::kCaption, e.GetTextChanged());
    });
}

void CanvasPane::ensureSelBox() {
    if (m_selBox.IsValid() || !m_viewport) return;
    m_selBox = m_viewport->CreateShape(0, 0, m_contentW, m_contentH);
}

void CanvasPane::updateSelBox() {
    if (!m_selBox.IsValid()) return;
    const ControlModel* m = findModel(m_selectedId);
    if (!m) {
        m_selBox.SetBool(PropertyNames::kVisible, false);
        return;
    }
    m_selBox.SetBool(PropertyNames::kVisible, true);
    m_selBox.SetEnum(PropertyNames::kScaleType, PropertyNames::kScaleTypeStretch);

    // 选中框：逻辑 rect × zoom 外扩 2px；8 向控点 6×6
    const float pad = 2.0f, hs = 6.0f;
    const float x = m->props.rect.x * m_zoom - pad;
    const float y = m->props.rect.y * m_zoom - pad;
    const float w = m->props.rect.w * m_zoom + pad * 2;
    const float h = m->props.rect.h * m_zoom + pad * 2;

    m_ui->ShapeClearPrimitives(m_selBox);
    int ib = m_ui->ShapeAddPrimitive(m_selBox, "rect", x, y, w, h);
    m_ui->ShapeSetPrimitiveColor(m_selBox, ib, PropertyNames::kStroke,
                                 UIColor{0x4A, 0x90, 0xD9, 255});
    m_ui->ShapeSetPrimitiveFloat(m_selBox, ib, PropertyNames::kLineWidthProp, 1.5f);
    auto handle = [&](float hx, float hy) {
        int ih = m_ui->ShapeAddPrimitive(m_selBox, "filled-rect", hx - hs / 2, hy - hs / 2, hs, hs);
        m_ui->ShapeSetPrimitiveColor(m_selBox, ih, PropertyNames::kFill, UIColor{255, 255, 255, 255});
        m_ui->ShapeSetPrimitiveColor(m_selBox, ih, PropertyNames::kStroke,
                                     UIColor{0x4A, 0x90, 0xD9, 255});
        m_ui->ShapeSetPrimitiveFloat(m_selBox, ih, PropertyNames::kLineWidthProp, 1.0f);
    };
    const float x2 = x + w, y2 = y + h;
    handle(x, y);  handle(x2, y);  handle(x, y2);  handle(x2, y2);
    handle(x + w / 2, y);  handle(x + w / 2, y2);
    handle(x, y + h / 2);  handle(x2, y + h / 2);
}

void CanvasPane::tickInput() {
    // Delete 键：删除选中控件（键源每帧轮询一次，返回按下沿）
    if (m_keySource && m_keySource()) deleteSelected();

    if (!m_viewport || !m_mouseSource) return;
    float wx = 0, wy = 0;
    bool down = m_mouseSource(wx, wy);

    // 视口区域判定（窗口坐标；工具栏/分割器/面板均在区域外，不参与画布交互）
    const float vx0 = m_areaX, vy0 = m_areaY + m_toolbarH;
    const bool inVp = (wx >= vx0 && wx < vx0 + m_contentW &&
                       wy >= vy0 && wy < vy0 + m_contentH);

    float cx = 0, cy = 0;
    ScreenToCanvas(wx, wy, cx, cy);

    const bool downEdge = down && !m_mouseWasDown;
    const bool upEdge = !down && m_mouseWasDown;

    // 拖动已选中控件
    if (!m_dragId.empty()) {
        ControlModel* m = findModel(m_dragId);
        if (!m) { m_dragId.clear(); m_mouseWasDown = down; return; }
        if (down) {
            float nx = cx - m_dragOffX, ny = cy - m_dragOffY;
            if (m_snapEnabled) {
                nx = std::round(nx / m_gridCell) * m_gridCell;
                ny = std::round(ny / m_gridCell) * m_gridCell;
            }
            m->props.rect.x = nx;
            m->props.rect.y = ny;
            if (m_dragCtl.IsValid())
                m_dragCtl.SetRect(nx * m_zoom, ny * m_zoom,
                                  m->props.rect.w * m_zoom, m->props.rect.h * m_zoom);
            updateSelBox();
        } else {
            m_dragId.clear();   // 松开结束拖动
        }
        m_mouseWasDown = down;
        return;
    }

    // 按下沿：命中检测 → 选中并开始拖动 / 取消选中
    if (downEdge && inVp) {
        std::string hit;
        if (hitTest(cx, cy, hit)) {
            selectItem(hit);
            if (ControlModel* m = findModel(hit)) {
                m_dragOffX = cx - m->props.rect.x;
                m_dragOffY = cy - m->props.rect.y;
                m_dragId = hit;
                if (PlacedControl* pc = placedById(hit))
                    m_dragCtl = pc->ctl;
            }
        } else {
            selectItem("");
        }
        m_mouseWasDown = down;
        return;
    }

    if (upEdge) {
        // 松开且未开始拖动：无操作
    }
    m_mouseWasDown = down;
}

CanvasPane::PlacedControl* CanvasPane::placedById(const std::string& id) {
    for (auto& it : m_items)
        if (it.model->props.id == id) return &it;
    return nullptr;
}

// 删除选中控件：销毁视口控件 + 移除模型 + 取消选中（清空属性面板/隐藏选中框）。
void CanvasPane::deleteSelected() {
    if (m_selectedId.empty()) return;
    if (PlacedControl* pc = placedById(m_selectedId)) {
        if (pc->ctl.IsValid()) pc->ctl.Destroy();   // 视口内销毁（悬挂检测保护后续访问）
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it->model->props.id == m_selectedId) { m_items.erase(it); break; }
        }
    }
    if (m_dragId == m_selectedId) {   // 拖动中删除：清理拖动状态
        m_dragId.clear();
        m_dragCtl = Control();
    }
    selectItem("");   // 取消选中 → 属性面板清零 + 选中框隐藏
}

void CanvasPane::SetToolboxTree(Control tree) {
    m_toolboxTree = tree;
    if (!m_toolboxTree.IsValid()) return;
    m_toolboxTree.SetCallback(PropertyNames::kEventSelect, [this](const Event& e) {
        const std::string id = e.GetNodeId();
        if (id.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (id == m_lastToolId) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastToolTime).count();
            m_lastToolId.clear();
            if (ms < 500) {
                // 双击：画布中央放置（逻辑坐标）
                placeControl(id, m_contentW / (2 * m_zoom), m_contentH / (2 * m_zoom));
                return;
            }
        }
        m_lastToolId = id;
        m_lastToolTime = now;
    });
}

void CanvasPane::TickInput() {
    tickInput();
}

}  // namespace Designer