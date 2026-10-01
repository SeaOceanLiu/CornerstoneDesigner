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
#include "Designer/ConstDef.h"

#include "Designer/ControlModel.h"

#include "UICornerstone.h"
#include "Control.h"
#include "DynamicApi.h"
#include "Event.h"
#include "PropertyNames.h"

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <chrono>
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

constexpr int kGridDotStep = 4;                // 网格点线：沿线每 4px 一个点（交点处必有）
constexpr unsigned char kGridLineR = 0xB0;     // 网格线浅灰（避免干扰控件边界色判断）
constexpr unsigned char kGridLineG = 0xB0;
constexpr unsigned char kGridLineB = 0xB0;
constexpr unsigned char kGridLineA = 0xC0;

// 网格瓦片：tilePx×tilePx，白底 + 浅灰点线（每 cellPx 一条，左上边界对齐）。
// 平铺后线在 0, cellPx, 2*cellPx… 处连续；瓦片右/下边缘不画线（由下一瓦片左/上边界提供）。
// showLines=false 生成纯白瓦片（网格开关经瓦片重生成实现——白底/背景色保持）。
std::vector<unsigned char> buildGridTilePng(int tilePx, int cellPx, bool showLines) {
    std::vector<unsigned char> raw;
    raw.reserve((size_t)tilePx * ((size_t)tilePx * 4 + 1));
    for (int y = 0; y < tilePx; ++y) {
        raw.push_back(0);   // filter 0
        bool yLine = (y % cellPx == 0);
        for (int x = 0; x < tilePx; ++x) {
            const bool line = showLines && (yLine || (x % cellPx == 0));
            if (line && (x + y) % kGridDotStep == 0) {   // 沿线点阵（交点处必有）
                raw.push_back(kGridLineR); raw.push_back(kGridLineG);
                raw.push_back(kGridLineB); raw.push_back(kGridLineA);
            } else {
                raw.push_back(0xFF); raw.push_back(0xFF); raw.push_back(0xFF); raw.push_back(0xFF);
            }
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

// ── 属性面板：动态行排版节奏（统一相对位置：描述行 + 控件行 + 组间距）──
constexpr float kPropLblFont = 16.0f;          // 描述行字号
constexpr float kPropLblH = 22.0f;             // 描述行高
constexpr float kPropCtlH = 26.0f;             // 控件行高
constexpr float kPropGapLbl = 2.0f;            // 描述行与控件行间距
constexpr float kPropRowGap = 8.0f;            // 行组间距
constexpr float kPropTextAreaH = 40.0f;        // 文本输入行高（容两行，超出内部滚动）
constexpr float kPropCheckH = 24.0f;           // 勾选行高
constexpr float kPropInsetX = 2.0f;            // 行控件左边距
constexpr float kPropRowInset = 16.0f;         // 行宽左右留白合计（右侧滚动条通道）
constexpr float kPropBarW = 12.0f;             // 动态区滚动条宽（浮于行区右侧）
constexpr float kPropScrollHideBand = 40.0f;   // 滚动隐藏缓冲带（行移出视口上/下沿的判定余量）
constexpr float kColorRowW = 120.0f;           // 颜色槽行宽（固定，不随面板伸缩）
constexpr float kShadowStep = 0.5f;            // 阴影偏移步进
constexpr float kShadowRange = 50.0f;          // 阴影偏移范围（±）

// 属性面板深色主题（编程创建控件无主题：显式与深色面板协调）
constexpr UIColor kPropPanelBg{0x1E, 0x1E, 0x1E, 255};
constexpr UIColor kPropPanelText{0xE0, 0xE0, 0xE0, 255};
constexpr UIColor kPropPanelBorder{0x40, 0x40, 0x40, 255};
constexpr UIColor kPropPanelBorderFocus{0x4A, 0x90, 0xD9, 255};

// ── 画布/工具箱 ──
constexpr float kToolboxColorRowW = 120.0f;    // 工具箱颜色项宽
constexpr int kTreeItemH = 28;                 // 控件树行高
constexpr float kResizeEpsilon = 0.5f;         // resize 缘判定像素容差（几何等价阈值）
constexpr float kDefaultFontPx = 16.0f;        // 控件字号读失败时的回落缺省（引擎缺省字号）
constexpr float kMinContentH = 8.0f;           // 资源面板内容高度下限（钳制）
constexpr float kZoomMax = 2.0f;               // 画布缩放上限
constexpr float kSliderMin = 0.0f;             // Slider 工具项创建参数：范围/缺省值
constexpr float kSliderMax = 100.0f;
constexpr float kSliderDefault = 50.0f;
constexpr float kCascStep = 16.0f;             // 放置瀑布偏移步长（避免完全重叠）
constexpr int kCascMod = 8;                    // 瀑布偏移取模周期
constexpr float kPropDynHDefault = 440.0f;     // 动态行区兜底高（与布局声明一致）

// 字体名大小写不敏感比较（引擎 P0-46 读回枚举名小写化——清单为驼峰原名）
bool ieqNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

// 路径类属性（资源/文件引用）：值中任何换行/空白都非合法路径成分——
// TextArea 多行文本天然携带换行（尾随或误回车），写回前必须清理，
// 否则引擎 loadFromFile 以 'xxx.png\n' 打开必然失败
bool isPathLikeProp(const std::string& p) {
    return p == "image" || p == "image-resource" ||
           p == "normal-image" || p == "hover-image" ||
           p == "pressed-image" || p == "disabled-image" ||
           p == "animation" || p == "file" || p == "font-file";
}

std::string sanitizePropValue(const std::string& prop, std::string s) {
    auto isEolWs = [](char c) {
        return c == '\n' || c == '\r' || c == ' ' || c == '\t';
    };
    if (isPathLikeProp(prop)) {
        // 路径类：移除全部换行 + 首尾空白
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](char c) { return c == '\n' || c == '\r'; }),
                s.end());
        while (!s.empty() && isEolWs(s.front())) s.erase(s.begin());
        while (!s.empty() && isEolWs(s.back())) s.pop_back();
        return s;
    }
    // 文本类：仅裁尾随换行（多行正文保留内部换行）
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace

namespace Designer {

// 控件文本字号（px）：统一 font-size 键（引擎已废弃 caption-size，并全控件
// 归一 Int 分发域——Label/Button 均 setIntProperty/getIntProperty）；
// 读失败（0）回落 16（引擎缺省字号）。
static float readFontPx(const Control& ctl) {
    const float v = (float)ctl.GetInt(PropertyNames::kFontSize);
    return v > 0 ? v : kDefaultFontPx;
}
static void writeFontPx(Control& ctl, float px) {
    ctl.SetInt(PropertyNames::kFontSize, (int)px);
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
    if (m_contentH < kMinContentH) m_contentH = kMinContentH;

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

    std::vector<unsigned char> png = buildGridTilePng(tilePx, cellPx, m_gridVisible);
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
    m_grid.SetBool(PropertyNames::kVisible, true);   // Image 恒可见（网格开关经瓦片重生成）
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
    const UIStateColor border{ UIColor{0xB0, 0xB0, 0xB0, 255}, kPropPanelBorderFocus,
                               kPropPanelBorderFocus, UIColor{0xC0, 0xC0, 0xC0, 255} };
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
    if (zoom > kZoomMax) zoom = kZoomMax;
    if (zoom == m_zoom) return;
    m_zoom = zoom;
    // 画布缩放交给引擎（画布语义）：控件逻辑 rect 不变，drawRect/字号由
    // refreshScaleWith 链自动跟随（override 防 resize recompute 重置）
    if (m_viewport) m_viewport->SetInstanceScale(m_zoom, m_zoom);
    rebuildGridTile();   // 瓦片纹理按新物理密度重建 + image-resource 重载
    if (m_grid.IsValid())
        m_grid.SetRect(0, 0, m_contentW / m_zoom, m_contentH / m_zoom);   // rect 抵消（铺满）
}

void CanvasPane::SetGridCellSize(float px) {
    if (px < ConstDef::kSnapThreshold) px = ConstDef::kSnapThreshold;
    if (px == m_gridCell) return;
    m_gridCell = px;
    rebuildGridTile();
}

void CanvasPane::SetGridVisible(bool visible) {
    if (visible == m_gridVisible) return;
    m_gridVisible = visible;
    rebuildGridTile();   // 重生成瓦片（带线/纯白）——仅去网格线、白底背景保持
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
        { "tStatus", 220, 28,  "状态栏",       "status" },
        { "tAni",    120, 120, "动画",        "ani" },
        { "tDialog", 240, 150, "对话框",      "dlg" },
        { "tCtx",    140, 80,  "右键菜单",     "ctx" },
        { "tPopup",  160, 100, "弹窗",        "popup" },
        { "tConfirm", 200, 100, "确认框",      "confirm" },
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
        { "tPopup", "popup" }, { "tConfirm", "confirm-popup" },
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
    else if (tb == "tSlider") ctl = m_viewport->CreateSlider(x, y, w, h, kSliderMin, kSliderMax, kSliderDefault);
    else if (tb == "tScroll") ctl = m_viewport->CreateScrollBar(x, y, w, h, 0);
    else if (tb == "tShape")  ctl = m_viewport->CreateShape(x, y, w, h);
    else if (tb == "tNum")    ctl = m_viewport->CreateNumericUpDown(x, y, w, h);
    else if (tb == "tColor")  ctl = m_viewport->CreateColorPicker(x, y, w, h, "#3478F6");
    else if (tb == "tCombo")  ctl = m_viewport->CreateComboBox(x, y, w, h);
    else if (tb == "tList")   ctl = m_viewport->CreateListView(x, y, w, h);
    else if (tb == "tTree")   ctl = m_viewport->CreateTreeView(x, y, w, h);
    else if (tb == "tSplit")  ctl = m_viewport->CreateSplitter(x, y, w, h, 0);
    else if (tb == "tTabs")   ctl = m_viewport->CreateTabControl(x, y, w, h);
    else if (tb == "tCtx") {
        ctl = m_viewport->CreateContextMenu(x, y, w, h);
        // 引擎右键菜单语义创建后默认隐藏（显示经 popup 调用）——设计器编辑态常显；
        // P0-28：关闭 dismiss 语义（外部点击/Esc 不隐藏——取消选择不再消失）
        if (ctl.IsValid()) {
            ctl.SetBool("close-on-click-outside", 0);
            ctl.SetBool("close-on-esc", 0);
            ctl.SetBool(PropertyNames::kVisible, 1);
        }
    } else if (tb == "tMenu")   ctl = m_viewport->CreateMenuBar(x, y, w, h);
    else if (tb == "tWin")
        ctl = m_viewport->CreateWinFrame(cap, x, y, w, h);
    else if (tb == "tDialog") {
        // 真实 Dialog 控件（引擎探针实证 colors 生效；此前占位映射 WinFrame
        // 致"背景被内容面板遮挡"误判——组合语义非引擎缺陷）；
        // P0-28：关闭 dismiss 语义（编辑态常显）
        ctl = m_viewport->CreateDialog(u8"确定", u8"取消", x, y, w, h);
        if (ctl.IsValid()) {
            ctl.SetBool("close-on-click-outside", 0);
            ctl.SetBool("close-on-esc", 0);
        }
    }
    else if (tb == "tPopup") {
        // P0-27d：真实 Popup（不自动 open——编辑态显式 visible；引擎清单
        // P0-28：close-on-click-outside/close-on-esc 关闭 dismiss 语义）
        ctl = m_viewport->CreatePopup(x, y, w, h);
        if (ctl.IsValid()) {
            ctl.SetBool("close-on-click-outside", 0);
            ctl.SetBool("close-on-esc", 0);
            ctl.SetBool(PropertyNames::kVisible, 1);
        }
    } else if (tb == "tConfirm") {
        // P0-27d：真实 ConfirmPopup（confirmText 单参数签名；同 Popup 编辑态语义）
        ctl = m_viewport->CreateConfirmPopup(u8"确定", x, y, w, h);
        if (ctl.IsValid()) {
            ctl.SetBool("close-on-click-outside", 0);
            ctl.SetBool("close-on-esc", 0);
            ctl.SetBool(PropertyNames::kVisible, 1);
        }
    } else if (tb == "tStatus")
        ctl = m_viewport->CreateStatusBar(x, y, w, h);
    else if (tb == "tImage")
        ctl = m_viewport->CreateImage("provider:dc_canvas_grid", x, y, w, h);
    else if (tb == "tAni") {
        // 缺省动画（assets 必备文件）：创建即加载——空路径创建引擎不支持
        // （prepare 对未加载 throw 回滚，引擎清单 P0-25）；用户经 path 行换动画
        ctl = m_viewport->CreateAnimation(
            "assets/animations/rotateBtn/rotateBtn.jsonc", x, y, w, h);
        // 放置即播放（预览意图）；此后 path 换动画由引擎 wasPlaying 语义保持状态
        if (ctl.IsValid()) ctl.SetBool(PropertyNames::kPlaying, 1);
    } else ctl = m_viewport->CreatePanel(x, y, w, h);   // 占位类型
    return ctl;
}

void CanvasPane::placeControl(const std::string& toolboxId, float cx, float cy) {
    if (!m_viewport) return;
    float w = 120, h = 32;
    const char* caption = "";
    const char* prefix = "ctl";
    if (!controlSpec(toolboxId, w, h, caption, prefix)) return;

    // 瀑布偏移避免完全重叠
    float casc = (float)(m_seq % kCascMod) * kCascStep;
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

    // 选中新控件（HandleControl setTarget 重新附加，alwaysOnTop 置顶）
    m_selectedId.clear();
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
    if (m_selectedId == id) return;
    m_selectedId = id;
    PlacedControl* pc = id.empty() ? nullptr : placedById(id);
    if (pc && pc->ctl.IsValid()) ensureSelHandle(pc->ctl);
    else detachSelHandle();
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
    // caption 回填：check-box/win-frame 的文本在内部 Label（经句柄直控读）
    std::string capVal = m ? m->props.extended.value("caption", "") : "";
    if (m && pc && pc->ctl.IsValid()) {
        const std::string& tp = m->props.type;
        if ((tp == "check-box" || tp == "win-frame") && m_ui) {
            void* h = pc->ctl.GetPtr(tp == "win-frame"
                ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel);
            if (h) capVal = m_viewport->FromHandle(h).GetString(PropertyNames::kCaption);
        }
    }
    m_propCaption.SetString(PropertyNames::kTextContent, capVal);
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
    // 可见/启用回填（读控件实际值；未选中清零）
    m_propVisible.SetBool(PropertyNames::kChecked, pc ? pc->ctl.GetBool(PropertyNames::kVisible) : false);
    m_propEnabled.SetBool(PropertyNames::kChecked, pc ? pc->ctl.GetBool(PropertyNames::kEnabled) : false);
    m_propSync = false;
}

// 模型 rect → 视口控件 rect（逻辑直写，bench scale 负责渲染缩放；
// HandleControl 每帧按 target getDrawRect 绘制，自动跟随）。
void CanvasPane::applyModelRect(ControlModel* m) {
    if (!m) return;
    if (PlacedControl* pc = placedById(m->props.id))
        pc->ctl.SetRect(m->props.rect.x, m->props.rect.y, m->props.rect.w, m->props.rect.h);
}

void CanvasPane::SetPropertyControls(Control x, Control y, Control w, Control h,
                                     Control caption, Control color,
                                     Control visible, Control enabled) {
    m_propX = x; m_propY = y; m_propW = w; m_propH = h;
    m_propCaption = caption; m_propColor = color;
    m_propVisible = visible; m_propEnabled = enabled;
    visible.SetBool(PropertyNames::kTriState, false);    // bool 两态（引擎缺省三态循环）
    enabled.SetBool(PropertyNames::kTriState, false);
    // 可见/启用勾选（check-changed，intVal=CheckState）→ 被选控件标准属性直写
    m_propVisible.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
        if (m_propSync) return;
        if (PlacedControl* pc = placedById(m_selectedId))
            pc->ctl.SetBool(PropertyNames::kVisible, e.GetCheckState() != 0);
    });
    m_propEnabled.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
        if (m_propSync) return;
        if (PlacedControl* pc = placedById(m_selectedId))
            pc->ctl.SetBool(PropertyNames::kEnabled, e.GetCheckState() != 0);
    });
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
        const std::string v = e.GetTextChanged();
        m->props.extended["caption"] = v;
        // 动态区文本行同步（先行——不依赖控件写路径，防回环）
        if (m_captionEditor.IsValid()) {
            m_propSync = true;
            m_captionEditor.SetString(PropertyNames::kTextContent, v);
            m_propSync = false;
        }
        PlacedControl* pc = placedById(m->props.id);
        if (!pc || !pc->ctl.IsValid()) return;
        // check-box（无 caption 属性分发）/ win-frame（title 键）经内部 Label 直控
        const std::string& tp = m->props.type;
        if ((tp == "check-box" || tp == "win-frame") && m_ui) {
            void* h = pc->ctl.GetPtr(tp == "win-frame"
                ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel);
            if (h) {
                m_viewport->FromHandle(h).SetString(PropertyNames::kCaption, v);
                return;
            }
        }
        pc->ctl.SetString(PropertyNames::kCaption, v);
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

