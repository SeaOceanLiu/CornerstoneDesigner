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
#include "DynamicApi.h"
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

// 手柄索引 → 光标形态（0=默认 1=移动 2=NWSE 3=NESW 4=NS 5=EW）；定义见 hitHandle 前
static int handleCursorKind(int hi);

// 控件文本字号（px）：Label/EditBox 等用 font-size，Button 用 caption-size；
// 两路都写（不支持的返回 0 无害）；读失败（0）回落 16（引擎缺省字号）。
static float readFontPx(const Control& ctl) {
    float v = (float)ctl.GetInt(PropertyNames::kFontSize);   // Label/EditBox 等
    if (v <= 0) v = ctl.GetFloat(PropertyNames::kCaptionSize);   // Button
    return v > 0 ? v : 16.0f;
}
static void writeFontPx(Control& ctl, float px) {
    ctl.SetInt(PropertyNames::kFontSize, (int)px);        // Label/EditBox 等（setIntProperty）
    ctl.SetFloat(PropertyNames::kCaptionSize, px);        // Button（setFloatProperty）
}

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

    // 网格 Image 铺满视口内容区（视口局部逻辑坐标；bench scale=zoom 下
    // rect÷zoom 抵消，drawRect=物理尺寸——覆盖层不被内容缩放连带）。
    if (m_grid.IsValid()) {
        m_grid.SetRect(0, 0, m_contentW / m_zoom, m_contentH / m_zoom);
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
    // rect=逻辑尺寸（bench scale=zoom 下÷zoom 抵消，drawRect 铺满视口物理区）；
    // 瓦片纹理按 cellPx=gridCell×zoom 物理密度生成（tile 几何平铺不缩放纹理）
    m_grid = m_viewport->CreateImage("provider:dc_canvas_grid", 0, 0,
                                     m_contentW / m_zoom, m_contentH / m_zoom);
    if (!m_grid.IsValid()) {
        std::printf("[CanvasPane] CreateImage(grid) failed\n");
        return;
    }
    m_grid.SetEnum(PropertyNames::kScaleType, PropertyNames::kScaleTypeTile);
    m_grid.SetBool(PropertyNames::kVisible, m_gridVisible);
}

