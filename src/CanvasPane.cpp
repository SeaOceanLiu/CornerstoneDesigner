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

bool CanvasPane::Create(UICornerstone::UICornerstone& ui, Control canvasArea, float toolbarH) {
    m_ui = &ui;
    m_canvasArea = canvasArea;
    m_toolbarH = toolbarH;
    if (!m_canvasArea.IsValid() || !m_ui) return false;

    // 顺序关键：先注册瓦片资源（触发 owner 内存 provider 创建并挂载），
    // 再创建子视口（视口初始化时继承 owner 的 provider 指针）。
    registerGridTile();
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

// 视口窗口区域（内容区绝对坐标，含工具栏）。
SRect CanvasPane::ViewportRect() const {
    if (!m_viewport) return SRect{ 0, 0, 0, 0 };
    return SRect{ m_areaX, m_areaY + m_toolbarH, m_contentW, m_contentH };
}

}  // namespace Designer