// JSON 键（camelCase）→ 运行时属性名（kebab-case）。
// （92 键统一后 camelCase 特例映射已清空——schema 键即运行时键）
static std::string jsonKeyToProp(const std::string& key) {
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

// P0-26 schema 重构：colors 组态子集解析——类型 def 的 colors.$ref → 组 def →
// 子组 $ref → state-color-N → 态名清单。返回 { 子组名 → 态名清单 }；
// 类型未声明 colors（组件实例等）返回空 → 动态区不生成颜色组。
static std::map<std::string, std::vector<std::string>> resolveColorGroups(
    const nlohmann::json& defs, const nlohmann::json& typeDef) {
    std::map<std::string, std::vector<std::string>> out;
    static const std::map<std::string, std::vector<std::string>> kStateSets = {
        { "state-color-4",  { "normal", "hover", "pressed", "disabled" } },
        { "state-color-3d", { "normal", "hover", "disabled" } },
        { "state-color-3p", { "normal", "hover", "pressed" } },
        { "state-color-2",  { "normal", "hover" } },
        { "state-color-1",  { "normal" } },
    };
    if (!typeDef.contains("properties") || !typeDef["properties"].contains("colors"))
        return out;
    const auto& colorsDecl = typeDef["properties"]["colors"];
    if (!colorsDecl.contains("$ref")) return out;
    const std::string colorsRef = colorsDecl["$ref"].get<std::string>();
    static const std::string kPrefix = "#/$defs/";
    if (colorsRef.rfind(kPrefix, 0) != 0) return out;
    const std::string colorsDefName = colorsRef.substr(kPrefix.size());
    if (!defs.contains(colorsDefName) || !defs[colorsDefName].contains("properties"))
        return out;
    for (auto it = defs[colorsDefName]["properties"].begin();
         it != defs[colorsDefName]["properties"].end(); ++it) {
        if (!it.value().contains("$ref")) continue;
        const std::string scRef = it.value()["$ref"].get<std::string>();
        if (scRef.rfind(kPrefix, 0) != 0) continue;
        const auto& states = kStateSets.find(scRef.substr(kPrefix.size()));
        if (states != kStateSets.end()) out[it.key()] = states->second;
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
        std::printf("[CLIP] readBack=%d\n",
                    m_propDynamic.GetBool(PropertyNames::kClipChildren) ? 1 : 0);
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
    const auto t0 = std::chrono::steady_clock::now();
    for (Control& c : m_dynamicRows)
        if (c.IsValid()) c.Destroy();
    m_dynamicRows.clear();
    m_dynamicRowY.clear();
    m_fixedWidthRows.clear();
    m_captionEditor = Control();
    m_schemaTarget = Control();
    m_rowRefreshers.clear();
    const auto t1 = std::chrono::steady_clock::now();
    if (!m_propDynamic.IsValid()) return;
    const ControlModel* m = findModel(m_selectedId);
    if (!m) {   // 未选中：动态区留空（滚动条一并隐藏）
        if (m_dynScroll.IsValid()) m_dynScroll.SetBool(PropertyNames::kVisible, false);
        m_rowsBuiltSig.clear();
        return;
    }
    const PlacedControl* pc = placedById(m->props.id);
    if (!pc || !pc->ctl.IsValid()) return;
    if (!m_schema) return;

    const auto& defs = (*m_schema)["$defs"];
    if (!defs.contains(m->props.type)) return;
    const nlohmann::json& def = defs[m->props.type];
    if (!def.contains("properties")) return;

    // E 方案快路径：同类型行集相同（schema 确定）——跳过销毁重建，仅执行各行
    // 刷值闭包（读选中控件当前值回填行显示，毫秒级）
    if (m_rowsBuiltSig == m->props.type && !m_rowRefreshers.empty()) {
        m_schemaTarget = pc->ctl;
        m_propSync = true;   // 程序化回填不触发联动回调
        for (auto& fn : m_rowRefreshers)
            if (fn) fn();
        m_propSync = false;
        applyDynScroll(m_dynScroll.GetFloat(PropertyNames::kValue));
        std::printf("[DBG-perf] refresh-only (sig=%s, refreshers=%zu)\n",
                    m_rowsBuiltSig.c_str(), m_rowRefreshers.size());
        std::fflush(stdout);
        return;
    }

    // 合并 common（allOf $ref）与类型自有属性
    // （P0-26 重构后 common 仅真通用键：id/enabled/visible/margin/scale/rect 等）
    nlohmann::json merged = def["properties"];
    const auto& commonProps = defs["common"]["properties"];
    for (auto it = commonProps.begin(); it != commonProps.end(); ++it)
        if (!merged.contains(it.key())) merged[it.key()] = it.value();

    // 跳过：布局/资源/嵌套复杂对象/事件类；caption/enabled/visible 固定区已有
    // （schema 键名已统一 kebab——skip 名单同步 kebab）
    static const char* kSkip[] = {
        "type", "id", "name", "rect", "scale", "margin", "layout", "anchor",
        "events", "context-menu", "actors", "luotiAni", "caption-label",
        "file", "resourceId", "providerName",   // 解析期键（引擎无运行时 setter）
        "match-parent-rect", "debug-draw",
        "expand", "offset", "size", "style", "alignment",
        "check-state", "vertical-align", "popup", "position",
        "caption", "enabled", "visible",
        "font-file", "font-resource",   // 资源路径类（border-visible 已随 P0-26 视觉组下放启用）
        "flow-weight",                                     // 布局权重（画布自由布局无意义）
        "col", "colSpan", "row", "rowSpan", "gap", "padding",   // 容器布局键
        "clip-children",
    };

    const UIRect box = m_propDynamic.GetRect();    // 容器实际尺寸（v-flow 排布后）
    const float viewH = box.h, barW = kPropBarW;
    const float viewRowsH = viewH;                 // 行区全高（滚动条浮于右侧）

    // 滚动条：首次创建常驻（orientation=0 垂直；position-changed → applyDynScroll）
    if (!m_dynScroll.IsValid()) {
        m_dynScroll = m_ui->CreateScrollBar(box.w - barW, 0.0f, barW, viewH, 0);
        m_dynScroll.SetFloat(PropertyNames::kRangeMin, 0.0f);
        m_dynScroll.SetFloat(PropertyNames::kPageSize, viewRowsH);
        m_dynScroll.SetFloat(PropertyNames::kStepSize, kPropCheckH);
    m_dynScroll.SetCallback(PropertyNames::kEventPositionChanged,
                            [this](const Event& e) { applyDynScroll(e.GetValueChanged()); });
    m_propDynamic.AddChild(m_dynScroll);
    // 容器级滚轮（引擎 Panel mouse-wheel 事件，载荷 floatVal=scrollY：+1 上/-1 下）：
    // 最近订阅者消费——动态区内由 propDynamic、面板其他区域由 tabProperty（App 接线）
    m_propDynamic.SetCallback(PropertyNames::kEventMouseWheel, [this](const Event& e) {
        ScrollDynamicBy(e.GetValueChanged());
    });
    }
    m_dynScroll.SetBool(PropertyNames::kVisible, true);

    m_schemaTarget = pc->ctl;
    m_schemaType = m->props.type;
    // 无文字概念的控件（图片/动画）：固定区 caption 行隐藏（背景色行已退役——
    // 背景色统一走动态区 colors.background.normal 色槽）
    const bool noText = (m_schemaType == "image" || m_schemaType == "animation");
    if (m_propCaption.IsValid())
        m_propCaption.SetBool(PropertyNames::kVisible, !noText);
    if (m_propColor.IsValid())
        m_propColor.SetBool(PropertyNames::kVisible, false);   // 已退役：动态区背景色槽替代
    const bool textIsCaption = (m_schemaType == "label" || m_schemaType == "button" ||
                                m_schemaType == "check-box" || m_schemaType == "image-button");

    float y = kPropInsetX;
    // 动态行排版节奏（统一相对位置：描述行 22 + 2 + 控件行 26 + 组间距 8）
    const float lblFont = kPropLblFont, lblH = kPropLblH, ctlH = kPropCtlH,
          gapLbl = kPropGapLbl, rowGap = kPropRowGap;
    const float rowW = box.w - kPropRowInset;   // 右侧留滚动条通道

    int rows = 0;
    std::string genKeys;
    // 分类折叠：分组标题行（扁平按钮）——点击展开/收起，收起组不创建行
    // （E 签名含折叠状态：切换组时重建、状态不变时同类 refresh 复用不受影响）
    auto makeGroupHeader = [&](const std::string& name) {
        const bool collapsed = m_collapsedGroups.count(name) != 0;
        Control hb = m_ui->CreateButton(
            std::string(collapsed ? "▸ " : "▾ ") + name, 0, 0, rowW, kPropCtlH);
        hb.SetStateColor(PropertyNames::kBackground,
                         UIStateColor{kPropPanelBg, kPropPanelBg, kPropPanelBg, kPropPanelBg});
        hb.SetStateColor(PropertyNames::kBorder,
                         UIStateColor{kPropPanelBg, kPropPanelBg, kPropPanelBg, kPropPanelBg});
        hb.SetStateColor(PropertyNames::kText,
                         UIStateColor{UIColor{0xB8, 0xB8, 0xB8, 255}, UIColor{0xE8, 0xE8, 0xE8, 255},
                                      UIColor{0xE8, 0xE8, 0xE8, 255}, UIColor{0x90, 0x90, 0x90, 255}});
        hb.SetRect(kPropInsetX, y, rowW, kPropCtlH);
        hb.SetCallback(PropertyNames::kEventClick, [this, name](const Event&) {
            if (m_collapsedGroups.count(name)) m_collapsedGroups.erase(name);
            else m_collapsedGroups.insert(name);
            rebuildDynamicRows();   // 重排：仅创建展开组的行
        });
        m_propDynamic.AddChild(hb);
        m_dynamicRows.push_back(hb);
        m_dynamicRowY.push_back(y);
        y += ctlH + kPropRowGap;
        ++rows;
        genKeys += " [组:" + name + (collapsed ? "|收" : "|开") + "]";
    };
    auto groupCollapsed = [&](const std::string& name) {
        return m_collapsedGroups.count(name) != 0;
    };
    bool commonHeaderDone = false;   // 常规组标题哨兵（colors 段后首个通用行前插入）
    std::string lastOwner;           // 当前属性分组（组切换时插组标题行）
    long long msLabel = 0, msTextArea = 0, msColor = 0, msNud = 0, msCheck = 0, msOther = 0;
    auto sw = [](long long& acc, const auto& fn) {
        const auto s = std::chrono::steady_clock::now();
        fn();
        acc += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s).count();
    };
    auto makeLabel = [&](const std::string& label) {
        sw(msLabel, [&] {
        Control lb = m_ui->CreateLabel(label, lblFont, 0, 0, rowW, lblH);
        lb.SetColor(PropertyNames::kText, UIColor{0x90, 0x90, 0x90, 255});
        lb.SetRect(kPropInsetX, y, rowW, lblH);
        m_propDynamic.AddChild(lb);
        m_dynamicRows.push_back(lb);
        m_dynamicRowY.push_back(y);
        y += lblH + gapLbl;
        });
    };
    // 先收集再排序：nlohmann 对象按字母序遍历，资源/路径类键（animation、
    // 四态图等）会被普通属性切断——聚尾生成（用户视角的属性分组）
    std::vector<std::pair<std::string, const nlohmann::json*>> kvs;
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        // colors 键按子组拆伪键（colors@background 等）——分组排序后同组连续呈现
        if (it.key() == "colors") {
            const auto cg = resolveColorGroups((*m_schema)["$defs"], def);
            for (const auto& [grpName, states] : cg)
                kvs.push_back({"colors@" + grpName, &it.value()});
            continue;
        }
        kvs.push_back({it.key(), &it.value()});
    }
    {   // [诊断] 伪键清单
        std::string dbg;
        for (const auto& kv : kvs)
            if (kv.first.rfind("colors@", 0) == 0) dbg += kv.first + " ";
        std::printf("[DBG-dyn] kvs=%zu colorsPseudo: %s\n", kvs.size(), dbg.c_str());
        std::fflush(stdout);
    }
    {   // [诊断] kvs 键集与 font 键存在性
        bool hasFont = false, hasFontSize = false;
        std::string dbg;
        for (const auto& kv : kvs) {
            if (kv.first == "font") hasFont = true;
            if (kv.first == "font-size") hasFontSize = true;
            dbg += kv.first + " ";
        }
        std::printf("[DBG-dyn] kvs=%zu font=%d font-size=%d keys: %s\n",
                    kvs.size(), hasFont ? 1 : 0, hasFontSize ? 1 : 0, dbg.c_str());
        std::fflush(stdout);
    }
    auto groupOf = [](const std::string& key) -> int {
        if (key == "colors@background") return 0;                    // 背景色
        if (key == "colors@border") return 1;                        // 边框色
        if (key == "colors@text" || key == "colors@text-shadow") return 2;  // 字体及阴影
        std::string p0 = jsonKeyToProp(key);
        if (key == "text" || key == "font" || p0 == "font-size") return 2;
        return 3;                                                    // 常规
    };
    std::stable_sort(kvs.begin(), kvs.end(), [&](const auto& a, const auto& b) {
        const int ga = groupOf(a.first), gb = groupOf(b.first);
        if (ga != gb) return ga < gb;
        return !isPathLikeProp(jsonKeyToProp(a.first)) &&
               isPathLikeProp(jsonKeyToProp(b.first));   // 组内：资源/路径类聚尾
    });
    int lastGroup = -1;   // 当前已呈现组序号（-1=尚未）
    // 类型标识行（选中控件类型一目了然——首字母大写的 schema 类型名）
    if (!m_schemaType.empty()) {
        std::string typeName = m_schemaType;
        typeName[0] = std::toupper((unsigned char)typeName[0]);
        Control lb = m_ui->CreateLabel(typeName, kPropLblFont, 0, 0, rowW, kPropLblH);
        lb.SetColor(PropertyNames::kText, UIColor{0xE8, 0xE8, 0xE8, 255});
        lb.SetRect(kPropInsetX, y, rowW, kPropLblH);
        m_propDynamic.AddChild(lb);
        m_dynamicRows.push_back(lb);
        m_dynamicRowY.push_back(y);
        y += kPropLblH + kPropRowGap;
        ++rows;
        genKeys += " [类型:" + typeName + "]";
    }
    for (const auto& kv : kvs) {
        std::string key = kv.first;
        bool skip = false;
        for (const char* s : kSkip)
            if (key == s) { skip = true; break; }
        if (!skip && m_schemaType == "button" &&
            (key == "image" || key == "image-resource" || key == "imageResource"))
            skip = true;   // Button 无 image 运行时分发（资源入口是四态图/animation）
        if (skip) continue;
        const nlohmann::json& spec = *kv.second;
        // 组切换：按排序组序号呈现组标题（同组连续→仅一次；折叠组跳过行创建）
        const int grp = groupOf(kv.first);
        static const char* kGroupNames[] = { "背景色", "边框色", "字体及阴影", "常规" };
        if (lastGroup != grp) {
            makeGroupHeader(kGroupNames[grp]);
            lastGroup = grp;
        }
        if (m_collapsedGroups.count(kGroupNames[grp])) continue;   // 收起：不创建该组行
        std::string colorSubGrp;   // colors 伪键的子组名（非伪键为空）
        if (key.rfind("colors@", 0) == 0) {
            colorSubGrp = key.substr(7);
            key = "colors";
        }
        const std::string t = spec.value("type", "");

        // 颜色组展开：colors.<组>.<态> → 运行时 "组[.态]"（normal 省略后缀，键名点号风格）
        // 行先放占位 Button（背景=当前色+hex 文本），点击才创建 ColorPicker（3.5s 构建惰性化）
        if (key == "colors" && spec.is_object()) {
            // P0-26 schema 重构：按类型 def 声明的态子集生成（colors-full 四态/
            // colors-menu 三态/colors-basic1 单态……），未声明组不生成；
            // 子组归属四分组（background→背景色、border→边框色、text/text-shadow→字体及阴影）
            const auto colorGroupsAll =
                resolveColorGroups((*m_schema)["$defs"], def);
            const std::string owner = (colorSubGrp == "background") ? "背景色"
                                    : (colorSubGrp == "border") ? "边框色"
                                    : "字体及阴影";
            std::printf("[DBG-dyn] colorSlot grp=%s collapsed=%d\n",
                        colorSubGrp.c_str(), groupCollapsed(owner) ? 1 : 0);
            std::fflush(stdout);
            if (groupCollapsed(owner)) continue;   // 收起：不创建该组色槽
            auto itg = colorGroupsAll.find(colorSubGrp);
            if (itg == colorGroupsAll.end()) {
                std::printf("[DBG-dyn] colorGroupsAll miss %s\n", colorSubGrp.c_str());
                std::fflush(stdout);
                continue;
            }
            const auto& [grpName, states] = *itg;
        const bool hasShadow = def.contains("properties") &&
                               def["properties"].contains("shadow");
        const bool isListLike = (m_schemaType == "list-view" ||
                                 m_schemaType == "tree-view");
        auto renderShadowRows = [&]() {
            if (!hasShadow || isListLike) return;
            const bool isLabelT = (m_schemaType == "label");
            const bool legacyInner = (m_schemaType == "button" ||
                                      m_schemaType == "check-box" ||
                                      m_schemaType == "win-frame");
            const char* attLabelKey = (m_schemaType == "win-frame")
                ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel;
            Control shTarget = m_schemaTarget;
            if (legacyInner && !isLabelT) {
                void* h = m_schemaTarget.GetPtr(attLabelKey);
                if (h) shTarget = m_viewport->FromHandle(h);
            }
            makeLabel("shadow.enabled");
            {
                Control cb = m_ui->CreateCheckBox("shadow.enabled", 0, 0, rowW, ctlH);
                cb.SetRect(kPropInsetX, y, rowW, ctlH);
                cb.SetBool(PropertyNames::kTriState, false);
                bool on = false;
                if (m_schemaType == "label") on = m_schemaTarget.GetBool(PropertyNames::kShadow);
                else if (m_schemaType == "button") on = m_schemaTarget.GetBool(PropertyNames::kShadow);
                else on = shTarget.IsValid() && shTarget.GetBool(PropertyNames::kShadow);
                cb.SetBool(PropertyNames::kChecked, on);
                m_rowRefreshers.push_back([this, cb]() mutable {
                    const bool ron = m_schemaType == "win-frame"
                        ? [&] { void* h = m_schemaTarget.GetPtr(PropertyNames::kTitleLabel);
                                return h && m_viewport->FromHandle(h).GetBool(PropertyNames::kShadow); }()
                        : m_schemaTarget.GetBool(PropertyNames::kShadow);
                    cb.SetBool(PropertyNames::kChecked, ron);
                });
                cb.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e2) {
                    const bool on2 = e2.GetCheckState() != 0;
                    if (m_schemaType == "win-frame") {
                        void* h = m_schemaTarget.GetPtr(PropertyNames::kTitleLabel);
                        if (h) m_viewport->FromHandle(h).SetBool(PropertyNames::kShadow, on2);
                        return;
                    }
                    if (m_schemaTarget.IsValid())
                        m_schemaTarget.SetBool(PropertyNames::kShadow, on2);
                });
                m_propDynamic.AddChild(cb);
                m_dynamicRows.push_back(cb);
                m_dynamicRowY.push_back(y);
                y += ctlH + rowGap;
                ++rows;
                genKeys += " shadow.enabled";
            }
            static const char* kOff[] = { "shadow.offset.x", "shadow.offset.y" };
            static const char* kOffProp[] = { "shadow-offset-x", "shadow-offset-y" };
            for (int oi = 0; oi < 2; ++oi) {
                makeLabel(kOff[oi]);
                Control nud = m_ui->CreateNumericUpDown(0, 0, rowW, ctlH);
                nud.SetRect(kPropInsetX, y, rowW, ctlH);
                nud.SetFloat(PropertyNames::kStep, kShadowStep);
                nud.SetFloat(PropertyNames::kRangeMin, -kShadowRange);
                nud.SetFloat(PropertyNames::kRangeMax, kShadowRange);
                const float v = shTarget.IsValid() ? shTarget.GetFloat(kOffProp[oi]) : 0.0f;
                nud.SetFloat(PropertyNames::kValue, v);
                m_rowRefreshers.push_back([this, nud, legacyInner, attLabelKey, oi]() mutable {
                    float rv = 0.0f;
                    if (m_schemaType == "label" || !legacyInner) {
                        rv = m_schemaTarget.GetFloat(kOffProp[oi]);
                    } else {
                        void* h = m_schemaTarget.GetPtr(attLabelKey);
                        if (h) rv = m_viewport->FromHandle(h).GetFloat(kOffProp[oi]);
                    }
                    nud.SetFloat(PropertyNames::kValue, rv);
                });
                nud.SetCallback(PropertyNames::kEventValueChanged,
                                [this, legacyInner, attLabelKey, oi](const Event& e2) {
                    if (!m_schemaTarget.IsValid()) return;
                    if (m_schemaType == "label" || !legacyInner) {
                        m_schemaTarget.SetFloat(kOffProp[oi], e2.GetValueChanged());
                        return;
                    }
                    void* h = m_schemaTarget.GetPtr(attLabelKey);
                    if (h) m_viewport->FromHandle(h).SetFloat(kOffProp[oi], e2.GetValueChanged());
                });
                m_propDynamic.AddChild(nud);
                m_dynamicRows.push_back(nud);
                m_dynamicRowY.push_back(y);
                y += ctlH + rowGap;
                ++rows;
                genKeys += " " + std::string(kOff[oi]);
            }
        };

            // 字体及阴影组续：shadow 行紧随 text/text-shadow 槽（同组连续不重复标题）
                const std::string gk = grpName;   // schema 子组名即运行时组键（text-shadow）
                for (const std::string& st : states) {
                    // 背景色入动态区（固定区背景色行已退役——schema 驱动统一）；
                    // win-frame 无 hover/pressed 状态机（窗口语义）——不可达槽跳过
                    // （text.normal/disabled 可达保留）
                    if (m_schemaType == "win-frame" && grpName == "text" &&
                        (st == "hover" || st == "pressed"))
                        continue;
                    const std::string prop = gk + (st == "normal"
                        ? std::string() : ("." + st));
                    const std::string lbl = grpName + "." + st;
                    // 内部文本 Label 的暴露键按类型分派：WinFrame 用其专有 title-label
                    // （caption-label 不为其发明新键——92 键一致性），其余用 caption-label
                    const char* capKey = (m_schemaType == "win-frame")
                        ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel;
                    // 回读：text/textShadow 组的实际色在内部 Label 上
                    // （Button 基类字段不反映直控结果）——从其读
                    UIColor cc;
                    if ((grpName == "text" || grpName == "text-shadow") && m_ui) {
                        void* capH = m_schemaTarget.GetPtr(capKey);
                        cc = capH ? m_viewport->FromHandle(capH).GetColor(prop.c_str())
                                  : m_schemaTarget.GetColor(prop.c_str());
                    } else {
                        cc = m_schemaTarget.GetColor(prop.c_str());
                    }
                    char hex[12];
                    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X",
                                  cc.r, cc.g, cc.b, cc.a);
                    makeLabel(lbl);

                    // 颜色槽：直建 ColorPicker（引擎惰性化——关闭态秒建、弹窗首次打开才构建）
                    // 宽固定 120（宽度同步跳过）；color-changed → 写对应属性键
                    sw(msColor, [&] {
                    Control cp = m_ui->CreateColorPicker(kPropInsetX, y, kColorRowW, ctlH, hex);
                    cp.SetCallback(PropertyNames::kEventColorChanged,
                                   [this, prop, gk, grpName, st, capKey](const Event& e) {
                        if (!m_schemaTarget.IsValid()) return;
                        const UIColor c = e.GetChangedColor();
                        // text/textShadow 组：直控内部 caption Label
                        // （GetPtr("caption-label")——Button 单色 setter 不同步 caption 的绕行回收）
                        if ((grpName == "text" || grpName == "text-shadow") && m_ui) {
                            void* capH = m_schemaTarget.GetPtr(capKey);
                            if (capH) {
                                // caption 属于被设计控件子树（子视口实例）——
                                // FromHandle 须用子视口域（主实例域会 validate 失败落空）
                                auto cap = m_viewport->FromHandle(capH);
                                const std::string capProp = gk + (st == "normal"
                                    ? std::string() : ("." + st));
                                cap.SetColor(capProp.c_str(), c);
                                // CheckBox recreate 丢 caption 状态——存 extended（拖动结束恢复用）
                                ControlModel* m2 = findModel(m_selectedId);
                                if (m2) {
                                    char hex8[12];
                                    std::snprintf(hex8, sizeof(hex8), "#%02X%02X%02X%02X",
                                                  c.r, c.g, c.b, c.a);
                                    m2->props.extended[prop + "-color"] = hex8;
                                }
                                return;
                            }
                        }
                        m_schemaTarget.SetColor(prop.c_str(), c);   // 兜底（无 caption 控件等）
                        {
                            ControlModel* m2 = findModel(m_selectedId);
                            if (m2) {
                                char hex8[12];
                                std::snprintf(hex8, sizeof(hex8), "#%02X%02X%02X%02X",
                                              c.r, c.g, c.b, c.a);
                                m2->props.extended[prop + "-color"] = hex8;
                            }
                        }
                    });
                    m_fixedWidthRows.insert(m_dynamicRows.size());
                    m_propDynamic.AddChild(cp);
                    m_dynamicRows.push_back(cp);
                    m_dynamicRowY.push_back(y);
                    y += ctlH + rowGap;
                    ++rows;
                    genKeys += " " + lbl;
                    // E：刷值闭包（同类型复用时重读色值回填槽）
                    m_rowRefreshers.push_back([this, cp, prop, grpName, gk, st, capKey]() mutable {
                        UIColor cc2;
                        if ((grpName == "text" || grpName == "text-shadow") && m_ui) {
                            void* capH = m_schemaTarget.GetPtr(capKey);
                            cc2 = capH ? m_viewport->FromHandle(capH).GetColor(prop.c_str())
                                       : m_schemaTarget.GetColor(prop.c_str());
                        } else {
                            cc2 = m_schemaTarget.GetColor(prop.c_str());
                        }
                        char hex2[12];
                        std::snprintf(hex2, sizeof(hex2), "#%02X%02X%02X%02X",
                                      cc2.r, cc2.g, cc2.b, cc2.a);
                        cp.SetColor(prop.c_str(), cc2);
                        cp.SetString(PropertyNames::kTextContent, hex2);
                    });
                    });   // msColor
                }
            if (colorSubGrp == "text-shadow") renderShadowRows();

            continue;
        }

        // font 对象键特判：字体名 string 行（引擎 kFont 运行时分发——
        // P032_37 批 button/check-box/win-frame 等补齐；font 为 $ref(font-spec)
        // 无 type 字段——按 key 判定而非 type）
        std::string effT = t;
        if (key == "font") effT = "string";
        if (effT != "string" && effT != "number" && effT != "integer" && effT != "boolean") continue;

        // font 对象键：字体名 ComboBox（注册字体清单枚举——防手输拼写错误；
        // 写回/读回经引擎 P0-46 string 别名通道 → Enum）
        if (key == "font") {
            makeLabel(key);
            Control fcb = m_ui->CreateComboBox(kPropInsetX, y, rowW, ctlH);
            // 引擎字体枚举清单 API（单一数据源——P047_48 批 2.3）；
            // P0-47 扩展：kFont 未设置时引擎读回实际生效的缺省字体名——
            // 无需"(缺省)"虚拟项，直接按读回名匹配选中
            const auto fontNames = UICornerstone::UICornerstone::GetFontNames();
            const std::string cur = m_schemaTarget.GetString(PropertyNames::kFont);
            int curIdx = 0;
            for (int i = 0; i < (int)fontNames.size(); ++i) {
                m_ui->ComboBoxAddItem(fcb, fontNames[i], fontNames[i]);
                if (ieqNoCase(cur, fontNames[i])) curIdx = i;
            }
            fcb.SetInt(PropertyNames::kSelectedIndex, curIdx);
            m_rowRefreshers.push_back([this, fcb, fontNames]() mutable {
                const std::string cur2 = m_schemaTarget.GetString(PropertyNames::kFont);
                for (int i = 0; i < (int)fontNames.size(); ++i)
                    if (ieqNoCase(cur2, fontNames[i])) {
                        fcb.SetInt(PropertyNames::kSelectedIndex, i);
                        break;
                    }
            });
            fcb.SetCallback(PropertyNames::kEventSelectionChanged,
                            [this, fcb, fontNames](const Event&) mutable {
                const int idx = fcb.GetInt(PropertyNames::kSelectedIndex);
                if (idx >= 0 && idx < (int)fontNames.size() && m_schemaTarget.IsValid()) {
                    m_schemaTarget.SetString(PropertyNames::kFont, fontNames[idx]);
                }
            });
            m_propDynamic.AddChild(fcb);
            m_dynamicRows.push_back(fcb);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
            ++rows;
            genKeys += " font";
            continue;
        }

        // schema 键 → 运行时属性：kebab 规则 + 引擎命名差异特例
        std::string prop = jsonKeyToProp(key);
        if (key == "text" && textIsCaption) prop = PropertyNames::kCaption;
        if (key == "border-visible") continue;   // 已并入边框色组（槽后开关行）——通用循环不生成

        // （path→animation 特判已删：引擎 P0-24 别名生效，path 即运行时键）
        makeLabel(key);

        if (effT == "string") {
            // TextArea：多行值（如 Label 文本含换行）直接输入真实换行，
            // 无需转义；行高 40 容两行，超出内部滚动
            sw(msTextArea, [&] {
            Control ta = m_ui->CreateTextArea(0, 0, rowW, kPropTextAreaH);
            ta.SetRect(kPropInsetX, y, rowW, kPropTextAreaH);
            // 编程创建无主题：显式深色面板协调样式
            ta.SetStateColor(PropertyNames::kBackground,
                             UIStateColor{kPropPanelBg, kPropPanelBg, kPropPanelBg, kPropPanelBg});
            ta.SetStateColor(PropertyNames::kText,
                             UIStateColor{kPropPanelText, kPropPanelText, kPropPanelText, kPropPanelText});
            ta.SetStateColor(PropertyNames::kBorder,
                             UIStateColor{kPropPanelBorder, kPropPanelBorderFocus, kPropPanelBorderFocus, kPropPanelBorder});
            // 内部文本 Label 键分派：check-box（无 caption 属性分发）与 win-frame
            // （title 键）经其内部 Label 直控（Label caption 读写完整）；其余直设属性
            const bool viaLabel = (m_schemaType == "check-box" || m_schemaType == "win-frame");
            const char* innerKey = (m_schemaType == "win-frame")
                ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel;
            std::string v;
            if (viaLabel) {
                void* h = m_schemaTarget.GetPtr(innerKey);
                v = h ? m_viewport->FromHandle(h).GetString(PropertyNames::kCaption) : "";
            } else {
                v = m_schemaTarget.GetString(prop.c_str());
                // 路径类引擎只写不读（normal-image 等）——extended 兜底显示
                if (v.empty() && isPathLikeProp(prop)) {
                    if (ControlModel* m2 = findModel(m_selectedId))
                        v = m2->props.extended.value(prop, "");
                }
            }
            ta.SetString(PropertyNames::kTextContent, v);
            // E：刷值闭包（同类型复用时重读选中控件值回填）
            m_rowRefreshers.push_back([this, ta, prop, viaLabel, innerKey]() mutable {
                std::string rv;
                if (viaLabel) {
                    void* h = m_schemaTarget.GetPtr(innerKey);
                    rv = h ? m_viewport->FromHandle(h).GetString(PropertyNames::kCaption) : "";
                } else {
                    rv = m_schemaTarget.GetString(prop.c_str());
                    if (rv.empty() && isPathLikeProp(prop)) {
                        if (ControlModel* m2 = findModel(m_selectedId))
                            rv = m2->props.extended.value(prop, "");
                    }
                }
                ta.SetString(PropertyNames::kTextContent, rv);
            });
            // 文本行（caption/title）与固定区"文本"行双向同步
            const bool isCaptionRow = (prop == PropertyNames::kCaption || prop == "title");
            if (isCaptionRow) m_captionEditor = ta;
            ta.SetCallback(PropertyNames::kEventTextChanged,
                           [this, prop, viaLabel, innerKey, isCaptionRow](const Event& e) {
                               if (m_propSync) return;
                               if (!m_schemaTarget.IsValid()) return;
                               const std::string v2 =
                                   sanitizePropValue(prop, e.GetTextChanged());
                               if (isCaptionRow && m_propCaption.IsValid()) {
                                   m_propSync = true;
                                   m_propCaption.SetString(PropertyNames::kTextContent, v2);
                                   m_propSync = false;
                               }
                               if (viaLabel) {
                                   void* h = m_schemaTarget.GetPtr(innerKey);
                                   if (h) m_viewport->FromHandle(h).SetString(
                                       PropertyNames::kCaption, v2);
                                   return;
                               }
                               if (isPathLikeProp(prop)) {
                                   // textChanged 每键写回——过滤明显无效中间态
                                   // （空串/无扩展名且非 provider: 引用），避免
                                   // loadFromFile 过程性失败日志刷屏；空串引擎无
                                   // 卸载语义，保持上次有效值
                                   const bool providerRef =
                                       v2.rfind("provider:", 0) == 0;
                                   // 空串放行（引擎加载失败 → Actor 无纹理 →
                                   // 图消失 = 用户视角的"删除"）；仅过滤非空
                                   // 无扩展名的中间态（'D:' 等 drive-relative
                                   // 会拼 basePath 刷失败日志）
                                   if (!providerRef && !v2.empty() &&
                                       v2.find('.') == std::string::npos)
                                       return;
                                   if (ControlModel* m2 = findModel(m_selectedId))
                                       m2->props.extended[prop] = v2;
                               }
                               m_schemaTarget.SetString(prop.c_str(), v2);
                               // playing 状态不干预：引擎 wasPlaying 语义
                               // （播放中换动画→重播、暂停中换→保持暂停）；
                               // 首次预览由 placeControl 放置时自动播放承担
                           });
            m_propDynamic.AddChild(ta);
            m_dynamicRows.push_back(ta);
            m_dynamicRowY.push_back(y);
            y += kPropTextAreaH + rowGap;
            });   // msTextArea
        } else if (effT == "number" || effT == "integer") {
            sw(msNud, [&] {
            Control nud = m_ui->CreateNumericUpDown(0, 0, rowW, kPropCtlH);
            nud.SetRect(kPropInsetX, y, rowW, kPropCtlH);
            // 按 schema 类型分派（引擎已归一：font-size 全控件 Int 域、
            // number 类走 Float 域）；读失败（0）→ 放置记录的 font-size 兜底
            const bool isInt = (t == "integer");
            if (!isInt) {   // number 类：小数步进+1 位小数（缺省 decimals=0 会 snap 到整数
                            // 网格且整数显示，0.1 被归零）
                nud.SetFloat(PropertyNames::kStep, 0.1f);
                nud.SetInt(PropertyNames::kDecimals, 1);
            }
            float v = isInt ? (float)m_schemaTarget.GetInt(prop.c_str())
                            : m_schemaTarget.GetFloat(prop.c_str());
            if (v <= 0 && (prop == "font-size" || prop == "caption-size"))
                v = m->props.extended.value("font-size", 0.0f);
            if (v > 0) nud.SetFloat(PropertyNames::kValue, v);
            m_rowRefreshers.push_back([this, nud, prop, isInt, fsFallback = m->props.extended.value("font-size", 0.0f)]() mutable {
                float rv = isInt ? (float)m_schemaTarget.GetInt(prop.c_str())
                                 : m_schemaTarget.GetFloat(prop.c_str());
                if (rv <= 0 && (prop == "font-size" || prop == "caption-size"))
                    rv = fsFallback;
                if (rv > 0) nud.SetFloat(PropertyNames::kValue, rv);
            });
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
            y += ctlH + rowGap;
            });   // msNud
        } else {
            sw(msCheck, [&] {
            Control cb = m_ui->CreateCheckBox(key, 0, 0, rowW, kPropCheckH);
            cb.SetRect(kPropInsetX, y, rowW, kPropCheckH);
            cb.SetBool(PropertyNames::kTriState, false);   // bool 属性两态（引擎缺省三态循环）
            // CheckBox 不发 click，发 check-changed（intVal=CheckState 枚举）
            cb.SetCallback(PropertyNames::kEventCheckChanged,
                           [this, prop](const Event& e) {
                if (m_schemaTarget.IsValid())
                    m_schemaTarget.SetBool(prop.c_str(), e.GetCheckState() != 0);
            });
            // 勾选状态回填（引擎 getter 补齐后 text-shadow-enable 等可读回）
            cb.SetBool(PropertyNames::kChecked,
                       m_schemaTarget.GetBool(prop.c_str()));
            m_rowRefreshers.push_back([this, cb, prop]() mutable {
                cb.SetBool(PropertyNames::kChecked,
                           m_schemaTarget.GetBool(prop.c_str()));
            });
            m_propDynamic.AddChild(cb);
            m_dynamicRows.push_back(cb);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
            });   // msCheck
        }
        ++rows;
        genKeys += " " + key;
    }

    // （Button 资源 5 行已删——引擎 P0-19 schema 补齐 normal/hover/pressed/
    // disabled-image + animation 后由通用 string 行生成，读回/写回/清理全兼容）

    // 文字阴影统一段（def 驱动：P0-26 schema 声明 shadow 的类型全覆盖）：
    // enabled：label/button/check-box 自身键（引擎分发 Label→setShadow /
    //   Button→textShadowEnable / CheckBox→宿主+caption 同步）；win-frame 经
    //   title-label 直控；其余类型自身键（引擎 P0-26 补齐的内部 Label 转发：
    //   valueLabel/closedLabel/textLabel/渲染助手）
    // offset：label 自身 / button+check-box+win-frame 内部 Label 直控 / 其余自身键
    // 例外：list-view/tree-view 的阴影为 item 级专用键（cell/item-text-shadow），
    //   通用 shadow 键不适用——暂不生成（后续单独行）

        const auto tEnd = std::chrono::steady_clock::now();
    std::printf("[DBG-perf] rebuildDynamicRows total=%lldms (destroy=%lldms build=%lldms) rows=%d\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t0).count(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - t1).count(),
                rows);
    std::printf("[DBG-perf]   by-type: label=%lld textArea=%lld color=%lld nud=%lld check=%lld other=%lldms\n",
                msLabel, msTextArea, msColor, msNud, msCheck, msOther);
    std::fflush(stdout);
    // 行集就绪：签名含折叠状态（切换折叠 → 重建展开组；状态不变同类 → refresh 快路径）
    {
        std::string coll;
        for (const auto& g : m_collapsedGroups) coll += g + ",";
        m_rowsBuiltSig = m_schemaType + "|" + coll;
    }
    m_dynScrollMax = (y + kPropInsetX) - viewRowsH;
    if (m_dynScrollMax < 0) m_dynScrollMax = 0;
    m_dynScroll.SetFloat(PropertyNames::kRangeMax, m_dynScrollMax > 0 ? m_dynScrollMax : 1.0f);
    m_dynScroll.SetFloat(PropertyNames::kValue, 0.0f);
    m_dynScroll.SetBool(PropertyNames::kVisible, m_dynScrollMax > 0);
    applyDynScroll(0.0f);
    std::printf("[CanvasPane] dynamic rows for '%s': %d (%s)\n",
                m_schemaType.c_str(), rows, genKeys.c_str());
}