// 重建网格瓦片（缩放档/gridCell 变化）。
// 不销毁重建 m_grid（新建 Image 会挂到视口顶层，不透明瓦片盖住已放置控件——
// "缩放后控件消失"根因）：覆盖注册同名资源后经 "image-resource" 属性触发重载，
// 控件对象不动、z 序保持原位。
void CanvasPane::rebuildGridTile() {
    if (!m_viewport) return;
    registerGridTile();
    if (m_grid.IsValid())
        m_grid.SetString(PropertyNames::kImageResource, "provider:dc_canvas_grid");
    else
        createGridImage();   // 首次：创建（此时位于底层，先于放置控件）
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
    // 阴影色显式设置：引擎缺省纯白（ConstDef DEFAULT_TEXT_SHADOW_*），浅色面上不可见
    const UIStateColor shadow{ UIColor{0, 0, 0, 90}, UIColor{0, 0, 0, 90},
                               UIColor{0, 0, 0, 110}, UIColor{0, 0, 0, 40} };
    if (toolboxId == "tBtn") {
        ctl.SetStateColor(PropertyNames::kBackground, face);
        ctl.SetStateColor(PropertyNames::kBorder, border);
        ctl.SetStateColor(PropertyNames::kText, text);
        ctl.SetStateColor(PropertyNames::kTextShadow, shadow);
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
    // 画布缩放交给引擎（画布语义）：控件逻辑 rect 不变，drawRect/字号由
    // refreshScaleWith 链自动跟随（override 防 resize recompute 重置）
    if (m_viewport) m_viewport->SetInstanceScale(m_zoom, m_zoom);
    rebuildGridTile();   // 瓦片纹理按新物理密度重建 + image-resource 重载
    if (m_grid.IsValid())
        m_grid.SetRect(0, 0, m_contentW / m_zoom, m_contentH / m_zoom);   // rect 抵消（铺满）
    updateSelBox();      // 图元逻辑直绘（常量÷zoom 保视觉恒定）+ Shape 本体 rect
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

bool CanvasPane::GetPlacedViewRect(const std::string& id, SRect& out) const {
    for (const auto& it : m_items) {
        if (it.model && it.model->props.id == id) {
            if (!it.ctl.IsValid()) return false;
            const UIRect r = it.ctl.GetRect();
            out = SRect{ r.x, r.y, r.w, r.h };
            return true;
        }
    }
    return false;
}

bool CanvasPane::GetPlacedFontPx(const std::string& id, float& out) const {
    for (const auto& it : m_items) {
        if (it.model && it.model->props.id == id) {
            if (!it.ctl.IsValid()) return false;
            out = readFontPx(it.ctl);
            return true;
        }
    }
    return false;
}

bool CanvasPane::SetPlacedFontPx(const std::string& id, float px) {
    for (auto& it : m_items) {
        if (it.model && it.model->props.id == id) {
            if (!it.ctl.IsValid()) return false;
            writeFontPx(it.ctl, px);
            return true;
        }
    }
    return false;
}

bool CanvasPane::GetPlacedColor(const std::string& id, UIColor& out) const {
    for (const auto& it : m_items) {
        if (it.model && it.model->props.id == id) {
            if (!it.ctl.IsValid()) return false;
            out = it.ctl.GetColor(PropertyNames::kBackground);
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

    // 逻辑 rect 直建（bench scale=zoom 负责渲染缩放）
    Control ctl = createViewportControl(toolboxId, x, y, w, h, *model);
    if (!ctl.IsValid()) return;
    styleControl(toolboxId, ctl);   // 编程创建无主题样式：显式浅色面/深字/描边

    m_items.push_back({ std::move(model), ctl });
    std::printf("[Canvas] placed %s id=%s at (%.0f,%.0f)\n",
                m_items.back().model->props.type.c_str(),
                m_items.back().model->props.id.c_str(), x, y);

    // 记录逻辑基础字号（引擎字号属性存逻辑值，缩放由 refreshScaleWith 自动跟随）；
    // 供动态行 getter 读失败时的回填兜底
    m_items.back().model->props.extended["font-size"] = readFontPx(ctl);

    // 控件树同步：新增节点（标签 = 默认标题 + id）
    syncTreeAdd(m_items.back().model->props.id,
                std::string(caption) + " " + m_items.back().model->props.id);

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
    syncTreeSelection();
    rebuildDynamicRows();
    if (m_clickHandler) m_clickHandler(id, 0, 0);
}

// 选中变化 → 属性面板回填（m_propSync 抑制回写）；未选中清零。
void CanvasPane::fillPropertyPanel() {
    if (!m_propX.IsValid()) return;
    const ControlModel* m = findModel(m_selectedId);
    const PlacedControl* pc = m ? placedById(m->props.id) : nullptr;
    m_propSync = true;
    m_propX.SetFloat(PropertyNames::kValue, m ? m->props.rect.x : 0);
    m_propY.SetFloat(PropertyNames::kValue, m ? m->props.rect.y : 0);
    m_propW.SetFloat(PropertyNames::kValue, m ? m->props.rect.w : 0);
    m_propH.SetFloat(PropertyNames::kValue, m ? m->props.rect.h : 0);
    m_propCaption.SetString(PropertyNames::kTextContent,
                            m ? m->props.extended.value("caption", "") : "");
    // 颜色回填：读控件背景色（normal 态），失败用缺省；SetString 走 ColorPicker
    // 的 hex 写入口（setColor），记录 hex 供回调 echo 抑制
    UIColor cc{0x4A, 0x90, 0xD9, 255};
    if (pc && pc->ctl.IsValid())
        cc = pc->ctl.GetColor(PropertyNames::kBackground);
    if (m_propColor.IsValid()) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", cc.r, cc.g, cc.b);
        m_propColor.SetString(PropertyNames::kColor, hex);
        m_lastColorHex = hex;
    }
    m_propSync = false;
}

// 模型 rect → 视口控件 rect（× zoom）+ 选中框同步。
void CanvasPane::applyModelRect(ControlModel* m) {
    if (!m) return;    if (PlacedControl* pc = placedById(m->props.id))
        pc->ctl.SetRect(m->props.rect.x * m_zoom, m->props.rect.y * m_zoom,
                        m->props.rect.w * m_zoom, m->props.rect.h * m_zoom);
    updateSelBox();
}

void CanvasPane::SetPropertyControls(Control x, Control y, Control w, Control h,
                                     Control caption, Control color) {
    m_propX = x; m_propY = y; m_propW = w; m_propH = h;
    m_propCaption = caption; m_propColor = color;
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
    // 颜色：ColorPicker 经 color-changed（onOK 确认触发，Binding Event
    // GetChangedColor 取 UIColor 载荷）
    m_propColor.SetCallback(PropertyNames::kEventColorChanged, [this](const Event& e) {
        const UIColor c = e.GetChangedColor();
        onColorPicked(c.r, c.g, c.b);
    });
}

// 用户确认颜色 → 写选中控件背景色（normal 态）+ 模型记录。
void CanvasPane::onColorPicked(unsigned char r, unsigned char g, unsigned char b) {
    if (m_selectedId.empty()) return;
    ControlModel* m = findModel(m_selectedId);
    PlacedControl* pc = placedById(m_selectedId);
    if (!m || !pc || !pc->ctl.IsValid()) return;
    const UIColor c{ r, g, b, 255 };
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    m_lastColorHex = hex;
    pc->ctl.SetColor(PropertyNames::kBackground, c);
    m->props.extended["color"] = hex;
}

// ── Schema 驱动动态属性区 ──

// JSON 键（camelCase）→ 运行时属性名（kebab-case）；特例覆盖引擎简写命名。
static std::string jsonKeyToProp(const std::string& key) {
    static const std::pair<const char*, const char*> kSpecial[] = {
        { "checkColor", "check" },       { "crossColor", "cross" },
        { "indeterminateColor", "indeterminate" }, { "boxBorderColor", "box-border" },
    };
    for (const auto& s : kSpecial)
        if (key == s.first) return s.second;
    std::string out;
    for (char c : key) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            out += '-';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

bool CanvasPane::LoadSchema(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("[CanvasPane] schema not found: %s\n", path.c_str());
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    try {
        m_schema = std::make_unique<nlohmann::json>(nlohmann::json::parse(text));
        std::printf("[CanvasPane] schema loaded (%zu defs)\n",
                    (*m_schema)["$defs"].size());
        return true;
    } catch (const std::exception& e) {
        std::printf("[CanvasPane] schema parse failed: %s\n", e.what());
        m_schema.reset();
        return false;
    }
}

void CanvasPane::SetDynamicPropsContainer(Control box) {
    m_propDynamic = box;
    if (m_propDynamic.IsValid()) {
        m_propDynamic.SetBool(PropertyNames::kClipChildren, true);   // 真裁剪（滚动）
        rebuildDynamicRows();
    }
}

// 选中控件 → 按 schema 类型生成动态属性行（label 行 + 编辑控件行）。
// 支持 string(EditBox)/number·integer(NUD)/boolean(CheckBox)；嵌套对象、枚举、
// 布局资源类（rect/actors/luotiAni…）首版跳过。
// 布局自管（absolute）：Panel::addControl 不触发 v-flow reflow（仅 Bench 会），
// 初始叠放根因——行 y 手动累加；底部 ScrollBar 做"窗口滚动"（超界行隐藏，
// 等效裁剪——引擎 panel 无裁剪能力）。
void CanvasPane::rebuildDynamicRows() {
    for (Control& c : m_dynamicRows)
        if (c.IsValid()) c.Destroy();
    m_dynamicRows.clear();
    m_dynamicRowY.clear();
    m_schemaTarget = Control();

    if (!m_propDynamic.IsValid()) return;
    const ControlModel* m = findModel(m_selectedId);
    if (!m) {   // 未选中：动态区留空（滚动条一并隐藏）
        if (m_dynScroll.IsValid()) m_dynScroll.SetBool(PropertyNames::kVisible, false);
        return;
    }
    const PlacedControl* pc = placedById(m->props.id);
    if (!pc || !pc->ctl.IsValid()) return;
    if (!m_schema) return;

    const auto& defs = (*m_schema)["$defs"];
    if (!defs.contains(m->props.type)) return;
    const nlohmann::json& def = defs[m->props.type];
    if (!def.contains("properties")) return;

    // 合并 common（allOf $ref）与类型自有属性
    nlohmann::json merged = def["properties"];
    const auto& commonProps = defs["common"]["properties"];
    for (auto it = commonProps.begin(); it != commonProps.end(); ++it)
        if (!merged.contains(it.key())) merged[it.key()] = it.value();

    // 跳过：布局/资源/嵌套复杂对象/事件类；caption/enabled/visible 固定区已有
    // （schema 键名已统一 kebab——skip 名单同步 kebab）
    static const char* kSkip[] = {
        "type", "id", "name", "rect", "scale", "margin", "layout", "anchor",
        "events", "context-menu", "actors", "luotiAni", "caption-label",
        "file", "image", "image-resource", "resourceId", "providerName",
        "font", "colors", "match-parent-rect", "playing", "debug-draw",
        "expand", "offset", "shadow", "size", "style", "alignment",
        "check-state", "vertical-align", "popup", "position",
        "caption", "enabled", "visible",
        "border-visible", "font-file", "font-resource",   // 资源路径/调试类
    };

    const float viewH = 288.0f, barH = 14.0f;
    const float viewRowsH = viewH - barH;

    // 滚动条：首次创建常驻（position-changed → applyDynScroll）
    if (!m_dynScroll.IsValid()) {
        m_dynScroll = m_ui->CreateScrollBar(0, viewH - barH, 216.0f, barH, 1);
        m_dynScroll.SetFloat(PropertyNames::kRangeMin, 0.0f);
        m_dynScroll.SetFloat(PropertyNames::kPageSize, viewRowsH);
        m_dynScroll.SetFloat(PropertyNames::kStepSize, 24.0f);
        m_dynScroll.SetCallback(PropertyNames::kEventPositionChanged,
                                [this](const Event& e) { applyDynScroll(e.GetValueChanged()); });
        m_propDynamic.AddChild(m_dynScroll);
    }
    m_dynScroll.SetBool(PropertyNames::kVisible, true);

    m_schemaTarget = pc->ctl;
    m_schemaType = m->props.type;
    const bool textIsCaption = (m_schemaType == "label" || m_schemaType == "button" ||
                                m_schemaType == "check-box" || m_schemaType == "image-button");
    float y = 2.0f;
    int rows = 0;
    std::string genKeys;
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        const std::string key = it.key();
        bool skip = false;
        for (const char* s : kSkip)
            if (key == s) { skip = true; break; }
        if (skip) continue;
        const nlohmann::json& spec = it.value();
        const std::string t = spec.value("type", "");
        if (t != "string" && t != "number" && t != "integer" && t != "boolean") continue;

        // schema 键 → 运行时属性：kebab 规则 + 引擎命名差异特例
        std::string prop = jsonKeyToProp(key);
        if (key == "text" && textIsCaption) prop = PropertyNames::kCaption;
        // common 属性对特定控件无效：button 文本字号是 caption-size（font-size 分发缺失）
        if (key == "font-size" && m_schemaType == "button") continue;
        const float rowW = 208.0f;

        // 键名 label 行
        Control lb = m_ui->CreateLabel(key, 13.0f, 0, 0, rowW, 18.0f);
        lb.SetColor(PropertyNames::kText, UIColor{0x90, 0x90, 0x90, 255});
        lb.SetRect(2.0f, y, rowW, 18.0f);
        m_propDynamic.AddChild(lb);
        m_dynamicRows.push_back(lb);
        m_dynamicRowY.push_back(y);
        y += 18.0f;

        if (t == "string") {
            Control eb = m_ui->CreateEditBox(0, 0, rowW, 26.0f);
            eb.SetRect(2.0f, y, rowW, 26.0f);
            const std::string v = m_schemaTarget.GetString(prop.c_str());
            eb.SetString(PropertyNames::kTextContent, v);
            eb.SetCallback(PropertyNames::kEventTextChanged,
                           [this, prop](const Event& e) {
                               if (m_schemaTarget.IsValid())
                                   m_schemaTarget.SetString(prop.c_str(), e.GetTextChanged());
                           });
            m_propDynamic.AddChild(eb);
            m_dynamicRows.push_back(eb);
            m_dynamicRowY.push_back(y);
            y += 26.0f;
        } else if (t == "number" || t == "integer") {
            Control nud = m_ui->CreateNumericUpDown(0, 0, rowW, 26.0f);
            nud.SetRect(2.0f, y, rowW, 26.0f);
            const bool isInt = (t == "integer");
            float v = isInt ? (float)m_schemaTarget.GetInt(prop.c_str())
                            : m_schemaTarget.GetFloat(prop.c_str());
            if (v <= 0 && (prop == "font-size" || prop == "caption-size"))
                v = m->props.extended.value("font-size", 0.0f);
            if (v > 0) nud.SetFloat(PropertyNames::kValue, v);
            nud.SetCallback(PropertyNames::kEventValueChanged,
                            [this, prop, isInt](const Event& e) {
                                if (!m_schemaTarget.IsValid()) return;
                                if (isInt) m_schemaTarget.SetInt(prop.c_str(),
                                                                 (int)e.GetValueChanged());
                                else m_schemaTarget.SetFloat(prop.c_str(), e.GetValueChanged());
                            });
            m_propDynamic.AddChild(nud);
            m_dynamicRows.push_back(nud);
            m_dynamicRowY.push_back(y);
            y += 26.0f;
        } else {
            Control cb = m_ui->CreateCheckBox(key, 0, 0, rowW, 24.0f);
            cb.SetRect(2.0f, y, rowW, 24.0f);
            cb.SetBool(PropertyNames::kTriState, false);   // bool 属性两态（引擎缺省三态循环）
            // CheckBox 不发 click，发 check-changed（intVal=CheckState 枚举）
            cb.SetCallback(PropertyNames::kEventCheckChanged,
                           [this, prop](const Event& e) {
                if (m_schemaTarget.IsValid())
                    m_schemaTarget.SetBool(prop.c_str(), e.GetCheckState() != 0);
            });
            m_propDynamic.AddChild(cb);
            m_dynamicRows.push_back(cb);
            m_dynamicRowY.push_back(y);
            y += 24.0f;
        }
        y += 4.0f;
        ++rows;
        genKeys += " " + key;
    }

    m_dynScrollMax = (y + 2.0f) - viewRowsH;
    if (m_dynScrollMax < 0) m_dynScrollMax = 0;
    m_dynScroll.SetFloat(PropertyNames::kRangeMax, m_dynScrollMax > 0 ? m_dynScrollMax : 1.0f);
    m_dynScroll.SetFloat(PropertyNames::kValue, 0.0f);
    m_dynScroll.SetBool(PropertyNames::kVisible, m_dynScrollMax > 0);
    applyDynScroll(0.0f);
    std::printf("[CanvasPane] dynamic rows for '%s': %d (%s)\n",
                m_schemaType.c_str(), rows, genKeys.c_str());
}

// 滚动 → 行偏移（超界裁剪由 propDynamic 的 clip-children 承担，纯偏移）。
void CanvasPane::applyDynScroll(float value) {
    for (size_t i = 0; i < m_dynamicRows.size(); ++i) {
        if (!m_dynamicRows[i].IsValid() || i >= m_dynamicRowY.size()) continue;
        const float vy = m_dynamicRowY[i] - value;
        const UIRect r = m_dynamicRows[i].GetRect();
        m_dynamicRows[i].SetRect(r.x, vy, r.w, r.h);
    }
}

void CanvasPane::ensureSelBox() {
    if (m_selBox.IsValid() || !m_viewport) return;
    // 本体逻辑尺寸（÷zoom 抵消 bench scale，drawRect 覆盖视口物理区）
    m_selBox = m_viewport->CreateShape(0, 0, m_contentW / m_zoom, m_contentH / m_zoom);
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
    // 本体 rect 同步（视口 resize/缩放变化后保持覆盖）
    m_selBox.SetRect(0, 0, m_contentW / m_zoom, m_contentH / m_zoom);

    // 逻辑直绘：图元=控件逻辑 rect 外扩（pad/hs/线宽 ÷zoom → drawRect 视觉恒定）；
    // m_handlePts 缓存物理坐标（逻辑×zoom）供命中检测（ScreenToCanvas 逆换算一致）
    const float pad = 2.0f / m_zoom, hs = 6.0f / m_zoom;
    const float x = m->props.rect.x - pad;
    const float y = m->props.rect.y - pad;
    const float w = m->props.rect.w + pad * 2;
    const float h = m->props.rect.h + pad * 2;

    m_ui->ShapeClearPrimitives(m_selBox);
    int ib = m_ui->ShapeAddPrimitive(m_selBox, "rect", x, y, w, h);
    m_ui->ShapeSetPrimitiveColor(m_selBox, ib, PropertyNames::kStroke,
                                 UIColor{0x4A, 0x90, 0xD9, 255});
    m_ui->ShapeSetPrimitiveFloat(m_selBox, ib, PropertyNames::kLineWidthProp, 1.5f / m_zoom);
    auto handle = [&](float hx, float hy) {
        int ih = m_ui->ShapeAddPrimitive(m_selBox, "filled-rect", hx - hs / 2, hy - hs / 2, hs, hs);
        m_ui->ShapeSetPrimitiveColor(m_selBox, ih, PropertyNames::kFill, UIColor{255, 255, 255, 255});
        m_ui->ShapeSetPrimitiveColor(m_selBox, ih, PropertyNames::kStroke,
                                     UIColor{0x4A, 0x90, 0xD9, 255});
        m_ui->ShapeSetPrimitiveFloat(m_selBox, ih, PropertyNames::kLineWidthProp, 1.0f / m_zoom);
    };
    const float x2 = x + w, y2 = y + h;
    // 顺序与 Handle 枚举一致：TL TR BL BR T B L R（缓存供命中检测）
    const float pts[kHandleCount][2] = {
        {x, y},      {x2, y},      {x, y2},      {x2, y2},
        {x + w / 2, y}, {x + w / 2, y2}, {x, y + h / 2}, {x2, y + h / 2},
    };
    for (int i = 0; i < kHandleCount; ++i) {
        m_handlePts[i] = { pts[i][0] * m_zoom, pts[i][1] * m_zoom };
        handle(pts[i][0], pts[i][1]);
    }
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

    // 光标反馈：resize/拖动中 → move；悬停手柄 → 对应双向箭头；其余默认箭头。
    // 引擎控件 hover 会经 Cursor::setCurrent 覆盖外部 SetCursor → 激活期（非默认
    // 形态）须每帧下发；退出激活只发一次恢复，平时不碰（引擎区域光标自管理，
    // 且每帧 SDL_SetCursor 开销显著）。仅画布视口内生效。
    if (inVp && m_cursorSink) {
        int cursorKind = 0;
        if (!m_resizeId.empty()) cursorKind = handleCursorKind(static_cast<int>(m_resizeHandle));
        else if (!m_dragId.empty()) cursorKind = 1;
        else if (!m_selectedId.empty()) {
            if (const int hi = hitHandle(cx, cy); hi >= 0) cursorKind = handleCursorKind(hi);
        }
        if (cursorKind != 0 || m_lastCursorKind != 0) m_cursorSink(cursorKind);
        m_lastCursorKind = cursorKind;
    }

    const bool downEdge = down && !m_mouseWasDown;
    const bool upEdge = !down && m_mouseWasDown;

    // 手柄 resize 拖动
    if (!m_resizeId.empty()) {
        ControlModel* m = findModel(m_resizeId);
        if (!m) { m_resizeId.clear(); m_resizeCtl = Control(); m_mouseWasDown = down; return; }
        if (down) {
            applyResize(m, cx, cy);
        } else {
            m_resizeId.clear();
            m_resizeCtl = Control();
        }
        m_mouseWasDown = down;
        return;
    }

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
                m_dragCtl.SetRect(nx, ny, m->props.rect.w, m->props.rect.h);
            updateSelBox();
            fillPropertyPanel();   // 拖动中同步属性面板（m_propSync 抑制回写）
        } else {
            m_dragId.clear();   // 松开结束拖动
        }
        m_mouseWasDown = down;
        return;
    }

    // 按下沿：手柄命中 → resize；控件命中 → 选中并拖动；空白 → 取消选中
    if (downEdge && inVp) {
        if (const int hi = hitHandle(cx, cy); hi >= 0 && !m_selectedId.empty()) {
            if (ControlModel* m = findModel(m_selectedId)) {
                m_resizeId = m_selectedId;
                m_resizeHandle = static_cast<Handle>(hi);
                m_resizeStartX = cx;
                m_resizeStartY = cy;
                m_resizeStartRect = m->props.rect;
                if (PlacedControl* pc = placedById(m_selectedId))
                    m_resizeCtl = pc->ctl;
                m_mouseWasDown = down;
                return;
            }
        }
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

// 手柄索引 → 光标形态（0=默认 1=移动 2=NWSE 3=NESW 4=NS 5=EW）。
// Handle: TL=0 TR=1 BL=2 BR=3 T=4 B=5 L=6 R=7
static int handleCursorKind(int hi) {
    switch (hi) {
    case 0: case 3: return 2;   // TL/BR → NWSE 双箭头
    case 1: case 2: return 3;   // TR/BL → NESW 双箭头
    case 4: case 5: return 4;   // T/B  → NS 双箭头
    case 6: case 7: return 5;   // L/R  → EW 双箭头
    default: return 0;
    }
}

// 手柄命中：鼠标（逻辑坐标）→ 视口像素，与手柄中心距离判定（容差 6px）。
int CanvasPane::hitHandle(float cx, float cy) const {
    const float mx = cx * m_zoom, my = cy * m_zoom;
    for (int i = 0; i < kHandleCount; ++i) {
        const float dx = mx - m_handlePts[i].first;
        const float dy = my - m_handlePts[i].second;
        if (dx * dx + dy * dy <= 36.0f) return i;
    }
    return -1;
}

// 手柄 resize：以按下时 rect/鼠标为基准，按方向更新逻辑 rect（网格吸附+最小 8），
// 同步视口控件、选中框与属性面板。
void CanvasPane::applyResize(ControlModel* m, float cx, float cy) {
    float dx = cx - m_resizeStartX;
    float dy = cy - m_resizeStartY;
    const SRect& s = m_resizeStartRect;
    SRect r = s;
    switch (m_resizeHandle) {
    case Handle::TL: r.x = s.x + dx; r.y = s.y + dy; r.w = s.w - dx; r.h = s.h - dy; break;
    case Handle::TR: r.y = s.y + dy; r.w = s.w + dx; r.h = s.h - dy; break;
    case Handle::BL: r.x = s.x + dx; r.w = s.w - dx; r.h = s.h + dy; break;
    case Handle::BR: r.w = s.w + dx; r.h = s.h + dy; break;
    case Handle::T:  r.y = s.y + dy; r.h = s.h - dy; break;
    case Handle::B:  r.h = s.h + dy; break;
    case Handle::L:  r.x = s.x + dx; r.w = s.w - dx; break;
    case Handle::R:  r.w = s.w + dx; break;
    }
    if (m_snapEnabled) {
        const float g = m_gridCell;
        r.x = std::round(r.x / g) * g;
        r.y = std::round(r.y / g) * g;
        r.w = std::round(r.w / g) * g;
        r.h = std::round(r.h / g) * g;
    }
    // 最小尺寸 8：限制后保持对侧边/角不动
    const float minS = 8.0f;
    if (r.w < minS) r.w = minS;
    if (r.h < minS) r.h = minS;
    const bool fixRight = (m_resizeHandle == Handle::TL || m_resizeHandle == Handle::BL ||
                           m_resizeHandle == Handle::L);
    const bool fixBottom = (m_resizeHandle == Handle::TL || m_resizeHandle == Handle::TR ||
                            m_resizeHandle == Handle::T);
    if (fixRight)   r.x = s.x + s.w - r.w;
    if (fixBottom)  r.y = s.y + s.h - r.h;

    m->props.rect = r;
    if (m_resizeCtl.IsValid())
        m_resizeCtl.SetRect(r.x, r.y, r.w, r.h);
    updateSelBox();
    fillPropertyPanel();
}

CanvasPane::PlacedControl* CanvasPane::placedById(const std::string& id) {
    for (auto& it : m_items)
        if (it.model->props.id == id) return &it;
    return nullptr;
}

// 右侧控件树：select 事件 → 选中画布控件（m_treeSync 抑制编程式回环）。
void CanvasPane::SetCanvasTree(Control tree) {
    m_canvasTree = tree;
    if (!m_canvasTree.IsValid()) return;
    m_canvasTree.SetCallback(PropertyNames::kEventSelect, [this](const Event& e) {
        if (m_treeSync) return;
        const std::string id = e.GetNodeId();
        if (id.empty() || !findModel(id)) return;   // 空选/未知节点忽略
        selectItem(id);
    });
}

// 画布控件增删/选中 → 树同步（编程式，触发 select 回调被 m_treeSync 抑制）。
// tvTree 在主实例，经 m_ui 成员方法操作。
void CanvasPane::syncTreeAdd(const std::string& id, const std::string& label) {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    m_ui->TreeViewAddNode(m_canvasTree, "", id, label);
    m_treeSync = false;
}

void CanvasPane::syncTreeRemove(const std::string& id) {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    m_ui->TreeViewRemoveNode(m_canvasTree, id);
    m_treeSync = false;
}

void CanvasPane::syncTreeSelection() {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    if (m_selectedId.empty()) m_ui->TreeViewClearSelection(m_canvasTree);
    else m_ui->TreeViewSelectNode(m_canvasTree, m_selectedId);
    m_treeSync = false;
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
    if (m_resizeId == m_selectedId) {   // resize 中删除：清理 resize 状态
        m_resizeId.clear();
        m_resizeCtl = Control();
    }
    syncTreeRemove(m_selectedId);
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