// 滚动 → 行偏移（超界裁剪由 propDynamic 的 clip-children 承担，纯偏移）。
// 滚轮滚动动态区（mouse-wheel 事件载荷 scrollY 驱动；+1 向上 → 内容下移）
void CanvasPane::ScrollDynamicBy(float scrollY) {
    if (!m_dynScroll.IsValid() || m_dynScrollMax <= 0) return;
    float v = m_dynScroll.GetFloat(PropertyNames::kValue) - scrollY * kPropCheckH;
    if (v < 0) v = 0;
    if (v > m_dynScrollMax) v = m_dynScrollMax;
    m_dynScroll.SetFloat(PropertyNames::kValue, v);   // setValue → position-changed → applyDynScroll
}


// 动态区宽度跟随（splitter 拖动 propertyPanel 宽变化）：滚动条贴右缘、普通行宽同步；
// 色槽宽固定 120 不参与（参考外观区形态）。
// hex 字符串（#RRGGBBAA / #RRGGBB）→ UIColor（解析失败返回黑）
static UIColor hexToColor(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return UIColor{0, 0, 0, 255};
    auto byte = [&](size_t off) -> unsigned char {
        auto v = std::strtol(hex.substr(off, 2).c_str(), nullptr, 16);
        return static_cast<unsigned char>(v);
    };
    UIColor c{ byte(1), byte(3), byte(5), 255 };
    if (hex.size() >= 9) c.a = byte(7);
    return c;
}

void CanvasPane::syncDynamicWidth() {
    if (!m_propDynamic.IsValid() || !m_dynScroll.IsValid()) return;
    const UIRect box = m_propDynamic.GetRect();
    if (box.w <= kPropRowInset || box.w == m_seenDynW) return;
    m_seenDynW = box.w;
    const float rowW = box.w - kPropRowInset;   // 右侧留滚动条通道
    m_dynScroll.SetRect(box.w - kPropBarW, 0.0f, kPropBarW, box.h);
    for (size_t i = 0; i < m_dynamicRows.size(); ++i) {
        if (!m_dynamicRows[i].IsValid()) continue;
        if (m_fixedWidthRows.count(i)) continue;   // ColorPicker 行宽固定 120
        UIRect r = m_dynamicRows[i].GetRect();
        m_dynamicRows[i].SetRect(r.x, r.y, rowW, r.h);
    }
}

void CanvasPane::applyDynScroll(float value) {
    const float viewH = m_propDynamic.IsValid() ? m_propDynamic.GetRect().h
                                                : kPropDynHDefault;  // 兜底：与布局声明 propDynamic 高一致
    for (size_t i = 0; i < m_dynamicRows.size(); ++i) {
        if (!m_dynamicRows[i].IsValid() || i >= m_dynamicRowY.size()) continue;
        const float vy = m_dynamicRowY[i] - value;
        const UIRect r = m_dynamicRows[i].GetRect();
        m_dynamicRows[i].SetRect(r.x, vy, r.w, r.h);
        // clip-children 已开启但绘制仍可穿透（引擎嵌套 clip 语义），超界隐藏兜底
        m_dynamicRows[i].SetBool(PropertyNames::kVisible, vy > -kPropScrollHideBand && vy < viewH);
    }
}

// 引擎 HandleControl：选中框+8 向手柄+方向光标+拖拽 resize（原生组件，
// 挂 target 父容器+alwaysOnTop，draw 按 target getDrawRect 跟随 bench scale）。
// 首次创建并接线几何 filter（网格吸附）；改选经 SetHandleTarget 复用切换；
// detach 经 SetHandleTarget 空 target（移出容器+恢复光标）。
void CanvasPane::ensureSelHandle(Control target) {
    if (!m_viewport || !m_ui) return;
    if (!m_selHandle.IsValid()) {
        m_selHandle = m_ui->CreateHandleControl(target, 0, 0, 1, 1);
        if (!m_selHandle.IsValid()) return;
        m_ui->SetHandleRectFilter(m_selHandle, &CanvasPane::rectFilterThunk, this);
    } else {
        m_ui->SetHandleTarget(m_selHandle, target);
    }
}

// 取消选中/删除目标：detach（移出容器+恢复光标），实例保留复用。
void CanvasPane::detachSelHandle() {
    if (m_selHandle.IsValid())
        m_ui->SetHandleTarget(m_selHandle, Control());
}

// 拖拽几何过滤（HandleControl setRect 前回调，target 局部逻辑坐标）：
// 网格吸附量化（四边独立贴网格；引擎 minSize 钳位在前，filter 值最终）。
int CanvasPane::rectFilterThunk(UIControlHandle, float* x, float* y,
                                float* w, float* h, void* userData) {
    return static_cast<CanvasPane*>(userData)->filterRect(*x, *y, *w, *h);
}

int CanvasPane::filterRect(float& x, float& y, float& w, float& h) {
    if (!m_snapEnabled) return 0;
    const float g = m_gridCell;
    const ControlModel* m = findModel(m_selectedId);
    if (!m) return 0;
    const SRect& p = m->props.rect;   // 上帧 filter 输出（已贴网格）——锚定边推断基准

    // 纯移动（尺寸未变，Move 手柄/选框拖拽）：量化位置即可
    if (std::fabs(w - p.w) < kResizeEpsilon && std::fabs(h - p.h) < kResizeEpsilon) {
        x = std::round(x / g) * g;
        y = std::round(y / g) * g;
        return 1;
    }

    // resize：量化尺寸；按锚定边保持对缘（NW/W/SW 动左缘 → 保持右缘 = p 右缘，
    // p 已贴网格 + w 量化 → 新 x 自动贴网格，对缘不漂移）
    w = std::round(w / g) * g;
    h = std::round(h / g) * g;
    if (w < g) w = g;
    if (h < g) h = g;
    const bool leftFixed = std::fabs(x - p.x) < kResizeEpsilon;   // 左缘未动（SE/E/S 族）
    const bool topFixed  = std::fabs(y - p.y) < kResizeEpsilon;   // 上缘未动（SE/E/S 族）
    if (!leftFixed) x = (p.x + p.w) - w;
    if (!topFixed)  y = (p.y + p.h) - h;
    return 1;
}

void CanvasPane::tickInput() {
    // Delete 键：删除选中控件（键源每帧轮询一次，返回按下沿）
    if (m_keySource && m_keySource()) deleteSelected();
    syncDynamicWidth();   // 动态区宽度跟随（splitter 拖动后滚动条贴边/行宽同步）

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

    // 引擎 HandleControl 拖拽感知：rect 差异（filter 吸附后引擎 setRect）→
    // 同步模型/属性面板（吸附已由 filter 实时承担，此处纯同步）
    if (m_selectedId.empty() == false && m_dragId.empty()) {
        if (ControlModel* m = findModel(m_selectedId)) {
            if (PlacedControl* pc = placedById(m->props.id); pc && pc->ctl.IsValid()) {
                const UIRect vr = pc->ctl.GetRect();
                if (vr.w != m->props.rect.w || vr.h != m->props.rect.h ||
                    vr.x != m->props.rect.x || vr.y != m->props.rect.y) {
                    m->props.rect = SRect{ vr.x, vr.y, vr.w, vr.h };
                    fillPropertyPanel();
                }
            }
        }
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
            fillPropertyPanel();   // 拖动中同步属性面板（m_propSync 抑制回写）
        } else {
            // CheckBox 拖动 → setRect → recreate → 内部 caption 全新重建
            // （文本/直控色丢失）——从 model.extended 恢复
            if (m->props.type == "check-box" && m_dragCtl.IsValid()) {
                void* h = m_dragCtl.GetPtr(PropertyNames::kCaptionLabel);
                if (h) {
                    auto cap = m_viewport->FromHandle(h);
                    cap.SetString(PropertyNames::kCaption,
                                  m->props.extended.value("caption", ""));
                    const auto& ex = m->props.extended;
                    static const char* kCC[] = { "text", "text.hover", "text.pressed",
                                                 "text.disabled", "text-shadow",
                                                 "text-shadow.hover", "text-shadow.pressed",
                                                 "text-shadow.disabled" };
                    for (const char* k : kCC) {
                        auto it = ex.find(std::string(k) + "-color");
                        if (it != ex.end() && it.value().is_string()) {
                            cap.SetColor(k, hexToColor(it.value().get<std::string>()));
                        }
                    }
                }
            }
            m_dragId.clear();   // 松开结束拖动
        }
        m_mouseWasDown = down;
        return;
    }

    // 按下沿：选中控件手柄带让给 HandleControl（拖拽 resize）；控件命中 → 选中并
    // 拖动；空白 → 取消选中
    if (downEdge && inVp) {
        // 手柄精确让区：HandleHitTest 命中 → HandleControl 接管拖拽；未命中
        // （本体/空白）走设计器逻辑——本体边缘（非手柄处）恢复移动能力
        if (m_selectedId.empty() == false && m_selHandle.IsValid()) {
            int ht = 0;
            // 坐标域：m_handleAreas 基于 target getDrawRect（父链递归到视口根，
            // 视口根 rect=窗口全局位置）→ 全局域，传 SDL 窗口坐标原样
            const bool hh = m_ui->HandleHitTest(m_selHandle, wx, wy, ht);
            if (hh) {
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