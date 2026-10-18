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
#include <fstream>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <vector>
#include <filesystem>

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

// 网格瓦片：tilePx×tilePx，底色=画布（Bench）背景色 + 对比色点线（每 cellPx 一条，
// 左上边界对齐）。线色按底色亮度取对比（浅底加深 / 深底提亮 / 透明底中性灰）——
// 任意底色下网格可见（用户需求：画布使用设置的背景色且网格不消失）。
// 平铺后线在 0, cellPx, 2*cellPx… 处连续；瓦片右/下边缘不画线（由下一瓦片左/上边界提供）。
// showLines=false 生成纯底色瓦片（网格开关经瓦片重生成实现）。
std::vector<unsigned char> buildGridTilePng(int tilePx, int cellPx, bool showLines,
                                            unsigned char br, unsigned char bg,
                                            unsigned char bb, unsigned char ba) {
    // 对比线色：相对亮度 0.299R+0.587G+0.114B
    const float lum = (0.299f * br + 0.587f * bg + 0.114f * bb) / 255.0f;
    unsigned char lr, lg, lb;
    if (ba == 0) {                 // 透明底：中性灰（可见）
        lr = 0x9A; lg = 0x9A; lb = 0x9A;
    } else if (lum > 0.5f) {       // 浅底 → 同色系加深（55%）
        lr = (unsigned char)(br * 0.55f);
        lg = (unsigned char)(bg * 0.55f);
        lb = (unsigned char)(bb * 0.55f);
    } else {                       // 深底 → 向白混合 55%（提亮）
        lr = (unsigned char)(br + (255 - br) * 0.55f);
        lg = (unsigned char)(bg + (255 - bg) * 0.55f);
        lb = (unsigned char)(bb + (255 - bb) * 0.55f);
    }
    const unsigned char la = (ba == 0) ? 0xFF : (ba < 200 ? 200 : ba);   // 线不透明度下限（半透明底可读）
    std::vector<unsigned char> raw;
    raw.reserve((size_t)tilePx * ((size_t)tilePx * 4 + 1));
    for (int y = 0; y < tilePx; ++y) {
        raw.push_back(0);   // filter 0
        bool yLine = (y % cellPx == 0);
        for (int x = 0; x < tilePx; ++x) {
            const bool line = showLines && (yLine || (x % cellPx == 0));
            if (line && (x + y) % kGridDotStep == 0) {   // 沿线点阵（交点处必有）
                raw.push_back(lr); raw.push_back(lg);
                raw.push_back(lb); raw.push_back(la);
            } else {
                raw.push_back(br); raw.push_back(bg); raw.push_back(bb); raw.push_back(ba);
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

constexpr int kTileCells = 4;                  // 每瓦片最大 4×4 格（减少每帧平铺绘制调用数）
constexpr int kMaxTilePx = 512;                // 瓦片尺寸上限（防缩放档过大）

// ── 属性面板：动态行排版节奏（统一相对位置：描述行 + 控件行 + 组间距）──
constexpr float kPropLblFont = 16.0f;          // 描述行字号
constexpr float kPropLblH = 22.0f;             // 描述行高
constexpr float kPropCtlH = 26.0f;             // 控件行高
constexpr float kPropGapLbl = 2.0f;            // 描述行与控件行间距
constexpr float kPropRowGap = 8.0f;            // 行组间距
// 属性 TextArea 高度（保证≥3 整行可视——含内部滚动条出现时的扣减）：
// 文本行高 ≈22px（16 号字号实测）+ 上下边距 ≈8 + 横/竖滚动条厚 16 → 3×22+8+16 = 90
constexpr float kPropTextAreaH = 92.0f;        // 普通文本行：≥3 整行（滚动条出现仍满足）
constexpr float kPropItemsH = 112.0f;          // 结构化 items 行：≥4 整行（多行数据编辑）
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
constexpr float kAlignThreshold = 6.0f;        // 智能对齐吸附阈值（边缘/中心对齐——像素）
constexpr float kBandMinDrag = 3.0f;           // 框选判定阈值（画布逻辑像素——小于视为空白点击）
constexpr float kDefaultFontPx = 16.0f;        // 控件字号读失败时的回落缺省（引擎缺省字号）
constexpr float kMinContentH = 8.0f;           // 资源面板内容高度下限（钳制）
constexpr float kCanvasBarW = 12.0f;           // 画布滚动条边条宽（视口右缘/下缘让条）
constexpr float kZoomMax = 2.0f;               // 画布缩放上限
constexpr float kSliderMin = 0.0f;             // Slider 工具项创建参数：范围/缺省值
constexpr float kSliderMax = 100.0f;
constexpr float kSliderDefault = 50.0f;
constexpr float kCascStep = 16.0f;             // 放置瀑布偏移步长（避免完全重叠）
constexpr int kCascMod = 8;                    // 瀑布偏移取模周期
constexpr float kPropDynHDefault = 440.0f;     // 动态行区兜底高（与布局声明一致）

// 工具箱项 → 图标路径（拖放 ghost 跟随图案；与 main_layout 工具箱 leadingControl 一致）
std::string toolboxIconOf(const std::string& tb) {
    static const std::pair<const char*, const char*> kIcons[] = {
        { "tBtn", "gen/icons/button.svg" }, { "tLabel", "gen/icons/label.svg" },
        { "tImage", "gen/icons/image.svg" }, { "tCheck", "gen/icons/check-box.svg" },
        { "tEdit", "gen/icons/edit-box.svg" }, { "tArea", "gen/icons/text-area.svg" },
        { "tBar", "gen/icons/progress-bar.svg" }, { "tSlider", "gen/icons/slider.svg" },
        { "tScroll", "gen/icons/scroll-bar.svg" }, { "tShape", "gen/icons/shape.svg" },
        { "tPanel", "gen/icons/panel.svg" }, { "tWin", "gen/icons/win-frame.svg" },
        { "tSplit", "gen/icons/splitter.svg" }, { "tTabs", "gen/icons/tab-control.svg" },
        { "tCombo", "gen/icons/combo-box.svg" }, { "tList", "gen/icons/list-view.svg" },
        { "tTree", "gen/icons/tree-view.svg" }, { "tNum", "gen/icons/numeric-up-down.svg" },
        { "tColor", "gen/icons/color-picker.svg" }, { "tMenu", "gen/icons/menu-bar.svg" },
        { "tStatus", "gen/icons/status-bar.svg" }, { "tAni", "gen/icons/animation.svg" },
        { "tDialog", "gen/icons/dialog.svg" }, { "tCtx", "gen/icons/context-menu.svg" },
    };
    for (const auto& ic : kIcons)
        if (tb == ic.first) return ic.second;
    return std::string();
}

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
           p == "path" ||      // P0-24：引擎 "path" ≡ "animation" 别名（Animation 资源引用读回兜底）
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

    // 画布滚动条（canvasArea 子级；纸面超出视口时显示——视口右缘/下缘让条，非覆盖）
    m_vScroll = m_ui->CreateScrollBar(0, 0, kCanvasBarW, 100, 0);
    m_hScroll = m_ui->CreateScrollBar(0, 0, 100, kCanvasBarW, 1);
    if (m_vScroll.IsValid()) {
        m_canvasArea.AddChild(m_vScroll);
        m_vScroll.SetBool(PropertyNames::kVisible, false);
        m_vScroll.SetCallback(PropertyNames::kEventPositionChanged, [this](const Event& e) {
            if (m_scrollSync) return;
            m_panY = -e.GetValueChanged();
            clampPan();
            applyPan();
            syncScrollBars();
        });
    }
    if (m_hScroll.IsValid()) {
        m_canvasArea.AddChild(m_hScroll);
        m_hScroll.SetBool(PropertyNames::kVisible, false);
        m_hScroll.SetCallback(PropertyNames::kEventPositionChanged, [this](const Event& e) {
            if (m_scrollSync) return;
            m_panX = -e.GetValueChanged();
            clampPan();
            applyPan();
            syncScrollBars();
        });
    }

    // 顺序关键：先注册瓦片资源（触发 owner 内存 provider 创建并挂载），
    // 再创建子视口（视口初始化时继承 owner 的 provider 指针）。
    registerGridTile();
    refreshLayout();
    createGridImage();
    applyCanvasSize();     // 设计画布尺寸（基准窗体）→ bench 固定 1280×800
    // 滚轮：子视口根（Bench）容器级 wheel 回调（P0-61 Root()）——Ctrl 缩放/普通垂直滚动
    if (m_viewport) {
        Control root = m_viewport->Root();
        if (root.IsValid())
            root.SetCallback(PropertyNames::kEventMouseWheel, [this](const Event& e) {
                onWheel(e.GetValueChanged());
            });
    }
    resetUndo();   // F2.5：初始基线（空画布）——保证首个操作（如放置）可撤销
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
    // 控件 id 行：空闲到期提交（编辑完成再改名——防逐键 churn）
    if (m_idRenamePending && std::chrono::steady_clock::now() >= m_idRenameDeadline)
        commitPendingIdRename();
}
void CanvasPane::RenderViewport() {
    if (m_viewport) m_viewport->Render();
}

// 视口尺寸 = 内容区扣除滚动条边条（高溢出→右缘竖条占宽、滚 Y；宽溢出→下缘横条占高、
// 滚 X；按满尺寸判定、独立预留防振荡）。设计尺寸/缩放变化须重算——SetViewport 触发
// resized→recompute（锚点重置，调用方须随后重放平移）。
void CanvasPane::updateViewportRect() {
    if (!m_ui) return;
    const bool overH = (m_designH * m_zoom) > m_contentH + 0.5f;
    const bool overW = (m_designW * m_zoom) > m_contentW + 0.5f;
    m_vpW = m_contentW - (overH ? kCanvasBarW : 0.0f);
    m_vpH = m_contentH - (overW ? kCanvasBarW : 0.0f);
    if (!m_viewport) {
        m_viewport = m_ui->CreateViewport(m_areaX, m_areaY + m_toolbarH, m_vpW, m_vpH);
    } else {
        m_viewport->SetViewport(m_areaX, m_areaY + m_toolbarH, m_vpW, m_vpH);
    }
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

    updateViewportRect();   // 视口尺寸 = 内容区扣除滚动条边条

    updateGridRect();   // 纸面 rect = 设计尺寸（与视口尺寸解耦）
    m_panAppliedX = m_panAppliedY = 0;   // SetViewport→recompute 重置锚点——记账归零
    clampPan();
    applyPan();
    syncScrollBars();
}

// 设计画布尺寸（基准窗体）：off 模式 SetCanvasSize 仅记录——SetViewport（同值）强制
// resized→recompute，使 bench 固定为设计尺寸（不随视口/缩放漂移；P0-61 根锚定与导出基准）。
void CanvasPane::applyCanvasSize() {
    if (!m_viewport) return;
    m_viewport->SetCanvasSize(m_designW, m_designH);   // P0-61③：即时应用（setRect+recompute+reflow）
    updateViewportRect();   // 边条预留随设计尺寸变化（SetViewport→recompute）
    updateGridRect();
    m_panAppliedX = m_panAppliedY = 0;   // recompute 重置锚点——记账归零
    clampPan();
    applyPan();
    syncScrollBars();
    refreshStatusRight();
}

// 纸面/网格 rect = 设计尺寸（逻辑；drawRect=设计尺寸×zoom——纸面随缩放缩放）
void CanvasPane::updateGridRect() {
    if (m_grid.IsValid()) m_grid.SetRect(0, 0, m_designW, m_designH);
}

void CanvasPane::SetCanvasSize(float w, float h) {
    if (w < 320.0f) w = 320.0f;
    if (h < 240.0f) h = 240.0f;
    if (w == m_designW && h == m_designH) return;
    m_designW = w;
    m_designH = h;
    applyCanvasSize();
    commitUndo("canvas");   // F2.5：画布尺寸为独立撤销步
}

// 状态栏鼠标画布坐标（逻辑坐标；视口外 "-"）——取整去抖，仅变化时写入。
void CanvasPane::updateStatusPos(bool inVp, float cx, float cy) {
    static bool lastValid = false;
    static int lastX = 0, lastY = 0;
    int ix = 0, iy = 0;
    if (inVp) { ix = (int)std::lround(cx); iy = (int)std::lround(cy); }
    if (inVp == lastValid && ix == lastX && iy == lastY) return;
    lastValid = inVp;
    lastX = ix;
    lastY = iy;
    if (inVp)
        writeStatusItem("statusPos", u8"坐标 " + std::to_string(ix) + ", " + std::to_string(iy));
    else
        writeStatusItem("statusPos", u8"坐标 -");
}

// 平移钳制：纸面小于视口→居中；大于→边缘不越出视口（可平移查看全貌）。
void CanvasPane::clampPan() {
    const float vw = m_vpW / m_zoom, vh = m_vpH / m_zoom;
    if (m_designW <= vw) {
        m_panX = (vw - m_designW) * 0.5f;
    } else {
        if (m_panX > 0.0f) m_panX = 0.0f;
        if (m_panX < vw - m_designW) m_panX = vw - m_designW;
    }
    if (m_designH <= vh) {
        m_panY = (vh - m_designH) * 0.5f;
    } else {
        if (m_panY > 0.0f) m_panY = 0.0f;
        if (m_panY < vh - m_designH) m_panY = vh - m_designH;
    }
}

// 平移 → 引擎视口锚点（增量）：recomputeViewportTransform 会把锚点重置为视口原点
// （SetViewport/SetCanvasSize 路径），故 recompute 后须把记账清零再重算；
// 缩放变化（setScale 不重置锚点）走增量更新。
void CanvasPane::applyPan() {
    if (!m_viewport) return;
    const float tx = m_panX * m_zoom, ty = m_panY * m_zoom;
    m_viewport->SetViewportAnchor(tx - m_panAppliedX, ty - m_panAppliedY);
    m_panAppliedX = tx;
    m_panAppliedY = ty;
}

// 画布滚动条：rect/范围/值/可见性。右缘竖条=滚 Y（高溢出时显示）；下缘横条=滚 X
// （宽溢出时显示）；值 = 平移取反（向右/下滚动 = 纸面向左/上移动）。可见性判定与
// refreshLayout 的边条预留同口径（满尺寸比较）——不重叠、不振荡。
void CanvasPane::syncScrollBars() {
    if (!m_hScroll.IsValid() || !m_vScroll.IsValid()) return;
    const float vw = m_vpW / m_zoom, vh = m_vpH / m_zoom;
    const bool overH = (m_designH * m_zoom) > m_contentH + 0.5f;
    const bool overW = (m_designW * m_zoom) > m_contentW + 0.5f;
    m_scrollSync = true;
    // 右缘竖条：滚 Y
    m_vScroll.SetRect(m_vpW, m_toolbarH, kCanvasBarW, m_vpH);
    m_vScroll.SetFloat(PropertyNames::kRangeMin, 0.0f);
    m_vScroll.SetFloat(PropertyNames::kRangeMax, overH ? (m_designH - vh) : 1.0f);
    m_vScroll.SetFloat(PropertyNames::kPageSize, vh);
    m_vScroll.SetFloat(PropertyNames::kStepSize, 40.0f);
    m_vScroll.SetFloat(PropertyNames::kValue, overH ? -m_panY : 0.0f);
    m_vScroll.SetBool(PropertyNames::kVisible, overH);
    // 下缘横条：滚 X
    m_hScroll.SetRect(0, m_toolbarH + m_vpH, m_vpW, kCanvasBarW);
    m_hScroll.SetFloat(PropertyNames::kRangeMin, 0.0f);
    m_hScroll.SetFloat(PropertyNames::kRangeMax, overW ? (m_designW - vw) : 1.0f);
    m_hScroll.SetFloat(PropertyNames::kPageSize, vw);
    m_hScroll.SetFloat(PropertyNames::kStepSize, 40.0f);
    m_hScroll.SetFloat(PropertyNames::kValue, overW ? -m_panX : 0.0f);
    m_hScroll.SetBool(PropertyNames::kVisible, overW);
    m_scrollSync = false;
}

// 生成瓦片 PNG 并注册（首次调用触发 owner 内存 provider 创建/挂载）。
void CanvasPane::registerGridTile() {
    if (!m_ui) return;
    int cellPx = (int)std::ceil(m_gridCell * m_zoom);
    if (cellPx < 2) cellPx = 2;
    // 瓦片格数按上限缩减（不得改 cellPx——否则大档位网格间距被压缩，100/200 无区别）：
    // cellPx×tileCells ≤ kMaxTilePx，至少 1 格（cellPx 超上限时瓦片即单格）
    int tileCells = kMaxTilePx / cellPx;
    if (tileCells < 1) tileCells = 1;
    if (tileCells > kTileCells) tileCells = kTileCells;
    int tilePx = cellPx * tileCells;

    std::vector<unsigned char> png = buildGridTilePng(tilePx, cellPx, m_gridVisible,
                                                      m_benchBg.r, m_benchBg.g,
                                                      m_benchBg.b, m_benchBg.a);
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
                                     m_designW, m_designH);
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

// 放置控件默认样式：白纸上可见（浅色面 + 深色字 + 描边）。
// 编程创建的控件不带主题样式：Button 默认面色透明（露出视口深底）、Hover 蓝（引擎缺省）。
// UIColor → "#RRGGBBAA"（模型颜色记录用；与引擎布局 JSON 1:1）
static std::string uiColorHex(const UIColor& c) {
    char b[12];
    std::snprintf(b, sizeof(b), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    return b;
}
// 模型颜色记录：extended["colors"][组] = {normal/hover/pressed/disabled}（引擎兼容形态）
static void recordModelColors(ControlModel& model, const char* grp, const UIStateColor& sc) {
    model.props.extended["colors"][grp] = {
        {"normal", uiColorHex(sc.normal)}, {"hover", uiColorHex(sc.hover)},
        {"pressed", uiColorHex(sc.pressed)}, {"disabled", uiColorHex(sc.disabled)},
    };
}

void CanvasPane::styleControl(const std::string& toolboxId, Control ctl, ControlModel& model) {
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
        recordModelColors(model, "background", face);
        recordModelColors(model, "border", border);
        recordModelColors(model, "text", text);
        recordModelColors(model, "text-shadow", shadow);
    } else if (toolboxId == "tTabs") {
        // TabControl：页签文字用**亮色四态**——引擎页签底为深色（硬编码，P0-90 前不可配），
        // 深字在深底上不可读（用户实测"黑底深字"）；背景/边框仍写模型（导出/P0-90 后生效）
        const UIStateColor tabText{ UIColor{0xEB, 0xEB, 0xEB, 255}, UIColor{0xEB, 0xEB, 0xEB, 255},
                                    UIColor{0xEB, 0xEB, 0xEB, 255}, UIColor{0x99, 0x99, 0x99, 255} };
        ctl.SetStateColor(PropertyNames::kText, tabText);
        ctl.SetStateColor(PropertyNames::kBackground, face);
        ctl.SetStateColor(PropertyNames::kBorder, border);
        recordModelColors(model, "text", tabText);
        recordModelColors(model, "background", face);
        recordModelColors(model, "border", border);
    } else if (toolboxId == "tLabel" || toolboxId == "tCheck" ||
               toolboxId == "tNum" || toolboxId == "tCombo") {
        ctl.SetStateColor(PropertyNames::kText, text);
        recordModelColors(model, "text", text);
        if (toolboxId == "tCheck") {
            ctl.SetStateColor(PropertyNames::kBorder, border);
            recordModelColors(model, "border", border);
        }
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
    updateGridRect();    // 纸面 rect = 设计尺寸（drawRect=设计尺寸×zoom）
    updateViewportRect();   // 边条预留随缩放变化（SetViewport→recompute→锚点重置）
    m_panAppliedX = m_panAppliedY = 0;
    clampPan();
    applyPan();
    syncScrollBars();
    refreshStatusRight();   // 状态栏缩放百分比同步
    if (m_zoomChangedFn) m_zoomChangedFn();
}

// 缩放并保持光标下画布点屏幕位置不变（滚轮缩放锚点）：
// 旧画布点 = (mouse-area)/oldZoom - pan；新 pan = (mouse-area)/newZoom - 旧画布点。
void CanvasPane::SetZoomAt(float zoom, float sx, float sy) {
    const float cx = (sx - m_areaX) / m_zoom - m_panX;
    const float cy = (sy - (m_areaY + m_toolbarH)) / m_zoom - m_panY;
    SetZoom(zoom);
    m_panX = (sx - m_areaX) / m_zoom - cx;
    m_panY = (sy - (m_areaY + m_toolbarH)) / m_zoom - cy;
    clampPan();
    applyPan();
    syncScrollBars();
}

// 滚轮：Ctrl+滚轮=按档缩放（光标锚点）；普通滚轮=垂直滚动（一档 40 逻辑像素）。
// 事件源：子视口根（Bench 容器级 wheel 回调，P0-61 Root()）。
void CanvasPane::onWheel(float scrollY) {
    if (scrollY == 0.0f) return;
    static const float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    float mx = 0, my = 0;
    if (m_mouseSource) m_mouseSource(mx, my);
    if (m_ctrlSource && m_ctrlSource()) {
        float nz = m_zoom;
        if (scrollY > 0.0f) {          // 放大：取大于当前的最小档
            nz = kZooms[5];
            for (int i = 0; i < 6; ++i)
                if (kZooms[i] > m_zoom + 0.001f) { nz = kZooms[i]; break; }
        } else {                       // 缩小：取小于当前的最大档
            nz = kZooms[0];
            for (int i = 5; i >= 0; --i)
                if (kZooms[i] < m_zoom - 0.001f) { nz = kZooms[i]; break; }
        }
        SetZoomAt(nz, mx, my);
        return;
    }
    m_panY += scrollY * 40.0f;   // 向上滚 → 查看上方（纸面下移）
    clampPan();
    applyPan();
    syncScrollBars();
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
    refreshStatusRight();   // 状态栏网格开关状态同步
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
    cx = (sx - m_areaX) / m_zoom - m_panX;   // 纸面原点 = 视口原点 + pan×zoom
    cy = (sy - (m_areaY + m_toolbarH)) / m_zoom - m_panY;
    return true;
}

bool CanvasPane::CanvasToScreen(float cx, float cy, float& sx, float& sy) const {
    if (m_zoom <= 0) return false;
    sx = ((cx + m_panX) * m_zoom) + m_areaX;
    sy = ((cy + m_panY) * m_zoom) + m_areaY + m_toolbarH;
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
    return SRect{ m_areaX, m_areaY + m_toolbarH, m_vpW, m_vpH };
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

// P1-a/P1-b：容器类控件（内容区可承载子控件——WinFrame 严格 ClientPanel）
static bool isContainerType(const std::string& type) {
    return type == "panel" || type == "win-frame" ||
           type == "popup" || type == "dialog" ||
           type == "tab-control";   // 放置目标 = 当前页 Panel（页 Panel 设计器自管）
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
    else if (tb == "tArea") {
        ctl = m_viewport->CreateTextArea(x, y, w, h);
        if (ctl.IsValid()) ctl.SetBool(PropertyNames::kAutoScroll, false);   // P0-68：设计控件编辑式光标跟随
    }
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
    else if (tb == "tImage") {
        // 缺省样例图（引擎 assets 随构建拷贝；此前用画布网格纹理 provider:dc_canvas_grid
        // 系设计器内部资源——导出后在预览/应用侧无法解析）。**存在性守卫**：加载路径
        // 的模型已有 image（用户值）时不得覆盖。
        const std::string img = model.props.extended.value("image", "assets/images/bitmap1.bmp");
        ctl = m_viewport->CreateImage(img, x, y, w, h);
        if (!model.props.extended.contains("image")) model.props.extended["image"] = img;
    } else if (tb == "tAni") {
        // 缺省动画（assets 必备文件）：创建即加载——空路径创建引擎不支持
        // （prepare 对未加载 throw 回滚，引擎清单 P0-25）；用户经 path 行换动画。
        // **存在性守卫**：加载路径的模型已有 path（用户值）时不得覆盖。
        const std::string anim = model.props.extended.value(
            "path", "assets/animations/rotateBtn/rotateBtn.jsonc");
        ctl = m_viewport->CreateAnimation(anim, x, y, w, h);
        // 放置即播放（预览意图）；此后 path 换动画由引擎 wasPlaying 语义保持状态
        if (ctl.IsValid()) ctl.SetBool(PropertyNames::kPlaying, 1);
        if (!model.props.extended.contains("path")) model.props.extended["path"] = anim;
        if (!model.props.extended.contains("playing")) model.props.extended["playing"] = true;
    } else ctl = m_viewport->CreatePanel(x, y, w, h);   // 占位类型
    return ctl;
}

void CanvasPane::placeControl(const std::string& toolboxId, float cx, float cy, bool fromDrag) {
    std::printf("[Place] placeControl tool=%s at=(%.0f,%.0f) drag=%d\n",
                toolboxId.c_str(), cx, cy, fromDrag ? 1 : 0);
    if (!m_viewport) return;
    float w = 120, h = 32;
    const char* caption = "";
    const char* prefix = "ctl";
    if (!controlSpec(toolboxId, w, h, caption, prefix)) {
        std::printf("[Place]   spec-miss tool=%s\n", toolboxId.c_str());
        return;
    }

    // P1-b 放置目标双通道（07 §4.2，最新者优先）：
    //   ①画布命中——命中容器内容区（命中子控件则上溯最近容器祖先）→ 目标=该容器；
    //   ②控件树显式指定——树中选中容器 → 目标=该容器（m_placeTargetId）；
    //   未命中/未指定 → 画布根。WinFrame 严格 ClientPanel（标题栏/框架命中不深入）。
    std::string targetParentId;      // 空=画布根
    Control targetParent;            // 有效=容器挂载（AddChild）；无效=bench 根（工厂默认）
    SRect contentRect{ 0, 0, 0, 0 }; // 目标容器内容区全局 rect（坐标换算/钳制）
    {
        std::string candId;
        if (hitTest(cx, cy, candId)) {
            PlacedControl* pc = placedById(candId);
            // 命中非容器 → 沿父链上溯最近容器（放置为容器内兄弟控件）
            while (pc && !isContainerType(pc->model->props.type) && !pc->parentId.empty()) {
                candId = pc->parentId;
                pc = placedById(candId);
            }
            if (pc && isContainerType(pc->model->props.type)) {
                Control par;
                SRect cr;
                // 内容区判定（WinFrame：标题栏/框架命中不深入——沿用当前目标）
                if (resolveContainer(candId, par, cr) &&
                    cx >= cr.x && cx < cr.x + cr.w && cy >= cr.y && cy < cr.y + cr.h)
                    targetParentId = candId;
            }
        }
        if (targetParentId.empty() && !m_placeTargetId.empty()) {
            // 通道②回退：树选中的当前放置目标（已删除则清理）
            if (placedById(m_placeTargetId)) targetParentId = m_placeTargetId;
            else m_placeTargetId.clear();
        }
        if (!targetParentId.empty()) {
            if (resolveContainer(targetParentId, targetParent, contentRect))
                m_placeTargetId = targetParentId;   // 通道①更新当前目标（最新者优先）
            else
                targetParentId.clear();
        }
    }

    // 拖放放置：左上角=落点（精确、无瀑布）；双击中央：居中+瀑布防重叠
    float casc = fromDrag ? 0.0f : (float)(m_seq % kCascMod) * kCascStep;
    float x = fromDrag ? cx : (cx - w / 2 + casc);
    float y = fromDrag ? cy : (cy - h / 2 + casc);
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
    if (!ctl.IsValid()) {
        std::printf("[Place]   create-invalid tool=%s\n", toolboxId.c_str());
        return;
    }
    styleControl(toolboxId, ctl, *model);   // 编程创建无主题样式：显式浅色面/深字/描边（同步模型）

    // P1-b 容器挂载：AddChild 到容器 + 坐标换算为容器局部（钳制内容区，rect 记账同步）；
    // 双击（非拖放）落容器 → 内容区居中（显式目标容器的直觉落位）
    if (targetParent.IsValid()) {
        float lx, ly;
        if (fromDrag) { lx = x - contentRect.x; ly = y - contentRect.y; }
        else          { lx = (contentRect.w - w) * 0.5f; ly = (contentRect.h - h) * 0.5f; }
        if (lx > contentRect.w - w) lx = contentRect.w - w;
        if (ly > contentRect.h - h) ly = contentRect.h - h;
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        m_viewport->AddChild(targetParent, ctl);
        ctl.SetRect(lx, ly, w, h);
        model->props.rect = { lx, ly, w, h };
        // TabControl 目标：实际挂载父 = 当前页 Panel——记账 parentId 同步解析（否则
        // 序列化丢页内子树）
        if (PlacedControl* tp = placedById(targetParentId))
            if (tp->model->props.type == "tab-control")
                targetParentId = resolvedParentIdOf(targetParentId);
        refreshPlaceTargetHint();   // 通道①命中已更新目标 → 状态栏同步
        syncModelsFromEngine();     // P0-59：容器挂载触发重排（布局容器）——同步全部模型
    }
    m_items.push_back({ std::move(model), ctl, targetParentId });   // P1-a：容器子控件记账
    // P0-59：引擎 id 表注册（child-id 定位前提）——必须注册到**子视口实例**的 id 表
    // （根 bench 的 findChildById 查其自身 context；主实例注册查不到）
    if (m_viewport) m_viewport->SetControlId(ctl, m_items.back().model->props.id);
    // 用户需求：StatusBar 缺省底拉伸（贴父容器/根底缘、水平拉伸）——顶层首次锚定自动切根布局
    if (toolboxId == "tStatus") {
        PlacedControl& pc = m_items.back();
        Control par = anchorParentOf(pc);
        if (par.IsValid()) {
            if (pc.parentId.empty() && par.GetEnum(PropertyNames::kLayout) != "anchor")
                par.SetEnum(PropertyNames::kLayout, "anchor");
            par.SetString(PropertyNames::kChildTargetId, pc.model->props.id);
            par.SetEnum(PropertyNames::kChildAnchor, "bottom-stretch");
            pc.model->props.extended["anchor"] = "bottom-stretch";
            syncModelsFromEngine();   // P0-60：锚点 setter 即时重排——同步全部模型
        }
    }
    refreshStatusRight();   // 状态栏控件数同步
    std::printf("[Canvas] placed %s id=%s at (%.0f,%.0f)\n",
                m_items.back().model->props.type.c_str(),
                m_items.back().model->props.id.c_str(), x, y);

    // 记录逻辑基础字号（引擎字号属性存逻辑值，缩放由 refreshScaleWith 自动跟随）；
    // 供动态行 getter 读失败时的回填兜底
    m_items.back().model->props.extended["font-size"] = readFontPx(ctl);

    // 控件树同步：新增节点（层级挂接——容器子控件挂父节点下；标签 = 默认标题 + id）
    syncTreeAdd(m_items.back().parentId, m_items.back().model->props.id,
                std::string(caption) + " " + m_items.back().model->props.id);

    // TabControl：放置即用——默认两页（页 Panel/树节点经 applyTabsText→syncTabs 创建，
    // 随放置一步撤销；页内放置/页背景编辑从控件树选中页节点开始）。
    // 注意：applyTabsText 会向 m_items 追加页 Panel——先存被放置控件 id（back() 不再指向它）
    const std::string placedId = m_items.back().model->props.id;
    if (toolboxId == "tTabs") {
        const nlohmann::json tabs = m_items.back().model->props.extended.value("tabs", nlohmann::json::array());
        if (!tabs.is_array() || tabs.empty())
            if (ControlModel* tm2 = findModel(placedId)) applyTabsText(*tm2, u8"页 1\n页 2");
    }
    // 选中新控件（HandleControl setTarget 重新附加，alwaysOnTop 置顶）
    m_selectedId.clear();
    selectItem(placedId);
    commitUndo();   // F2.5：放置为独立撤销步
}

// 有效可见性：自身 + 祖先链的引擎可见位。非当前 Tab 页由引擎 applyCurrentPage 置
// visible=false（模型不镜像该内部切换）——隐藏页及其子树不得参与命中（否则反向遍历
// 命中最后创建的隐藏页 → 放置/选中目标错页）。
bool CanvasPane::effectivelyVisible(const PlacedControl& pc) {
    for (const PlacedControl* p = &pc; p != nullptr;) {
        if (p->ctl.IsValid() && !p->ctl.GetBool(PropertyNames::kVisible)) return false;
        p = p->parentId.empty() ? nullptr : placedById(p->parentId);
    }
    return true;
}

bool CanvasPane::hitTest(float cx, float cy, std::string& outId) {
    // 轮 1：容器子控件优先（深度语义；helper 全局原点——含父链与 WinFrame 内容偏移）
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it) {
        if (it->parentId.empty()) continue;
        if (!effectivelyVisible(*it)) continue;
        // Tab 页 Panel：几何由引擎管理（applyCurrentPage 设内容区 rect）——模型 rect 为
        // 设计器记账（新建页=整 Tab 大小，会覆盖页签带 → 点页签被误判为点页 ✗）。
        // 命中用**引擎 rect**（Tab 局部）+ 父全局原点。
        if (it->model->props.type == "panel" && it->ctl.IsValid()) {
            PlacedControl* par = it->parentId.empty() ? nullptr : placedById(it->parentId);
            if (par && par->model->props.type == "tab-control") {
                const UIRect pr = it->ctl.GetRect();
                float pgx = 0, pgy = 0;
                parentGlobalOriginOf(*it, pgx, pgy);
                const float gx2 = pgx + pr.x, gy2 = pgy + pr.y;
                if (cx >= gx2 && cx < gx2 + pr.w && cy >= gy2 && cy < gy2 + pr.h) {
                    outId = it->model->props.id;
                    return true;
                }
                continue;
            }
        }
        float gx = 0, gy = 0;
        globalOriginOf(*it, gx, gy);
        const SRect& lr = it->model->props.rect;
        if (cx >= gx && cx < gx + lr.w && cy >= gy && cy < gy + lr.h) {
            outId = it->model->props.id;
            return true;
        }
    }
    // 轮 2：顶层控件
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it) {
        if (!it->parentId.empty()) continue;
        if (!effectivelyVisible(*it)) continue;
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
    setSelection(id.empty() ? std::vector<std::string>{}
                            : std::vector<std::string>{ id });
}

// P2 多选：选择集统一入口（去重/过滤失效 id；末位=主选）——手柄/外框/面板/树/对齐启用同步。
void CanvasPane::setSelection(std::vector<std::string> ids) {
    commitPendingIdRename();   // 切换选中前先提交未完成的 id 编辑
    std::vector<std::string> norm;
    for (const auto& id : ids) {
        if (id.empty() || !findModel(id)) continue;
        if (std::find(norm.begin(), norm.end(), id) != norm.end()) continue;
        norm.push_back(id);
    }
    const bool benchWas = m_benchSelected;
    m_benchSelected = false;   // 控件选择/清空 → 画布取消选中（互斥）
    if (norm == m_selIds && !benchWas) return;
    m_selIds = std::move(norm);
    m_selectedId = m_selIds.empty() ? std::string() : m_selIds.back();

    PlacedControl* pc = m_selectedId.empty() ? nullptr : placedById(m_selectedId);
    // Tab 页 Panel：几何由 TabControl 管理——不建手柄（锁死在 Tab 内，禁拖动/缩放）
    if (pc && pc->ctl.IsValid() && !isSelectedTabPage()) ensureSelHandle(pc->ctl);
    else detachSelHandle();
    updateSelOutlines();
    if (m_alignCombo.IsValid())
        m_alignCombo.SetBool(PropertyNames::kEnabled, m_selIds.size() >= 2);
    fillPropertyPanel();
    syncTreeSelection();
    rebuildDynamicRows();
    refreshEventPanel();
    if (m_clickHandler) m_clickHandler(m_selectedId, 0, 0);
}

// 选中画布（Bench）——空白单击路径；与控件选择互斥（setSelection 清 bench 标记）。
void CanvasPane::selectBench() {
    commitPendingIdRename();   // 切换画布前先提交未完成的 id 编辑
    m_benchSelected = true;
    m_selIds.clear();
    m_selectedId.clear();
    detachSelHandle();
    updateSelOutlines();
    if (m_alignCombo.IsValid()) m_alignCombo.SetBool(PropertyNames::kEnabled, false);
    fillPropertyPanel();
    syncTreeSelection();
    rebuildDynamicRows();
    refreshEventPanel();
    if (m_clickHandler) m_clickHandler(std::string(), 0, 0);
}

// P2 多选：Ctrl+单击切换成员。
void CanvasPane::toggleSelection(const std::string& id) {
    std::vector<std::string> ids = m_selIds;
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it != ids.end()) ids.erase(it);
    else ids.push_back(id);
    setSelection(std::move(ids));
}

// P2 多选：次级选中外框（主选由 HandleControl 呈现）——橙 1px 描边，池化复用。
void CanvasPane::updateSelOutlines() {
    if (!m_viewport) return;
    const size_t need = m_selIds.size() > 1 ? m_selIds.size() - 1 : 0;
    while (m_selOutlines.size() < need) {
        Control p = m_viewport->CreatePanel(0, 0, 1, 1);
        if (!p.IsValid()) break;
        p.SetBool(PropertyNames::kAlwaysOnTop, 1);      // z 序置顶（后续新增控件不覆盖）
        p.SetBool(PropertyNames::kTransparent, 1);      // 仅描边
        p.SetBool(PropertyNames::kBorderVisible, 1);
        const UIColor kOutline{0xFF, 0x88, 0x00, 0xC0};   // 四态同色（hover 不落回默认深色）
        p.SetStateColor(PropertyNames::kBorder,
                        UIStateColor{kOutline, kOutline, kOutline, kOutline});
        p.SetBool(PropertyNames::kVisible, 0);
        m_selOutlines.push_back(p);
    }
    size_t k = 0;
    for (size_t i = 0; i + 1 < m_selIds.size(); ++i) {   // 末位=主选（不描边）
        PlacedControl* pc = placedById(m_selIds[i]);
        if (!pc || !pc->ctl.IsValid()) continue;
        float gx = 0, gy = 0;
        globalOriginOf(*pc, gx, gy);
        const SRect& r = pc->model->props.rect;
        Control& o = m_selOutlines[k++];
        o.SetRect(gx, gy, r.w, r.h);
        o.SetBool(PropertyNames::kVisible, 1);
    }
    for (size_t i = k; i < m_selOutlines.size(); ++i)
        m_selOutlines[i].SetBool(PropertyNames::kVisible, 0);
}

// P2 框选：起框——重建面板（新控件追加到视口末尾=置顶；容器后放置也不会盖住框）
// + 记录起点与并入模式。面板颜色/尺寸由 updateBand 首帧创建。
void CanvasPane::beginBand(float cx, float cy, bool additive) {
    if (m_bandRect.IsValid()) {
        m_bandRect.Destroy();
        m_bandRect = Control();
    }
    m_bandActive = true;
    m_bandAdditive = additive;
    m_bandSX = cx;
    m_bandSY = cy;
}

// P2 框选：拖拽中更新橡皮筋矩形（画布逻辑坐标；视口裁剪由渲染承担）。
void CanvasPane::updateBand(float cx, float cy) {
    if (!m_viewport) return;
    if (!m_bandRect.IsValid()) {
        m_bandRect = m_viewport->CreatePanel(0, 0, 1, 1);
        if (!m_bandRect.IsValid()) return;
        m_bandRect.SetBool(PropertyNames::kAlwaysOnTop, 1);   // 后续新增控件不覆盖
        // 四态同色（鼠标始终在框内 → hover 态；仅设 normal 会落回默认深色"黑底"）
        const UIColor kBandFill{0x4A, 0x90, 0xD9, 0x30};
        const UIColor kBandLine{0x4A, 0x90, 0xD9, 0xC0};
        m_bandRect.SetBool(PropertyNames::kTransparent, 0);   // 半透明填充可见
        m_bandRect.SetBool(PropertyNames::kBorderVisible, 1);
        m_bandRect.SetStateColor(PropertyNames::kBackground,
                                 UIStateColor{kBandFill, kBandFill, kBandFill, kBandFill});
        m_bandRect.SetStateColor(PropertyNames::kBorder,
                                 UIStateColor{kBandLine, kBandLine, kBandLine, kBandLine});
    }
    const float x0 = std::min(m_bandSX, cx), y0 = std::min(m_bandSY, cy);
    m_bandRect.SetRect(x0, y0, std::fabs(cx - m_bandSX), std::fabs(cy - m_bandSY));
    m_bandRect.SetBool(PropertyNames::kVisible, 1);
}

// P2 框选：松开判定——微小移动=空白点击（非并入清空/并入不变）；否则相交控件集
// （可见控件；祖先去重防组拖动双倍位移；Ctrl 启动并入当前选择）。
// 上下文：起点在容器内容区内部 → 仅选该容器后代（容器内框选子控件）；起点画布空白/
// 容器框架区 → 顶层语义（容器优先——框架区起框选容器本体）。
void CanvasPane::finalizeBand(float cx, float cy) {
    m_bandActive = false;
    if (m_bandRect.IsValid()) m_bandRect.SetBool(PropertyNames::kVisible, 0);
    const float x0 = std::min(m_bandSX, cx), y0 = std::min(m_bandSY, cy);
    const float x1 = std::max(m_bandSX, cx), y1 = std::max(m_bandSY, cy);
    const bool dragged = (x1 - x0) >= kBandMinDrag || (y1 - y0) >= kBandMinDrag;
    // 起点容器解析（框选上下文）：起点命中容器内容区（含后代命中→上溯最近容器）
    std::string bandRoot;
    {
        std::string hitId;
        if (hitTest(m_bandSX, m_bandSY, hitId)) {
            PlacedControl* pc = placedById(hitId);
            while (pc && !isContainerType(pc->model->props.type) && !pc->parentId.empty()) {
                hitId = pc->parentId;
                pc = placedById(hitId);
            }
            if (pc && isContainerType(pc->model->props.type)) {
                Control par;
                SRect cr;
                if (resolveContainer(hitId, par, cr) &&
                    m_bandSX >= cr.x && m_bandSX < cr.x + cr.w &&
                    m_bandSY >= cr.y && m_bandSY < cr.y + cr.h)
                    bandRoot = hitId;   // WinFrame 标题栏/框架命中不算（选容器本体）
            }
        }
    }
    if (!dragged) {
        if (m_bandAdditive) return;                  // Ctrl 空白点击：选择不变
        if (!bandRoot.empty()) selectItem(bandRoot); // 容器内容区单击 → 选中容器本体
        else selectBench();                          // 画布空白单击：选中画布（Bench）
        return;
    }
    auto isDescendantOf = [&](const std::string& id, const std::string& root) -> bool {
        PlacedControl* p = placedById(id);
        while (p && !p->parentId.empty()) {
            if (p->parentId == root) return true;
            p = placedById(p->parentId);
        }
        return false;
    };
    std::vector<std::string> hits;
    for (auto& it : m_items) {
        if (!it.ctl.IsValid()) continue;
        if (!effectivelyVisible(it)) continue;
        if (!bandRoot.empty() && !isDescendantOf(it.model->props.id, bandRoot)) continue;
        float gx = 0, gy = 0;
        globalOriginOf(it, gx, gy);
        const SRect& r = it.model->props.rect;
        if (gx < x1 && gx + r.w > x0 && gy < y1 && gy + r.h > y0)
            hits.push_back(it.model->props.id);
    }
    // 祖先去重：某成员的祖先也在命中集 → 去掉成员（保留容器，子控件随动）
    std::vector<std::string> top;
    for (const auto& id : hits) {
        bool hasSelAncestor = false;
        PlacedControl* p = placedById(id);
        while (p && !p->parentId.empty()) {
            p = placedById(p->parentId);
            if (!p) break;
            if (std::find(hits.begin(), hits.end(), p->model->props.id) != hits.end()) {
                hasSelAncestor = true;
                break;
            }
        }
        if (!hasSelAncestor) top.push_back(id);
    }
    if (m_bandAdditive) {
        std::vector<std::string> merged = m_selIds;
        for (const auto& id : top)
            if (std::find(merged.begin(), merged.end(), id) == merged.end())
                merged.push_back(id);
        setSelection(std::move(merged));
    } else {
        setSelection(std::move(top));
    }
}

// P2 多选：选择集全局包围盒（画布逻辑坐标）。
void CanvasPane::selectionGlobalBBox(float& x, float& y, float& w, float& h) {
    x = y = w = h = 0;
    bool first = true;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (const auto& id : m_selIds) {
        PlacedControl* pc = placedById(id);
        if (!pc || !pc->ctl.IsValid()) continue;
        float gx = 0, gy = 0;
        globalOriginOf(*pc, gx, gy);
        const SRect& r = pc->model->props.rect;
        if (first) { minX = gx; minY = gy; maxX = gx + r.w; maxY = gy + r.h; first = false; }
        else {
            if (gx < minX) minX = gx;
            if (gy < minY) minY = gy;
            if (gx + r.w > maxX) maxX = gx + r.w;
            if (gy + r.h > maxY) maxY = gy + r.h;
        }
    }
    if (!first) { x = minX; y = minY; w = maxX - minX; h = maxY - minY; }
}

// P0-61：锚定目标父级——容器子控件→容器；顶层→根（bench，经 Root() 暴露）。
Control CanvasPane::anchorParentOf(const PlacedControl& pc) {
    if (!pc.parentId.empty()) {
        PlacedControl* p = placedById(pc.parentId);
        return (p && p->ctl.IsValid()) ? p->ctl : Control();
    }
    return m_viewport ? m_viewport->Root() : Control();
}

// P0-59：引擎重排（布局切换/锚点应用/容器挂载）后同步全部模型 rect——
// 引擎会移动多个子控件（未锚定者塌左上等），设计器模型必须跟随，否则 hitTest 失配选不中。
void CanvasPane::syncModelsFromEngine() {
    for (auto& it : m_items) {
        if (!it.ctl.IsValid()) continue;
        const UIRect r = it.ctl.GetRect();
        it.model->props.rect = SRect{ r.x, r.y, r.w, r.h };
    }
    if (!m_selectedId.empty()) fillPropertyPanel();
    updateSelOutlines();
}

// P2 多选：控件父容器全局原点（含 WinFrame ClientPanel 内容偏移；顶层=0）。
void CanvasPane::parentGlobalOriginOf(const PlacedControl& pc, float& px, float& py) {
    px = 0; py = 0;
    if (pc.parentId.empty()) return;
    PlacedControl* par = placedById(pc.parentId);
    if (!par) return;
    globalOriginOf(*par, px, py);
    if (par->model->props.type == "win-frame" && par->ctl.IsValid()) {
        void* h = par->ctl.GetPtr("client-panel");
        if (h) {
            const auto cr = m_viewport->FromHandle(h).GetRect();
            px += cr.x; py += cr.y;
        }
    }
}

// P2 多选组拖动：快照其余成员起始局部 rect + 主选起始全局/局部（设计器路径用绝对；
// 引擎路径用 m_groupPrev 增量基准）。
void CanvasPane::beginGroupDrag(const std::string& primaryId) {
    m_groupDrag = false;
    m_groupEntries.clear();
    if (m_selIds.size() < 2) return;
    PlacedControl* pp = placedById(primaryId);
    if (!pp) return;
    float pgx = 0, pgy = 0;
    globalOriginOf(*pp, pgx, pgy);
    m_groupStartX = pgx;
    m_groupStartY = pgy;
    m_groupPrevX = pp->model->props.rect.x;
    m_groupPrevY = pp->model->props.rect.y;
    for (const auto& id : m_selIds) {
        if (id == primaryId) continue;
        PlacedControl* pc = placedById(id);
        if (!pc || !pc->ctl.IsValid()) continue;
        m_groupEntries.push_back({ id, pc->model->props.rect });
    }
    m_groupDrag = !m_groupEntries.empty();
}

// P2 多选组拖动：其余选中增量平移（父不动 → 局部增量 = 全局增量）+ 外框跟随。
void CanvasPane::applyGroupDelta(float dx, float dy) {
    if (!m_groupDrag) return;
    for (auto& e : m_groupEntries) {
        PlacedControl* pc = placedById(e.id);
        if (!pc || !pc->ctl.IsValid()) continue;
        SRect& r = pc->model->props.rect;
        r.x += dx; r.y += dy;
        pc->ctl.SetRect(r.x, r.y, r.w, r.h);
    }
    updateSelOutlines();
}

// P2 多选组拖动：设计器路径——按起始快照绝对落位（主选已由拖动路径写绝对位置）。
void CanvasPane::applyGroupFromStart(float dx, float dy) {
    if (!m_groupDrag) return;
    for (auto& e : m_groupEntries) {
        PlacedControl* pc = placedById(e.id);
        if (!pc || !pc->ctl.IsValid()) continue;
        SRect& r = pc->model->props.rect;
        r.x = e.startLocal.x + dx;
        r.y = e.startLocal.y + dy;
        pc->ctl.SetRect(r.x, r.y, r.w, r.h);
    }
    updateSelOutlines();
}

// P2 多选对齐：命令下拉接线（≥2 启用）。
void CanvasPane::SetAlignCommandControl(Control combo) {
    m_alignCombo = combo;
    if (m_alignCombo.IsValid())
        m_alignCombo.SetBool(PropertyNames::kEnabled, m_selIds.size() >= 2);
}

// P2 多选对齐/分布（0-5 对齐六模式、6 等宽、7 等高、8 水平分布、9 垂直分布）：
// 局部换算写回；手柄/外框/面板同步。
void CanvasPane::AlignSelection(int mode) {
    if (m_alignCombo.IsValid())
        m_alignCombo.SetInt(PropertyNames::kSelectedIndex, -1);   // 命令式：执行后清选中（下次任意项可触发）
    if (m_selIds.size() < 2 || mode < 0 || mode > 9) return;

    // 等宽/等高：以主选（最后选中）为基准，保左/上缘改宽/高
    if (mode == 6 || mode == 7) {
        const ControlModel* ref = findModel(m_selectedId);
        if (!ref) return;
        const float rw = ref->props.rect.w, rh = ref->props.rect.h;
        for (const auto& id : m_selIds) {
            PlacedControl* pc = placedById(id);
            if (!pc || !pc->ctl.IsValid()) continue;
            SRect& r = pc->model->props.rect;
            if (mode == 6) r.w = rw; else r.h = rh;
            pc->ctl.SetRect(r.x, r.y, r.w, r.h);
        }
        updateSelOutlines();
        fillPropertyPanel();
        commitUndo();   // F2.5：等宽/等高为独立撤销步
        return;
    }

    // 分布：按左/上缘排序等间距（首末不动；间隙可为负——重叠时仍等距）
    if (mode == 8 || mode == 9) {
        std::vector<PlacedControl*> sel;
        for (const auto& id : m_selIds) {
            PlacedControl* pc = placedById(id);
            if (pc && pc->ctl.IsValid()) sel.push_back(pc);
        }
        if (sel.size() < 2) return;
        const bool horizontal = (mode == 8);
        std::sort(sel.begin(), sel.end(), [&](PlacedControl* a, PlacedControl* b) {
            float ax = 0, ay = 0, bx2 = 0, by2 = 0;
            globalOriginOf(*a, ax, ay);
            globalOriginOf(*b, bx2, by2);
            return (horizontal ? ax : ay) < (horizontal ? bx2 : by2);
        });
        float fgx = 0, fgy = 0, lgx = 0, lgy = 0;
        globalOriginOf(*sel.front(), fgx, fgy);
        globalOriginOf(*sel.back(), lgx, lgy);
        const float firstSize = horizontal ? sel.front()->model->props.rect.w
                                           : sel.front()->model->props.rect.h;
        const float lastSize  = horizontal ? sel.back()->model->props.rect.w
                                           : sel.back()->model->props.rect.h;
        const float span  = horizontal ? (lgx + lastSize - fgx) : (lgy + lastSize - fgy);
        float total = 0.0f;
        for (auto* pc : sel)
            total += horizontal ? pc->model->props.rect.w : pc->model->props.rect.h;
        const float gap = (span - total) / (float)(sel.size() - 1);
        float cursor = horizontal ? fgx : fgy;
        for (auto* pc : sel) {
            float gx = 0, gy = 0;
            globalOriginOf(*pc, gx, gy);
            const float w = pc->model->props.rect.w, h = pc->model->props.rect.h;
            const float nx = horizontal ? cursor : gx;
            const float ny = horizontal ? gy : cursor;
            float px = 0, py = 0;
            parentGlobalOriginOf(*pc, px, py);
            pc->model->props.rect.x = nx - px;
            pc->model->props.rect.y = ny - py;
            pc->ctl.SetRect(nx - px, ny - py, w, h);
            cursor += (horizontal ? w : h) + gap;
        }
        updateSelOutlines();
        fillPropertyPanel();
        commitUndo();   // F2.5：分布为独立撤销步
        return;
    }

    // 六对齐：选择集全局包围盒
    float bx = 0, by = 0, bw = 0, bh = 0;
    selectionGlobalBBox(bx, by, bw, bh);
    for (const auto& id : m_selIds) {
        PlacedControl* pc = placedById(id);
        if (!pc || !pc->ctl.IsValid()) continue;
        float gx = 0, gy = 0;
        globalOriginOf(*pc, gx, gy);
        const float w = pc->model->props.rect.w, h = pc->model->props.rect.h;
        float nx = gx, ny = gy;
        switch (mode) {
        case 0: nx = bx; break;                          // 左对齐
        case 1: nx = bx + (bw - w) * 0.5f; break;        // 水平居中
        case 2: nx = bx + bw - w; break;                 // 右对齐
        case 3: ny = by; break;                          // 顶对齐
        case 4: ny = by + (bh - h) * 0.5f; break;        // 垂直居中
        default: ny = by + bh - h; break;                // 底对齐
        }
        float px = 0, py = 0;
        parentGlobalOriginOf(*pc, px, py);
        pc->model->props.rect.x = nx - px;
        pc->model->props.rect.y = ny - py;
        pc->ctl.SetRect(nx - px, ny - py, w, h);
    }
    updateSelOutlines();
    fillPropertyPanel();
    commitUndo();   // F2.5：对齐为独立撤销步
}

// 选中变化 → 属性面板回填（m_propSync 抑制回写）；未选中清零。
// P2 多选：X/Y/W/H=选择集包围盒（只读）；caption/色/可见/启用=共有值（编辑批量写全部）。
void CanvasPane::fillPropertyPanel() {
    if (!m_propX.IsValid()) return;
    m_propSync = true;
    if (m_propVisible.IsValid()) m_propVisible.SetBool(PropertyNames::kVisible, true);   // 画布选中曾隐藏 → 恢复
    if (m_propEnabled.IsValid()) m_propEnabled.SetBool(PropertyNames::kVisible, true);
    if (m_benchSelected) {
        // 画布（Bench）：X/Y 无意义（只读 0）；W/H = 设计画布尺寸（可编辑 → SetCanvasSize）；
        // caption/颜色/可见/启用 隐藏（画布行在动态区）
        m_propX.SetBool(PropertyNames::kEnabled, false);
        m_propY.SetBool(PropertyNames::kEnabled, false);
        m_propW.SetBool(PropertyNames::kEnabled, true);
        m_propH.SetBool(PropertyNames::kEnabled, true);
        m_propX.SetFloat(PropertyNames::kValue, 0);
        m_propY.SetFloat(PropertyNames::kValue, 0);
        m_propW.SetFloat(PropertyNames::kValue, m_designW);
        m_propH.SetFloat(PropertyNames::kValue, m_designH);
        if (m_propCaption.IsValid()) m_propCaption.SetBool(PropertyNames::kVisible, false);
        if (m_propColor.IsValid()) m_propColor.SetBool(PropertyNames::kVisible, false);
        if (m_propVisible.IsValid()) m_propVisible.SetBool(PropertyNames::kVisible, false);
        if (m_propEnabled.IsValid()) m_propEnabled.SetBool(PropertyNames::kVisible, false);
        m_propSync = false;
        return;
    }
    if (m_selIds.size() > 1) {
        float bx = 0, by = 0, bw = 0, bh = 0;
        selectionGlobalBBox(bx, by, bw, bh);
        m_propX.SetFloat(PropertyNames::kValue, bx);
        m_propY.SetFloat(PropertyNames::kValue, by);
        m_propW.SetFloat(PropertyNames::kValue, bw);
        m_propH.SetFloat(PropertyNames::kValue, bh);
        m_propX.SetBool(PropertyNames::kEnabled, false);   // 只读：批量移动经拖动/对齐
        m_propY.SetBool(PropertyNames::kEnabled, false);
        m_propW.SetBool(PropertyNames::kEnabled, false);
        m_propH.SetBool(PropertyNames::kEnabled, false);
        // caption：全部一致 → 显示；否则空（编辑写全部）
        std::string cap = readCaptionOf(m_selIds.front());
        for (size_t i = 1; i < m_selIds.size(); ++i)
            if (readCaptionOf(m_selIds[i]) != cap) { cap.clear(); break; }
        m_propCaption.SetString(PropertyNames::kTextContent, cap);
        // 颜色：全部一致 → 显示；否则缺省
        UIColor cc{0x4A, 0x90, 0xD9, 255};
        bool first = true;
        for (const auto& id : m_selIds) {
            const PlacedControl* pc = placedById(id);
            if (!pc || !pc->ctl.IsValid()) continue;
            const UIColor c = pc->ctl.GetColor(PropertyNames::kBackground);
            if (first) { cc = c; first = false; }
            else if (c.r != cc.r || c.g != cc.g || c.b != cc.b) { cc = UIColor{0x4A, 0x90, 0xD9, 255}; break; }
        }
        if (m_propColor.IsValid()) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", cc.r, cc.g, cc.b);
            m_propColor.SetString(PropertyNames::kColor, hex);
            m_lastColorHex = hex;
        }
        // 可见/启用：全部为真 → 勾选（编辑写全部）
        bool allVis = true, allEn = true;
        for (const auto& id : m_selIds) {
            const PlacedControl* pc = placedById(id);
            if (!pc || !pc->ctl.IsValid()) continue;
            if (!pc->ctl.GetBool(PropertyNames::kVisible)) allVis = false;
            if (!pc->ctl.GetBool(PropertyNames::kEnabled)) allEn = false;
        }
        m_propVisible.SetBool(PropertyNames::kChecked, allVis);
        m_propEnabled.SetBool(PropertyNames::kChecked, allEn);
        m_propSync = false;
        return;
    }
    m_propX.SetBool(PropertyNames::kEnabled, true);   // 单选：恢复可编辑
    m_propY.SetBool(PropertyNames::kEnabled, true);
    m_propW.SetBool(PropertyNames::kEnabled, true);
    m_propH.SetBool(PropertyNames::kEnabled, true);
    const ControlModel* m = findModel(m_selectedId);
    const PlacedControl* pc = m ? placedById(m->props.id) : nullptr;
    m_propX.SetFloat(PropertyNames::kValue, m ? m->props.rect.x : 0);
    m_propY.SetFloat(PropertyNames::kValue, m ? m->props.rect.y : 0);
    m_propW.SetFloat(PropertyNames::kValue, m ? m->props.rect.w : 0);
    m_propH.SetFloat(PropertyNames::kValue, m ? m->props.rect.h : 0);
    // caption 回填：check-box/win-frame 的文本在内部 Label（经句柄直控读）
    m_propCaption.SetString(PropertyNames::kTextContent, readCaptionOf(m_selectedId));
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
    commitUndo("rect:" + m->props.id);   // F2.5：X/Y/W/H NUD（0.7s 合并）
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
        const bool v = e.GetCheckState() != 0;
        for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
            PlacedControl* pc = placedById(id);
            if (!pc || !pc->ctl.IsValid()) continue;
            pc->ctl.SetBool(PropertyNames::kVisible, v);
            pc->model->props.visible = v;   // 模型同步（保存数据源）
        }
        commitUndo("b:visible");   // F2.5：可见性勾选
    });
    m_propEnabled.SetCallback(PropertyNames::kEventCheckChanged, [this](const Event& e) {
        if (m_propSync) return;
        const bool v = e.GetCheckState() != 0;
        for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
            PlacedControl* pc = placedById(id);
            if (!pc || !pc->ctl.IsValid()) continue;
            pc->ctl.SetBool(PropertyNames::kEnabled, v);
            pc->model->props.extended["enabled"] = v;   // 模型同步（保存数据源）
        }
        commitUndo("b:enabled");   // F2.5：启用勾选
    });
    // NUD 事件载荷为 Float（GetValueChanged）；lambda 按值捕获（不可悬空）
    auto bindRect = [this](Control nud, int which) {
        nud.SetCallback(PropertyNames::kEventValueChanged, [this, which](const Event& e) {
            if (m_propSync) return;
            const float v = e.GetValueChanged();
            if (m_benchSelected) {   // 画布：W/H 行 = 设计画布尺寸
                if (which == 2) SetCanvasSize(v, m_designH);
                else if (which == 3) SetCanvasSize(m_designW, v);
                return;
            }
            ControlModel* m = findModel(m_selectedId);
            if (!m) return;
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
        if (m_selIds.empty()) return;
        const std::string v = e.GetTextChanged();
        // 动态区文本行同步（先行——不依赖控件写路径，防回环）
        if (m_captionEditor.IsValid()) {
            m_propSync = true;
            m_captionEditor.SetString(PropertyNames::kTextContent, v);
            m_propSync = false;
        }
        for (const auto& id : m_selIds)   // P2 多选：批量写全部选中
            applyCaptionTo(id, v);
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
    if (m_selIds.empty()) return;
    const UIColor c{ r, g, b, 255 };
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    m_lastColorHex = hex;
    for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
        ControlModel* m = findModel(id);
        PlacedControl* pc = placedById(id);
        if (!m || !pc || !pc->ctl.IsValid()) continue;
        pc->ctl.SetColor(PropertyNames::kBackground, c);
        m->props.extended["color"] = hex;
    }
}

// P2 多选共有属性：caption 读取（check-box/win-frame 的文本在内部 Label，经句柄直控读）。
std::string CanvasPane::readCaptionOf(const std::string& id) {
    const ControlModel* m = findModel(id);
    if (!m) return "";
    std::string capVal = m->props.extended.value("caption", "");
    const PlacedControl* pc = placedById(id);
    if (pc && pc->ctl.IsValid() && m_ui) {
        const std::string& tp = m->props.type;
        if (tp == "check-box" || tp == "win-frame") {
            void* h = pc->ctl.GetPtr(tp == "win-frame"
                ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel);
            if (h) capVal = m_viewport->FromHandle(h).GetString(PropertyNames::kCaption);
        }
    }
    return capVal;
}

// P2 多选共有属性：caption 写入（类型特例：check-box/win-frame 内部 Label 直控）。
void CanvasPane::applyCaptionTo(const std::string& id, const std::string& v) {
    ControlModel* m = findModel(id);
    if (!m) return;
    m->props.extended["caption"] = v;
    PlacedControl* pc = placedById(id);
    if (!pc || !pc->ctl.IsValid()) return;
    const std::string& tp = m->props.type;
    if ((tp == "check-box" || tp == "win-frame") && m_ui) {
        void* h = pc->ctl.GetPtr(tp == "win-frame"
            ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel);
        if (h) {
            m_viewport->FromHandle(h).SetString(PropertyNames::kCaption, v);
            commitUndo("t:caption");   // F2.5：caption 编辑（0.7s 合并）
            return;
        }
    }
    pc->ctl.SetString(PropertyNames::kCaption, v);
    commitUndo("t:caption");   // F2.5：caption 编辑（0.7s 合并）
}

// P2 多选：颜色槽写回单个目标——text/text-shadow 组经内部 caption/title Label 直控
// （类型分派；caption 直控结果存 model.extended，CheckBox recreate 后拖动结束恢复用）。
void CanvasPane::writeColorSlotTo(const std::string& id, const std::string& prop,
                                  const std::string& grpName, const UIColor& c) {
    ControlModel* m = findModel(id);
    PlacedControl* pc = placedById(id);
    if (!m || !pc || !pc->ctl.IsValid()) return;
    char hex8[12];
    std::snprintf(hex8, sizeof(hex8), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    // 引擎兼容颜色形态：extended["colors"][组][态]（与布局 JSON 1:1——保存/导出零转换）
    const std::string state = (prop.size() > grpName.size() && prop[grpName.size()] == '.')
        ? prop.substr(grpName.size() + 1) : "normal";
    m->props.extended["colors"][grpName][state] = hex8;
    if (grpName == "text" || grpName == "text-shadow") {
        const char* capKey = (m->props.type == "win-frame")
            ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel;
        void* capH = pc->ctl.GetPtr(capKey);
        if (capH) {
            // 宿主级同步：caption 重建按宿主状态色重着色（Button::rebuildCaption 用
            // m_textColor）——仅设内部 Label 会被重建覆盖（撤销/重开后文字变灰）
            pc->ctl.SetColor(prop.c_str(), c);
            m_viewport->FromHandle(capH).SetColor(prop.c_str(), c);
            commitUndo("c:" + grpName);   // F2.5：颜色编辑（同组 0.7s 合并——多选批量=一步）
            return;
        }
    }
    pc->ctl.SetColor(prop.c_str(), c);
    commitUndo("c:" + grpName);   // F2.5：颜色编辑（同组 0.7s 合并——多选批量=一步）
}

// P2 多选：string 行写回单个目标——check-box/win-frame 内部 Label 直控；
// 路径类记录 extended（引擎只写不读的显示兜底）。
void CanvasPane::writeStringPropTo(const std::string& id, const std::string& prop,
                                   const std::string& v) {
    ControlModel* m = findModel(id);
    PlacedControl* pc = placedById(id);
    if (!m || !pc || !pc->ctl.IsValid()) return;
    const std::string& tp = m->props.type;
    m->props.extended[prop] = v;   // 模型同步（保存数据源——全部字符串键）
    if (tp == "check-box" || tp == "win-frame") {
        void* h = pc->ctl.GetPtr(tp == "win-frame"
            ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel);
        if (h) {
            m_viewport->FromHandle(h).SetString(PropertyNames::kCaption, v);
            commitUndo("t:" + prop);   // F2.5：文本编辑（同键 0.7s 合并——多选批量=一步）
            return;
        }
    }
    pc->ctl.SetString(prop.c_str(), v);
    commitUndo("t:" + prop);   // F2.5：文本编辑（同键 0.7s 合并——多选批量=一步）
}

static UIColor hexToColor(const std::string& hex);   // 定义见后（拖动恢复段）

// 结构化：行式文本 → 非空行清单（\r 忽略）。
static std::vector<std::string> splitNonEmptyLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 结构化：combo-box items 文本 → 模型（一行一项；跳过空行）+ 引擎同步
// （ClearItems + AddItem；引擎无 GetItem 读回——模型为数据源）。
void CanvasPane::applyComboItemsText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> labels = splitNonEmptyLines(text);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& l : labels) arr.push_back({ {"label", l}, {"value", l} });
    m.props.extended["items"] = std::move(arr);
    syncComboItems(m);
    commitUndo("struct:" + m.props.id);   // F2.5：combo items 编辑（0.7s 合并）
}

// 结构化：combo-box items 模型 → 引擎（ClearItems + AddItem；加载/打开路径复用）。
void CanvasPane::syncComboItems(ControlModel& m) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_ui) return;
    m_viewport->ComboBoxClearItems(pc->ctl);
    const nlohmann::json items = m.props.extended.value("items", nlohmann::json::array());
    if (!items.is_array()) return;
    for (const auto& it : items) {
        std::string lab, val;
        if (it.is_object()) { lab = it.value("label", ""); val = it.value("value", lab); }
        else if (it.is_string()) lab = val = it.get<std::string>();
        if (!lab.empty()) m_viewport->ComboBoxAddItem(pc->ctl, lab, val);
    }
}

static std::vector<std::string> splitUnescaped(const std::string& s, char sep);   // 定义见后（行式读取段）
static std::string fieldUnescape(const std::string& s, const char* special);
static std::string fieldEscape(const std::string& s, const char* special);

// 统一结构化 item 属性（各行式编辑器共用）：
// 背景色 → 字体色 → 字号 → 字体名 → 阴影色 → 偏移x → 偏移y
// （严格按序、空字段跳过、尾部可省；类型不符=忽略该字段）
struct StructItemAttrs {
    std::string bg, tc, fontName, sc;   // '#RRGGBB[AA]' / 字体枚举 token
    std::string hoverBg;                // P0-64：显式悬停背景色（hb# 前缀；仅悬停态）
    int fontSize = 0;                   // 0 = 继承
    float ox = 1.0f, oy = 1.0f;
    float radius = 0.0f;                // P0-72①：菜单面板阴影半径（r# 前缀）
    bool hasRadius = false;             // 显式给出（含 0——r#0 = 去掉阴影，须往返保留）
    bool empty() const {
        return bg.empty() && tc.empty() && fontName.empty() && sc.empty() &&
               hoverBg.empty() && fontSize == 0 && !hasRadius;
    }
};
// 引擎当前支持的字体 token（FontName 枚举 6 项；schema 枚举的超集暂不支持）。
static bool isSupportedFontToken(const std::string& t) {
    static const char* const kFonts[] = {
        "asul-bold", "harmonyos-sans-sc-regular", "harmonyos-sans-sc-thin",
        "maplemono-nf-cn-regular", "muyao-softbrush", "quando-regular",
    };
    for (const char* f : kFonts)
        if (_stricmp(t.c_str(), f) == 0) return true;
    return false;
}

// f = 按 '@' 拆分的字段序列；off = 属性起始下标（文本字段之后）。
// 解析：'#色' 按序填 背景→字体色→阴影色（空字段=跳过色位，封顶阴影槽）；
// 数字按序填 字号→偏移x→偏移y；其余字符串=字体名——类型容忍（误置槽位自动归位）。
static StructItemAttrs parseStructAttrs(const std::vector<std::string>& f, size_t off) {
    StructItemAttrs a;
    int cpos = 0, num = 0;
    for (size_t i = off; i < f.size(); ++i) {
        const std::string& s = f[i];
        if (s.empty()) {
            if (cpos < 2) ++cpos;
            continue;
        }
        if (s.size() >= 9 && s[0] == 'h' && s[1] == 'b' && s[2] == '#') {
            a.hoverBg = s.substr(2);   // 键前缀：悬停背景色（P0-64）
            continue;
        }
        if (s.size() >= 3 && s[0] == 'r' && s[1] == '#') {
            try { a.radius = std::stof(s.substr(2)); } catch (...) { a.radius = 0.0f; }   // 菜单面板阴影半径（P0-72①）
            if (a.radius < 0.0f) a.radius = 0.0f;
            a.hasRadius = true;   // r#0 显式（去掉阴影）——往返保留
            continue;
        }
        if (s.size() >= 8 && s[1] == '#' && (s[0] == 'b' || s[0] == 't' || s[0] == 's')) {
            const std::string c = s.substr(1);   // 键前缀色（消歧义，免占位）：b#背景 t#字体色 s#阴影
            if (s[0] == 'b') a.bg = c;
            else if (s[0] == 't') a.tc = c;
            else a.sc = c;
            continue;
        }
        if (s.size() >= 7 && s[0] == '#') {
            if (cpos == 0) a.bg = s;
            else if (cpos == 1) a.tc = s;
            else if (cpos == 2) a.sc = s;
            if (cpos < 3) ++cpos;
        } else if (std::isdigit((unsigned char)s[0]) ||
                   ((s[0] == '-' || s[0] == '+') && s.size() > 1 &&
                    std::isdigit((unsigned char)s[1]))) {
            if (num == 0) {
                try { a.fontSize = std::stoi(s); } catch (...) { a.fontSize = 0; }
                if (a.fontSize < 0) a.fontSize = 0;
                ++num;
            } else if (num == 1) {
                try { a.ox = std::stof(s); } catch (...) { a.ox = 1.0f; }
                ++num;
            } else if (num == 2) {
                try { a.oy = std::stof(s); } catch (...) { a.oy = 1.0f; }
                ++num;
            }
        } else if (a.fontName.empty()) {
            a.fontName = s;
        }
    }
    return a;
}
// 输出 item 属性到行式文本：色位仅在需要保持后续槽位时输出空占位；
// 阴影存在而字号未设 → 输出 "0" 占位（继承字号，保证偏移归位）。
static void appendStructAttrs(std::string& seg, const StructItemAttrs& a) {
    const bool hasSc = !a.sc.empty();
    std::vector<std::string> f;
    if (!a.bg.empty()) f.push_back(a.bg);
    else if (!a.tc.empty() || hasSc) f.push_back("");      // 占位：保持字体色槽位
    if (!a.tc.empty()) f.push_back(a.tc);
    else if (hasSc) f.push_back("");                      // 占位：保持阴影色槽位
    if (a.fontSize > 0) f.push_back(std::to_string(a.fontSize));
    else if (hasSc) f.push_back("0");                     // 占位：保持偏移数字槽位
    if (!a.fontName.empty()) f.push_back(a.fontName);
    if (hasSc) {
        f.push_back(a.sc);
        f.push_back(std::to_string((int)a.ox));
        f.push_back(std::to_string((int)a.oy));
    }
    for (const auto& v : f) { seg += "@"; seg += v; }
    if (!a.hoverBg.empty()) { seg += "@hb#"; seg += a.hoverBg; }   // 键前缀：悬停背景色
    if (a.hasRadius) {                                            // 键前缀：菜单面板阴影半径（P0-72①；0=显式去掉）
        seg += "@r#";
        seg += std::to_string((int)a.radius);
    }
}

// 结构化：status-bar items 文本 → 模型（一行一段"文本@属性..."；统一属性序）
// + 引擎同步（按模型旧 id 移除 + 重新添加 + P0-55 段色 + P0-58 段级字号/阴影
// + P0-63⑤ 段级字体名）。
void CanvasPane::applyStatusBarItemsText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    struct Seg { std::string text; StructItemAttrs a; };
    std::vector<Seg> segs;
    for (const auto& ln : lines) {
        const std::vector<std::string> p = splitUnescaped(ln, '@');
        Seg s;
        s.text = fieldUnescape(p.empty() ? std::string() : p[0], "@");
        s.a = parseStructAttrs(p, 1);
        segs.push_back(std::move(s));
    }
    const nlohmann::json oldItems = m.props.extended.value("items", nlohmann::json::array());
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < segs.size(); ++i) {
        const StructItemAttrs& a = segs[i].a;
        nlohmann::json e = { {"id", "stb_" + std::to_string(i)}, {"text", segs[i].text} };
        if (!a.bg.empty()) e["background-color"] = a.bg;
        if (!a.hoverBg.empty()) e["hover-background-color"] = a.hoverBg;
        if (!a.tc.empty()) e["text-color"] = a.tc;
        if (a.fontSize > 0) e["font-size"] = a.fontSize;
        if (!a.fontName.empty()) e["font-name"] = a.fontName;
        if (!a.sc.empty()) {
            e["text-shadow"] = a.sc;
            e["text-shadow-offset-x"] = a.ox;
            e["text-shadow-offset-y"] = a.oy;
        }
        arr.push_back(std::move(e));
    }
    m.props.extended["items"] = std::move(arr);
    syncStatusBarItems(m, oldItems);
    commitUndo("struct:" + m.props.id);   // F2.5：status-bar items 编辑（0.7s 合并）   // 按旧 id 移除（缩短列表不留残段）+ 重建
}

// 结构化：status-bar items 模型 → 引擎（按 oldItems 旧 id 移除 + 重新添加 + 段色/字号/
// 字体/阴影；加载/打开路径 oldItems 传空）。
void CanvasPane::syncStatusBarItems(ControlModel& m, const nlohmann::json& oldItems) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_ui) return;
    if (oldItems.is_array())
        for (const auto& it : oldItems)
            if (it.is_object() && it.contains("id"))
                m_viewport->StatusBarRemoveItem(pc->ctl, it.value("id", ""));
    const nlohmann::json items = m.props.extended.value("items", nlohmann::json::array());
    if (!items.is_array()) return;
    for (const auto& it : items) {
        if (!it.is_object()) continue;
        const std::string id = it.value("id", "");
        if (id.empty()) continue;
        m_viewport->StatusBarAddItem(pc->ctl, id, it.value("text", ""), false);
        const std::string tc = it.value("text-color", "");
        if (!tc.empty()) m_viewport->StatusBarSetItemTextColor(pc->ctl, id, hexToColor(tc), nullptr);
        const std::string bg = it.value("background-color", "");
        if (!bg.empty()) m_viewport->StatusBarSetItemBackgroundColor(pc->ctl, id, hexToColor(bg));
        const int fs = it.value("font-size", 0);
        if (fs > 0) m_viewport->StatusBarSetItemFontSize(pc->ctl, id, (float)fs);
        const std::string fn = it.value("font-name", "");
        if (!fn.empty() && isSupportedFontToken(fn))
            m_viewport->StatusBarSetItemFontName(pc->ctl, id, fn);
        const std::string sc = it.value("text-shadow", "");
        if (!sc.empty())
            m_viewport->StatusBarSetItemTextShadow(pc->ctl, id, hexToColor(sc),
                it.value("text-shadow-offset-x", 1.0f), it.value("text-shadow-offset-y", 1.0f));
        const std::string hbg = it.value("hover-background-color", "");
        if (!hbg.empty())
            m_viewport->StatusBarSetItemHoverBackgroundColor(pc->ctl, id, hexToColor(hbg));
    }
}

static UIColor hexToColor(const std::string& hex);   // 定义见后（拖动恢复段）

// 结构化：list-view columns 文本 → 模型（一行一列"标题@宽度@属性..."；宽度缺省 100；
// 统一属性序）+ 引擎同步（按旧数量从尾移除 + 逐列添加 + 表头样式/背景/阴影/字体）
// + 行重放（列变更清空行单元格）。
void CanvasPane::applyListColumnsText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    struct Col { std::string title; float width = 100.0f; StructItemAttrs a; };
    std::vector<Col> cols;
    for (const auto& ln : lines) {
        const std::vector<std::string> f = splitUnescaped(ln, '@');
        Col c;
        c.title = fieldUnescape(f.empty() ? std::string() : f[0], "@");
        if (f.size() > 1 && !f[1].empty()) {
            try { c.width = std::stof(f[1]); } catch (...) { c.width = 100.0f; }
        }
        if (c.width < 20.0f) c.width = 20.0f;
        c.a = parseStructAttrs(f, 2);
        cols.push_back(std::move(c));
    }
    const nlohmann::json oldCols = m.props.extended.value("columns", nlohmann::json::array());
    const int oldColCount = oldCols.is_array() ? (int)oldCols.size() : 0;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : cols) {
        nlohmann::json e = { {"title", c.title}, {"width", c.width} };
        if (!c.a.bg.empty()) e["header-background"] = c.a.bg;
        if (!c.a.tc.empty()) e["header-color"] = c.a.tc;
        if (c.a.fontSize > 0) e["header-size"] = c.a.fontSize;
        if (!c.a.fontName.empty()) e["header-font-name"] = c.a.fontName;
        if (!c.a.sc.empty()) {
            e["header-shadow"] = c.a.sc;
            e["header-shadow-offset-x"] = c.a.ox;
            e["header-shadow-offset-y"] = c.a.oy;
        }
        arr.push_back(std::move(e));
    }
    m.props.extended["columns"] = std::move(arr);
    syncListColumns(m, oldColCount);
    commitUndo("struct:" + m.props.id);   // F2.5：columns 编辑（0.7s 合并）   // 按引擎旧列数移除（缩短列表不留残列）+ 重建
}

// 结构化：list-view columns 模型 → 引擎（按 engineColCount 旧列数从尾移除 + 逐列添加 +
// 表头全样式 + 行重放；加载/打开路径 engineColCount=0）。
void CanvasPane::syncListColumns(ControlModel& m, int engineColCount) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_ui) return;
    for (int i = engineColCount - 1; i >= 0; --i) m_viewport->ListViewRemoveColumn(pc->ctl, i);
    const nlohmann::json cols = m.props.extended.value("columns", nlohmann::json::array());
    if (cols.is_array()) {
        for (size_t i = 0; i < cols.size(); ++i) {
            const auto& c = cols[i];
            if (!c.is_object()) continue;
            m_viewport->ListViewAddColumn(pc->ctl, c.value("title", ""), c.value("width", 100.0f));
            const std::string hc = c.value("header-color", "");
            const int hs = c.value("header-size", 0);
            if (!hc.empty() || hs > 0) {
                const UIColor h = hc.empty() ? UIColor{235, 235, 235, 255} : hexToColor(hc);
                m_viewport->ListViewSetColumnHeaderStyle(pc->ctl, (int)i, h, hs);
            }
            const std::string hb = c.value("header-background", "");
            if (!hb.empty())
                m_viewport->ListViewSetColumnHeaderBackground(pc->ctl, (int)i, hexToColor(hb));
            const std::string hsh = c.value("header-shadow", "");
            if (!hsh.empty())
                m_viewport->ListViewSetColumnHeaderShadow(pc->ctl, (int)i, hexToColor(hsh),
                    c.value("header-shadow-offset-x", 1.0f), c.value("header-shadow-offset-y", 1.0f));
            const std::string hfn = c.value("header-font-name", "");
            if (!hfn.empty() && isSupportedFontToken(hfn))
                m_viewport->ListViewSetColumnHeaderFontName(pc->ctl, (int)i, hfn);
        }
    }
    // 引擎列变更会清空既有行单元格（insertColumn 插空串）——按模型重放行内容/样式
    const nlohmann::json rows = m.props.extended.value("rows", nlohmann::json::array());
    syncListRows(m, rows.is_array() ? (int)rows.size() : 0);
}

static std::vector<std::string> splitUnescaped(const std::string& s, char sep);   // 定义见后（行式读取段）
static std::string fieldUnescape(const std::string& s, const char* special);

// 结构化：list-view rows 文本 → 模型（一行一记录，单元格 | 分隔；单元格"内容@属性..."，
// 统一属性序；单元格字体名引擎暂不支持——模型保留）+ 引擎重放。
void CanvasPane::applyListRowsText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < lines.size(); ++i) {
        nlohmann::json cells = nlohmann::json::array();
        for (const auto& seg : splitUnescaped(lines[i], '|')) {
            const std::vector<std::string> p = splitUnescaped(seg, '@');
            const std::string t = fieldUnescape(p.empty() ? std::string() : p[0], "|@");
            const StructItemAttrs a = parseStructAttrs(p, 1);
            if (a.empty()) {
                cells.push_back(t);
            } else {
                nlohmann::json e = { {"text", t} };
                if (!a.bg.empty()) e["background-color"] = a.bg;
                if (!a.hoverBg.empty()) e["hover-background-color"] = a.hoverBg;   // P0-64③
                if (!a.tc.empty()) e["text-color"] = a.tc;
                if (a.fontSize > 0) e["font-size"] = a.fontSize;
                if (!a.fontName.empty()) e["font-name"] = a.fontName;
                if (!a.sc.empty()) {
                    e["text-shadow"] = a.sc;
                    e["text-shadow-offset-x"] = a.ox;
                    e["text-shadow-offset-y"] = a.oy;
                }
                cells.push_back(std::move(e));
            }
        }
        arr.push_back({ {"id", "row_" + std::to_string(i)}, {"cells", cells} });
    }
    const nlohmann::json old = m.props.extended.value("rows", nlohmann::json::array());
    const int oldCount = old.is_array() ? (int)old.size() : 0;
    m.props.extended["rows"] = std::move(arr);
    syncListRows(m, oldCount);
    commitUndo("struct:" + m.props.id);   // F2.5：rows 编辑（0.7s 合并）
}

// 结构化：list-view rows 模型 → 引擎重放（RemoveRow 从尾 + AddRow + 单元格样式/字体；
// 列变更会清空引擎行单元格——applyListColumnsText 后亦调用本函数重放）。
void CanvasPane::syncListRows(ControlModel& m, int engineRowCount) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_ui) return;
    const nlohmann::json rows = m.props.extended.value("rows", nlohmann::json::array());
    if (!rows.is_array()) return;
    for (int i = engineRowCount - 1; i >= 0; --i) m_viewport->ListViewRemoveRow(pc->ctl, i);
    for (const auto& r : rows) {
        if (!r.is_object()) continue;
        std::vector<std::string> texts;
        const auto& cells = r.value("cells", nlohmann::json::array());
        if (cells.is_array())
            for (const auto& c : cells)
                texts.push_back(c.is_string() ? c.get<std::string>()
                                              : (c.is_object() ? c.value("text", "") : std::string()));
        m_viewport->ListViewAddRow(pc->ctl, r.value("id", ""), texts);
    }
    for (int ri = 0; ri < (int)rows.size(); ++ri) {
        const auto& r = rows[(size_t)ri];
        if (!r.is_object()) continue;
        const auto& cells = r.value("cells", nlohmann::json::array());
        if (!cells.is_array()) continue;
        for (int ci = 0; ci < (int)cells.size(); ++ci) {
            const auto& c = cells[(size_t)ci];
            if (!c.is_object()) continue;
            const std::string bg = c.value("background-color", "");
            const std::string tc = c.value("text-color", "");
            const std::string sc = c.value("text-shadow", "");
            const int fs = c.value("font-size", 0);
            if (!bg.empty() || fs > 0)   // 仅字号 → 背景透明占位（alpha 0 不绘制）
                m_viewport->ListViewSetCellStyle(pc->ctl, ri, ci,
                    bg.empty() ? UIColor{0, 0, 0, 0} : hexToColor(bg), fs);
            const std::string fname = c.value("font-name", "");
            if (!fname.empty() && isSupportedFontToken(fname))
                m_viewport->ListViewSetCellFontName(pc->ctl, ri, ci, fname);
            const std::string hbg = c.value("hover-background-color", "");
            if (!hbg.empty())   // P0-64③：单元格显式悬停背景（可无常态背景——P0-65① hasBg 门槛）
                m_viewport->ListViewSetCellHoverBackgroundColor(pc->ctl, ri, ci, hexToColor(hbg));
            if (!tc.empty()) m_viewport->ListViewSetCellTextColor(pc->ctl, ri, ci, hexToColor(tc));
            if (!sc.empty())
                m_viewport->ListViewSetCellShadow(pc->ctl, ri, ci, hexToColor(sc),
                    c.value("text-shadow-offset-x", 1.0f), c.value("text-shadow-offset-y", 1.0f));
        }
    }
}

static std::string treeLabelEscape(const std::string& s);     // 定义见后（行式读取段）
static std::string treeLabelUnescape(const std::string& s);
static std::vector<std::string> splitUnescaped(const std::string& s, char sep);
static std::string fieldUnescape(const std::string& s, const char* special);

// 结构化：tree-view items 文本 → 模型（层级 = 行首 '>' 数量，1 个/层；行首空格保留为
// 标签内容；"标签@属性..."统一属性序；标签首 '>'、'@'、'\' 经转义；空行忽略）
// + 引擎同步（ClearItems + AddNode 重建；id 自动生成 tn_i；父索引恒小于子——父先建）。
void CanvasPane::applyTreeItemsText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    struct Node {
        std::string id; std::string label; int parent = -1;
        StructItemAttrs a;
    };
    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> stack;   // (depth, node index)
    for (const auto& ln : lines) {
        size_t mark = 0;
        while (mark < ln.size() && ln[mark] == '>') ++mark;   // 层级标记（转义 '\>' 不计）
        const int depth = (int)mark;
        while (!stack.empty() && stack.back().first >= depth) stack.pop_back();
        const std::vector<std::string> f = splitUnescaped(ln.substr(mark), '@');
        Node n;
        n.id = "tn_" + std::to_string(nodes.size() + 1);
        n.label = treeLabelUnescape(f.empty() ? std::string() : f[0]);   // 行首空格保留；转义还原
        n.a = parseStructAttrs(f, 1);
        n.parent = stack.empty() ? -1 : stack.back().second;
        nodes.push_back(std::move(n));
        stack.push_back({ depth, (int)nodes.size() - 1 });
    }
    // 模型：嵌套 JSON（反向挂接——子先入父）
    std::vector<nlohmann::json> jn(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const StructItemAttrs& a = nodes[i].a;
        jn[i] = { {"id", nodes[i].id}, {"label", nodes[i].label} };
        if (!a.bg.empty()) jn[i]["background-color"] = a.bg;
        if (!a.hoverBg.empty()) jn[i]["hover-background-color"] = a.hoverBg;   // P0-64③
        if (!a.tc.empty()) jn[i]["text-color"] = a.tc;
        if (a.fontSize > 0) jn[i]["font-size"] = a.fontSize;
        if (!a.fontName.empty()) jn[i]["font-name"] = a.fontName;
        if (!a.sc.empty()) {
            jn[i]["text-shadow"] = a.sc;
            jn[i]["text-shadow-offset-x"] = a.ox;
            jn[i]["text-shadow-offset-y"] = a.oy;
        }
    }
    for (int i = (int)nodes.size() - 1; i >= 0; --i) {
        if (nodes[i].parent < 0) continue;
        auto& ch = jn[(size_t)nodes[i].parent]["children"];
        if (ch.is_null()) ch = nlohmann::json::array();
        ch.insert(ch.begin(), jn[i]);   // 反序遍历（子先于父完成）+ 头插 = 兄弟顺序与输入一致
    }
    nlohmann::json roots = nlohmann::json::array();
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent < 0) roots.push_back(jn[i]);
    m.props.extended["items"] = std::move(roots);
    syncTreeItems(m);
    commitUndo("struct:" + m.props.id);   // F2.5：tree items 编辑（0.7s 合并）
}

// 结构化：tree-view items 模型 → 引擎（ClearItems + 先序 AddNode + 节点样式/字体；
// 加载/打开路径复用）。
void CanvasPane::syncTreeItems(ControlModel& m) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_ui) return;
    const nlohmann::json items = m.props.extended.value("items", nlohmann::json::array());
    if (!items.is_array()) return;
    m_viewport->TreeViewClearItems(pc->ctl);
    std::function<void(const nlohmann::json&, const std::string&)> walk =
        [&](const nlohmann::json& arr, const std::string& pid) {
        if (!arr.is_array()) return;
        for (const auto& it : arr) {
            if (!it.is_object()) continue;
            const std::string id = it.value("id", "");
            if (id.empty()) continue;
            m_viewport->TreeViewAddNode(pc->ctl, pid, id, it.value("label", ""), true);
            const std::string tc = it.value("text-color", "");
            if (!tc.empty()) m_viewport->TreeViewSetNodeTextColor(pc->ctl, id, hexToColor(tc), nullptr);
            const std::string bg = it.value("background-color", "");
            if (!bg.empty()) m_viewport->TreeViewSetNodeBackgroundColor(pc->ctl, id, hexToColor(bg));
            const std::string hbg = it.value("hover-background-color", "");
            if (!hbg.empty())
                m_viewport->TreeViewSetNodeHoverBackgroundColor(pc->ctl, id, hexToColor(hbg));
            const std::string sc = it.value("text-shadow", "");
            if (!sc.empty())
                m_viewport->TreeViewSetNodeShadow(pc->ctl, id, hexToColor(sc),
                    it.value("text-shadow-offset-x", 1.0f), it.value("text-shadow-offset-y", 1.0f));
            // 逐节点字体：名称走专用 API（size<=0 继承）；仅字号走通用链
            const int fs = it.value("font-size", 0);
            const std::string fn = it.value("font-name", "");
            const bool wantFont = !fn.empty() && isSupportedFontToken(fn);
            if (wantFont) {
                m_viewport->TreeViewSetNodeFont(pc->ctl, id, fn, fs);
            } else if (fs > 0) {
                pc->ctl.SetString(PropertyNames::kTreeItemId, id);
                pc->ctl.SetInt(PropertyNames::kTreeItemFontSize, fs);
            }
            if (it.contains("children")) walk(it["children"], id);
        }
    };
    walk(items, "");
}

// 放置/记账用：解析目标容器在模型中的实际父 id（TabControl → 当前页 Panel id；
// 其余容器=自身）。序列化按 parentId 组树——TabControl 记账必须落到页 Panel。
// id 是否 Tab 页 Panel（页几何由 TabControl 引擎侧管理——禁手柄/禁拖动/禁反向同步）
bool CanvasPane::isTabPage(const std::string& id) {
    PlacedControl* pgp = placedById(id);
    PlacedControl* par = (pgp && !pgp->parentId.empty()) ? placedById(pgp->parentId) : nullptr;
    return par && par->model->props.type == "tab-control";
}

bool CanvasPane::isSelectedTabPage() {
    return isTabPage(m_selectedId);
}

// 页序调整（用户需求：拖动页签排序的内核）。引擎仅支持追加——以"池保活"往返实现
// 任意位置插入：摘除 [lo..end)（TabRemovePage 句柄保活）→ 按新序 TabAddPage+TabSetPage
// 重挂；模型 arr 同步重排 + 树标签重排号 + 当前页跟随。
bool CanvasPane::moveTabPage(const std::string& tabsId, int from, int to) {
    ControlModel* m = findModel(tabsId);
    PlacedControl* pc = placedById(tabsId);
    if (!m || !pc || !pc->ctl.IsValid() || !m_viewport) return false;
    if (!m->props.extended.contains("tabs") || !m->props.extended["tabs"].is_array()) return false;
    nlohmann::json& arr = m->props.extended["tabs"];
    const int n = (int)arr.size();
    if (from < 0 || to < 0 || from >= n || to >= n || from == to) return false;
    const int curIdx = pc->ctl.GetInt(PropertyNames::kTabCurrentIndex);
    const std::string curPageId = (curIdx >= 0 && curIdx < n)
        ? arr[(size_t)curIdx].value("page-id", std::string()) : std::string();
    // 引擎迁移（P0-91③：内部 erase+insert——页 shared_ptr 原样、不经池）
    if (!m_viewport->TabMovePage(pc->ctl, from, to)) return false;
    // 模型 arr 同序重排 + 树标签重排号 + 当前页跟随（按 id 重定位）
    nlohmann::json moved = arr[(size_t)from];
    arr.erase(arr.begin() + from);
    arr.insert(arr.begin() + to, moved);
    for (int i = 0; i < n; ++i) {
        const std::string pid = arr[(size_t)i].value("page-id", "");
        const std::string title = arr[(size_t)i].value("title", "");
        syncTreeRename(pid, u8"页 " + std::to_string(i + 1) + u8"·" + title);
        if (!curPageId.empty() && pid == curPageId)
            pc->ctl.SetInt(PropertyNames::kTabCurrentIndex, i);
    }
    commitUndo("struct:" + tabsId);
    return true;
}

// P0-91①：页签项命中（TabGetItemRect；Tab 局部逻辑坐标 → 画布坐标）。
// clampToNearest=true（拖放目标位）：未命中按最近项中心就近钳位（全在右侧 → 0）；
// false（点击判定）：未命中返回 -1（空白页签——不得钳位误判为项）。
int CanvasPane::tabItemIndexAt(const std::string& tabId, float cx, float cy, bool clampToNearest) {
    PlacedControl* pc = placedById(tabId);
    if (!pc || !pc->ctl.IsValid() || !m_viewport) return -1;
    const nlohmann::json tabs = pc->model->props.extended.value("tabs", nlohmann::json::array());
    const int n = tabs.is_array() ? (int)tabs.size() : 0;
    if (n <= 0) return -1;
    float tx0 = 0, ty0 = 0;
    globalOriginOf(*pc, tx0, ty0);
    int before = -1;
    for (int i = 0; i < n; ++i) {
        UIRect r{};
        if (!m_viewport->TabGetItemRect(pc->ctl, i, r)) continue;
        const float gx = tx0 + r.x, gy = ty0 + r.y;
        if (cx >= gx && cx < gx + r.w && cy >= gy && cy < gy + r.h) return i;
        if (clampToNearest && gx + r.w * 0.5f < cx) before = i;
    }
    if (!clampToNearest) return -1;
    return before >= 0 ? before : 0;
}

// P0-90：Tab 页签配色写入（tab-* 单色键直通；extended 字符串持久化，与导出/导入同形）
void CanvasPane::writeTabColorTo(const std::string& id, const std::string& key, const UIColor& c) {
    ControlModel* m = findModel(id);
    PlacedControl* pc = placedById(id);
    if (!m || !pc || !pc->ctl.IsValid()) return;
    char hex8[12];
    std::snprintf(hex8, sizeof(hex8), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    m->props.extended[key] = hex8;
    pc->ctl.SetColor(key.c_str(), c);
    commitUndo("c:" + key);
}

// 树选中页 Panel（父为 TabControl）→ 同步切换当前页：所见即所编/所放（用户反馈）
void CanvasPane::syncTabCurrentToPage(const std::string& pageId) {
    const PlacedControl* pc = placedById(pageId);
    PlacedControl* par = pc ? placedById(pc->parentId) : nullptr;
    if (!par || par->model->props.type != "tab-control" || !par->ctl.IsValid()) return;
    const nlohmann::json tabs = par->model->props.extended.value("tabs", nlohmann::json::array());
    if (!tabs.is_array()) return;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (tabs[i].value("page-id", "") == pageId) {
            par->ctl.SetInt(PropertyNames::kTabCurrentIndex, (int)i);
            return;
        }
}

std::string CanvasPane::resolvedParentIdOf(const std::string& targetId) {
    const PlacedControl* pc = placedById(targetId);
    if (!pc || pc->model->props.type != "tab-control") return targetId;
    const int cur = pc->ctl.IsValid() ? pc->ctl.GetInt(PropertyNames::kTabCurrentIndex) : -1;
    const nlohmann::json tabs = pc->model->props.extended.value("tabs", nlohmann::json::array());
    if (!tabs.is_array() || cur < 0 || cur >= (int)tabs.size()) return targetId;
    const std::string pageId = tabs[(size_t)cur].value("page-id", "");
    return pageId.empty() ? targetId : pageId;
}

// 销毁放置控件子树（含后代）：引擎控件销毁 + 模型条目移除 + 树节点移除（无选择语义；
// 页 Panel 移除/清空等内部流程用）。选中项落在子树内 → 取消选中。
void CanvasPane::removePlacedSubtree(const std::string& rootId) {
    std::vector<std::string> toRemove;
    std::function<void(const std::string&)> collect = [&](const std::string& pid) {
        if (std::find(toRemove.begin(), toRemove.end(), pid) != toRemove.end()) return;
        toRemove.push_back(pid);
        for (auto& it : m_items)
            if (it.parentId == pid) collect(it.model->props.id);
    };
    collect(rootId);
    const bool selHit = std::find(toRemove.begin(), toRemove.end(), m_selectedId) != toRemove.end();
    for (auto rit = toRemove.rbegin(); rit != toRemove.rend(); ++rit) {
        if (PlacedControl* pc = placedById(*rit))
            if (pc->ctl.IsValid()) pc->ctl.Destroy();
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
            if (it->model->props.id == *rit) { m_items.erase(it); break; }
        syncTreeRemove(*rit);
    }
    if (selHit) selectItem("");
    refreshStatusRight();
}

// 结构化：tab-control tabs 文本 → 模型（一行一页签标题；空行忽略；页 id 按索引保留）
// + 引擎同步（尾删 + 逐页标题/新增——页 Panel 设计器自管）。
// tabs 文本行 ↔ 旧页配对：①标题精确匹配（保内容：插入/删除/改序）；
// ②等行数且未命中 → 按序兜底（就地改标题）；③无匹配 → 新页（无 id）。
// 未匹配的旧页 = 被删（deletedOut）。原实现仅按序配对：删除中间/首行时页 id 整体错位
// → 引擎尾删误删其它页、被删页控件残留（用户实测）。
nlohmann::json CanvasPane::pairTabsWithOld(const nlohmann::json& oldTabs,
                                           const std::vector<std::string>& lines,
                                           std::vector<std::string>& deletedOut) {
    const int oldCount = oldTabs.is_array() ? (int)oldTabs.size() : 0;
    nlohmann::json arr = nlohmann::json::array();
    std::vector<bool> used((size_t)oldCount, false);
    for (size_t i = 0; i < lines.size(); ++i) {
        nlohmann::json e = { { "title", lines[i] } };
        std::string pid;
        if (oldCount > 0) {
            for (int j = 0; j < oldCount; ++j)   // ①标题匹配
                if (!used[(size_t)j] && oldTabs[(size_t)j].is_object() &&
                    oldTabs[(size_t)j].value("title", "") == lines[i]) {
                    pid = oldTabs[(size_t)j].value("page-id", "");
                    used[(size_t)j] = true;
                    break;
                }
            if (pid.empty() && (int)lines.size() == oldCount && i < (size_t)oldCount &&
                oldTabs[i].is_object() && !used[i]) {   // ②等行数按序兜底（就地改标题）
                pid = oldTabs[i].value("page-id", "");
                used[i] = true;
            }
        }
        if (!pid.empty()) e["page-id"] = pid;
        arr.push_back(std::move(e));
    }
    for (int j = 0; j < oldCount; ++j)
        if (!used[(size_t)j])
            deletedOut.push_back(oldTabs[(size_t)j].value("page-id", std::string()));
    return arr;
}

// 删除页确认弹窗（懒建复用）：确定 → 放行一次重入 applyTabsText；取消 → 行文本按模型还原。
void CanvasPane::showTabDeleteConfirm(const std::string& tabsId, const std::string& pendingText,
                                      const std::string& msg) {
    if (!m_ui) return;
    if (!m_tabDeleteDlg.IsValid()) {
        m_tabDeleteDlg = m_ui->CreateDialog(u8"确定", u8"取消", 0, 0, 420, 150);
        if (!m_tabDeleteDlg.IsValid()) return;
        auto lb = m_ui->CreateLabel(u8"", 16.0f, 16, 20, 388, 70);
        m_tabDeleteDlg.AddChild(lb);
        m_tabDeleteLabel = lb;
        m_tabDeleteDlg.SetCallback(PropertyNames::kEventClose, [this](const Event& e) {
            const bool ok = (e.GetCloseResult() == 1);
            const std::string id = m_pendingTabsId;
            const std::string text = m_pendingTabsText;
            m_pendingTabsId.clear();
            m_pendingTabsText.clear();
            if (ok && !id.empty()) {
                if (ControlModel* mm = findModel(id)) {
                    m_tabDeleteConfirmed = true;
                    applyTabsText(*mm, text);
                    m_tabDeleteConfirmed = false;
                }
            } else {
                rebuildDynamicRows();   // 取消：行文本还原（以模型为准）
            }
        });
        m_tabDeleteDlg.SetBool(PropertyNames::kVisible, 0);
    }
    if (m_tabDeleteLabel.IsValid())
        m_tabDeleteLabel.SetString(PropertyNames::kCaption, msg);
    m_pendingTabsId = tabsId;
    m_pendingTabsText = pendingText;
    m_tabDeleteDlg.SetBool(PropertyNames::kVisible, 1);
}

void CanvasPane::applyTabsText(ControlModel& m, const std::string& text) {
    std::fflush(stdout);
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    const nlohmann::json oldTabs = m.props.extended.value("tabs", nlohmann::json::array());
    std::vector<std::string> deleted;
    nlohmann::json arr = pairTabsWithOld(oldTabs, lines, deleted);
    // 被删页含控件 → 弹窗确认（取消则行文本还原；确认经 m_tabDeleteConfirmed 放行一次）
    if (!m_tabDeleteConfirmed) {
        std::string msg;
        for (const auto& d : deleted) {
            if (d.empty()) continue;
            int kids = 0;
            for (const auto& it : m_items)
                if (it.parentId == d) ++kids;
            if (kids <= 0) continue;
            std::string title = d;
            if (oldTabs.is_array())
                for (const auto& o : oldTabs)
                    if (o.is_object() && o.value("page-id", "") == d) {
                        title = o.value("title", d);
                        break;
                    }
            if (msg.empty()) msg = u8"以下页含控件，确认删除？ ";
            else msg += u8"、";
            msg += title;
            msg += u8"（" + std::to_string(kids) + u8" 个控件）";
        }
        if (!msg.empty()) {
            showTabDeleteConfirm(m.props.id, text, msg);
            return;   // 暂缓应用（确认/取消回调处理）
        }
    }
    m.props.extended["tabs"] = std::move(arr);
    syncTabs(m, oldTabs);
    commitUndo("struct:" + m.props.id);   // F2.5：tabs 编辑（0.7s 合并）
}

// 结构化：tab-control tabs 模型 → 引擎（尾删多余页（引擎页 + 页 Panel 子树）+ 逐页
// 标题/新增页；页 Panel 自管（与 TabControl 同实例））。加载路径 oldTabs 传空。
void CanvasPane::syncTabs(ControlModel& m, const nlohmann::json& oldTabs) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_viewport) return;
    // m_items 会在新增页时 push_back 扩容——PlacedControl* 会悬垂，句柄/rect 先取副本
    Control tabCtl = pc->ctl;   // 非 const（Binding Tab* 方法签名 Control&）
    const SRect tabRect = pc->model->props.rect;
    const nlohmann::json tabs = m.props.extended.value("tabs", nlohmann::json::array());
    const int oldCount = oldTabs.is_array() ? (int)oldTabs.size() : 0;
    const int newCount = tabs.is_array() ? (int)tabs.size() : 0;
    auto keptInNew = [&](const std::string& pid) {
        if (pid.empty() || !tabs.is_array()) return false;
        for (const auto& t : tabs)
            if (t.is_object() && t.value("page-id", "") == pid) return true;
        return false;
    };
    // 删除：旧页 page-id 未出现在新 arr → 按旧索引**降序** TabRemovePage + 子树移除
    // （按 id 判定支持删除任意行；降序保持低索引稳定——原实现仅尾删，删中间/首行误删尾页）
    for (int i = oldCount - 1; i >= 0; --i) {
        const std::string pageId = oldTabs[(size_t)i].value("page-id", "");
        if (keptInNew(pageId)) continue;
        m_viewport->TabRemovePage(tabCtl, i);
        if (!pageId.empty()) removePlacedSubtree(pageId);
    }
    // 缺句柄的页：创建页 Panel（加载路径页已由 walk 预建 → 跳过；引擎挂载统一在下方）
    for (int i = 0; i < newCount; ++i) {
        const std::string title = tabs[(size_t)i].value("title", "");
        std::string pageId = tabs[(size_t)i].value("page-id", "");
        PlacedControl* pagePc = pageId.empty() ? nullptr : placedById(pageId);
        if (pagePc && pagePc->ctl.IsValid()) continue;
        auto pageModel = std::make_unique<ControlModel>();
        pageModel->props.type = "panel";
        pageModel->props.id = "tabpage_" + std::to_string(++m_seq);
        pageModel->props.extended = nlohmann::json::object();   // 恒对象（value() 安全）
        const SRect r = tabRect;
        pageModel->props.rect = { 0, 0, r.w, r.h };
        Control pageCtl = createViewportControl("tPanel", 0, 0, r.w, r.h, *pageModel);
        if (!pageCtl.IsValid()) continue;
        pageId = pageModel->props.id;
        m_items.push_back({ std::move(pageModel), pageCtl, m.props.id });
        m_viewport->SetControlId(pageCtl, pageId);
        syncTreeAdd(m.props.id, pageId, u8"页 " + std::to_string(i + 1) + u8"·" + title);
        m.props.extended["tabs"][(size_t)i]["page-id"] = pageId;
    }
    // 引擎挂载（引擎仅支持追加，序 = 保留旧页旧序 + 其余按 arr 序追加）：
    // ① 保留旧页：按旧序 TabSetPage/Title；② 旧 arr 中不存在的页（新页/加载预建页）：
    // TabAddPage 追加 + TabSetPage/Title（**加载路径必须走此分支**——否则页成孤儿 → 崩溃）
    auto inOldTabs = [&](const std::string& pid) {
        if (pid.empty() || !oldTabs.is_array()) return false;
        for (const auto& o : oldTabs)
            if (o.is_object() && o.value("page-id", "") == pid) return true;
        return false;
    };
    auto titleOfPageId = [&](const std::string& pid) -> std::string {
        if (tabs.is_array())
            for (const auto& t : tabs)
                if (t.is_object() && t.value("page-id", "") == pid) return t.value("title", "");
        return std::string();
    };
    int enginePos = 0;
    for (int j = 0; j < oldCount; ++j) {
        const std::string pid = oldTabs[(size_t)j].value("page-id", "");
        if (!keptInNew(pid)) continue;
        PlacedControl* pagePc = placedById(pid);
        if (!pagePc || !pagePc->ctl.IsValid()) continue;
        const std::string title = titleOfPageId(pid);
        m_viewport->TabSetPage(tabCtl, enginePos, pagePc->ctl);
        m_viewport->TabSetTitle(tabCtl, enginePos, title);
        syncTreeRename(pid, u8"页 " + std::to_string(enginePos + 1) + u8"·" + title);
        ++enginePos;
    }
    // 注意：上方创建循环会把新 page-id 回写模型——此处须重读（本地 tabs 为旧副本）
    const nlohmann::json tabsNow = m.props.extended.value("tabs", nlohmann::json::array());
    for (int i = 0; i < newCount; ++i) {
        const std::string pid = tabsNow.is_array() && i < (int)tabsNow.size()
            ? tabsNow[(size_t)i].value("page-id", "") : std::string();
        if (pid.empty() || inOldTabs(pid)) continue;
        PlacedControl* pagePc = placedById(pid);
        if (!pagePc || !pagePc->ctl.IsValid()) continue;
        const std::string title = tabsNow[(size_t)i].value("title", "");
        m_viewport->TabAddPage(tabCtl, title);
        m_viewport->TabSetPage(tabCtl, enginePos, pagePc->ctl);
        m_viewport->TabSetTitle(tabCtl, enginePos, title);
        syncTreeRename(pid, u8"页 " + std::to_string(enginePos + 1) + u8"·" + title);
        ++enginePos;
    }
    // 页模型 rect 同步（引擎 applyCurrentPage 设内容区 rect——模型记账须一致：
    // 否则新建页模型=整 Tab 大小，hitTest 覆盖页签带 → 点页签被误判为点页）
    for (int i = 0; i < newCount; ++i) {
        const std::string pid = tabsNow.is_array() && i < (int)tabsNow.size()
            ? tabsNow[(size_t)i].value("page-id", "") : std::string();
        PlacedControl* pg = pid.empty() ? nullptr : placedById(pid);
        if (!pg || !pg->ctl.IsValid()) continue;
        const UIRect pr = pg->ctl.GetRect();
        pg->model->props.rect = SRect{ pr.x, pr.y, pr.w, pr.h };
    }
}

// 菜单标签转义（'\'→'\\'、'@'→'\@'；行首 '>' → '\>'）。
static std::string menuLabelEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '@') out += "\\@";
        else out += c;
    }
    if (!out.empty() && out[0] == '>') out.insert(out.begin(), '\\');
    return out;
}

// 结构化：menu-bar menus 文本 → 模型（菜单行无 '>'；项行 '>'×层级；'--'=分隔符；
// 项"标题@快捷键"；转义 '\'） + 引擎同步（旧菜单按 caption 移除后重建——P0-69①）。
void CanvasPane::applyMenusText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    struct Node {
        bool menu = false;          // true=顶层菜单；false=菜单项
        bool separator = false;
        std::string caption, shortcut;
        StructItemAttrs a;          // 面板级属性（仅菜单行：字号/字体名/阴影半径）
        int depth = 0, parent = -1;
    };
    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> stack;   // (depth, node index)
    for (const auto& ln : lines) {
        size_t mark = 0;
        while (mark < ln.size() && ln[mark] == '>') ++mark;
        const std::string rest = ln.substr(mark);
        Node n;
        n.depth = (int)mark;
        if (mark == 0) {
            n.menu = true;   // 顶层菜单行（面板级属性：@字号@字体名...@r#半径——统一属性序）
            const std::vector<std::string> f = splitUnescaped(rest, '@');
            n.caption = fieldUnescape(f.empty() ? std::string() : f[0], ">@");
            n.a = parseStructAttrs(f, 1);
        } else if (rest == "--") {
            n.separator = true;
        } else {
            const std::vector<std::string> f = splitUnescaped(rest, '@');   // 首个未转义 @ 分标题/快捷键
            n.caption = fieldUnescape(f.empty() ? std::string() : f[0], ">@");
            if (f.size() > 1) n.shortcut = f[1];
        }
        while (!stack.empty() && stack.back().first >= n.depth) stack.pop_back();
        n.parent = stack.empty() ? -1 : stack.back().second;
        nodes.push_back(std::move(n));
        stack.push_back({ (int)mark, (int)nodes.size() - 1 });
    }
    // 模型：嵌套 JSON（反序挂接 + 头插保持顺序——同 tree 修复口径）
    std::vector<nlohmann::json> jn(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].menu) {
            jn[i] = { { "caption", nodes[i].caption }, { "items", nlohmann::json::array() } };
            const StructItemAttrs& a = nodes[i].a;   // 面板级属性（P0-72①）
            if (a.fontSize > 0 || !a.fontName.empty()) {
                nlohmann::json fo = nlohmann::json::object();
                if (a.fontSize > 0) fo["size"] = a.fontSize;
                if (!a.fontName.empty()) fo["name"] = a.fontName;
                jn[i]["font"] = std::move(fo);
            }
            if (a.hasRadius) jn[i]["shadow-radius"] = a.radius;   // 0 = 显式去掉阴影（引擎语义待定，P0-77）
        } else if (nodes[i].separator) {
            jn[i] = { { "type", "separator" } };
        } else {
            jn[i] = { { "caption", nodes[i].caption } };
            if (!nodes[i].shortcut.empty()) jn[i]["shortcut"] = nodes[i].shortcut;
        }
    }
    for (int i = (int)nodes.size() - 1; i >= 0; --i) {
        if (nodes[i].parent < 0) continue;
        auto& items = jn[(size_t)nodes[i].parent]["items"];
        if (items.is_null()) items = nlohmann::json::array();
        items.insert(items.begin(), jn[i]);
    }
    nlohmann::json menus = nlohmann::json::array();
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent < 0 && nodes[i].menu) menus.push_back(jn[i]);
    const nlohmann::json oldMenus = m.props.extended.value("menus", nlohmann::json::array());
    m.props.extended["menus"] = std::move(menus);
    syncMenus(m, oldMenus);
    commitUndo("struct:" + m.props.id);   // F2.5：menus 编辑（0.7s 合并）
}

// 结构化：menu-bar menus 模型 → 引擎（按 oldMenus 旧 caption 移除 + 逐菜单重建 panel；
// 加载/打开路径 oldMenus 传空）。
void CanvasPane::syncMenus(ControlModel& m, const nlohmann::json& oldMenus) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_viewport) return;
    // 菜单面板/项必须创建在**画布视口实例**（与 MenuBar 同实例）——弹出定位基于
    // 面板自身 BENCH（attachMenuPanel：bench = 面板实例 bench + MenuBar drawRect 坐标域），
    // 创建在主实例会落到主 bench → 弹层位置错乱。
    if (oldMenus.is_array())
        for (const auto& om : oldMenus)
            if (om.is_object()) m_viewport->MenuBarRemoveMenu(pc->ctl, om.value("caption", ""));
    const nlohmann::json menus = m.props.extended.value("menus", nlohmann::json::array());
    if (!menus.is_array()) return;
    std::function<void(Control, const nlohmann::json&)> fill =
        [&](Control panel, const nlohmann::json& items) {
        if (!items.is_array()) return;
        for (const auto& it : items) {
            if (!it.is_object()) continue;
            if (it.value("type", "") == "separator") {
                m_viewport->MenuPanelAddSeparator(panel);
                continue;
            }
            Control mi = m_viewport->CreateMenuItem(it.value("caption", ""), 0);
            if (!mi.IsValid()) continue;
            const std::string sc = it.value("shortcut", "");
            if (!sc.empty()) mi.SetString(PropertyNames::kShortcut, sc);
            m_viewport->MenuPanelAddItem(panel, mi);
            if (it.contains("items") && it["items"].is_array()) {
                Control sub = m_viewport->CreateMenuPanel(1.0f, 1.0f);
                if (sub.IsValid()) {
                    fill(sub, it["items"]);
                    m_viewport->MenuItemSetSubMenu(mi, sub);
                }
            }
        }
    };
    for (const auto& mn : menus) {
        if (!mn.is_object()) continue;
        Control panel = m_viewport->CreateMenuPanel(1.0f, 1.0f);
        if (!panel.IsValid()) continue;
        fill(panel, mn.value("items", nlohmann::json::array()));
        // P0-72③ 已交付（引擎 addMenu 自动回放 bar 状态）——原设计器手动回放过渡已删
        m_viewport->MenuBarAddMenu(pc->ctl, mn.value("caption", ""), panel);
        // 每菜单属性在回放之后应用（P0-72①：menus[i].font/shadow-radius——与引擎
        // JSON 解析同序，避免被 bar 值覆盖）
        if (mn.contains("font") && mn["font"].is_object()) {
            if (mn["font"].contains("size") && mn["font"]["size"].is_number())
                panel.SetInt(PropertyNames::kFontSize, mn["font"]["size"].get<int>());
            if (mn["font"].contains("name") && mn["font"]["name"].is_string())
                panel.SetString(PropertyNames::kFont, mn["font"]["name"].get<std::string>());
        }
        if (mn.contains("shadow-radius") && mn["shadow-radius"].is_number())
            panel.SetFloat(PropertyNames::kShadowRadius, mn["shadow-radius"].get<float>());
    }
}

// 结构化：shape points 文本 → 模型（一行一点"x,y"；空行忽略）+ 引擎同步
// （ShapeSetPoints 全量替换——无需移除 API）。
void CanvasPane::applyShapePointsText(ControlModel& m, const std::string& text) {
    const std::vector<std::string> lines = splitNonEmptyLines(text);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& ln : lines) {
        const size_t comma = ln.find(',');
        if (comma == std::string::npos) continue;
        float x = 0, y = 0;
        try { x = std::stof(ln.substr(0, comma)); } catch (...) { continue; }
        try { y = std::stof(ln.substr(comma + 1)); } catch (...) { continue; }
        arr.push_back({ { "x", x }, { "y", y } });
    }
    m.props.extended["points"] = std::move(arr);
    syncShapePoints(m);
    commitUndo("struct:" + m.props.id);   // F2.5：shape points 编辑（0.7s 合并）
}

// 结构化：shape points 模型 → 引擎（全量替换）。
void CanvasPane::syncShapePoints(ControlModel& m) {
    PlacedControl* pc = placedById(m.props.id);
    if (!pc || !pc->ctl.IsValid() || !m_ui) return;
    const nlohmann::json pts = m.props.extended.value("points", nlohmann::json::array());
    std::vector<std::pair<float, float>> out;
    if (pts.is_array())
        for (const auto& p : pts)
            if (p.is_object() && p.contains("x") && p.contains("y"))
                out.push_back({ p.value("x", 0.0f), p.value("y", 0.0f) });
    m_viewport->ShapeSetPoints(pc->ctl, out);
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
        rebuildDynamicRows();
    }
}

// 结构化：combo-box items 模型 → 行式文本（一行一项）。
static std::string comboItemsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    if (ex.contains("items") && ex["items"].is_array()) {
        for (const auto& it : ex["items"]) {
            std::string lab;
            if (it.is_object()) lab = it.value("label", "");
            else if (it.is_string()) lab = it.get<std::string>();
            if (lab.empty()) continue;
            if (!out.empty()) out += "\n";
            out += lab;
        }
    }
    return out;
}

// 结构化：status-bar items 模型 → 行式文本（一行一段"文本@属性..."；统一属性序）。
static std::string statusBarItemsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    if (ex.contains("items") && ex["items"].is_array()) {
        for (const auto& it : ex["items"]) {
            std::string t;
            if (it.is_object()) t = it.value("text", "");
            else if (it.is_string()) t = it.get<std::string>();
            if (t.empty()) continue;
            StructItemAttrs a;
            if (it.is_object()) {
                a.bg = it.value("background-color", "");
                a.hoverBg = it.value("hover-background-color", "");
                a.tc = it.value("text-color", "");
                a.fontSize = it.value("font-size", 0);
                a.fontName = it.value("font-name", "");
                a.sc = it.value("text-shadow", "");
                a.ox = it.value("text-shadow-offset-x", 1.0f);
                a.oy = it.value("text-shadow-offset-y", 1.0f);
            }
            std::string line = fieldEscape(t, "@");
            appendStructAttrs(line, a);
            if (!out.empty()) out += "\n";
            out += line;
        }
    }
    return out;
}

// 结构化：字段文本转义（'\' 与 special 字符前加 '\'）——与属性/单元格分隔消歧。
static std::string fieldEscape(const std::string& s, const char* special) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\\' || strchr(special, c) != nullptr) out += '\\';
        out += c;
    }
    return out;
}

// 结构化：list-view columns 模型 → 行式文本（一行一列"标题@宽度@属性..."；统一属性序）。
static std::string listColumnsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    if (ex.contains("columns") && ex["columns"].is_array()) {
        for (const auto& it : ex["columns"]) {
            if (!it.is_object()) continue;
            StructItemAttrs a;
            a.bg = it.value("header-background", "");
            a.tc = it.value("header-color", "");
            a.fontSize = it.value("header-size", 0);
            a.fontName = it.value("header-font-name", "");
            a.sc = it.value("header-shadow", "");
            a.ox = it.value("header-shadow-offset-x", 1.0f);
            a.oy = it.value("header-shadow-offset-y", 1.0f);
            std::string line = fieldEscape(it.value("title", ""), "@");
            line += "@" + std::to_string((int)it.value("width", 100.0f));
            appendStructAttrs(line, a);
            if (!out.empty()) out += "\n";
            out += line;
        }
    }
    return out;
}

// P0-59：锚定 14 档（与 schema common.anchor enum 一致——含 fill 四边拉伸）
static const char* const kAnchorValues[14] = {
    "top-left", "top-center", "top-right", "mid-left", "center", "mid-right",
    "bottom-left", "bottom-center", "bottom-right",
    "top-stretch", "bottom-stretch", "left-stretch", "right-stretch", "fill",
};
static const char* const kAnchorLabels[14] = {
    u8"左上", u8"上中", u8"右上", u8"左中", u8"居中", u8"右中",
    u8"左下", u8"下中", u8"右下", u8"顶拉伸", u8"底拉伸", u8"左拉伸", u8"右拉伸",
    u8"填充（四边）",
};
// P0-59：布局模式 5 档
static const char* const kLayoutValues[5] = { "absolute", "h-flow", "v-flow", "anchor", "grid" };
static const char* const kLayoutLabels[5] = { u8"绝对", "h-flow", "v-flow", "anchor", "grid" };

// 结构化：list-view rows 模型 → 行式文本（一行一记录，单元格 | 分隔；
// 单元格"内容@属性..."统一属性序）。
static std::string listRowsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    if (ex.contains("rows") && ex["rows"].is_array()) {
        for (const auto& it : ex["rows"]) {
            if (!it.is_object()) continue;
            std::string line;
            if (it.contains("cells") && it["cells"].is_array())
                for (const auto& c : it["cells"]) {
                    std::string text;
                    StructItemAttrs a;
                    if (c.is_string()) {
                        text = c.get<std::string>();
                    } else if (c.is_object()) {
                        text = c.value("text", "");
                        a.bg = c.value("background-color", "");
                        a.hoverBg = c.value("hover-background-color", "");
                        a.tc = c.value("text-color", "");
                        a.fontSize = c.value("font-size", 0);
                        a.fontName = c.value("font-name", "");
                        a.sc = c.value("text-shadow", "");
                        a.ox = c.value("text-shadow-offset-x", 1.0f);
                        a.oy = c.value("text-shadow-offset-y", 1.0f);
                    }
                    std::string seg = fieldEscape(text, "|@");
                    appendStructAttrs(seg, a);
                    if (!line.empty()) line += "|";
                    line += seg;
                }
            if (!out.empty()) out += "\n";
            out += line;
        }
    }
    return out;
}

// 结构化：按未转义分隔符拆分（'\x' 序列整体保留——交由 fieldUnescape 还原）。
static std::vector<std::string> splitUnescaped(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[i];
            cur += s[i + 1];
            ++i;
        } else if (s[i] == sep) {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    out.push_back(std::move(cur));
    return out;
}
// 结构化：转义序列还原（'\\' 与 '\'+special → 原字符；其余 '\x' 保留——用户原始反斜杠不丢失）。
static std::string fieldUnescape(const std::string& s, const char* special) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() &&
            (s[i + 1] == '\\' || strchr(special, s[i + 1]) != nullptr)) {
            out += s[i + 1];
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// 结构化：tree 标签转义（'\' 与 '@' 前加 '\'；标签首 '>' → '\>'）——层级标记/属性消歧。
static std::string treeLabelEscape(const std::string& s) {
    std::string out = fieldEscape(s, "@");
    if (!out.empty() && out[0] == '>') out.insert(out.begin(), '\\');
    return out;
}
static std::string treeLabelUnescape(const std::string& s) { return fieldUnescape(s, ">@"); }

// 结构化：tree-view items 模型 → 行式文本（层级 = 行首 '>' 数量，1 个/层；空格归标签；
// "标签@属性..."统一属性序）。
static std::string treeItemsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    std::function<void(const nlohmann::json&, int)> walk = [&](const nlohmann::json& arr, int depth) {
        if (!arr.is_array()) return;
        for (const auto& it : arr) {
            if (!it.is_object()) continue;
            if (!out.empty()) out += "\n";
            out.append((size_t)depth, '>');
            out += treeLabelEscape(it.value("label", ""));
            StructItemAttrs a;
            a.bg = it.value("background-color", "");
            a.hoverBg = it.value("hover-background-color", "");
            a.tc = it.value("text-color", "");
            a.fontSize = it.value("font-size", 0);
            a.fontName = it.value("font-name", "");
            a.sc = it.value("text-shadow", "");
            a.ox = it.value("text-shadow-offset-x", 1.0f);
            a.oy = it.value("text-shadow-offset-y", 1.0f);
            appendStructAttrs(out, a);
            if (it.contains("children")) walk(it["children"], depth + 1);
        }
    };
    if (ex.contains("items")) walk(ex["items"], 0);
    return out;
}

// 结构化：menu-bar menus 模型 → 行式文本（菜单行无 '>'；项 '>'×层级；'--' 分隔；
// 项"标题@快捷键"；标签转义）。
static std::string menusText(const ControlModel& m) {
    std::string out;
    std::function<void(const nlohmann::json&, int)> walk = [&](const nlohmann::json& items, int depth) {
        if (!items.is_array()) return;
        for (const auto& it : items) {
            if (!it.is_object()) continue;
            if (!out.empty()) out += "\n";
            out.append((size_t)depth, '>');
            if (it.value("type", "") == "separator") {
                out += "--";
            } else {
                out += menuLabelEscape(it.value("caption", ""));
                const std::string sc = it.value("shortcut", "");
                if (!sc.empty()) out += "@" + sc;
            }
            if (it.contains("items")) walk(it["items"], depth + 1);
        }
    };
    const auto& ex = m.props.extended;
    if (ex.contains("menus") && ex["menus"].is_array())
        for (const auto& mn : ex["menus"]) {
            if (!mn.is_object()) continue;
            if (!out.empty()) out += "\n";
            std::string line = menuLabelEscape(mn.value("caption", ""));
            StructItemAttrs a;   // 面板级属性（P0-72①：字号/字体名/阴影半径）
            if (mn.contains("font") && mn["font"].is_object()) {
                if (mn["font"].contains("size") && mn["font"]["size"].is_number())
                    a.fontSize = mn["font"]["size"].get<int>();
                if (mn["font"].contains("name") && mn["font"]["name"].is_string())
                    a.fontName = mn["font"]["name"].get<std::string>();
            }
            if (mn.contains("shadow-radius") && mn["shadow-radius"].is_number()) {
                a.radius = mn["shadow-radius"].get<float>();
                a.hasRadius = true;
            }
            appendStructAttrs(line, a);
            out += line;
            walk(mn.value("items", nlohmann::json::array()), 1);
        }
    return out;
}

// 结构化：tab-control tabs 模型 → 行式文本（一行一页签标题）。
static std::string tabsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    if (ex.contains("tabs") && ex["tabs"].is_array())
        for (const auto& t : ex["tabs"]) {
            if (!t.is_object()) continue;
            if (!out.empty()) out += "\n";
            out += t.value("title", "");
        }
    return out;
}

// 结构化：shape points 模型 → 行式文本（一行一点"x,y"）。
static std::string shapePointsText(const ControlModel& m) {
    std::string out;
    const auto& ex = m.props.extended;
    if (ex.contains("points") && ex["points"].is_array()) {
        for (const auto& p : ex["points"]) {
            if (!p.is_object()) continue;
            char b[48];
            std::snprintf(b, sizeof(b), "%g,%g", p.value("x", 0.0f), p.value("y", 0.0f));
            if (!out.empty()) out += "\n";
            out += b;
        }
    }
    return out;
}

// 结构化：按类型/键分发行式读取（combo/status/tree items、list columns/rows、shape points）。
static std::string structRowText(const ControlModel& m, const std::string& key) {
    if (m.props.type == "list-view")
        return key == "rows" ? listRowsText(m) : listColumnsText(m);
    if (m.props.type == "status-bar") return statusBarItemsText(m);
    if (m.props.type == "tree-view") return treeItemsText(m);
    if (m.props.type == "shape" && key == "points") return shapePointsText(m);
    if (m.props.type == "menu-bar" && key == "menus") return menusText(m);
    if (m.props.type == "tab-control" && key == "tabs") return tabsText(m);
    return comboItemsText(m);
}

// P2 多选：类型 def 的键集（merged properties + colors@伪键）——共有属性交集过滤用。
static std::set<std::string> schemaKeySet(const nlohmann::json& defs, const std::string& type) {
    std::set<std::string> keys;
    if (!defs.contains(type)) return keys;
    const auto& d = defs[type];
    if (!d.contains("properties")) return keys;
    nlohmann::json merged = d["properties"];
    const auto& common = defs["common"]["properties"];
    for (auto it = common.begin(); it != common.end(); ++it)
        if (!merged.contains(it.key())) merged[it.key()] = it.value();
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        if (it.key() == "colors") {
            for (const auto& [grpName, states] : resolveColorGroups(defs, d))
                keys.insert("colors@" + grpName);
        } else {
            keys.insert(it.key());
        }
    }
    return keys;
}

// 动态行池：同种类行跨类型复用（隐藏不销毁——内存换性能；仅首次创建走引擎 CreateX）。
// 首次某 kind → create 创建并入容器/入库；再次 → 取出复用（可见置真），调用方随后
// 重设 rect/样式/回调（回调覆盖注册；闭包按当前选中重绑）。未用行由 hideUnusedPoolRows 隐藏。
Control CanvasPane::takePoolRow(const std::string& kind, const std::function<Control()>& create) {
    auto it = m_rowPool.find(kind);
    if (it != m_rowPool.end() && it->second.IsValid()) {
        m_rowPoolUsed.insert(kind);
        it->second.SetBool(PropertyNames::kVisible, true);
        if (m_ui && !m_ui->DebugValidateHandle(it->second))   // P0-84c：Release 归属自检
            std::printf("[CanvasPane] row handle NOT owned by main instance: %s\n", kind.c_str());
        return it->second;
    }
    Control c = create();
    if (c.IsValid()) {
        m_propDynamic.AddChild(c);   // 仅首次创建入容器
        if (m_ui && !m_ui->DebugValidateHandle(c))   // P0-84c：Release 归属自检
            std::printf("[CanvasPane] row handle NOT owned by main instance (new): %s\n", kind.c_str());
        m_rowPool[kind] = c;
        m_rowPoolUsed.insert(kind);
    }
    return c;
}

// 本趟未使用的池行隐藏（常驻保留——下次同类直接复用）
void CanvasPane::hideUnusedPoolRows() {
    for (auto& [kind, c] : m_rowPool)
        if (m_rowPoolUsed.count(kind) == 0 && c.IsValid())
            c.SetBool(PropertyNames::kVisible, false);
}

// 选中控件 → 按 schema 类型生成动态属性行（label 行 + 编辑控件行）。
// 支持 string(EditBox)/number·integer(NUD)/boolean(CheckBox)；嵌套对象、枚举、
// 布局资源类（rect/actors/luotiAni…）首版跳过。
// 布局自管（absolute）：Panel::addControl 不触发 v-flow reflow（仅 Bench 会），
// 初始叠放根因——行 y 手动累加；底部 ScrollBar 做"窗口滚动"（超界行隐藏，
// 等效裁剪——引擎 panel 无裁剪能力）。
// 行池复用：切换控件不销毁行——未用行隐藏、复用行重排+刷值（仅首次创建慢）。
// 画布设置对话框（P0-84 绕过）：对话框输入通道已证可用（同资源弹窗）；引擎修复
// 画布行内联输入后本对话框可保留为便捷入口（不影响）。
void CanvasPane::ShowBenchSettingsDialog() {
    if (!m_ui) return;
    if (!m_benchDlg.IsValid()) {
        m_benchDlg = m_ui->CreateDialog(u8"确定", u8"取消", 0, 0, 380, 190);
        if (!m_benchDlg.IsValid()) return;
        auto lbI = m_ui->CreateLabel(u8"画布 id", 16.0f, 20, 24, 70, 26);
        m_benchDlgId = m_ui->CreateEditBox(100, 24, 250, 26);
        auto lbT = m_ui->CreateLabel(u8"窗口标题", 16.0f, 20, 64, 70, 26);
        m_benchDlgTitle = m_ui->CreateEditBox(100, 64, 250, 26);
        m_benchDlg.AddChild(lbI);
        m_benchDlg.AddChild(m_benchDlgId);
        m_benchDlg.AddChild(lbT);
        m_benchDlg.AddChild(m_benchDlgTitle);
        m_benchDlg.SetCallback(PropertyNames::kEventClose, [this](const Event& e) {
            if (e.GetCloseResult() != 1) return;   // 取消
            const std::string rawId = m_benchDlgId.IsValid()
                ? m_benchDlgId.GetString(PropertyNames::kTextContent) : "";
            const std::string title = m_benchDlgTitle.IsValid()
                ? m_benchDlgTitle.GetString(PropertyNames::kTextContent) : "";
            std::string id;
            for (char c : rawId)
                if (!std::isspace((unsigned char)c)) id += c;   // 空白（含换行）全移除
            bool idOk = true;
            if (id == "@bench") idOk = false;
            for (auto& it : m_items)
                if (it.model && it.model->props.id == id) idOk = false;
            if (!id.empty() && !idOk) {
                setStatusHint(u8"画布 id 未修改（空白/重复/保留键）：" + id);
            } else if (!id.empty() && id != m_benchId) {
                m_benchId = id;
                syncTreeRename("@bench", benchTreeNodeLabel());
                commitUndo("bench-id");   // F2.5：对话框 id 编辑
                setStatusHint(u8"画布 id 已改为：" + id);
            }
            if (title != m_benchTitle) {
                m_benchTitle = title;
                commitUndo("bench-title");   // F2.5：对话框标题编辑
            }
            setStatusHint(u8"画布属性已更新（id / 标题）");
            rebuildDynamicRows();   // 行显示同步
        });
        m_benchDlg.SetBool(PropertyNames::kVisible, 0);   // 首开即关（复用）
    }
    m_benchDlgId.SetString(PropertyNames::kTextContent, m_benchId);
    m_benchDlgTitle.SetString(PropertyNames::kTextContent, m_benchTitle);
    m_benchDlg.SetBool(PropertyNames::kVisible, 1);
}

// 画布（Bench）属性动态行：类型 / id（代码引用）/ 窗口标题 / 缩放模式 / 背景色
// （alpha 支持——00 透明：不填充透出底层，游戏场景 UI 叠加）。行池前缀 "bench:"。
void CanvasPane::buildBenchRows() {
    const UIRect box = m_propDynamic.GetRect();
    const float viewH = box.h, barW = kPropBarW;
    if (!m_dynScroll.IsValid()) {
        m_dynScroll = m_ui->CreateScrollBar(box.w - barW, 0.0f, barW, viewH, 0);
        m_dynScroll.SetFloat(PropertyNames::kRangeMin, 0.0f);
        m_dynScroll.SetFloat(PropertyNames::kPageSize, viewH);
        m_dynScroll.SetFloat(PropertyNames::kStepSize, kPropCheckH);
        m_dynScroll.SetCallback(PropertyNames::kEventPositionChanged,
                                [this](const Event& e) { applyDynScroll(e.GetValueChanged()); });
        m_propDynamic.AddChild(m_dynScroll);
    }
    m_dynScroll.SetBool(PropertyNames::kVisible, true);
    m_schemaTarget = Control();
    m_schemaType.clear();
    const float rowW = box.w - kPropRowInset;
    const float lblFont = kPropLblFont, lblH = kPropLblH, ctlH = kPropCtlH,
          gapLbl = kPropGapLbl, rowGap = kPropRowGap;
    float y = kPropInsetX;
    int rows = 0;
    std::string genKeys;
    auto makeLabel = [&](const std::string& label) {
        Control lb = takePoolRow("bench:lbl:" + label, [&] {
            return m_ui->CreateLabel(label, lblFont, 0, 0, rowW, lblH);
        });
        lb.SetColor(PropertyNames::kText, UIColor{0x90, 0x90, 0x90, 255});
        lb.SetRect(kPropInsetX, y, rowW, lblH);
        m_dynamicRows.push_back(lb);
        m_dynamicRowY.push_back(y);
        y += lblH + gapLbl;
    };
    // 快路径：行集已建 → 仅刷值（同控件行池口径）
    if (m_rowsBuiltSig == "bench" && !m_rowRefreshers.empty()) {
        m_propSync = true;
        for (auto& fn : m_rowRefreshers)
            if (fn) fn();
        m_propSync = false;
        applyDynScroll(m_dynScroll.GetFloat(PropertyNames::kValue));
        return;
    }
    m_rowsBuiltSig = "bench";
    // 类型行
    {
        Control lb = takePoolRow("bench:type", [&] {
            return m_ui->CreateLabel(u8"画布（Bench）", kPropLblFont, 0, 0, rowW, kPropLblH);
        });
        lb.SetColor(PropertyNames::kText, UIColor{0xE8, 0xE8, 0xE8, 255});
        lb.SetRect(kPropInsetX, y, rowW, kPropLblH);
        m_dynamicRows.push_back(lb);
        m_dynamicRowY.push_back(y);
        y += kPropLblH + kPropRowGap;
        ++rows;
        genKeys += " [画布]";
    }
    // 文本行（id / 标题）共用样式
    auto makeTextRow = [&](const std::string& kind, const std::string& label,
                           const std::function<std::string()>& get,
                           const std::function<void(const std::string&)>& set) {
        makeLabel(label);
        Control ta = takePoolRow(kind, [&] {
            return m_ui->CreateEditBox(kPropInsetX, y, rowW, kPropCtlH);
        });
        ta.SetRect(kPropInsetX, y, rowW, kPropCtlH);
        ta.SetStateColor(PropertyNames::kBackground,
                         UIStateColor{kPropPanelBg, kPropPanelBg, kPropPanelBg, kPropPanelBg});
        ta.SetStateColor(PropertyNames::kText,
                         UIStateColor{kPropPanelText, kPropPanelText, kPropPanelText, kPropPanelText});
        ta.SetStateColor(PropertyNames::kBorder,
                         UIStateColor{kPropPanelBorder, kPropPanelBorderFocus, kPropPanelBorderFocus, kPropPanelBorder});
        ta.SetString(PropertyNames::kTextContent, get());
        m_rowRefreshers.push_back([ta, get]() mutable {
            ta.SetString(PropertyNames::kTextContent, get());
        });
        ta.SetCallback(PropertyNames::kEventTextChanged, [this, get, set](const Event& e) {
            if (m_propSync) return;
            const std::string v = sanitizePropValue("caption", e.GetTextChanged());
            if (v == get()) return;
            set(v);
        });
        m_dynamicRows.push_back(ta);
        m_dynamicRowY.push_back(y);
        y += kPropCtlH + rowGap;
        ++rows;
    };
    makeTextRow("bench:id", u8"画布 id（代码引用）",
                [this]() { return m_benchId; },
                [this](const std::string& v) {
                    std::string t = v;
                    const auto b = t.find_first_not_of(" \t");
                    const auto e2 = t.find_last_not_of(" \t");
                    t = (b == std::string::npos) ? std::string() : t.substr(b, e2 - b + 1);
                    bool ok = true;
                    for (char c : t)
                        if (std::isspace((unsigned char)c)) ok = false;
                    if (ok)
                        for (auto& it : m_items)
                            if (it.model && it.model->props.id == t) ok = false;
                    if (!ok) {
                        setStatusHint(u8"画布 id 未修改（空白/与控件重复）：" + t);
                        return;
                    }
                    m_benchId = t;
                    syncTreeRename("@bench", benchTreeNodeLabel());   // 树根节点显示 Bench 控件 id
                    commitUndo("bench-id");   // F2.5：画布 id（编辑完成 0.7s 合并）
                    setStatusHint(u8"画布 id 已改为：" + t);
                });
    makeTextRow("bench:title", u8"窗口标题",
                [this]() { return m_benchTitle; },
                [this](const std::string& v) { m_benchTitle = v; commitUndo("bench-title"); });
    // P0-84 绕过：对话框输入通道（已证可用）——提供 id/标题 的可用编辑入口
    {
        makeLabel(u8"画布设置");
        Control b = takePoolRow("bench:dlg", [&] {
            return m_ui->CreateButton(u8"画布设置…（id / 窗口标题）", kPropInsetX, y, rowW, kPropCtlH);
        });
        b.SetRect(kPropInsetX, y, rowW, kPropCtlH);
        b.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            ShowBenchSettingsDialog();
        });
        m_dynamicRows.push_back(b);
        m_dynamicRowY.push_back(y);
        y += kPropCtlH + rowGap;
        ++rows;
        genKeys += " dlg";
    }
    // 缩放模式（off/fit/stretch；导出 viewport.scale-mode——预览/应用生效）
    {
        makeLabel(u8"缩放模式");
        Control cb = takePoolRow("bench:scale", [&] {
            Control c = m_ui->CreateComboBox(kPropInsetX, y, rowW, ctlH);
            m_ui->ComboBoxAddItem(c, "off", "0");
            m_ui->ComboBoxAddItem(c, "fit", "1");
            m_ui->ComboBoxAddItem(c, "stretch", "2");
            return c;
        });
        cb.SetRect(kPropInsetX, y, rowW, ctlH);
        cb.SetInt(PropertyNames::kSelectedIndex, m_benchScaleMode);
        m_rowRefreshers.push_back([this, cb]() mutable {
            cb.SetInt(PropertyNames::kSelectedIndex, m_benchScaleMode);
        });
        cb.SetCallback(PropertyNames::kEventSelectionChanged, [this, cb](const Event&) mutable {
            if (m_propSync) return;
            const int idx = cb.GetInt(PropertyNames::kSelectedIndex);
            if (idx < 0 || idx > 2) return;
            m_benchScaleMode = idx;
            commitUndo("bench-scale");   // F2.5
            setStatusHint(idx == 0 ? u8"缩放模式：off（画布跟随窗口）"
                          : idx == 1 ? u8"缩放模式：fit（等比居中）"
                                     : u8"缩放模式：stretch（拉伸铺满）");
        });
        m_dynamicRows.push_back(cb);
        m_dynamicRowY.push_back(y);
        y += ctlH + rowGap;
        ++rows;
        genKeys += " scale-mode";
    }
    // 背景色（ColorPicker 含 alpha；00 透明 = 不填充/透出底层——游戏场景 UI 叠加）
    {
        makeLabel(u8"背景色（alpha=00 透明）");
        char hex[12];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X",
                      m_benchBg.r, m_benchBg.g, m_benchBg.b, m_benchBg.a);
        Control cp = takePoolRow("bench:bg", [&] {
            return m_ui->CreateColorPicker(kPropInsetX, y, kColorRowW, ctlH, hex);
        });
        cp.SetRect(kPropInsetX, y, kColorRowW, ctlH);
        cp.SetString(PropertyNames::kColor, hex);
        cp.SetString(PropertyNames::kTextContent, hex);
        m_rowRefreshers.push_back([this, cp]() mutable {
            char h2[12];
            std::snprintf(h2, sizeof(h2), "#%02X%02X%02X%02X",
                          m_benchBg.r, m_benchBg.g, m_benchBg.b, m_benchBg.a);
            cp.SetString(PropertyNames::kColor, h2);
            cp.SetString(PropertyNames::kTextContent, h2);
        });
        cp.SetCallback(PropertyNames::kEventColorChanged, [this](const Event& e) {
            if (m_propSync) return;
            m_benchBg = e.GetChangedColor();
            applyBenchBackground();
            commitUndo("bench-bg");   // F2.5：颜色编辑（0.7s 合并）
        });
        m_fixedWidthRows.insert(m_dynamicRows.size());
        m_dynamicRows.push_back(cp);
        m_dynamicRowY.push_back(y);
        y += ctlH + rowGap;
        ++rows;
        genKeys += " background";
    }
    hideUnusedPoolRows();
    m_dynScrollMax = (y + kPropInsetX) - viewH;
    if (m_dynScrollMax < 0) m_dynScrollMax = 0;
    m_dynScroll.SetFloat(PropertyNames::kRangeMax, m_dynScrollMax > 0 ? m_dynScrollMax : 1.0f);
    m_dynScroll.SetFloat(PropertyNames::kValue, 0.0f);
    m_dynScroll.SetBool(PropertyNames::kVisible, m_dynScrollMax > 0);
    applyDynScroll(0.0f);
    std::printf("[CanvasPane] bench rows built: %d (%s)\n", rows, genKeys.c_str());
}

// 画布背景 → 网格瓦片底色（WYSIWYG：纸面=设计区、底色=Bench 背景、网格线取对比色）。
void CanvasPane::applyBenchBackground() {
    rebuildGridTile();
}

void CanvasPane::rebuildDynamicRows() {
    m_dynamicRows.clear();
    m_dynamicRowY.clear();
    m_fixedWidthRows.clear();
    m_captionEditor = Control();
    m_schemaTarget = Control();
    m_schemaTargets.clear();
    m_rowRefreshers.clear();
    m_rowPoolUsed.clear();
    if (!m_propDynamic.IsValid()) return;
    if (m_benchSelected) { buildBenchRows(); return; }   // 画布（Bench）行集
    const ControlModel* m = findModel(m_selectedId);
    if (!m) {   // 未选中：动态区留空（滚动条一并隐藏；池行全隐藏不销毁）
        if (m_dynScroll.IsValid()) m_dynScroll.SetBool(PropertyNames::kVisible, false);
        m_rowsBuiltSig.clear();
        m_schemaTargets.clear();
        hideUnusedPoolRows();
        return;
    }
    const PlacedControl* pc = placedById(m->props.id);
    if (!pc || !pc->ctl.IsValid()) { hideUnusedPoolRows(); return; }
    if (!m_schema) { hideUnusedPoolRows(); return; }

    const auto& defs = (*m_schema)["$defs"];
    if (!defs.contains(m->props.type)) { hideUnusedPoolRows(); return; }
    const nlohmann::json& def = defs[m->props.type];
    if (!def.contains("properties")) { hideUnusedPoolRows(); return; }

    // P2 多选：批量写目标（全部选中；读回/回填以主选为准）+ 行集签名含全部类型
    m_schemaTargets.clear();
    std::string typeSig;
    for (const auto& id : m_selIds) {
        PlacedControl* t = placedById(id);
        if (!t || !t->ctl.IsValid()) continue;
        m_schemaTargets.push_back(t->ctl);
        if (!typeSig.empty()) typeSig += ",";
        typeSig += t->model->props.type;
    }
    if (m_schemaTargets.empty()) return;

    // E 方案快路径：同类型行集相同（schema 确定）——跳过销毁重建，仅执行各行
    // 刷值闭包（读选中控件当前值回填行显示，毫秒级）；P2 多选签名含全部类型
    if (m_rowsBuiltSig == typeSig && !m_rowRefreshers.empty()) {
        m_schemaTarget = pc->ctl;
        m_propSync = true;   // 程序化回填不触发联动回调
        for (auto& fn : m_rowRefreshers)
            if (fn) fn();
        m_propSync = false;
        applyDynScroll(m_dynScroll.GetFloat(PropertyNames::kValue));
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
        "type", "name", "rect", "scale", "margin",   // layout/anchor 走定制行（P0-59）；id 走重命名定制行
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
        Control hb = takePoolRow("grp:" + name + (collapsed ? ":收" : ":开"), [&] {
            return m_ui->CreateButton(
                std::string(collapsed ? "▸ " : "▾ ") + name, 0, 0, rowW, kPropCtlH);
        });
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
    auto makeLabel = [&](const std::string& label) {
        Control lb = takePoolRow("lbl:" + label, [&] {
            return m_ui->CreateLabel(label, lblFont, 0, 0, rowW, lblH);
        });
        lb.SetColor(PropertyNames::kText, UIColor{0x90, 0x90, 0x90, 255});
        lb.SetRect(kPropInsetX, y, rowW, lblH);
        m_dynamicRows.push_back(lb);
        m_dynamicRowY.push_back(y);
        y += lblH + gapLbl;
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
    // P2 多选：仅保留全部选中类型的共有键（同类型=全集；交集空则只有组标题/类型行）
    if (m_selIds.size() > 1) {
        std::set<std::string> common = schemaKeySet(defs, m->props.type);
        for (size_t i = 0; i + 1 < m_selIds.size(); ++i) {
            const ControlModel* mi = findModel(m_selIds[i]);
            if (!mi) continue;
            const std::set<std::string> ks = schemaKeySet(defs, mi->props.type);
            std::set<std::string> inter;
            for (const auto& k : common)
                if (ks.count(k)) inter.insert(k);
            common.swap(inter);
        }
        kvs.erase(std::remove_if(kvs.begin(), kvs.end(),
                                 [&](const auto& kv) { return !common.count(kv.first); }),
                  kvs.end());
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
    // 类型标识行（选中控件类型一目了然——首字母大写的 schema 类型名；多选显示计数）
    if (!m_schemaType.empty()) {
        std::string typeName;
        if (m_selIds.size() > 1) {
            typeName = u8"多选 ×" + std::to_string(m_selIds.size());
        } else {
            typeName = m_schemaType;
            typeName[0] = std::toupper((unsigned char)typeName[0]);
        }
        Control lb = takePoolRow("type:" + typeName, [&] {
            return m_ui->CreateLabel(typeName, kPropLblFont, 0, 0, rowW, kPropLblH);
        });
        lb.SetColor(PropertyNames::kText, UIColor{0xE8, 0xE8, 0xE8, 255});
        lb.SetRect(kPropInsetX, y, rowW, kPropLblH);
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
            if (groupCollapsed(owner)) continue;   // 收起：不创建该组色槽
            auto itg = colorGroupsAll.find(colorSubGrp);
            if (itg == colorGroupsAll.end()) continue;
            const auto& [grpName, states] = *itg;
        bool hasShadow = def.contains("properties") &&
                         def["properties"].contains("shadow");
        // P2 多选：全部选中类型都声明 shadow 才生成阴影段（交集语义）
        if (hasShadow && m_selIds.size() > 1) {
            for (const auto& id : m_selIds) {
                const ControlModel* mi = findModel(id);
                if (!mi || !defs.contains(mi->props.type) ||
                    !defs[mi->props.type].contains("properties") ||
                    !defs[mi->props.type]["properties"].contains("shadow")) {
                    hasShadow = false;
                    break;
                }
            }
        }
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
                Control cb = takePoolRow("chkshadow:enabled", [&] {
                    return m_ui->CreateCheckBox("shadow.enabled", 0, 0, rowW, ctlH);
                });
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
                    for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中（类型分派）
                        PlacedControl* pc2 = placedById(id);
                        if (!pc2 || !pc2->ctl.IsValid()) continue;
                        pc2->model->props.extended["shadow"] = on2;   // 模型同步
                        if (pc2->model->props.type == "win-frame") {
                            void* h = pc2->ctl.GetPtr(PropertyNames::kTitleLabel);
                            if (h) m_viewport->FromHandle(h).SetBool(PropertyNames::kShadow, on2);
                            continue;
                        }
                        pc2->ctl.SetBool(PropertyNames::kShadow, on2);
                    }
                });
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
                Control nud = takePoolRow(std::string("nudshadow:") + (oi == 0 ? "x" : "y"), [&] {
                    return m_ui->CreateNumericUpDown(0, 0, rowW, ctlH);
                });
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
                                [this, oi](const Event& e2) {
                    const float v2 = e2.GetValueChanged();
                    for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中（类型分派）
                        PlacedControl* pc2 = placedById(id);
                        if (!pc2 || !pc2->ctl.IsValid()) continue;
                        pc2->model->props.extended[kOffProp[oi]] = v2;   // 模型同步
                        const std::string& tp2 = pc2->model->props.type;
                        const bool legacy2 = (tp2 == "button" || tp2 == "check-box" ||
                                              tp2 == "win-frame");
                        if (tp2 == "label" || !legacy2) {
                            pc2->ctl.SetFloat(kOffProp[oi], v2);
                            continue;
                        }
                        void* h = pc2->ctl.GetPtr(tp2 == "win-frame"
                            ? PropertyNames::kTitleLabel : PropertyNames::kCaptionLabel);
                        if (h) m_viewport->FromHandle(h).SetFloat(kOffProp[oi], v2);
                    }
                });
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
                    Control cp = takePoolRow("color:" + prop + ":" + grpName, [&] {
                        return m_ui->CreateColorPicker(kPropInsetX, y, kColorRowW, ctlH, hex);
                    });
                    cp.SetRect(kPropInsetX, y, kColorRowW, ctlH);
                    cp.SetString(PropertyNames::kColor, hex);          // 复用行：重填当前值（Picker 自身 color 域）
                    cp.SetString(PropertyNames::kTextContent, hex);
                    cp.SetCallback(PropertyNames::kEventColorChanged,
                                   [this, prop, grpName](const Event& e) {
                        const UIColor c = e.GetChangedColor();
                        for (const auto& id : m_selIds)   // P2 多选：批量写全部选中
                            writeColorSlotTo(id, prop, grpName, c);
                    });
                    m_fixedWidthRows.insert(m_dynamicRows.size());
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
                        cp.SetString(PropertyNames::kColor, hex2);   // Picker 自身 color 域（勿写目标属性名）
                        cp.SetString(PropertyNames::kTextContent, hex2);
                    });
                }
            if (colorSubGrp == "text-shadow") renderShadowRows();

            continue;
        }

        // 结构化：行式列表编辑（combo-box.items / status-bar.items / list-view.columns；
        // 模型持有 + 引擎同步；单选——结构化数据不做批量写；一行一项，空行忽略）
        const bool structItems = (key == "items" && (m_schemaType == "combo-box" ||
                                                     m_schemaType == "status-bar" ||
                                                     m_schemaType == "tree-view"));
        const bool structCols  = (key == "columns" && m_schemaType == "list-view");
        const bool structRows  = (key == "rows" && m_schemaType == "list-view");
        const bool structPts   = (key == "points" && m_schemaType == "shape");
        const bool structMenus = (key == "menus" && m_schemaType == "menu-bar");
        const bool structTabs  = (key == "tabs" && m_schemaType == "tab-control");
        if (structItems || structCols || structRows || structPts || structMenus || structTabs) {
            if (m_selIds.size() != 1) continue;
            makeLabel(structTabs ? u8"tabs（一行一页签标题）"
                                 : structMenus ? u8"menus（菜单行；项 > 层级，项@快捷键，-- 分隔）"
                                 : structPts ? u8"points（一行一点 x,y）"
                                 : structCols ? u8"columns（标题@宽度@背景色@字体色@字号@字体名@阴影色@偏移）"
                                 : structRows ? u8"rows（| 单元格；@ 背景色@字体色@字号@字体名@阴影色@偏移）"
                                 : (m_schemaType == "status-bar" ? u8"items（文本@背景色@字体色@字号@字体名@阴影色@偏移）"
                                     : (m_schemaType == "tree-view" ? u8"items（> 层级；@ 背景色@字体色@字号@字体名@阴影色@偏移）"
                                                                     : u8"items（一行一项）")));
            Control ta = takePoolRow("struct:" + key + ":" + m_schemaType, [&] {
                return m_ui->CreateTextArea(0, 0, rowW, kPropItemsH);
            });
            ta.SetRect(kPropInsetX, y, rowW, kPropItemsH);
            ta.SetBool(PropertyNames::kAutoScroll, false);   // P0-68：编辑式光标跟随（显式，不依赖缺省）
            ta.SetStateColor(PropertyNames::kBackground,
                             UIStateColor{kPropPanelBg, kPropPanelBg, kPropPanelBg, kPropPanelBg});
            ta.SetStateColor(PropertyNames::kText,
                             UIStateColor{kPropPanelText, kPropPanelText, kPropPanelText, kPropPanelText});
            ta.SetStateColor(PropertyNames::kBorder,
                             UIStateColor{kPropPanelBorder, kPropPanelBorderFocus, kPropPanelBorderFocus, kPropPanelBorder});
            ta.SetString(PropertyNames::kTextContent, structRowText(*m, key));
            m_rowRefreshers.push_back([this, ta, key]() mutable {
                if (const ControlModel* mm = findModel(m_selectedId))
                    ta.SetString(PropertyNames::kTextContent, structRowText(*mm, key));
            });
            ta.SetCallback(PropertyNames::kEventTextChanged, [this, key](const Event& e) {
                if (m_propSync) return;
                ControlModel* mm = findModel(m_selectedId);
                if (!mm) return;
                if (mm->props.type == "list-view") {
                    if (key == "rows") applyListRowsText(*mm, e.GetTextChanged());
                    else applyListColumnsText(*mm, e.GetTextChanged());
                } else if (mm->props.type == "status-bar") {
                    applyStatusBarItemsText(*mm, e.GetTextChanged());
                } else if (mm->props.type == "tree-view") {
                    applyTreeItemsText(*mm, e.GetTextChanged());
                } else if (mm->props.type == "shape") {
                    applyShapePointsText(*mm, e.GetTextChanged());
                } else if (mm->props.type == "menu-bar") {
                    applyMenusText(*mm, e.GetTextChanged());
                } else if (mm->props.type == "tab-control") {
                    applyTabsText(*mm, e.GetTextChanged());
                } else {
                    applyComboItemsText(*mm, e.GetTextChanged());
                }
            });
            m_dynamicRows.push_back(ta);
            m_dynamicRowY.push_back(y);
            y += kPropItemsH + rowGap;
            ++rows;
            genKeys += " " + key;
            continue;
        }

        // 独立色键（x-color / $ref→color def，如 list-view header-text/header-background/
        // header-shadow）：ColorPicker 行（批量写全部选中；读回主选）。
        bool isColorKey = false;
        if (spec.is_object()) {
            if (spec.value("x-color", false)) {
                isColorKey = true;
            } else if (spec.contains("$ref") && spec["$ref"].is_string()) {
                const std::string ref = spec["$ref"].get<std::string>();
                const std::string dn = ref.substr(ref.find_last_of('/') + 1);
                if (defs.contains(dn) && defs[dn].value("x-color", false)) isColorKey = true;
            }
        }
        if (isColorKey) {
            makeLabel(key);
            const std::string cprop = jsonKeyToProp(key);
            UIColor cc{0x4A, 0x90, 0xD9, 255};
            if (m_schemaTarget.IsValid()) cc = m_schemaTarget.GetColor(cprop.c_str());
            char hex[12];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", cc.r, cc.g, cc.b, cc.a);
            Control cp = takePoolRow("xcolor:" + key, [&] {
                return m_ui->CreateColorPicker(kPropInsetX, y, kColorRowW, ctlH, hex);
            });
            cp.SetRect(kPropInsetX, y, kColorRowW, ctlH);
            cp.SetString(PropertyNames::kColor, hex);              // 复用行：重填当前值（Picker 自身 color 域）
            cp.SetString(PropertyNames::kTextContent, hex);
            m_rowRefreshers.push_back([this, cp, cprop]() mutable {
                const UIColor c2 = m_schemaTarget.GetColor(cprop.c_str());
                char h2[12];
                std::snprintf(h2, sizeof(h2), "#%02X%02X%02X%02X", c2.r, c2.g, c2.b, c2.a);
                cp.SetString(PropertyNames::kColor, h2);           // Picker 自身 color 域（勿写目标属性名）
                cp.SetString(PropertyNames::kTextContent, h2);
            });
            cp.SetCallback(PropertyNames::kEventColorChanged, [this, cprop](const Event& e) {
                const UIColor c = e.GetChangedColor();
                const std::string h = uiColorHex(c);
                for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
                    PlacedControl* pc2 = placedById(id);
                    if (!pc2 || !pc2->ctl.IsValid()) continue;
                    pc2->ctl.SetColor(cprop.c_str(), c);
                    pc2->model->props.extended[cprop] = h;   // 模型同步
                }
            });
            m_fixedWidthRows.insert(m_dynamicRows.size());
            m_dynamicRows.push_back(cp);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
            ++rows;
            genKeys += " " + key;
            continue;
        }

        // 控件 id（用户需求：代码引用——重命名行；单选；全链更新见 renameControlId）
        if (key == "id") {
            if (m_selIds.size() != 1) continue;
            ControlModel* mm = findModel(m_selectedId);
            if (!mm) continue;
            makeLabel(u8"控件 id（代码引用；回车或移开生效）");
            Control ta = takePoolRow("edit:id", [&] {
                return m_ui->CreateEditBox(kPropInsetX, y, rowW, kPropCtlH);
            });
            ta.SetRect(kPropInsetX, y, rowW, kPropCtlH);
            ta.SetStateColor(PropertyNames::kBackground,
                             UIStateColor{kPropPanelBg, kPropPanelBg, kPropPanelBg, kPropPanelBg});
            ta.SetStateColor(PropertyNames::kText,
                             UIStateColor{kPropPanelText, kPropPanelText, kPropPanelText, kPropPanelText});
            ta.SetStateColor(PropertyNames::kBorder,
                             UIStateColor{kPropPanelBorder, kPropPanelBorderFocus, kPropPanelBorderFocus, kPropPanelBorder});
            ta.SetString(PropertyNames::kTextContent,
                         (m_idRenamePending && m_idRenameTarget == m_selectedId)
                             ? m_idRenameText : mm->props.id);
            m_rowRefreshers.push_back([this, ta]() mutable {
                if (m_idRenamePending && m_idRenameTarget == m_selectedId) {
                    ta.SetString(PropertyNames::kTextContent, m_idRenameText);   // 保留编辑中文本
                    return;
                }
                ControlModel* m2 = findModel(m_selectedId);
                if (m2) ta.SetString(PropertyNames::kTextContent, m2->props.id);
            });
            ta.SetCallback(PropertyNames::kEventTextChanged, [this](const Event& e) {
                if (m_propSync) return;
                ControlModel* m2 = findModel(m_selectedId);
                if (!m2) return;
                const std::string v = e.GetTextChanged();
                if (v == m2->props.id) {   // 无变化/程序回填
                    m_idRenamePending = false;
                    return;
                }
                // 编辑完成再提交（500ms 空闲；回车立即；切换选中时提交）
                // 仅"有效候选"启动计时——退格清空/中间非法态不自动回退（用户：删完未及时输入被复原）
                m_idRenameTarget = m_selectedId;
                m_idRenameText = v;
                m_idRenamePending = true;
                bool validCandidate = !v.empty() && v != "@bench";
                for (char c : v)
                    if (std::isspace((unsigned char)c)) validCandidate = false;
                if (validCandidate)
                    for (auto& it : m_items)
                        if (it.model && it.model->props.id == v) validCandidate = false;
                if (validCandidate)
                    m_idRenameDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                else
                    m_idRenameDeadline = std::chrono::steady_clock::time_point::max();   // 不自动提交
            });
            ta.SetCallback(PropertyNames::kEventEnter, [this](const Event&) {
                commitPendingIdRename();
            });
            m_dynamicRows.push_back(ta);
            m_dynamicRowY.push_back(y);
            y += kPropCtlH + rowGap;
            ++rows;
            genKeys += " id";
            continue;
        }

        // 布局模式（容器，P0-59 运行期切换）：绝对/h-flow/v-flow/anchor/grid
        if (key == "layout") {
            if (m_selIds.size() != 1) continue;
            makeLabel(u8"布局模式");
            Control lcb = takePoolRow("enum:layout", [&] {
                Control c = m_ui->CreateComboBox(kPropInsetX, y, rowW, ctlH);
                for (int i = 0; i < 5; ++i) m_ui->ComboBoxAddItem(c, kLayoutLabels[i], kLayoutValues[i]);
                return c;
            });
            lcb.SetRect(kPropInsetX, y, rowW, ctlH);
            const std::string curLay = m_schemaTarget.GetEnum(PropertyNames::kLayout);
            int curIdx = 0;
            for (int i = 0; i < 5; ++i) if (curLay == kLayoutValues[i]) { curIdx = i; break; }
            lcb.SetInt(PropertyNames::kSelectedIndex, curIdx);
            m_rowRefreshers.push_back([this, lcb]() mutable {
                const std::string rl = m_schemaTarget.GetEnum(PropertyNames::kLayout);
                for (int i = 0; i < 5; ++i)
                    if (rl == kLayoutValues[i]) { lcb.SetInt(PropertyNames::kSelectedIndex, i); return; }
                lcb.SetInt(PropertyNames::kSelectedIndex, -1);
            });
            lcb.SetCallback(PropertyNames::kEventSelectionChanged, [this, lcb](const Event&) mutable {
                if (m_propSync) return;
                const int idx = lcb.GetInt(PropertyNames::kSelectedIndex);
                if (idx < 0 || idx >= 5) return;
                ControlModel* mm = findModel(m_selectedId);
                if (!mm) return;
                if (PlacedControl* pc2 = placedById(mm->props.id))
                    if (pc2->ctl.IsValid())
                        pc2->ctl.SetEnum(PropertyNames::kLayout, kLayoutValues[idx]);
                mm->props.extended["layout"] = kLayoutValues[idx];
                syncModelsFromEngine();   // 布局切换触发引擎重排——同步全部模型
            });
            m_dynamicRows.push_back(lcb);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
            ++rows;
            genKeys += " layout";
            continue;
        }

        // 锚定（子控件级，P0-59/P0-61）：写父容器或根（顶层）的 child-id + anchor /
        // anchor-offset-x/y；生效前提 = 目标为 anchor 布局——顶层首次锚定时自动切根布局
        if (key == "anchor" || key == "anchorOffset") {
            if (m_selIds.size() != 1) continue;
            PlacedControl* selfPc = placedById(m_selectedId);
            if (!selfPc) continue;
            Control par = anchorParentOf(*selfPc);
            if (!par.IsValid()) continue;
            if (key == "anchor") {
                makeLabel(u8"锚定（父容器/根 anchor 布局下生效）");
                Control acb = takePoolRow("enum:anchor", [&] {
                    Control c = m_ui->CreateComboBox(kPropInsetX, y, rowW, ctlH);
                    for (int i = 0; i < 14; ++i) m_ui->ComboBoxAddItem(c, kAnchorLabels[i], kAnchorValues[i]);
                    return c;
                });
                acb.SetRect(kPropInsetX, y, rowW, ctlH);
                par.SetString(PropertyNames::kChildTargetId, selfPc->model->props.id);
                const std::string curA = par.GetEnum(PropertyNames::kChildAnchor);
                int curI = -1;
                for (int i = 0; i < 14; ++i) if (curA == kAnchorValues[i]) { curI = i; break; }
                acb.SetInt(PropertyNames::kSelectedIndex, curI);
                m_rowRefreshers.push_back([this, acb]() mutable {
                    PlacedControl* s2 = placedById(m_selectedId);
                    if (!s2) return;
                    Control p2 = anchorParentOf(*s2);
                    if (!p2.IsValid()) return;
                    p2.SetString(PropertyNames::kChildTargetId, s2->model->props.id);
                    const std::string rA = p2.GetEnum(PropertyNames::kChildAnchor);
                    for (int i = 0; i < 14; ++i)
                        if (rA == kAnchorValues[i]) { acb.SetInt(PropertyNames::kSelectedIndex, i); return; }
                    acb.SetInt(PropertyNames::kSelectedIndex, -1);
                });
                acb.SetCallback(PropertyNames::kEventSelectionChanged, [this, acb](const Event&) mutable {
                    if (m_propSync) return;
                    const int idx = acb.GetInt(PropertyNames::kSelectedIndex);
                    if (idx < 0 || idx >= 14) return;
                    PlacedControl* s2 = placedById(m_selectedId);
                    if (!s2) return;
                    Control p2 = anchorParentOf(*s2);
                    if (!p2.IsValid()) return;
                    // 顶层首次锚定：根切 anchor 布局（未锚定叠加物经 P0-60 稀疏保持原位）
                    if (s2->parentId.empty() && p2.GetEnum(PropertyNames::kLayout) != "anchor")
                        p2.SetEnum(PropertyNames::kLayout, "anchor");
                    p2.SetString(PropertyNames::kChildTargetId, s2->model->props.id);
                    p2.SetEnum(PropertyNames::kChildAnchor, kAnchorValues[idx]);
                    s2->model->props.extended["anchor"] = kAnchorValues[idx];
                    syncModelsFromEngine();   // P0-60：引擎 setter 即时重排——同步全部模型
                });
                m_dynamicRows.push_back(acb);
                m_dynamicRowY.push_back(y);
                y += ctlH + rowGap;
                ++rows;
                genKeys += " anchor";
                continue;
            }
            for (int oi = 0; oi < 2; ++oi) {   // anchorOffset：X/Y 两行
                makeLabel(oi == 0 ? u8"锚定偏移 X" : u8"锚定偏移 Y");
                Control nud = takePoolRow(std::string("nudanchor:") + (oi == 0 ? "x" : "y"), [&] {
                    return m_ui->CreateNumericUpDown(0, 0, rowW, kPropCtlH);
                });
                nud.SetRect(kPropInsetX, y, rowW, kPropCtlH);
                nud.SetFloat(PropertyNames::kStep, 1.0f);
                par.SetString(PropertyNames::kChildTargetId, selfPc->model->props.id);
                const float v0 = oi == 0 ? par.GetFloat(PropertyNames::kChildAnchorOffsetX)
                                         : par.GetFloat(PropertyNames::kChildAnchorOffsetY);
                nud.SetFloat(PropertyNames::kValue, v0);
                m_rowRefreshers.push_back([this, nud, oi]() mutable {
                    PlacedControl* s2 = placedById(m_selectedId);
                    if (!s2) return;
                    Control p2 = anchorParentOf(*s2);
                    if (!p2.IsValid()) return;
                    p2.SetString(PropertyNames::kChildTargetId, s2->model->props.id);
                    const float rv = oi == 0 ? p2.GetFloat(PropertyNames::kChildAnchorOffsetX)
                                             : p2.GetFloat(PropertyNames::kChildAnchorOffsetY);
                    nud.SetFloat(PropertyNames::kValue, rv);
                });
                nud.SetCallback(PropertyNames::kEventValueChanged, [this, oi](const Event& e) {
                    PlacedControl* s2 = placedById(m_selectedId);
                    if (!s2) return;
                    Control p2 = anchorParentOf(*s2);
                    if (!p2.IsValid()) return;
                    p2.SetString(PropertyNames::kChildTargetId, s2->model->props.id);
                    p2.SetFloat(oi == 0 ? PropertyNames::kChildAnchorOffsetX
                                        : PropertyNames::kChildAnchorOffsetY,
                                e.GetValueChanged());
                    s2->model->props.extended[oi == 0 ? "anchor-offset-x" : "anchor-offset-y"]
                        = e.GetValueChanged();   // 模型同步（保存数据源）
                    syncModelsFromEngine();   // P0-60：引擎 setter 即时重排——同步全部模型
                });
                m_dynamicRows.push_back(nud);
                m_dynamicRowY.push_back(y);
                y += ctlH + rowGap;
                ++rows;
                genKeys += oi == 0 ? " anchor-offset-x" : " anchor-offset-y";
            }
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
            // 引擎字体枚举清单 API（单一数据源——P047_48 批 2.3）；
            // P0-47 扩展：kFont 未设置时引擎读回实际生效的缺省字体名——
            // 无需"(缺省)"虚拟项，直接按读回名匹配选中
            const auto fontNames = UICornerstone::UICornerstone::GetFontNames();
            Control fcb = takePoolRow("enum:font", [&] {
                Control c = m_ui->CreateComboBox(kPropInsetX, y, rowW, ctlH);
                for (int i = 0; i < (int)fontNames.size(); ++i)
                    m_ui->ComboBoxAddItem(c, fontNames[i], fontNames[i]);
                return c;
            });
            fcb.SetRect(kPropInsetX, y, rowW, ctlH);
            const std::string cur = m_schemaTarget.GetString(PropertyNames::kFont);
            int curIdx = 0;
            for (int i = 0; i < (int)fontNames.size(); ++i)
                if (ieqNoCase(cur, fontNames[i])) curIdx = i;
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
                if (idx >= 0 && idx < (int)fontNames.size()) {
                    for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
                        PlacedControl* pc2 = placedById(id);
                        if (!pc2 || !pc2->ctl.IsValid()) continue;
                        pc2->ctl.SetString(PropertyNames::kFont, fontNames[idx]);
                        pc2->model->props.extended["font"] = fontNames[idx];   // 模型同步
                    }
                }
            });
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
            Control ta = takePoolRow("txt:" + key, [&] {
                return m_ui->CreateTextArea(0, 0, rowW, kPropTextAreaH);
            });
            ta.SetRect(kPropInsetX, y, rowW, kPropTextAreaH);
            ta.SetBool(PropertyNames::kAutoScroll, false);   // P0-68：编辑式光标跟随（显式，不依赖缺省）
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
                           [this, prop, isCaptionRow](const Event& e) {
                               if (m_propSync) return;
                               if (m_selIds.empty()) return;
                               const std::string v2 =
                                   sanitizePropValue(prop, e.GetTextChanged());
                               if (isCaptionRow && m_propCaption.IsValid()) {
                                   m_propSync = true;
                                   m_propCaption.SetString(PropertyNames::kTextContent, v2);
                                   m_propSync = false;
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
                               }
                               for (const auto& id : m_selIds)   // P2 多选：批量写全部选中
                                   writeStringPropTo(id, prop, v2);
                               // playing 状态不干预：引擎 wasPlaying 语义
                               // （播放中换动画→重播、暂停中换→保持暂停）；
                               // 首次预览由 placeControl 放置时自动播放承担
                           });
            m_dynamicRows.push_back(ta);
            m_dynamicRowY.push_back(y);
            y += kPropTextAreaH + rowGap;
        } else if (effT == "number" || effT == "integer") {
            Control nud = takePoolRow("nud:" + key, [&] {
                return m_ui->CreateNumericUpDown(0, 0, rowW, kPropCtlH);
            });
            nud.SetRect(kPropInsetX, y, rowW, kPropCtlH);
            // 按 schema 类型分派（引擎已归一：font-size 全控件 Int 域、
            // number 类走 Float 域）；读失败（0）→ 放置记录的 font-size 兜底
            const bool isInt = (t == "integer");
            // 复用行需重设步进/小数位（number 类：0.1 步进+1 位小数——缺省 decimals=0
            // 会 snap 到整数网格且整数显示，0.1 被归零）
            nud.SetFloat(PropertyNames::kStep, isInt ? 1.0f : 0.1f);
            nud.SetInt(PropertyNames::kDecimals, isInt ? 0 : 1);
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
                                const float v2 = e.GetValueChanged();
                                for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
                                    PlacedControl* pc2 = placedById(id);
                                    if (!pc2 || !pc2->ctl.IsValid()) continue;
                                    if (isInt) pc2->ctl.SetInt(prop.c_str(), (int)v2);
                                    else pc2->ctl.SetFloat(prop.c_str(), v2);
                                    pc2->model->props.extended[prop] = isInt ? (int)v2 : v2;   // 模型同步
                                }
                                commitUndo("n:" + prop);   // F2.5：数值行（0.7s 合并）
                            });
            m_dynamicRows.push_back(nud);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
        } else {
            Control cb = takePoolRow("chk:" + key, [&] {
                return m_ui->CreateCheckBox(key, 0, 0, rowW, kPropCheckH);
            });
            cb.SetRect(kPropInsetX, y, rowW, kPropCheckH);
            cb.SetBool(PropertyNames::kTriState, false);   // bool 属性两态（引擎缺省三态循环）
            // CheckBox 不发 click，发 check-changed（intVal=CheckState 枚举）
            cb.SetCallback(PropertyNames::kEventCheckChanged,
                           [this, prop](const Event& e) {
                const bool v2 = e.GetCheckState() != 0;
                for (const auto& id : m_selIds) {   // P2 多选：批量写全部选中
                    PlacedControl* pc2 = placedById(id);
                    if (!pc2 || !pc2->ctl.IsValid()) continue;
                    if (prop == PropertyNames::kReorderable) {
                        // 设计态：旗标只进模型（导出/预览生效）；live 控件保持关闭——
                        // 否则引擎内置拖拽在设计画布内激活，与宿主页签拖动互斗（双 ghost）✗
                        pc2->ctl.SetBool(PropertyNames::kReorderable, 0);
                        pc2->ctl.SetBool(PropertyNames::kDraggable, 0);
                        pc2->ctl.SetBool(PropertyNames::kDroppable, 0);
                    } else {
                        pc2->ctl.SetBool(prop.c_str(), v2);
                    }
                    pc2->model->props.extended[prop] = v2;   // 模型同步
                }
                commitUndo("b:" + prop);   // F2.5：勾选行
            });
            // 勾选状态回填（引擎 getter 补齐后 text-shadow-enable 等可读回）；
            // reorderable 读模型（设计态 live 恒关闭，读 live 会"勾选即弹回"）✓
            auto checkedOf = [this, prop]() -> bool {
                if (prop == PropertyNames::kReorderable) {
                    if (ControlModel* m = findModel(m_schemaTarget.GetId()))
                        return m->props.extended.value(PropertyNames::kReorderable, false);
                }
                return m_schemaTarget.GetBool(prop.c_str());
            };
            cb.SetBool(PropertyNames::kChecked, checkedOf());
            m_rowRefreshers.push_back([this, cb, prop, checkedOf]() mutable {
                cb.SetBool(PropertyNames::kChecked, checkedOf());
            });
            m_dynamicRows.push_back(cb);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
        }
        ++rows;
        genKeys += " " + key;
    }

    // P0-90：Tab 页签配色行（单色键；引擎 tab-* 直通——extended 持久化 + 引擎读回刷新）
    // 归入"页签配色"分组（可折叠，与其他分组一致）；"内容区底色"不生成——内容区被
    // 当前页 Panel 完全覆盖（applyCurrentPage 页 rect=内容区），视觉无效（用户实测）
    if (m_schemaType == "tab-control" && m_selIds.size() == 1) {
        makeGroupHeader(u8"页签配色");
        if (!m_collapsedGroups.count(u8"页签配色")) {
        static const struct { const char* key; const char* label; } kTabColors[] = {
            { PropertyNames::kTabBackground,         u8"页签背景（常态）" },
            { PropertyNames::kTabHoverBackground,    u8"页签背景（悬停）" },
            { PropertyNames::kTabSelectedBackground, u8"页签背景（选中）" },
            { PropertyNames::kTabIndicatorColor,     u8"选中指示条" },
        };
        for (const auto& tc : kTabColors) {
            makeLabel(tc.label);
            PlacedControl* pcT = placedById(m_selectedId);
            char hex[12] = "#00000000";
            if (pcT && pcT->ctl.IsValid()) {
                const UIColor cc = pcT->ctl.GetColor(tc.key);
                std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", cc.r, cc.g, cc.b, cc.a);
            }
            Control cp = takePoolRow(std::string("tabcolor:") + tc.key, [&] {
                return m_ui->CreateColorPicker(kPropInsetX, y, kColorRowW, ctlH, hex);
            });
            cp.SetRect(kPropInsetX, y, kColorRowW, ctlH);
            cp.SetString(PropertyNames::kColor, hex);
            cp.SetString(PropertyNames::kTextContent, hex);
            const std::string keyCopy = tc.key;
            m_rowRefreshers.push_back([this, cp, keyCopy]() mutable {
                PlacedControl* pcR = placedById(m_selectedId);
                if (!pcR || !pcR->ctl.IsValid()) return;
                const UIColor cc = pcR->ctl.GetColor(keyCopy.c_str());
                char h2[12];
                std::snprintf(h2, sizeof(h2), "#%02X%02X%02X%02X", cc.r, cc.g, cc.b, cc.a);
                cp.SetString(PropertyNames::kColor, h2);
                cp.SetString(PropertyNames::kTextContent, h2);
            });
            cp.SetCallback(PropertyNames::kEventColorChanged, [this, keyCopy](const Event& e) {
                if (m_propSync) return;
                writeTabColorTo(m_selectedId, keyCopy, e.GetChangedColor());
            });
            m_fixedWidthRows.insert(m_dynamicRows.size());
            m_dynamicRows.push_back(cp);
            m_dynamicRowY.push_back(y);
            y += ctlH + rowGap;
            ++rows;
            genKeys += " ";
            genKeys += keyCopy;
        }
        }
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

    // 行池：本趟未用行隐藏（保留常驻）；复用行统一刷值（读当前选中回填——
    // 新建行的创建期赋值冗余但无害，复用行由此拿到新控件值）
    hideUnusedPoolRows();
    if (!m_rowRefreshers.empty()) {
        m_propSync = true;
        for (auto& fn : m_rowRefreshers)
            if (fn) fn();
        m_propSync = false;
    }

    // 行集就绪：签名含折叠状态（切换折叠 → 重建展开组；状态不变同类 → refresh 快路径）
    {
        std::string coll;
        for (const auto& g : m_collapsedGroups) coll += g + ",";
        m_rowsBuiltSig = typeSig + "|" + coll;   // P2 多选：签名含全部选中类型
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
        m_selHandle = m_viewport->CreateHandleControl(target, 0, 0, 1, 1);
        if (!m_selHandle.IsValid()) return;
        m_viewport->SetHandleRectFilter(m_selHandle, &CanvasPane::rectFilterThunk, this);
    } else {
        m_viewport->SetHandleTarget(m_selHandle, target);
    }
}

// 取消选中/删除目标：detach（移出容器+恢复光标），实例保留复用。
void CanvasPane::detachSelHandle() {
    if (m_selHandle.IsValid())
        m_viewport->SetHandleTarget(m_selHandle, Control());
}

// 拖拽几何过滤（HandleControl setRect 前回调，target 局部逻辑坐标）：
// 网格吸附量化（四边独立贴网格；引擎 minSize 钳位在前，filter 值最终）。
int CanvasPane::rectFilterThunk(UIControlHandle, float* x, float* y,
                                float* w, float* h, void* userData) {
    return static_cast<CanvasPane*>(userData)->filterRect(*x, *y, *w, *h);
}

int CanvasPane::filterRect(float& x, float& y, float& w, float& h) {
    const float g = m_gridCell;
    const ControlModel* m = findModel(m_selectedId);
    if (!m) return 0;
    const SRect& p = m->props.rect;   // 上帧 filter 输出——锚定边推断基准

    // 纯移动（尺寸未变，Move 手柄/选框拖拽）：智能对齐优先（引擎拖动路径全覆盖），
    // 未命中轴回退网格；对齐与网格均未命中时不改动
    if (std::fabs(w - p.w) < kResizeEpsilon && std::fabs(h - p.h) < kResizeEpsilon) {
        bool hitX = false, hitY = false;
        if (m_alignEnabled) applyAlignSnap(x, y, w, h, hitX, hitY);   // 吸附到控件（独立）
        if (m_snapEnabled) {                                          // 吸附到网格（独立）
            if (!hitX) x = std::round(x / g) * g;
            if (!hitY) y = std::round(y / g) * g;
        }
        // P2 多选组拖动：主选过滤后局部位置增量 → 其余成员同步平移
        if (m_groupDrag) {
            const float dx = x - m_groupPrevX, dy = y - m_groupPrevY;
            if (dx != 0.0f || dy != 0.0f) {
                applyGroupDelta(dx, dy);
                m_groupPrevX = x;
                m_groupPrevY = y;
            }
        }
        return (m_snapEnabled || (m_alignEnabled && (hitX || hitY))) ? 1 : 0;
    }
    if (!m_snapEnabled) return 0;

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
    // 画布平移（中键拖拽 / 空格+左键拖拽）：视图操作——预览态也可用
    if (m_viewport && m_mouseSource) {
        float pwx = 0, pwy = 0;
        const bool pdown = m_mouseSource(pwx, pwy);
        const bool pInVp = (pwx >= m_areaX && pwx < m_areaX + m_vpW &&
                            pwy >= m_areaY + m_toolbarH &&
                            pwy < m_areaY + m_toolbarH + m_vpH);
        {   // 状态栏鼠标坐标（画布逻辑坐标；预览态同样更新）
            float ccx = 0, ccy = 0;
            ScreenToCanvas(pwx, pwy, ccx, ccy);
            updateStatusPos(pInVp, ccx, ccy);
        }
        const bool pDownEdge = pdown && !m_mouseWasDown;
        const bool space = m_spaceSource && m_spaceSource();
        float mx2 = 0, my2 = 0;
        const bool midDown = m_middleSource && m_middleSource(mx2, my2);
        const bool midEdge = midDown && !m_middleWasDown;
        m_middleWasDown = midDown;
        if (!m_panning && pInVp && (midEdge || (space && pDownEdge))) {
            m_panning = true;
            m_panStartSX = pwx;
            m_panStartSY = pwy;
            m_panStartX = m_panX;
            m_panStartY = m_panY;
            m_mouseWasDown = pdown;
            return;
        }
        if (m_panning) {
            if (midDown || (space && pdown)) {
                m_panX = m_panStartX + (pwx - m_panStartSX) / m_zoom;
                m_panY = m_panStartY + (pwy - m_panStartSY) / m_zoom;
                clampPan();
                applyPan();
                syncScrollBars();   // 拖动平移 → 滚动条滑块同步
            } else {
                m_panning = false;   // 松开结束
            }
            m_mouseWasDown = pdown;
            return;
        }
    }
    if (m_previewMode) return;   // 预览模式：设计器交互关闭（控件交互由引擎事件驱动）
    // Delete 键：删除选中控件（键源每帧轮询一次，返回按下沿）；
    // 属性面板编辑中（鼠标最后按下在面板内）→ Delete 归面板编辑器（SDL 轮询不感知引擎焦点）
    const bool delEdge = m_keySource && m_keySource();
    if (delEdge && !m_panelEditActive) deleteSelected();
    syncDynamicWidth();   // 动态区宽度跟随（splitter 拖动后滚动条贴边/行宽同步）

    if (!m_viewport || !m_mouseSource) return;
    float wx = 0, wy = 0;
    bool down = m_mouseSource(wx, wy);

    // 视口区域判定（窗口坐标；工具栏/分割器/面板均在区域外，不参与画布交互）
    const float vx0 = m_areaX, vy0 = m_areaY + m_toolbarH;
    const bool inVp = (wx >= vx0 && wx < vx0 + m_vpW &&
                       wy >= vy0 && wy < vy0 + m_vpH);

    float cx = 0, cy = 0;
    ScreenToCanvas(wx, wy, cx, cy);

    const bool downEdge = down && !m_mouseWasDown;
    const bool upEdge = !down && m_mouseWasDown;

    // 引擎浮层消费本帧点击（工具栏下拉/菜单项——App 回调标记）：画布交互整体跳过。
    // 弹层项显示在画布上方，引擎按 P0-52 路由 owner；设计器 SDL 轮询须同步跳过，
    // 否则该次按下被当作画布空白点击 → 误清选择。
    if (m_suppressCanvasClick) {
        m_suppressCanvasClick = false;
        m_mouseWasDown = down;
        return;
    }

    // 页签排序拖动（P0-91③）：释放时按目标位 TabMovePage 迁移；拖动中状态栏提示
    if (m_tabDragActive) {
        if (!down) {
            if (m_tabDragTo >= 0 && m_tabDragTo != m_tabDragFrom)
                moveTabPage(m_tabDragId, m_tabDragFrom, m_tabDragTo);
            m_tabDragActive = false;
            m_tabDragId.clear();
            m_tabDragFrom = m_tabDragTo = -1;
            if (m_tabDragGhost.IsValid()) {   // 拖动结束：销毁小图标
                m_tabDragGhost.Destroy();
                m_tabDragGhost = Control();
            }
        } else {
            const int to = tabItemIndexAt(m_tabDragId, cx, cy, /*clampToNearest=*/true);
            if (to >= 0 && to != m_tabDragTo) {
                m_tabDragTo = to;
                setStatusHint(u8"页序调整：释放后移动到第 " + std::to_string(to + 1) + u8" 位");
            }
            if (m_tabDragGhost.IsValid()) {   // 小图标跟随光标
                m_tabDragGhost.SetBool(PropertyNames::kVisible, 1);
                m_tabDragGhost.SetRect(cx + 12.0f, cy + 12.0f, 32.0f, 32.0f);
            }
        }
        m_mouseWasDown = down;
        return;
    }

    // Delete 归属：鼠标最后按下位置在属性面板内 → 面板编辑中（画布 Delete 忽略）。
    // 面板为 middleArea 子级（h-flow）——全局 rect = middleArea 原点 + 局部。
    if (downEdge) {
        bool inPanel = false;
        if (m_propPanel.IsValid()) {
            UIRect pr = m_propPanel.GetRect();
            Control ma = m_ui->FindControl("middleArea");
            if (ma.IsValid()) {
                const UIRect mr = ma.GetRect();
                pr.x += mr.x; pr.y += mr.y;
            }
            inPanel = (wx >= pr.x && wx < pr.x + pr.w && wy >= pr.y && wy < pr.y + pr.h);
        }
        m_panelEditActive = inPanel;
    }

    // 拖放 ghost：候选活跃时——鼠标旁跟随控件图标（画布内可见）；结束销毁
    {
        const bool curOn = !m_pendingPlaceTool.empty();
        if (curOn && !m_dragGhost.IsValid()) {
            const std::string icon = toolboxIconOf(m_pendingPlaceTool);
            if (!icon.empty()) {
                m_dragGhost = m_viewport->CreateImage(icon, 0, 0, 32.0f, 32.0f);
                if (m_dragGhost.IsValid()) {
                    m_dragGhost.SetBool(PropertyNames::kAlwaysOnTop, 1);   // z 序置顶
                    m_dragGhost.SetInt(PropertyNames::kAlpha, 180);   // 半透明
                    m_dragGhost.SetBool(PropertyNames::kVisible, 0);
                }
            }
        }
        if (curOn && m_dragGhost.IsValid()) {
            if (inVp) {
                m_dragGhost.SetBool(PropertyNames::kVisible, 1);
                m_dragGhost.SetRect(cx + 12.0f, cy + 12.0f, 32.0f, 32.0f);
            } else {
                m_dragGhost.SetBool(PropertyNames::kVisible, 0);
            }
        }
        if (!curOn && m_dragGhost.IsValid()) {
            m_dragGhost.Destroy();
            m_dragGhost = Control();
        }
    }

    // P2 框选：拖拽中更新橡皮筋矩形（画布逻辑坐标）
    if (m_bandActive && down) updateBand(cx, cy);

    // 工具拖放（拖放模式）：工具箱项按下（kEventSelect）→ 拖到画布松开 → 放置于松开处；
    // 在工具箱内松开（普通单击）→ 不放置、仅清除候选
    if (!m_pendingPlaceTool.empty() && !down) {
        std::printf("[Place] pending-drop tool=%s inVp=%d at=(%.0f,%.0f)\n",
                    m_pendingPlaceTool.c_str(), inVp ? 1 : 0, cx, cy);
        if (inVp) placeControl(m_pendingPlaceTool, cx, cy, true);   // 拖放：左上角=落点
        m_pendingPlaceTool.clear();
    }

    // 引擎 HandleControl 拖拽感知：rect 差异（filter 吸附后引擎 setRect）→
    // 同步模型/属性面板（吸附已由 filter 实时承担，此处纯同步）
    if (m_selectedId.empty() == false && m_dragId.empty() && !isSelectedTabPage()) {
        if (ControlModel* m = findModel(m_selectedId)) {
            if (PlacedControl* pc = placedById(m->props.id); pc && pc->ctl.IsValid()) {
                const UIRect vr = pc->ctl.GetRect();
                if (vr.w != m->props.rect.w || vr.h != m->props.rect.h ||
                    vr.x != m->props.rect.x || vr.y != m->props.rect.y) {
                    m->props.rect = SRect{ vr.x, vr.y, vr.w, vr.h };
                    fillPropertyPanel();
                    m_engineRectDirty = true;   // F2.5：引擎手柄拖拽/缩放中
                    m_engineRectId = m_selectedId;
                }
            }
        }
    }
    if (m_engineRectDirty && !down) {   // F2.5：松开 → 手柄拖拽/缩放一次撤销步
        m_engineRectDirty = false;
        if (!m_engineRectId.empty()) commitUndo("rect:" + m_engineRectId);
        m_engineRectId.clear();
    }

    // 拖动已选中控件
    if (!m_dragId.empty()) {
        ControlModel* m = findModel(m_dragId);
        if (!m) { m_dragId.clear(); m_mouseWasDown = down; return; }
        if (down) {
            float nx = cx - m_dragOffX, ny = cy - m_dragOffY;   // 画布全局目标
            // 智能对齐吸附（优先于网格；独立开关 m_alignEnabled）：边缘/中心最小差；命中显示参考线
            float lineX = -1.0f, lineY = -1.0f;
            if (m_alignEnabled) {
                const float w = m->props.rect.w, h = m->props.rect.h;
                const float dXs[3] = { nx, nx + w * 0.5f, nx + w };
                const float dYs[3] = { ny, ny + h * 0.5f, ny + h };
                float bestX = kAlignThreshold + 1.0f, bestY = kAlignThreshold + 1.0f;
                float snapDx = 0.0f, snapDy = 0.0f;
                for (auto& it : m_items) {
                    if (it.model->props.id == m_dragId || !it.ctl.IsValid()) continue;
                    float ogx = 0, ogy = 0;
                    globalOriginOf(it, ogx, ogy);
                    const float ow = it.model->props.rect.w, oh = it.model->props.rect.h;
                    const float oXs[3] = { ogx, ogx + ow * 0.5f, ogx + ow };
                    const float oYs[3] = { ogy, ogy + oh * 0.5f, ogy + oh };
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) {
                            const float dx = oXs[j] - dXs[i];
                            if (std::fabs(dx) < bestX) { bestX = std::fabs(dx); snapDx = dx; lineX = oXs[j]; }
                            const float dy = oYs[j] - dYs[i];
                            if (std::fabs(dy) < bestY) { bestY = std::fabs(dy); snapDy = dy; lineY = oYs[j]; }
                        }
                }
                if (bestX > kAlignThreshold) lineX = -1.0f; else nx += snapDx;
                if (bestY > kAlignThreshold) lineY = -1.0f; else ny += snapDy;
                // 参考线懒创建（橙色 1px——画布全高/全宽）
                if (!m_alignV.IsValid()) {
                    m_alignV = m_viewport->CreatePanel(0, 0, 1, 100);
                    if (m_alignV.IsValid()) {
                        m_alignV.SetBool(PropertyNames::kAlwaysOnTop, 1);   // z 序置顶
                        m_alignV.SetColor("background", UIColor{0xFF, 0x88, 0x00, 0xC0});
                        m_alignV.SetBool("border-visible", 0);
                    }
                }
                if (!m_alignH.IsValid()) {
                    m_alignH = m_viewport->CreatePanel(0, 0, 100, 1);
                    if (m_alignH.IsValid()) {
                        m_alignH.SetBool(PropertyNames::kAlwaysOnTop, 1);   // z 序置顶
                        m_alignH.SetColor("background", UIColor{0xFF, 0x88, 0x00, 0xC0});
                        m_alignH.SetBool("border-visible", 0);
                    }
                }
                // 每帧无条件更新显示状态（远离隐藏/命中显示——拖走再拖回均正确）
                const float cvH = m_contentH / m_zoom, cvW = m_contentW / m_zoom;
                if (m_alignV.IsValid()) {
                    m_alignV.SetBool(PropertyNames::kVisible, lineX >= 0);
                    if (lineX >= 0) m_alignV.SetRect(lineX, 0, 1, cvH);
                }
                if (m_alignH.IsValid()) {
                    m_alignH.SetBool(PropertyNames::kVisible, lineY >= 0);
                    if (lineY >= 0) m_alignH.SetRect(0, lineY, cvW, 1);
                }
            }
            // 网格吸附：智能未命中的轴才回退网格（智能优先）
            if (m_snapEnabled) {
                if (lineX < 0) nx = std::round(nx / m_gridCell) * m_gridCell;
                if (lineY < 0) ny = std::round(ny / m_gridCell) * m_gridCell;
            }
            // P2 多选组拖动：全局增量（局部换算前捕获；其余成员按起始快照平移）
            const float gdx = nx - m_groupStartX, gdy = ny - m_groupStartY;
            // 子控件：全局 → 容器局部（父全局原点 + WinFrame ClientPanel 内容偏移）
            if (!m_dragParentId.empty()) {
                if (PlacedControl* par = placedById(m_dragParentId)) {
                    float pgx = 0, pgy = 0;
                    globalOriginOf(*par, pgx, pgy);
                    if (par->model->props.type == "win-frame" && par->ctl.IsValid()) {
                        void* h = par->ctl.GetPtr("client-panel");
                        if (h) {
                            const auto cr = m_viewport->FromHandle(h).GetRect();
                            pgx += cr.x; pgy += cr.y;
                        }
                    }
                    nx -= pgx; ny -= pgy;
                }
            }
            m->props.rect.x = nx;
            m->props.rect.y = ny;
            if (m_dragCtl.IsValid())
                m_dragCtl.SetRect(nx, ny, m->props.rect.w, m->props.rect.h);
            if (m_groupDrag) applyGroupFromStart(gdx, gdy);   // P2 多选组拖动
            fillPropertyPanel();   // 拖动中同步属性面板（m_propSync 抑制回写）
        } else {
            if (m_alignV.IsValid()) m_alignV.SetBool(PropertyNames::kVisible, 0);
            if (m_alignH.IsValid()) m_alignH.SetBool(PropertyNames::kVisible, 0);
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
            if (m_dragOrigValid) {   // F2.5：拖动有位移 → 撤销步（单击未移动不记录）
                if (ControlModel* dm = findModel(m_dragId))
                    if (dm->props.rect.x != m_dragOrigRect.x ||
                        dm->props.rect.y != m_dragOrigRect.y)
                        commitUndo("rect:" + m_dragId);
                m_dragOrigValid = false;
            }
            m_dragId.clear();   // 松开结束拖动
            m_groupDrag = false;
            m_groupEntries.clear();
        }
        m_mouseWasDown = down;
        return;
    }

    // 按下沿：选中控件手柄带让给 HandleControl（拖拽 resize）；控件命中 → 选中并
    // 拖动；空白 → 取消选中。P2 多选：Ctrl+单击切换成员；多选成员拖动=组拖动。
    if (downEdge && inVp) {
        // Ctrl+单击：多选切换（优先于手柄让区/拖动——修饰键手势不启动拖拽）；
        // Ctrl+空白按下：启动并入式框选（松开判定，微小移动=不变）
        if (m_ctrlSource && m_ctrlSource()) {
            std::string hit;
            if (hitTest(cx, cy, hit)) {
                toggleSelection(hit);
            } else {
                beginBand(cx, cy, true);   // Ctrl+空白：并入式框选
            }
            m_mouseWasDown = down;
            return;
        }
        // Tab 页签项点击优先于手柄让区：页签带与 Tab 手柄 resize 上缘区重叠——曾被引擎
        // 截获为 resize，致"点页签选不中页/拖不动排序"（第二次点稍低才命中，用户实测两次）
        if (downEdge && inVp) {
            std::string hitItem;
            if (hitTest(cx, cy, hitItem)) {
                PlacedControl* hp = placedById(hitItem);
                if (hp && hp->model->props.type == "tab-control" && hp->ctl.IsValid()) {
                    const int itemIdx = tabItemIndexAt(hitItem, cx, cy, /*clampToNearest=*/false);
                    if (itemIdx >= 0) {
                        const nlohmann::json tabsJ = hp->model->props.extended.value(
                            "tabs", nlohmann::json::array());
                        const std::string pid = (tabsJ.is_array() && itemIdx < (int)tabsJ.size())
                            ? tabsJ[(size_t)itemIdx].value("page-id", "") : "";
                            if (!pid.empty()) {
                                setPlaceTarget(pid);
                                selectItem(pid);   // 选中该页（锁定）
                                m_tabDragActive = true;   // 排序拖动候选（释放时按目标位迁移）
                                m_tabDragId = hitItem;
                                m_tabDragFrom = itemIdx;
                                m_tabDragTo = itemIdx;
                                // 拖动光标旁小图标（复用工具箱 ghost 风格：置顶 + 半透明）
                                if (!m_tabDragGhost.IsValid()) {
                                    const std::string tIcon = toolboxIconOf("tTabs");
                                    if (!tIcon.empty()) {
                                        m_tabDragGhost = m_viewport->CreateImage(tIcon, 0, 0, 32.0f, 32.0f);
                                        if (m_tabDragGhost.IsValid()) {
                                            m_tabDragGhost.SetBool(PropertyNames::kAlwaysOnTop, 1);
                                            m_tabDragGhost.SetInt(PropertyNames::kAlpha, 200);
                                        }
                                    }
                                }
                                if (m_tabDragGhost.IsValid()) {
                                    m_tabDragGhost.SetBool(PropertyNames::kVisible, 1);
                                    m_tabDragGhost.SetRect(cx + 12.0f, cy + 12.0f, 32.0f, 32.0f);
                                }
                                std::printf("[tab-click] item=%d -> page=%s (canvas+tree synced)\n",
                                            itemIdx, pid.c_str());
                                m_mouseWasDown = down;
                                return;
                            }
                        m_mouseWasDown = down;
                        return;
                    }
                }
            }
        }
        // 手柄精确让区：HandleHitTest 命中 → HandleControl 接管拖拽；未命中
        // （本体/空白）走设计器逻辑——本体边缘（非手柄处）恢复移动能力。
        if (m_selectedId.empty() == false && m_selHandle.IsValid()) {
            int ht = 0;
            // 坐标域：m_handleAreas 基于 target getDrawRect（父链递归到视口根，
            // 视口根 rect=窗口全局位置）→ 全局域，传 SDL 窗口坐标原样
            const bool hh = m_viewport->HandleHitTest(m_selHandle, wx, wy, ht);
            if (hh) {
                if (PlacedControl* hsel = placedById(m_selectedId);
                    hsel && hsel->model->props.type == "tab-control")
                    std::printf("[tab-click] handle-hit ht=%d on selected tab (event consumed by handle)\n", ht);
                // 多选 + 主选本体移动区（HandleType::Move=1）→ 组拖动（引擎路径增量）
                if (m_selIds.size() > 1 && ht == 1) beginGroupDrag(m_selectedId);
                m_mouseWasDown = down;
                return;
            }
        }
        std::string hit;
        if (hitTest(cx, cy, hit)) {
            // TabControl 点击语义（用户定稿）：
            //   内容区（当前页 rect 内）→ 直接选中该页 Panel（单击即选，不经延迟框选）；
            //   页签项（点击引发引擎切换——本帧 currentIndex 变化）→ 选中新当前页；
            //   其余（空白页签/同项重按）→ 选中整个 Tab 控件本体。
            // 页 Panel 不建手柄（setSelection 侧）且此处早退 → 无法被拖出 Tab 区。
            if (PlacedControl* hpcTab = placedById(hit);
                hpcTab && hpcTab->model->props.type == "tab-control" && hpcTab->ctl.IsValid()) {
                // 精确判定（P0-91① TabGetItemRect——替代 hovered-tab 读回：其依赖最近一次
                // MouseMove，快速移动+点击取到陈旧值，"点项/空白"误判）：
                //   ≥0=点中页签项 → 选中该项的页；-1 且落在当前页 rect（内容区）→ 选中
                //   当前页；否则=空白页签 → 整个 Tab
                const int itemIdx = tabItemIndexAt(hit, cx, cy, /*clampToNearest=*/false);
                const nlohmann::json tabsJ =
                    hpcTab->model->props.extended.value("tabs", nlohmann::json::array());
                std::string pageId;
                if (itemIdx >= 0 && tabsJ.is_array() && itemIdx < (int)tabsJ.size())
                    pageId = tabsJ[(size_t)itemIdx].value("page-id", "");
                if (pageId.empty()) {
                    const std::string curPage = resolvedParentIdOf(hit);
                    if (curPage != hit)
                        if (PlacedControl* pg = placedById(curPage); pg && pg->ctl.IsValid()) {
                            float tx0 = 0, ty0 = 0;
                            globalOriginOf(*hpcTab, tx0, ty0);
                            const UIRect pr = pg->ctl.GetRect();   // 页 rect（Tab 局部=引擎内容区）
                            if (cx >= tx0 + pr.x && cx < tx0 + pr.x + pr.w &&
                                cy >= ty0 + pr.y && cy < ty0 + pr.y + pr.h)
                                pageId = curPage;
                        }
                }
                if (!pageId.empty()) {
                    setPlaceTarget(pageId);
                    selectItem(pageId);   // 直接选中页 Panel（锁定——不拖动）
                    if (itemIdx >= 0) {   // 页签项：进入排序拖动候选（释放时按目标位迁移）
                        m_tabDragActive = true;
                        m_tabDragId = hit;
                        m_tabDragFrom = itemIdx;
                        m_tabDragTo = itemIdx;
                    }
                    m_mouseWasDown = down;
                    return;
                }
                // 空白页签：选中整个 Tab——不早退，落入标准路径（可拖动移动 Tab）
                setPlaceTarget(hit);
            }
            // Tab 页 Panel（点内容空白区）：单击即选、禁拖动（几何由 Tab 管理，锁定在 Tab 内）
            if (isTabPage(hit)) {
                setPlaceTarget(hit);
                selectItem(hit);
                m_mouseWasDown = down;
                return;
            }
            const bool inSel = std::find(m_selIds.begin(), m_selIds.end(), hit) != m_selIds.end();
            // 容器内容区（未选中）：延迟判定——拖动=框选其子控件，单击=选中容器本体
            // （选中后拖动=移动容器——HandleControl 本体路径；WinFrame 标题栏不在此列）
            if (!inSel) {
                PlacedControl* hpc = placedById(hit);
                if (hpc && isContainerType(hpc->model->props.type)) {
                    Control par;
                    SRect cr;
                    if (resolveContainer(hit, par, cr) &&
                        cx >= cr.x && cx < cr.x + cr.w && cy >= cr.y && cy < cr.y + cr.h) {
                        beginBand(cx, cy, false);   // 容器内容区：延迟判定（拖动=框选子控件）
                        m_mouseWasDown = down;
                        return;
                    }
                }
            }
            if (inSel && m_selIds.size() > 1) {
                // 多选成员：保留选择集，命中项升为主选（手柄切换）→ 组拖动（设计器路径）
                if (m_selectedId != hit) {
                    std::vector<std::string> ids = m_selIds;
                    ids.erase(std::remove(ids.begin(), ids.end(), hit), ids.end());
                    ids.push_back(hit);
                    setSelection(std::move(ids));
                }
            } else {
                selectItem(hit);
            }
            if (ControlModel* m = findModel(hit)) {
                // 全局域拖动偏移（子控件经 helper 全局化；拖动中父不动——记父 id 转局部）
                m_dragParentId.clear();
                if (PlacedControl* pc = placedById(hit)) {
                    if (pc->parentId.empty()) {
                        m_dragOffX = cx - pc->model->props.rect.x;
                        m_dragOffY = cy - pc->model->props.rect.y;
                    } else {
                        float gx0 = 0, gy0 = 0;
                        globalOriginOf(*pc, gx0, gy0);
                        m_dragOffX = cx - gx0;
                        m_dragOffY = cy - gy0;
                        m_dragParentId = pc->parentId;
                    }
                    m_dragCtl = pc->ctl;
                }
                m_dragId = hit;
                beginGroupDrag(hit);
                if (ControlModel* dm = findModel(hit)) {   // F2.5：记录起始 rect（未移动不记历史）
                    m_dragOrigRect = dm->props.rect;
                    m_dragOrigValid = true;
                }
            }
        } else {
            // P2 框选：空白处按下 → 启动橡皮筋（松开判定空白点击/框选）
            beginBand(cx, cy, false);
        }
        m_mouseWasDown = down;
        return;
    }

    if (upEdge) {
        // 松开：辅助线隐藏（引擎拖动路径（filter）松开后不再回调——统一在此收尾）
        if (m_alignV.IsValid()) m_alignV.SetBool(PropertyNames::kVisible, 0);
        if (m_alignH.IsValid()) m_alignH.SetBool(PropertyNames::kVisible, 0);
        if (m_bandActive) finalizeBand(cx, cy);   // P2 框选：松开判定
        m_groupDrag = false;   // P2 多选组拖动收尾（引擎路径）
        m_groupEntries.clear();
    }
    m_mouseWasDown = down;
}

CanvasPane::PlacedControl* CanvasPane::placedById(const std::string& id) {
    for (auto& it : m_items)
        if (it.model->props.id == id) return &it;
    return nullptr;
}

// P1-a：控件画布全局原点 = 父链逐层（局部 rect + WinFrame ClientPanel 内容偏移）+ 自身局部 rect
void CanvasPane::globalOriginOf(const PlacedControl& pc, float& gx, float& gy) {
    gx = 0.0f; gy = 0.0f;
    std::vector<const PlacedControl*> chain;
    for (const PlacedControl* p = placedById(pc.parentId); p != nullptr;
         p = placedById(p->parentId))
        chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const PlacedControl* par = *it;
        if (par->ctl.IsValid()) {
            const auto r = par->ctl.GetRect();
            gx += r.x; gy += r.y;
        } else {
            gx += par->model->props.rect.x; gy += par->model->props.rect.y;
        }
        if (par->model->props.type == "win-frame" && par->ctl.IsValid()) {
            void* h = par->ctl.GetPtr("client-panel");
            if (h) {
                const auto cr = m_viewport->FromHandle(h).GetRect();
                gx += cr.x; gy += cr.y;
            }
        }
    }
    gx += pc.model->props.rect.x;
    gy += pc.model->props.rect.y;
}

// 智能对齐：x/y 为选中控件局部坐标（引用修正）——与其它控件边缘/中心最小差（6px）
// 命中轴吸附 + 参考线每帧更新；hitX/hitY 输出命中态（网格回退用）
void CanvasPane::applyAlignSnap(float& x, float& y, float w, float h,
                                bool& hitX, bool& hitY) {
    hitX = false; hitY = false;
    // 局部 → 全局（子控件经父链换算）
    float gx = x, gy = y;
    PlacedControl* selfPc = placedById(m_selectedId);
    if (selfPc && !selfPc->parentId.empty()) {
        if (PlacedControl* par = placedById(selfPc->parentId)) {
            float pgx = 0, pgy = 0;
            globalOriginOf(*par, pgx, pgy);
            if (par->model->props.type == "win-frame" && par->ctl.IsValid()) {
                void* hh = par->ctl.GetPtr("client-panel");
                if (hh) {
                    const auto cr = m_viewport->FromHandle(hh).GetRect();
                    pgx += cr.x; pgy += cr.y;
                }
            }
            gx += pgx; gy += pgy;
        }
    }
    float lineX = -1.0f, lineY = -1.0f;
    {
        const float dXs[3] = { gx, gx + w * 0.5f, gx + w };
        const float dYs[3] = { gy, gy + h * 0.5f, gy + h };
        float bestX = kAlignThreshold + 1.0f, bestY = kAlignThreshold + 1.0f;
        float snapDx = 0.0f, snapDy = 0.0f;
        for (auto& it : m_items) {
            if (it.model->props.id == m_selectedId || !it.ctl.IsValid()) continue;
            float ogx = 0, ogy = 0;
            globalOriginOf(it, ogx, ogy);
            const float ow = it.model->props.rect.w, oh = it.model->props.rect.h;
            const float oXs[3] = { ogx, ogx + ow * 0.5f, ogx + ow };
            const float oYs[3] = { ogy, ogy + oh * 0.5f, ogy + oh };
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const float dx = oXs[j] - dXs[i];
                    if (std::fabs(dx) < bestX) { bestX = std::fabs(dx); snapDx = dx; lineX = oXs[j]; }
                    const float dy = oYs[j] - dYs[i];
                    if (std::fabs(dy) < bestY) { bestY = std::fabs(dy); snapDy = dy; lineY = oYs[j]; }
                }
        }
        if (bestX > kAlignThreshold) lineX = -1.0f; else { x += snapDx; hitX = true; }
        if (bestY > kAlignThreshold) lineY = -1.0f; else { y += snapDy; hitY = true; }
    }
    // 参考线懒创建（橙色 1px——画布全高/全宽）
    if (!m_alignV.IsValid()) {
        m_alignV = m_viewport->CreatePanel(0, 0, 1, 100);
        if (m_alignV.IsValid()) {
            m_alignV.SetBool(PropertyNames::kAlwaysOnTop, 1);   // z 序置顶
            m_alignV.SetColor("background", UIColor{0xFF, 0x88, 0x00, 0xC0});
            m_alignV.SetBool("border-visible", 0);
        }
    }
    if (!m_alignH.IsValid()) {
        m_alignH = m_viewport->CreatePanel(0, 0, 100, 1);
        if (m_alignH.IsValid()) {
            m_alignH.SetBool(PropertyNames::kAlwaysOnTop, 1);   // z 序置顶
            m_alignH.SetColor("background", UIColor{0xFF, 0x88, 0x00, 0xC0});
            m_alignH.SetBool("border-visible", 0);
        }
    }
    const float cvH = m_contentH / m_zoom, cvW = m_contentW / m_zoom;
    if (m_alignV.IsValid()) {
        m_alignV.SetBool(PropertyNames::kVisible, lineX >= 0);
        if (lineX >= 0) m_alignV.SetRect(lineX, 0, 1, cvH);
    }
    if (m_alignH.IsValid()) {
        m_alignH.SetBool(PropertyNames::kVisible, lineY >= 0);
        if (lineY >= 0) m_alignH.SetRect(0, lineY, cvW, 1);
    }
}

// 右侧控件树：select 事件 → 选中画布控件（m_treeSync 抑制编程式回环）。
// 画布（Bench）控件树的根节点：固定内部键（不与控件 id 冲突——重命名校验保留），
// 标签展示 Bench 控件 id（用户需求：根节点显示 Bench 的控件 id）
static constexpr const char* kBenchTreeNode = "@bench";

std::string CanvasPane::benchTreeNodeLabel() const {
    return m_benchId.empty() ? u8"画布" : (u8"画布 " + m_benchId);
}

void CanvasPane::SetCanvasTree(Control tree) {
    m_canvasTree = tree;
    if (!m_canvasTree.IsValid()) return;
    m_ui->TreeViewAddNode(m_canvasTree, "", kBenchTreeNode, benchTreeNodeLabel());
    m_canvasTree.SetCallback(PropertyNames::kEventSelect, [this](const Event& e) {
        if (m_treeSync) return;
        const std::string id = e.GetNodeId();
        if (id == kBenchTreeNode) {   // 画布根节点 → 选中画布（Bench 属性）
            selectBench();
            setPlaceTarget(std::string());
            return;
        }
        if (id.empty() || !findModel(id)) return;   // 空选/未知节点忽略
        selectItem(id);
        syncTabCurrentToPage(id);   // 页节点 → 所属 TabControl 切到该页（所见即所编/所放）
        // P1-b 通道②：树选中容器 → 设为当前放置目标；选中非容器 → 回画布根
        const ControlModel* m = findModel(id);
        setPlaceTarget(m && isContainerType(m->props.type) ? id : std::string());
    });
}

// P1-b：主实例 statusBar 接线（放置目标提示；statusHint 段）。
void CanvasPane::SetStatusTarget(Control statusBar) {
    m_statusTarget = statusBar;
    refreshPlaceTargetHint();
    refreshStatusRight();
}

// P1-b：容器 id → 挂载父控件（WinFrame 严格 ClientPanel）+ 内容区全局 rect。
// 内容区全局原点经 globalOriginOf（父链累加 + ClientPanel 偏移——嵌套容器天然正确）。
bool CanvasPane::resolveContainer(const std::string& id, Control& outParent, SRect& outContentRect) {
    PlacedControl* pc = placedById(id);
    if (!pc || !pc->ctl.IsValid() || !isContainerType(pc->model->props.type)) return false;
    float gx = 0, gy = 0;
    globalOriginOf(*pc, gx, gy);
    if (pc->model->props.type == "win-frame") {
        void* h = pc->ctl.GetPtr("client-panel");
        if (!h) return false;
        outParent = m_viewport->FromHandle(h);
        if (!outParent.IsValid()) return false;
        const auto cr = outParent.GetRect();
        outContentRect = { gx + cr.x, gy + cr.y, cr.w, cr.h };
        return true;
    }
    if (pc->model->props.type == "tab-control") {
        // 放置目标 = 当前页 Panel（current-index → 模型 tabs[i].page-id）
        const int cur = pc->ctl.GetInt(PropertyNames::kTabCurrentIndex);   // 运行时键 "currentIndex"（camelCase）
        const nlohmann::json tabs = pc->model->props.extended.value("tabs", nlohmann::json::array());
        if (!tabs.is_array() || cur < 0 || cur >= (int)tabs.size()) return false;
        PlacedControl* pagePc = placedById(tabs[(size_t)cur].value("page-id", ""));
        if (!pagePc || !pagePc->ctl.IsValid()) return false;
        outParent = pagePc->ctl;
        float px = 0, py = 0;
        globalOriginOf(*pagePc, px, py);
        const auto pr = pagePc->ctl.GetRect();
        outContentRect = { px, py, pr.w, pr.h };
        return true;
    }
    outParent = pc->ctl;
    const auto r = pc->ctl.GetRect();
    outContentRect = { gx, gy, r.w, r.h };
    return true;
}

// P1-b：更新当前放置目标（空=画布根）+ 状态栏提示。
void CanvasPane::setPlaceTarget(const std::string& id) {
    if (m_placeTargetId == id) return;
    m_placeTargetId = id;
    refreshPlaceTargetHint();
}

void CanvasPane::refreshPlaceTargetHint() {
    if (!m_statusTarget.IsValid() || !m_ui) return;
    std::string text = u8"放置目标：";
    const ControlModel* m = m_placeTargetId.empty() ? nullptr : findModel(m_placeTargetId);
    if (m) {
        text += m->props.type + " " + m->props.id;
    } else {
        m_placeTargetId.clear();
        text += u8"画布根";
    }
    setStatusHint(text);
}

// 状态栏段写入（引擎 P0-53 已修复：updateStatusItemText 文本变化即触发 relayout）。
void CanvasPane::writeStatusItem(const char* itemId, const std::string& text) {
    if (!m_statusTarget.IsValid() || !m_ui) return;
    m_ui->StatusBarSetItemText(m_statusTarget, itemId, text);
}

void CanvasPane::setStatusHint(const std::string& text) {
    writeStatusItem("statusHint", text);
}

// ══════════════ 项目/布局 IO（菜单：新建/打开/保存/另存为/导入 JSON/导出 JSON）══════════════

// UTF-8 路径 → filesystem::path（MSVC 文件流窄字符按 ANSI 解释——中文/非 ASCII
// 路径会打开失败或生成乱码文件名；u8path 走宽字符路径）
// 超长路径（>250 字符）加 \\?\ 前缀规避 Windows MAX_PATH=260 限制——不依赖系统
// LongPathsEnabled 注册表项；前缀要求绝对且已规范化路径（relative → CWD 展开）
static std::filesystem::path utf8Path(const std::string& s) {
    std::filesystem::path p = std::filesystem::u8path(s);
#ifdef _WIN32
    if (p.wstring().size() > 250) {
        std::wstring ws = std::filesystem::absolute(p).lexically_normal().wstring();
        if (ws.rfind(L"\\\\?\\", 0) != 0) {
            if (ws.rfind(L"\\\\", 0) == 0)
                ws = L"\\\\?\\UNC\\" + ws.substr(2);
            else
                ws = L"\\\\?\\" + ws;
        }
        return std::filesystem::path(ws);
    }
#endif
    return p;
}

// 引擎控件类型串 → 工具箱 id（controlTypeOf 反查；加载/打开路径用）
static std::string toolboxIdOf(const std::string& type) {
    static const char* kAll[] = {
        "tBtn", "tLabel", "tImage", "tCheck", "tEdit", "tArea", "tBar",
        "tSlider", "tScroll", "tShape", "tPanel", "tWin", "tSplit", "tTabs",
        "tCombo", "tList", "tTree", "tNum", "tColor", "tMenu", "tStatus",
        "tAni", "tDialog", "tCtx", "tPopup", "tConfirm",
    };
    for (const char* tb : kAll)
        if (controlTypeOf(tb) == type) return tb;
    return "";
}

// 单个模型 → 引擎兼容属性 JSON（不含 children；保存 .csd 与导出 JSON 共用）。
// schema 感知：仅导出该类型 def（+common）声明的键——strict 校验零外溢
// （模型含放置期记账键如 font-size/caption，类型不支持时不得外发）。
static nlohmann::json modelPropsJson(const ControlModel& m, const nlohmann::json& defs) {
    nlohmann::json j;
    j["type"] = m.props.type;
    if (!m.props.id.empty()) j["id"] = m.props.id;
    j["rect"] = { { "x", m.props.rect.x }, { "y", m.props.rect.y },
                  { "w", m.props.rect.w }, { "h", m.props.rect.h } };
    if (!m.props.visible) j["visible"] = false;
    const auto& ex = m.props.extended;
    // 支持键集合（与属性面板 merged 同口径：类型 def + common）
    std::set<std::string> supported;
    const bool hasDefs = defs.is_object();
    if (hasDefs && defs.contains(m.props.type) && defs[m.props.type].contains("properties"))
        for (auto it = defs[m.props.type]["properties"].begin();
             it != defs[m.props.type]["properties"].end(); ++it)
            supported.insert(it.key());
    if (hasDefs && defs.contains("common") && defs["common"].contains("properties"))
        for (auto it = defs["common"]["properties"].begin();
             it != defs["common"]["properties"].end(); ++it)
            supported.insert(it.key());
    auto isSupported = [&](const std::string& k) {
        return !hasDefs || supported.count(k) != 0;
    };
    if (ex.contains("enabled") && isSupported("enabled")) j["enabled"] = ex["enabled"];
    // 颜色：仅导出该类型声明的组/态子集（resolveColorGroups 口径）
    if (ex.contains("colors") && ex["colors"].is_object() && isSupported("colors")) {
        nlohmann::json out = nlohmann::json::object();
        const auto groups = resolveColorGroups(defs, defs.contains(m.props.type)
            ? defs[m.props.type] : nlohmann::json::object());
        for (auto& [grp, states] : ex["colors"].items()) {
            if (!states.is_object()) continue;
            auto git = groups.find(grp);
            if (git == groups.end()) continue;   // 类型未声明该色组——不外发
            nlohmann::json st = nlohmann::json::object();
            for (auto& [sname, hex] : states.items()) {
                bool declared = false;
                for (const auto& s : git->second)
                    if (s == sname) { declared = true; break; }
                if (declared && hex.is_string()) st[sname] = hex;
            }
            if (!st.empty()) out[grp] = std::move(st);
        }
        if (!out.empty()) j["colors"] = std::move(out);
    }
    if (ex.contains("caption") && isSupported("caption")) j["caption"] = ex["caption"];
    // 字体：对象形态 {name,size} + 顶层 font-size 双发（各类型解析器覆盖面差异：
    // label/button/edit-box/combo-box 读 font 对象；list-view/status-bar/tab-control 读顶层）
    const bool wantFont = (ex.contains("font") && isSupported("font")) ||
                          (ex.contains("font-size") && isSupported("font-size"));
    if (wantFont) {
        nlohmann::json f = nlohmann::json::object();
        if (ex.contains("font") && ex["font"].is_string() && isSupported("font"))
            f["name"] = ex["font"];
        if (ex.contains("font-size") && ex["font-size"].is_number() && isSupported("font-size"))
            f["size"] = (int)ex["font-size"].get<float>();
        if (!f.empty()) j["font"] = std::move(f);
        if (ex.contains("font-size") && ex["font-size"].is_number() && isSupported("font-size"))
            j["font-size"] = (int)ex["font-size"].get<float>();
    }
    if (ex.contains("layout") && ex["layout"].is_string() && isSupported("layout"))
        j["layout"] = { { "type", ex["layout"].get<std::string>() } };
    if (ex.contains("anchor") && ex["anchor"].is_string() && isSupported("anchor"))
        j["anchor"] = ex["anchor"];
    if ((ex.contains("anchor-offset-x") || ex.contains("anchor-offset-y")) &&
        isSupported("anchorOffset")) {
        j["anchorOffset"] = { { "left", ex.value("anchor-offset-x", 0.0f) },
                              { "top", ex.value("anchor-offset-y", 0.0f) } };
    }
    for (auto& [k, v] : ex.items()) {
        static const char* kHandled[] = {
            "enabled", "colors", "caption", "font", "font-size", "color",
            "layout", "anchor", "anchor-offset-x", "anchor-offset-y" };
        bool handled = false;
        for (const char* h : kHandled)
            if (k == h) { handled = true; break; }
        if (handled) continue;
        if (!isSupported(k)) continue;   // 类型不支持的键不外发（strict）
        if (!j.contains(k)) j[k] = v;
    }
    return j;
}

// 模型树 → 引擎兼容布局 JSON（顶层 viewport + controls；保存 .csd 的 layout 段与导出共用）
nlohmann::json CanvasPane::serializeLayoutJson() const {
    const nlohmann::json defs = m_schema ? (*m_schema)["$defs"] : nlohmann::json::object();
    nlohmann::json j;
    {
        // 画布（Bench）属性随导出：背景色（alpha=0 透明——引擎不填充/透出底层）、
        // 缩放模式（off 省略）、id（代码引用——引擎 viewport.id 键待支持，先携带）
        nlohmann::json vp = { { "width", m_designW }, { "height", m_designH } };
        char hex8[12];
        std::snprintf(hex8, sizeof(hex8), "#%02X%02X%02X%02X",
                      m_benchBg.r, m_benchBg.g, m_benchBg.b, m_benchBg.a);
        vp["background"] = hex8;
        if (m_benchScaleMode == 1) vp["scale-mode"] = "fit";
        else if (m_benchScaleMode == 2) vp["scale-mode"] = "stretch";
        if (!m_benchId.empty()) vp["id"] = m_benchId;
        j["viewport"] = std::move(vp);
    }
    std::function<nlohmann::json(const ControlModel&)> node = [&](const ControlModel& m) {
        nlohmann::json jj = modelPropsJson(m, defs);
        // 动画：playing 缺省按放置语义（放置即播放）——旧项目模型未记录时导出补 true
        if (m.props.type == "animation" && !jj.contains("playing")) jj["playing"] = true;
        if (m.props.type == "tab-control") {
            // 页 Panel 入 tabs[i].page（引擎 JSON 形态）；page-id 为设计器记账——不外发
            nlohmann::json tabsOut = nlohmann::json::array();
            const auto& tabs = m.props.extended.value("tabs", nlohmann::json::array());
            if (tabs.is_array())
                for (const auto& t : tabs) {
                    if (!t.is_object()) continue;
                    nlohmann::json entry = { { "title", t.value("title", "") } };
                    for (const auto& it : m_items)
                        if (it.model->props.id == t.value("page-id", "")) {
                            entry["page"] = node(*it.model);
                            break;
                        }
                    tabsOut.push_back(std::move(entry));
                }
            jj["tabs"] = std::move(tabsOut);
            return jj;   // 不输出 children（页 Panel 已入 tabs[i].page）
        }
        nlohmann::json kids = nlohmann::json::array();
        for (const auto& it : m_items)
            if (it.parentId == m.props.id) kids.push_back(node(*it.model));
        if (!kids.empty()) jj["children"] = std::move(kids);
        return jj;
    };
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& it : m_items)
        if (it.parentId.empty()) arr.push_back(node(*it.model));
    j["controls"] = std::move(arr);
    if (!m_resourceProviders.empty()) {   // 布局级资源挂载点（引擎 LoadLayout 顶层键）
        nlohmann::json rp = nlohmann::json::array();
        for (const auto& p : m_resourceProviders)
            rp.push_back({ { "name", p.first }, { "path", p.second } });
        j["resourceProviders"] = std::move(rp);
    }
    return j;
}

// ─────────────────────────── 事件面板（F4） ───────────────────────────
// 事件类型注册表（04 §3.2）：JSON 导出名 / Binding 事件名 / 适用控件类型（逗号集）
struct EventSpec { const char* json; const char* bind; const char* types; };
static const EventSpec kEventSpecs[] = {
    { "onClick",            "click",             "button,label,check-box,combo-box,color-picker,numeric-up-down" },
    { "onTextChanged",      "text-changed",      "edit-box,text-area" },
    { "onEnter",            "enter",             "edit-box,text-area" },
    { "onSelectionChanged", "selection-changed", "combo-box,list-view" },
    { "onSelect",           "select",            "tree-view,list-view" },
    { "onExpand",           "expand",            "tree-view" },
    { "onCollapse",         "collapse",          "tree-view" },
    { "onCheckChanged",     "check-changed",     "check-box" },
    { "onValueChanged",     "value-changed",     "slider,scroll-bar,numeric-up-down,progress-bar" },
    { "onColorChanged",     "color-changed",     "color-picker" },
    { "onPositionChanged",  "position-changed",  "win-frame" },
    { "onClose",            "close",             "win-frame" },
};

static const EventSpec* findEventSpecByJson(const std::string& json) {
    for (const auto& sp : kEventSpecs)
        if (json == sp.json) return &sp;
    return nullptr;
}

// 模型属性 → 引擎控件（加载/打开路径；复用设计期写回助手——颜色/文本特例一致）
void CanvasPane::applyModelToEngine(PlacedControl& pc) {
    if (!pc.ctl.IsValid()) return;
    ControlModel& m = *pc.model;
    Control ctl = pc.ctl;
    auto& ex = m.props.extended;
    // 导入归一：树/状态栏项 background-color 对象形态（{normal,hover}——引擎导出/外部
    // 布局口径）拆回独立键（编辑模型口径：background-color 串 + hover-background-color）
    if (m.props.type == "tree-view" || m.props.type == "status-bar") {
        std::function<void(nlohmann::json&)> splitBg = [&](nlohmann::json& arr) {
            if (!arr.is_array()) return;
            for (auto& it : arr) {
                if (!it.is_object()) continue;
                if (it.contains("background-color") && it["background-color"].is_object()) {
                    const nlohmann::json bc = it["background-color"];
                    if (bc.contains("normal") && bc["normal"].is_string())
                        it["background-color"] = bc["normal"];
                    else
                        it.erase("background-color");
                    if (bc.contains("hover") && bc["hover"].is_string())
                        it["hover-background-color"] = bc["hover"];
                }
                if (it.contains("children")) splitBg(it["children"]);
            }
        };
        if (ex.contains("items")) splitBg(ex["items"]);
    }
    if (!m.props.visible) ctl.SetBool(PropertyNames::kVisible, false);
    if (ex.contains("enabled") && ex["enabled"].is_boolean())
        ctl.SetBool(PropertyNames::kEnabled, ex["enabled"].get<bool>());
    // 颜色（组/态 → writeColorSlotTo 语义：text/text-shadow 走内部 Label）
    if (ex.contains("colors") && ex["colors"].is_object()) {
        for (auto& [grp, states] : ex["colors"].items()) {
            if (!states.is_object()) continue;
            for (auto& [st, hex] : states.items()) {
                if (!hex.is_string()) continue;
                const std::string prop = grp + (st == "normal" ? "" : "." + st);
                writeColorSlotTo(m.props.id, prop, grp, hexToColor(hex.get<std::string>()));
            }
        }
    }
    if (ex.contains("caption") && ex["caption"].is_string())
        applyCaptionTo(m.props.id, ex["caption"].get<std::string>());
    // 字体（运行时通用通道：kFont 字符串别名 + kFontSize）
    if (ex.contains("font") && ex["font"].is_string())
        ctl.SetString(PropertyNames::kFont, ex["font"].get<std::string>());
    if (ex.contains("font-size") && ex["font-size"].is_number())
        ctl.SetInt(PropertyNames::kFontSize, ex["font-size"].get<int>());
    // 结构化数据（模型 → 引擎同步；复用文本编辑路径的同步函数）
    if (m.props.type == "combo-box" && ex.contains("items")) syncComboItems(m);
    if (m.props.type == "status-bar" && ex.contains("items"))
        syncStatusBarItems(m, nlohmann::json::array());   // 新建控件无旧段
    if (m.props.type == "list-view" && ex.contains("columns"))
        syncListColumns(m, 0);   // 新建控件无旧列
    if (m.props.type == "tree-view" && ex.contains("items")) syncTreeItems(m);
    if (m.props.type == "menu-bar" && ex.contains("menus"))
        syncMenus(m, nlohmann::json::array());   // 新建控件无旧菜单
    // P0-90：Tab 页签配色回放（tab-* 单色键；extended 字符串持久化）
    if (m.props.type == "tab-control") {
        static const char* kTabColorKeys[] = { PropertyNames::kTabBackground,
            PropertyNames::kTabHoverBackground, PropertyNames::kTabSelectedBackground,
            PropertyNames::kTabIndicatorColor, PropertyNames::kTabContentBackground };
        for (const char* k : kTabColorKeys) {
            auto it = ex.find(k);
            if (it != ex.end() && it->is_string())
                ctl.SetColor(k, hexToColor(it->get<std::string>()));
        }
    }
    if (m.props.type == "tab-control" && ex.contains("tabs"))
        syncTabs(m, nlohmann::json::array());    // 新建控件无旧页（页 Panel 由 walk 预建时幂等挂接）
    // 容器布局（锚定生效前提——先于锚定）
    if (ex.contains("layout") && ex["layout"].is_string())
        ctl.SetEnum(PropertyNames::kLayout, ex["layout"].get<std::string>());
    // 锚定（子控件级：父容器/根 child-id + anchor；顶层首次自动切根布局）
    if (ex.contains("anchor") && ex["anchor"].is_string()) {
        Control par = anchorParentOf(pc);
        if (par.IsValid()) {
            if (pc.parentId.empty() && par.GetEnum(PropertyNames::kLayout) != "anchor")
                par.SetEnum(PropertyNames::kLayout, "anchor");
            par.SetString(PropertyNames::kChildTargetId, m.props.id);
            par.SetEnum(PropertyNames::kChildAnchor, ex["anchor"].get<std::string>());
        }
    }
    if (ex.contains("anchor-offset-x") || ex.contains("anchor-offset-y")) {
        Control par = anchorParentOf(pc);
        if (par.IsValid()) {
            par.SetString(PropertyNames::kChildTargetId, m.props.id);
            par.SetFloat(PropertyNames::kChildAnchorOffsetX, ex.value("anchor-offset-x", 0.0f));
            par.SetFloat(PropertyNames::kChildAnchorOffsetY, ex.value("anchor-offset-y", 0.0f));
        }
    }
    // 其余 extended 键：schema 类型分派（enum→SetEnum / boolean / integer / number /
    // x-color→SetColor / string→writeStringPropTo；未知键按 JSON 类型兜底）
    const auto& defs = m_schema ? (*m_schema)["$defs"] : nlohmann::json::object();
    for (auto& [k, v] : ex.items()) {
        static const char* kHandled[] = {
            "enabled", "colors", "caption", "font", "font-size", "color",
            "layout", "anchor", "anchor-offset-x", "anchor-offset-y" };
        bool handled = false;
        for (const char* h : kHandled)
            if (k == h) { handled = true; break; }
        if (handled) continue;
        if (m.props.type == "combo-box" && k == "items") continue;
        if (m.props.type == "status-bar" && k == "items") continue;
        if (m.props.type == "list-view" && (k == "columns" || k == "rows")) continue;
        if (m.props.type == "tree-view" && k == "items") continue;
        // schema 规格（类型 def → common 兜底）
        const nlohmann::json* spec = nullptr;
        if (defs.contains(m.props.type) && defs[m.props.type].contains("properties") &&
            defs[m.props.type]["properties"].contains(k))
            spec = &defs[m.props.type]["properties"][k];
        else if (defs.contains("common") && defs["common"].contains("properties") &&
                 defs["common"]["properties"].contains(k))
            spec = &defs["common"]["properties"][k];
        bool isColorKey = false;
        if (spec) {
            if (spec->value("x-color", false)) isColorKey = true;
            else if (spec->contains("$ref") && (*spec)["$ref"].is_string()) {
                const std::string ref = (*spec)["$ref"].get<std::string>();
                const std::string dn = ref.substr(ref.find_last_of('/') + 1);
                if (defs.contains(dn) && defs[dn].value("x-color", false)) isColorKey = true;
            }
        }
        if (isColorKey && v.is_string()) {
            ctl.SetColor(k.c_str(), hexToColor(v.get<std::string>()));
        } else if (spec && spec->contains("enum") && v.is_string()) {
            ctl.SetEnum(k.c_str(), v.get<std::string>());
        } else if (spec && spec->value("type", "") == "boolean") {
            if (v.is_boolean()) ctl.SetBool(k.c_str(), v.get<bool>());
        } else if (spec && spec->value("type", "") == "integer") {
            if (v.is_number()) ctl.SetInt(k.c_str(), v.get<int>());
        } else if (spec && spec->value("type", "") == "number") {
            if (v.is_number()) ctl.SetFloat(k.c_str(), v.get<float>());
        } else if (v.is_boolean()) {
            ctl.SetBool(k.c_str(), v.get<bool>());
        } else if (v.is_number_integer()) {
            ctl.SetInt(k.c_str(), v.get<int>());
        } else if (v.is_number()) {
            ctl.SetFloat(k.c_str(), v.get<float>());
        } else if (v.is_string()) {
            writeStringPropTo(m.props.id, k, v.get<std::string>());
        }
    }
    // P0-94 设计态：引擎内置拖拽旗标强制关闭（须置于通用 extended 应用**之后**——
    // 否则被覆盖不生效；宿主拖动接管编辑交互，运行时行为经预览/导出生效）。
    // 三个旗标一并关闭：reorderable（内置换序）+ draggable（起拖门控）+ droppable（自目标
    // 解析）——任一开启即让引擎拖拽在设计画布内激活，与宿主拖动互斗（双 ghost/换序失效）✗
    ctl.SetBool(PropertyNames::kReorderable, 0);
    ctl.SetBool(PropertyNames::kDraggable, 0);
    ctl.SetBool(PropertyNames::kDroppable, 0);
    // 事件实演：模型 events → 引擎回调（设计期打点；导出后由宿主 RegisterAction 接管）
    // 注意：不得用空回调"解绑"——绑定层会把空 std::function 包成有效 thunk，
    // 事件触发即 std::bad_function_call（VC 运行库异常）；加载控件为新建无需解绑。
    if (ex.contains("events") && ex["events"].is_object()) {
        for (auto e = ex["events"].begin(); e != ex["events"].end(); ++e) {
            if (!e.value().is_string()) continue;
            if (const EventSpec* sp = findEventSpecByJson(e.key())) {
                const std::string cid = m.props.id;
                const std::string handler = e.value().get<std::string>();
                ctl.SetCallback(sp->bind, [cid, handler](const Event&) {
                    std::printf("[Events] %s: %s\n", cid.c_str(), handler.c_str());
                });
            }
        }
    }
}

// 清空画布（新建/打开/导入前置）：后代先删（控件销毁 + 模型条目移除 + 树节点移除）
// ── F2.5 撤销/重做（快照式：serializeLayoutJson → 栈；同键 0.7s 合并）──

namespace {
struct UndoSuspendGuard {
    bool& flag;
    bool prev;
    explicit UndoSuspendGuard(bool& f) : flag(f), prev(f) { flag = true; }
    ~UndoSuspendGuard() { flag = prev; }
};
double undoNowSeconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}   // namespace

void CanvasPane::commitUndo(const std::string& coalesceKey) {
    if (m_undoSuspended) return;
    if (m_dirtyFn) m_dirtyFn();   // 窗口标题"未保存 *"（编辑提交；撤销/重做内部不再经此）
    const std::string snap = serializeLayoutJson().dump();
    const double now = undoNowSeconds();
    if (m_undoStack.empty()) {
        m_undoStack.push_back(snap);
        m_undoPos = 0;
        m_undoKey = coalesceKey;
        m_undoTime = now;
        return;
    }
    if (!coalesceKey.empty() && coalesceKey == m_undoKey && now - m_undoTime < 0.7) {
        m_undoStack[m_undoPos] = snap;   // 合并：覆盖当前位置（保留最终值）
        m_undoTime = now;
        return;
    }
    m_undoStack.resize(m_undoPos + 1);   // 新操作清空重做段
    m_undoStack.push_back(snap);
    if (m_undoStack.size() > kMaxUndoSteps)
        m_undoStack.erase(m_undoStack.begin());
    m_undoPos = m_undoStack.size() - 1;
    m_undoKey = coalesceKey;
    m_undoTime = now;
}

void CanvasPane::resetUndo() {
    m_undoStack.clear();
    m_undoStack.push_back(serializeLayoutJson().dump());
    m_undoPos = 0;
    m_undoKey.clear();
    m_undoTime = 0.0;
}

void CanvasPane::restoreSnapshot(const std::string& json) {
    nlohmann::json layout = nlohmann::json::parse(json, nullptr, false);
    if (layout.is_discarded()) return;
    m_undoSuspended = true;
    if (buildFromLayoutJson(layout, true)) refreshStatusRight();
    m_undoSuspended = false;
}

bool CanvasPane::Undo() {
    if (m_undoPos == 0) return false;
    m_undoPos--;
    restoreSnapshot(m_undoStack[m_undoPos]);
    m_undoKey.clear();
    m_undoTime = 0.0;
    setStatusHint(u8"已撤销");
    if (m_dirtyFn) m_dirtyFn();   // 撤销同样偏离已保存态
    return true;
}

bool CanvasPane::Redo() {
    if (m_undoPos + 1 >= m_undoStack.size()) return false;
    m_undoPos++;
    restoreSnapshot(m_undoStack[m_undoPos]);
    m_undoKey.clear();
    m_undoTime = 0.0;
    setStatusHint(u8"已重做");
    if (m_dirtyFn) m_dirtyFn();
    return true;
}

void CanvasPane::clearCanvas() {
    std::vector<std::string> toRemove;
    std::function<void(const std::string&)> collect = [&](const std::string& pid) {
        if (std::find(toRemove.begin(), toRemove.end(), pid) != toRemove.end()) return;
        toRemove.push_back(pid);
        for (auto& it : m_items)
            if (it.parentId == pid) collect(it.model->props.id);
    };
    for (auto& it : m_items)
        if (it.parentId.empty()) collect(it.model->props.id);
    for (auto rit = toRemove.rbegin(); rit != toRemove.rend(); ++rit) {
        if (PlacedControl* pc = placedById(*rit))
            if (pc->ctl.IsValid()) pc->ctl.Destroy();
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
            if (it->model->props.id == *rit) { m_items.erase(it); break; }
        syncTreeRemove(*rit);
    }
    m_dragId.clear();
    m_dragParentId.clear();
    m_dragCtl = Control();
    m_placeTargetId.clear();
    selectItem("");
    refreshPlaceTargetHint();
    refreshStatusRight();
}

// 布局 JSON → 画布（打开/导入共用）：清空 → 逐控件创建（设计器自身创建路径——
// 避免引擎解析器覆盖面差异）→ 模型属性应用 → 树/状态同步。useViewportSize=true 时
// 读取顶层 viewport 设置设计画布尺寸（导入外部布局）。
bool CanvasPane::buildFromLayoutJson(const nlohmann::json& layout, bool useViewportSize) {
    if (!m_viewport || !layout.is_object() || !layout.contains("controls") ||
        !layout["controls"].is_array())
        return false;
    UndoSuspendGuard undoSuspend(m_undoSuspended);   // 加载/重建期间不记录撤销
    clearCanvas();
    if (layout.contains("viewport") && layout["viewport"].is_object()) {
        const auto& vp = layout["viewport"];
        if (useViewportSize) {
            const float w = vp.value("width", 0.0f);
            const float h = vp.value("height", 0.0f);
            if (w > 0 && h > 0) SetCanvasSize(w, h);
        }
        // 画布（Bench）属性回填（导入/打开均生效）
        m_benchBg = UIColor{0, 0, 0, 0};
        if (vp.contains("background") && vp["background"].is_string())
            m_benchBg = hexToColor(vp["background"].get<std::string>());
        m_benchScaleMode = 0;
        if (vp.contains("scale-mode") && vp["scale-mode"].is_string()) {
            const std::string sm = vp["scale-mode"].get<std::string>();
            m_benchScaleMode = (sm == "fit") ? 1 : (sm == "stretch") ? 2 : 0;
        }
        m_benchId = vp.value("id", std::string());
        applyBenchBackground();
    }
    // 资源挂载点（顶层 resourceProviders）——回填列表 + 挂载
    m_resourceProviders.clear();
    if (layout.contains("resourceProviders") && layout["resourceProviders"].is_array())
        for (const auto& rp : layout["resourceProviders"])
            if (rp.is_object()) {
                const std::string n = rp.value("name", "");
                const std::string p = rp.value("path", "");
                if (!n.empty() && !p.empty()) m_resourceProviders.push_back({ n, p });
            }
    refreshResourceList();
    mountResourceProviders();
    bool ok = true;
    int count = 0;
    std::function<void(std::unique_ptr<ControlModel>, const std::string&)> walk =
        [&](std::unique_ptr<ControlModel> model, const std::string& pid) {
        if (!model) return;
        const std::string tb = toolboxIdOf(model->props.type);
        if (tb.empty()) {
            std::printf("[CanvasPane] load: unknown type '%s' skipped\n", model->props.type.c_str());
            ok = false;
            return;
        }
        std::vector<std::unique_ptr<ControlModel>> kids;
        kids.swap(model->children);
        const std::string id = model->props.id;
        const SRect r = model->props.rect;
        Control ctl = createViewportControl(tb, r.x, r.y, r.w, r.h, *model);
        if (!ctl.IsValid()) { ok = false; return; }
        if (!pid.empty()) {
            if (PlacedControl* par = placedById(pid)) {
                if (par->ctl.IsValid()) m_viewport->AddChild(par->ctl, ctl);
                ctl.SetRect(r.x, r.y, r.w, r.h);   // 容器子控件：局部坐标
            }
        }
        m_items.push_back({ std::move(model), ctl, pid });
        if (m_viewport) m_viewport->SetControlId(ctl, id);
        // 本控件条目下标（页子树 walk 会追加——一律用下标，勿用 back() ✗）
        const size_t selfIdx = m_items.size() - 1;
        // TabControl 特例：tabs[i].page → 页 Panel（parentId=TabControl；引擎挂接由
        // applyModelToEngine → syncTabs 统一完成）
        const size_t tabIdx = selfIdx;
        if (m_items[tabIdx].model->props.type == "tab-control") {
            const nlohmann::json tabs =
                m_items[tabIdx].model->props.extended.value("tabs", nlohmann::json::array());
            nlohmann::json tabsModel = nlohmann::json::array();
            if (tabs.is_array())
                for (const auto& t : tabs) {
                    if (!t.is_object()) continue;
                    const nlohmann::json pj = t.value("page", nlohmann::json::object());
                    auto pageModel = ControlModel::fromJson(pj.is_object() ? pj : nlohmann::json::object());
                    if (!pageModel) continue;
                    if (pageModel->props.type.empty()) pageModel->props.type = "panel";
                    if (pageModel->props.id.empty())
                        pageModel->props.id = "tabpage_" + std::to_string(++m_seq);
                    const std::string pageId = pageModel->props.id;
                    const std::string pageTitle = t.value("title", "");
                    tabsModel.push_back({ { "title", pageTitle }, { "page-id", pageId } });
                    const int pageNo = (int)tabsModel.size();
                    walk(std::move(pageModel), id);   // 创建页 Panel + 后代
                    // 页节点标签统一为"页 N·标题"（walk 通用标签对页不友好——与 syncTabs 一致）
                    syncTreeRename(pageId, u8"页 " + std::to_string(pageNo) + u8"·" + pageTitle);
                }
            m_items[tabIdx].model->props.extended["tabs"] = std::move(tabsModel);
        }
        applyModelToEngine(m_items[selfIdx]);   // 本控件（页子树 walk 后 back() 已非本项——曾致
                                                // Tab 属性/配色/引擎页挂载全部丢失）
        const std::string cap = m_items[selfIdx].model->props.extended.value("caption", "");
        syncTreeAdd(pid, id, cap + " " + id);
        ++count;
        for (auto& k : kids) walk(std::move(k), id);
    };
    for (const auto& cj : layout["controls"]) {
        auto model = ControlModel::fromJson(cj);
        if (model) walk(std::move(model), "");
    }
    selectItem("");
    refreshPlaceTargetHint();
    refreshStatusRight();
    std::printf("[CanvasPane] layout loaded: %d controls (ok=%d)\n", count, (int)ok);
    return ok;
}

// 保存 .csd（metadata + layout 整包；layout 为唯一数据源）
bool CanvasPane::SaveProject(const std::string& path) {
    nlohmann::json csd;
    csd["app"] = "CornerstoneDesigner";
    csd["version"] = 1;
    csd["metadata"] = { { "canvas", { { "width", m_designW }, { "height", m_designH },
                                      { "title", m_benchTitle } } } };
    csd["layout"] = serializeLayoutJson();
    std::ofstream f(utf8Path(path), std::ios::binary);   // 宽字符路径
    if (!f) {
        setStatusHint(u8"保存失败：无法写入文件");
        std::printf("[CanvasPane] save failed (open): %s\n", path.c_str());
        return false;
    }
    f << csd.dump(2);
    const bool ok = f.good();
    f.close();
    if (ok) {
        m_projectPath = path;
        setStatusHint(u8"已保存：" + path);
        std::printf("[CanvasPane] project saved: %s\n", path.c_str());
    } else {
        setStatusHint(u8"保存失败：写入错误");
    }
    return ok;
}

// 打开 .csd（清空前置；画布尺寸取 metadata.canvas）
bool CanvasPane::OpenProject(const std::string& path) {
    std::ifstream f(utf8Path(path), std::ios::binary);   // 宽字符路径
    if (!f) { setStatusHint(u8"打开失败：无法读取文件"); return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    nlohmann::json csd;
    try {
        csd = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        std::printf("[CanvasPane] project parse failed: %s\n", e.what());
        setStatusHint(u8"打开失败：JSON 解析错误");
        return false;
    }
    if (!csd.contains("layout")) { setStatusHint(u8"打开失败：缺少 layout 段"); return false; }
    const float w = csd.contains("metadata") ? csd["metadata"].value("canvas", nlohmann::json::object()).value("width", 0.0f) : 0.0f;
    const float h = csd.contains("metadata") ? csd["metadata"].value("canvas", nlohmann::json::object()).value("height", 0.0f) : 0.0f;
    m_benchTitle = csd.contains("metadata")
        ? csd["metadata"].value("canvas", nlohmann::json::object()).value("title", std::string()) : std::string();
    if (!buildFromLayoutJson(csd["layout"], false)) {
        setStatusHint(u8"打开失败：layout 无效");
        return false;
    }
    if (w > 0 && h > 0) SetCanvasSize(w, h);
    resetUndo();   // F2.5：打开文档 = 新撤销基线
    m_projectPath = path;
    setStatusHint(u8"已打开：" + path);
    std::printf("[CanvasPane] project opened: %s\n", path.c_str());
    return true;
}

// F4.4/F7.2：代码导出文本生成器（ExportLayout 事件侧车与 ExportCode 共用）
std::string CanvasPane::buildEventsHeaderText() const {
    std::string out = "// events.h - Auto-generated by CornerstoneDesigner\n"
                      "#pragma once\n"
                      "#include \"UICornerstoneAPI.h\"\n"
                      "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
    std::vector<std::string> seen;
    for (const auto& b : collectEventBindings())
        if (std::find(seen.begin(), seen.end(), b.handler) == seen.end()) {
            seen.push_back(b.handler);
            out += "void " + b.handler + "(UIControlHandle ctl, void* user);\n";
        }
    out += "#ifdef __cplusplus\n}\n#endif\n";
    return out;
}

std::string CanvasPane::buildEventsSourceText() const {
    std::string out = "// events.c - Auto-generated by CornerstoneDesigner\n"
                      "#include \"events.h\"\n\n";
    std::vector<std::string> seen;
    for (const auto& b : collectEventBindings())
        if (std::find(seen.begin(), seen.end(), b.handler) == seen.end()) {
            seen.push_back(b.handler);
            out += "void " + b.handler + "(UIControlHandle ctl, void* user) {\n"
                   "    (void)ctl;\n"
                   "    (void)user;\n"
                   "    // TODO: 实现 " + b.handler + "\n"
                   "}\n\n";
        }
    return out;
}

std::string CanvasPane::buildEventListText() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& b : collectEventBindings())
        arr.push_back({ { "control", b.controlId }, { "event", b.jsonEvent }, { "handler", b.handler } });
    return arr.dump(2);
}

std::string CanvasPane::cEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// F7.3：宿主 main.c——LoadLibrary 显式解析 C ABI（无需导入库）→ CreateInstanceFromPlugin
// （自动加载 UIBackend_<backend>.dll）→ LoadLayoutFromFile → RegisterAction（events.c 实现）
// → 帧循环；--frames N 跑 N 帧自动退出（冒烟/CI）。
std::string CanvasPane::buildMainCText() const {
    std::string regs;
    std::vector<std::string> seen;
    for (const auto& b : collectEventBindings())
        if (std::find(seen.begin(), seen.end(), b.handler) == seen.end()) {
            seen.push_back(b.handler);
            regs += "    fnRegisterAction(inst, \"" + b.handler + "\", " + b.handler + ", NULL);\n";
        }
    if (regs.empty()) regs = "    /* （无事件绑定） */\n";
    const std::string title = cEscape(m_benchTitle.empty()
        ? std::string("Cornerstone Exported App") : m_benchTitle);
    char wbuf[16], hbuf[16];
    std::snprintf(wbuf, sizeof(wbuf), "%d", (int)m_designW);
    std::snprintf(hbuf, sizeof(hbuf), "%d", (int)m_designH);
    std::string out = R"MAIN(// main.c — Auto-generated by CornerstoneDesigner
// 最小宿主：显式加载 UICornerstone.dll（C ABI，无需导入库）→ 创建实例
// （CreateInstanceFromPlugin 自动加载 UIBackend_sdl3.dll 后端插件）→ 加载 layout.json
// → 注册事件回调（events.c 实现）→ 帧循环。
//
// 构建：cmake -S . -B build && cmake --build build --config Release
// 运行：build\Release\app.exe              （关闭窗口退出）
//       build\Release\app.exe --frames 120 （跑 120 帧后自动退出——冒烟/CI）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/UICornerstoneAPI.h"
#include "events.h"

/* ---- C ABI 函数指针（GetProcAddress 显式解析）---- */
static UIInstance (__cdecl *fnCreateInstanceFromPlugin)(const char*, const UIInstanceConfig*);
static void       (__cdecl *fnDestroyInstance)(UIInstance);
static int        (__cdecl *fnProcessEvents)(UIInstance);
static void       (__cdecl *fnUpdate)(UIInstance, double);
static void       (__cdecl *fnClear)(UIInstance);
static void       (__cdecl *fnRender)(UIInstance);
static void       (__cdecl *fnPresent)(UIInstance);
static int        (__cdecl *fnIsQuitRequested)(UIInstance);
static int        (__cdecl *fnLoadLayout)(UIInstance, const char*);
static void       (__cdecl *fnRegisterAction)(UIInstance, const char*, UIActionCallback, void*);

#define RESOLVE(var, name)                                                        \
    do {                                                                          \
        *(void**)(&(var)) = (void*)GetProcAddress(core, "UICornerstone_" name);   \
        if (!var) {                                                               \
            fprintf(stderr, u8"缺少 C ABI 导出：UICornerstone_" name "\n");       \
            FreeLibrary(core);                                                    \
            return 2;                                                             \
        }                                                                         \
    } while (0)

/* 读取布局文件（相对本程序工作目录）→ LoadLayout 字符串接口。
   注：C ABI 的 LoadLayoutFromFile 走资源提供者（相对 resourceRoot），
   宿主自行读取后经 LoadLayout 提交可避免资源根耦合。 */
static int load_layout_file(UIInstance inst, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = 0;   /* NUL 终止（等价 '\0'） */
    int ok = fnLoadLayout(inst, buf);
    free(buf);
    return ok;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(65001);   /* 控制台 UTF-8（中文提示） */
    /* 工作目录 = 本程序所在目录（layout.json / assets 相对 exe 解析，双击/任意 cwd 均可用） */
    {
        char exePath[MAX_PATH] = "";
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* slash = strrchr(exePath, 92);   /* 92 = 反斜杠（避免转义歧义） */
        if (slash) *slash = 0;
        SetCurrentDirectoryA(exePath);
    }
    int maxFrames = 0;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) maxFrames = atoi(argv[++i]);

    HMODULE core = LoadLibraryA("UICornerstone.dll");
    if (!core) { fprintf(stderr, u8"无法加载 UICornerstone.dll（需与本程序同目录）\n"); return 1; }
    RESOLVE(fnCreateInstanceFromPlugin, "CreateInstanceFromPlugin");
    RESOLVE(fnDestroyInstance, "DestroyInstance");
    RESOLVE(fnProcessEvents, "ProcessEvents");
    RESOLVE(fnUpdate, "Update");
    RESOLVE(fnClear, "Clear");
    RESOLVE(fnRender, "Render");
    RESOLVE(fnPresent, "Present");
    RESOLVE(fnIsQuitRequested, "IsQuitRequested");
    RESOLVE(fnLoadLayout, "LoadLayout");
    RESOLVE(fnRegisterAction, "RegisterAction");

    UIInstanceConfig cfg = UI_INSTANCE_CONFIG_DEFAULT;
    cfg.windowTitle  = u8"@T@";
    cfg.windowWidth  = @W@;
    cfg.windowHeight = @H@;
    cfg.windowFlags  = UICORN_WINDOW_FLAG_RESIZABLE;
    cfg.resourceRoot = "assets";

    UIInstance inst = fnCreateInstanceFromPlugin("sdl3", &cfg);
    if (!inst) { fprintf(stderr, u8"创建引擎实例失败（详见引擎日志）\n"); FreeLibrary(core); return 1; }

    /* 事件回调注册（须先于布局解析：解析器在 LoadLayout 时按名查表绑定 onClick） */
@REGS@
    if (!load_layout_file(inst, "layout.json")) {
        fprintf(stderr, u8"加载 layout.json 失败（详见引擎日志）\n");
        fnDestroyInstance(inst);
        FreeLibrary(core);
        return 1;
    }
    int frame = 0;
    while (!fnIsQuitRequested(inst) && (maxFrames <= 0 || frame < maxFrames)) {
        fnProcessEvents(inst);
        fnUpdate(inst, 1.0 / 60.0);
        fnClear(inst);
        fnRender(inst);
        fnPresent(inst);
        ++frame;
    }

    fnDestroyInstance(inst);
    FreeLibrary(core);
    return 0;
}
)MAIN";
    // 占位替换：@T@=窗口标题；@WH@=宽, 高；@REGS@=事件注册行
    auto replaceAll = [](std::string& text, const std::string& from, const std::string& to) {
        for (size_t p = text.find(from); p != std::string::npos; p = text.find(from, p + to.size()))
            text.replace(p, from.size(), to);
    };
    replaceAll(out, "@T@", title);
    replaceAll(out, "@W@", wbuf);
    replaceAll(out, "@H@", hbuf);
    replaceAll(out, "@REGS@", regs);
    return out;
}

// F7.3：生成工程 CMakeLists.txt（C11；POST_BUILD 把 DLL/layout/assets 拷到 exe 旁）
std::string CanvasPane::buildHostCMakeText() const {
    return R"CMK(# Auto-generated by CornerstoneDesigner —— 导出应用构建脚本
cmake_minimum_required(VERSION 3.20)
project(exported_app LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(app main.c events.c)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Release = GUI 子系统（无控制台窗口）；Debug 保留控制台（日志/printf 排障）。
# /ENTRY:mainCRTStartup：GUI 子系统下仍以 main() 为入口（MSVC）。
set_target_properties(app PROPERTIES WIN32_EXECUTABLE "$<CONFIG:Release>")
target_link_options(app PRIVATE "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>")

# 运行依赖拷到 exe 旁（引擎按 exe 工作目录解析 DLL / layout.json / assets）
if(WIN32)
    file(GLOB _EXPORT_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/*.dll")
    if(_EXPORT_DLLS)
        add_custom_command(TARGET app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_EXPORT_DLLS} "$<TARGET_FILE_DIR:app>"
        )
    endif()
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/layout.json" "$<TARGET_FILE_DIR:app>"
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
        add_custom_command(TARGET app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:app>/assets"
        )
    endif()
endif()
)CMK";
}

// F7.3/F7.4：生成工程 README（C 裸 C ABI / C++ Binding）
std::string CanvasPane::buildHostReadmeText(bool cpp) const {
    if (cpp)
        return R"RD(# 导出应用（CornerstoneDesigner 生成 —— C++ Binding 工程）

## 构建（一键脚本，推荐）
- 双击 `build_debug.bat` → 产物 `build\Debug\app.exe`（带控制台，日志/printf 可见）
- 双击 `build_release.bat` → 产物 `build\Release\app.exe`（GUI 子系统，**无控制台窗口**）

或手动：
```
cmake -S . -B build
cmake --build build --config Release
```
需要 MSVC（或可编译 C++17 / C11 的工具链）。排障请用 Debug 版（Release 无控制台输出）。

## 运行
```
build\Release\app.exe                 # 关闭窗口退出
build\Release\app.exe --frames 120    # 跑 120 帧后自动退出（冒烟/CI）
```

## 结构
- `main.cpp` — 宿主（C++ Binding：Create → LoadLayout → RegisterAction → 帧循环）
- `events.h` / `events.c` — 事件回调声明与骨架（在 events.c 中实现业务逻辑）
- `layout.json` — 引擎兼容布局（含控件扩展属性与 events 绑定）
- `event_list.json` — 事件清单（机器可读）
- `binding/` — UICornerstone C++ Binding 包（add_subdirectory 集成，纯动态加载，无需导入库）
- `include/` — 引擎头（UICornerstoneAPI.h / PropertyNames.h）
- `*.dll` — 运行时（UICornerstone.dll + UIBackend_sdl3.dll + SDL3 系）
- `assets/` — 字体等资源（文本渲染依赖；可按需裁剪）

## 说明
- 构建后 DLL/layout.json/assets 自动拷贝到 exe 同目录（工作目录按 exe 解析）；
- 布局内 `provider:<name>` 资源引用需在本目录提供对应资源（resourceProviders 块随 layout.json 保留）；
- 运行失败时引擎日志输出到控制台。
)RD";
    return R"RD(# 导出应用（CornerstoneDesigner 生成）

## 构建（一键脚本，推荐）
- 双击 `build_debug.bat` → 产物 `build\Debug\app.exe`（带控制台，日志/printf 可见）
- 双击 `build_release.bat` → 产物 `build\Release\app.exe`（GUI 子系统，**无控制台窗口**）

或手动：
```
cmake -S . -B build
cmake --build build --config Release
```
需要 MSVC（或可编译 C11 的工具链）。排障请用 Debug 版（Release 无控制台输出）。

## 运行
```
build\Release\app.exe                 # 关闭窗口退出
build\Release\app.exe --frames 120    # 跑 120 帧后自动退出（冒烟/CI）
```

## 结构
- `main.c` — 宿主（LoadLibrary 显式 C ABI：创建实例→加载布局→注册动作→帧循环）
- `events.h` / `events.c` — 事件回调声明与骨架（在 events.c 中实现业务逻辑）
- `layout.json` — 引擎兼容布局（含控件扩展属性与 events 绑定）
- `event_list.json` — 事件清单（机器可读）
- `include/UICornerstoneAPI.h` — 引擎 C ABI 头
- `*.dll` — 运行时（UICornerstone.dll + UIBackend_sdl3.dll + SDL3 系）
- `assets/` — 字体等资源（文本渲染依赖；可按需裁剪）

## 说明
- 构建后 DLL/layout.json/assets 自动拷贝到 exe 同目录（工作目录按 exe 解析）；
- 布局内 `provider:<name>` 资源引用需在本目录提供对应资源（resourceProviders 块随 layout.json 保留）；
- 运行失败时引擎日志输出到控制台。
)RD";
}

// F7.3/F7.4：生成工程一键构建脚本（build_debug.bat / build_release.bat）。
// CRLF + 纯 ASCII：cmd 以 OEM 代码页读取批处理，非 ASCII 内容可能破坏解析。
std::string CanvasPane::buildBuildScriptText(bool release) const {
    const char* cfg = release ? "Release" : "Debug";
    const char* exe = release ? "build\\Release\\app.exe" : "build\\Debug\\app.exe";
    std::string s;
    s += "@echo off\r\n";
    s += "rem ============================================================\r\n";
    s += "rem  CornerstoneDesigner - exported project build script (";
    s += cfg;
    s += ")\r\n";
    s += "rem  Double-click: configure + build; output: ";
    s += exe;
    s += "\r\n";
    s += "rem  Requires CMake + Visual Studio (MSVC) toolchain.\r\n";
    s += "rem ============================================================\r\n";
    s += "setlocal\r\n";
    s += "cd /d \"%~dp0\"\r\n";
    s += "\r\n";
    s += "cmake -S . -B build\r\n";
    s += "if errorlevel 1 goto :fail\r\n";
    s += "cmake --build build --config ";
    s += cfg;
    s += "\r\n";
    s += "if errorlevel 1 goto :fail\r\n";
    s += "\r\n";
    s += "echo.\r\n";
    s += "echo [OK] ";
    s += exe;
    s += "\r\n";
    s += "pause\r\n";
    s += "exit /b 0\r\n";
    s += "\r\n";
    s += ":fail\r\n";
    s += "echo.\r\n";
    s += "echo [FAILED] see messages above.\r\n";
    s += "pause\r\n";
    s += "exit /b 1\r\n";
    return s;
}

// F7.4：宿主 main.cpp——C++ Binding（Create → 读 layout.json 经 LoadLayout → RegisterAction
// 包装 events.c 回调 → 帧循环）；--frames N 冒烟。
std::string CanvasPane::buildMainCppText() const {
    std::string regs;
    std::vector<std::string> seen;
    for (const auto& b : collectEventBindings())
        if (std::find(seen.begin(), seen.end(), b.handler) == seen.end()) {
            seen.push_back(b.handler);
            regs += "    ui->RegisterAction(\"" + b.handler + "\", [](Control c) {\n"
                    "        " + b.handler + "(c.Handle(), nullptr);\n"
                    "    });\n";
        }
    if (regs.empty()) regs = "    /* （无事件绑定） */\n";
    const std::string title = cEscape(m_benchTitle.empty()
        ? std::string("Cornerstone Exported App") : m_benchTitle);
    char wbuf[16], hbuf[16];
    std::snprintf(wbuf, sizeof(wbuf), "%d", (int)m_designW);
    std::snprintf(hbuf, sizeof(hbuf), "%d", (int)m_designH);
    std::string out = R"MAINCPP(// main.cpp — Auto-generated by CornerstoneDesigner（C++ Binding 工程）
// 宿主：C++ Binding（纯动态加载——核心 DLL + 后端插件 DLL 运行时解析，无需导入库）
// → Create → 读取 layout.json 经 LoadLayout 提交 → RegisterAction 注册 events.c 回调
// → 帧循环。
//
// 构建：cmake -S . -B build && cmake --build build --config Release
// 运行：build\Release\app.exe              （关闭窗口退出）
//       build\Release\app.exe --frames 120 （跑 120 帧后自动退出——冒烟/CI）
#include "UICornerstone.h"
#include "Control.h"
#include "events.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

static std::string read_text_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(65001);   /* 控制台 UTF-8（中文提示） */
    /* 工作目录 = 本程序所在目录（layout.json / assets 相对 exe 解析） */
    {
        char exePath[MAX_PATH] = "";
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* slash = strrchr(exePath, 92);   /* 92 = 反斜杠 */
        if (slash) *slash = 0;
        SetCurrentDirectoryA(exePath);
    }
    int maxFrames = 0;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) maxFrames = atoi(argv[++i]);

    auto ui = UICornerstone::UICornerstone::Create(
        UICornerstone::UICornerstone::Config{}
            .WithBackend("sdl3")
            .WithResourceRoot("assets")
            .WithWindow(u8"@T@", @W@, @H@)
            .WithWindowFlags(UIWindowFlags::Resizable));
    if (!ui) { std::fprintf(stderr, u8"创建引擎实例失败\n"); return 1; }

    /* 事件回调注册（须先于 LoadLayout：解析器按名查表绑定 onClick） */
@REGS@
    const std::string layout = read_text_file("layout.json");
    if (layout.empty() || !ui->LoadLayout(layout)) {
        std::fprintf(stderr, u8"加载 layout.json 失败\n");
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    int frame = 0;
    while (!ui->IsQuitRequested() && (maxFrames <= 0 || frame < maxFrames)) {
        ui->ProcessEvents();

        auto now = Clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0.1) dt = 0.1;

        ui->Update(dt);
        ui->Clear();
        ui->Render();
        ui->Present();
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return 0;   /* ui 析构自动销毁实例 */
}
)MAINCPP";
    // 占位替换：@T@=窗口标题；@W@/@H@=窗口宽高；@REGS@=事件注册行
    auto replaceAll = [](std::string& text, const std::string& from, const std::string& to) {
        for (size_t p = text.find(from); p != std::string::npos; p = text.find(from, p + to.size()))
            text.replace(p, from.size(), to);
    };
    replaceAll(out, "@T@", title);
    replaceAll(out, "@W@", wbuf);
    replaceAll(out, "@H@", hbuf);
    replaceAll(out, "@REGS@", regs);
    return out;
}

// F7.4：生成工程 CMakeLists.txt（C++ Binding；参照 binding/CMakeLists.example.txt）
std::string CanvasPane::buildHostCppCMakeText() const {
    return R"CMK(# Auto-generated by CornerstoneDesigner —— 导出 C++（Binding）工程构建脚本
cmake_minimum_required(VERSION 3.16)
project(exported_cpp_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 发布包无核心构建树：置空跳过 binding 自带样例的构建
set(UICORNERSTONE_CORE_OUTPUT "" CACHE PATH "核心库产物目录（导出工程置空）")

# 引入 binding 包（纯动态加载静态包装库）
add_subdirectory(binding UICornerstoneBinding)

add_executable(app main.cpp events.c)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(app PRIVATE UICornerstoneBinding)

# Release = GUI 子系统（无控制台窗口）；Debug 保留控制台（日志/printf 排障）。
# /ENTRY:mainCRTStartup：GUI 子系统下仍以 main() 为入口（MSVC）。
set_target_properties(app PROPERTIES WIN32_EXECUTABLE "$<CONFIG:Release>")
target_link_options(app PRIVATE "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>")

# 运行依赖拷到 exe 旁（引擎按 exe 工作目录解析 DLL / layout.json / assets）
if(WIN32)
    file(GLOB _EXPORT_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/*.dll")
    if(_EXPORT_DLLS)
        add_custom_command(TARGET app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_EXPORT_DLLS} "$<TARGET_FILE_DIR:app>"
        )
    endif()
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/layout.json" "$<TARGET_FILE_DIR:app>"
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
        add_custom_command(TARGET app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:app>/assets"
        )
    endif()
endif()
)CMK";
}

bool CanvasPane::validateEventBindings(std::string& badName) const {
    std::vector<std::string> seen;
    for (const auto& b : collectEventBindings()) {
        if (!isValidIdentifier(b.handler)) { badName = b.handler; return false; }
        if (std::find(seen.begin(), seen.end(), b.handler) != seen.end()) { badName = b.handler; return false; }
        seen.push_back(b.handler);
    }
    return true;
}

// 导出引擎兼容布局 JSON（F7.1：LayoutParser 可解析；N4 验收面）
bool CanvasPane::ExportLayout(const std::string& path) {
    const nlohmann::json j = serializeLayoutJson();
    std::ofstream f(utf8Path(path), std::ios::binary);   // 宽字符路径
    if (!f) {
        setStatusHint(u8"导出失败：无法写入文件");
        return false;
    }
    f << j.dump(2);
    const bool ok = f.good();
    f.close();
    if (ok) {
        // F4.3 事件列表导出（与布局同目录同名基）：events.h（函数签名）+ event_list.json（机器可读）
        const auto binds = collectEventBindings();
        std::string base = path;
        const auto dot = base.find_last_of('.');
        const auto sep = base.find_last_of("/\\");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
            base = base.substr(0, dot);
        {   // 文本生成器与 ExportCode 共用（F4.4/F7.2）；带 UTF-8 BOM（MSVC 友好）
            std::ofstream h(utf8Path(base + "_events.h"), std::ios::binary);
            if (h) h << "\xEF\xBB\xBF" << buildEventsHeaderText();
        }
        {
            std::ofstream l(utf8Path(base + "_event_list.json"), std::ios::binary);
            if (l) l << buildEventListText();
        }
        setStatusHint(u8"已导出：" + path + u8"（含事件列表）");
        std::printf("[CanvasPane] layout exported: %s (events=%zu)\n", path.c_str(), binds.size());
    } else {
        setStatusHint(u8"导出失败：写入错误");
    }
    return ok;
}

// F4.4/F7.2 代码骨架导出：目录内四件套（layout.json + events.h + events.c + event_list.json）
bool CanvasPane::ExportCode(const std::string& dir) {
    return exportProject(dir, false);   // 可编译 C 项目（裸 C ABI 宿主）
}

bool CanvasPane::ExportCodeCpp(const std::string& dir) {
    return exportProject(dir, true);    // 可编译 C++ 项目（C++ Binding 宿主）
}

// F7.3/F7.4：完整可编译宿主工程导出（cpp=false：main.c 裸 C ABI；cpp=true：main.cpp + binding 包）
bool CanvasPane::exportProject(const std::string& dir, bool cpp) {
    std::string bad;
    if (!validateEventBindings(bad)) {
        setStatusHint(u8"导出失败：handler 非法或重名（" + bad + u8"）");
        return false;
    }
    std::error_code ec;
    const auto u8dir = utf8Path(dir);
    std::filesystem::create_directories(u8dir, ec);
    if (ec) {
        setStatusHint(u8"导出失败：无法创建目录 " + dir);
        return false;
    }
    static const std::string kBom = "\xEF\xBB\xBF";   // UTF-8 BOM（MSVC 正确识别中文注释）
    auto writeFile = [&](const char* name, const std::string& text) -> bool {
        std::ofstream f(u8dir / utf8Path(name), std::ios::binary);
        if (!f) return false;
        f << text;
        return (bool)f;
    };
    // 源码类文件带 BOM；JSON/CMake/README 不带（解析器/工具链按无 BOM UTF-8 读取）
    const bool ok = writeFile("layout.json", serializeLayoutJson().dump(2)) &&
                    writeFile("events.h", kBom + buildEventsHeaderText()) &&
                    writeFile("events.c", kBom + buildEventsSourceText()) &&
                    writeFile("event_list.json", buildEventListText()) &&
                    (cpp ? writeFile("main.cpp", kBom + buildMainCppText())
                         : writeFile("main.c", kBom + buildMainCText())) &&
                    writeFile("CMakeLists.txt", cpp ? buildHostCppCMakeText() : buildHostCMakeText()) &&
                    writeFile("README.md", buildHostReadmeText(cpp)) &&
                    writeFile("build_debug.bat", buildBuildScriptText(false)) &&
                    writeFile("build_release.bat", buildBuildScriptText(true));
    // 宿主工程分发件——引擎头（exe/sdk/include）+ 运行时 DLL + assets（字体）；
    // C++ 版另拷 binding 包（add_subdirectory 集成）
    std::vector<std::string> missing;
    if (!m_shareDir.empty()) {
        std::error_code ec2;
        const auto share = utf8Path(m_shareDir);
        const auto incDir = u8dir / "include";
        std::filesystem::create_directories(incDir, ec2);
        static const char* kHeaders[] = { "UICornerstoneAPI.h", "PropertyNames.h" };
        for (const char* h : kHeaders) {
            std::error_code ecH;
            std::filesystem::copy_file(share / "sdk" / "include" / h, incDir / h,
                std::filesystem::copy_options::overwrite_existing, ecH);
            if (ecH) missing.push_back(std::string("sdk/include/") + h);
        }
        if (cpp) {
            std::error_code ecB;
            std::filesystem::copy(share / "sdk" / "binding", u8dir / "binding",
                std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing, ecB);
            if (ecB) missing.push_back("sdk/binding/");
        }
        static const char* kDlls[] = { "UICornerstone.dll", "UIBackend_sdl3.dll",
                                       "SDL3.dll", "SDL3_ttf.dll", "SDL3_image.dll" };
        for (const char* d : kDlls) {
            std::error_code ec3;
            std::filesystem::copy_file(share / d, u8dir / d,
                std::filesystem::copy_options::overwrite_existing, ec3);
            if (ec3) missing.push_back(d);
        }
        std::error_code ec4;
        std::filesystem::copy(share / "assets", u8dir / "assets",
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing, ec4);
        if (ec4) missing.push_back("assets/");
    }
    const std::string kind = cpp ? u8"C++（Binding）" : u8"C";
    if (ok && missing.empty())
        setStatusHint(u8"已导出" + kind + u8"项目：" + dir +
                      u8"（可直接编译：CMake 构建脚本 + 运行时 DLL/assets 已随附）");
    else if (ok)
        setStatusHint(u8"已导出" + kind + u8"项目（部分分发件缺失：" +
                      std::to_string(missing.size()) + u8" 项）");
    else
        setStatusHint(u8"导出失败：写入错误");
    std::printf("[CanvasPane] %s project exported: %s (ok=%d missing=%zu)\n",
                cpp ? "cpp" : "c", dir.c_str(), ok ? 1 : 0, missing.size());
    return ok;
}

// 导入引擎布局 JSON（画布尺寸取顶层 viewport——存在时）
bool CanvasPane::ImportLayout(const std::string& path) {
    std::ifstream f(utf8Path(path), std::ios::binary);   // 宽字符路径
    if (!f) { setStatusHint(u8"导入失败：无法读取文件"); return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        std::printf("[CanvasPane] import parse failed: %s\n", e.what());
        setStatusHint(u8"导入失败：JSON 解析错误");
        return false;
    }
    if (!buildFromLayoutJson(j, true)) {
        setStatusHint(u8"导入失败：controls 无效");
        return false;
    }
    resetUndo();   // F2.5：导入 = 新撤销基线
    setStatusHint(u8"已导入：" + path);
    std::printf("[CanvasPane] layout imported: %s\n", path.c_str());
    return true;
}

// auto=2 自测：放置 → 保存 → 打开 → 导出 → 导入 往返校验（dir 为临时目录）
bool CanvasPane::SelfTestIo(const std::string& dir) {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) ++pass;
        else { ++fail; std::printf("  FAIL: %s\n", what); }
        std::fflush(stdout);
    };
    std::fflush(stdout);
    NewProject();
    SetCanvasSize(1440.0f, 900.0f);
    placeControl("tBtn", 120, 80, false);
    placeControl("tLabel", 320, 80, false);
    placeControl("tList", 200, 260, false);
    placeControl("tTree", 480, 260, false);
    placeControl("tMenu", 200, 60, false);
    placeControl("tTabs", 700, 420, false);
    placeControl("tPanel", 620, 120, false);
    placeControl("tStatus", 400, 700, false);
    placeControl("tImage", 900, 120, false);
    placeControl("tAni", 900, 320, false);
    // 容器子控件（放置目标通道）
    std::string panelId;
    for (auto& it : m_items)
        if (it.model->props.type == "panel") panelId = it.model->props.id;
    m_placeTargetId = panelId;
    placeControl("tLabel", 0, 0, false);
    m_placeTargetId.clear();

    // 写入可校验属性：颜色 / 字号 / 结构化（列头全样式 + 单元格对象）
    std::string listId, childId;
    for (auto& it : m_items) {
        if (it.model->props.type == "list-view") {
            listId = it.model->props.id;
            it.model->props.extended["colors"]["background"]["normal"] = "#112233FF";
            it.model->props.extended["font-size"] = 15;
            it.model->props.extended["columns"] = nlohmann::json::array({
                { { "title", u8"列A" }, { "width", 120.0f }, { "header-color", "#FF0000FF" },
                  { "header-size", 16 }, { "header-font-name", "quando-regular" } } });
            it.model->props.extended["rows"] = nlohmann::json::array({
                { { "id", "r0" }, { "cells", nlohmann::json::array({
                    "A1", { { "text", "B1" }, { "background-color", "#00FF00FF" },
                            { "hover-background-color", "#0000FFFF" }, { "font-size", 12 } } }) } } });
        }
        if (!it.parentId.empty()) childId = it.model->props.id;
    }
    // 树 items 兄弟顺序（回归：模型嵌套反序构建曾致显示与输入相反）——附 hb#
    // （悬停背景；导出对象形态回归——用户问题 3/4）
    for (auto& it : m_items) {
        if (it.model->props.type != "tree-view") continue;
        applyTreeItemsText(*it.model, u8">A@hb#00112233\n>>A1\n>>A2\n>B");
    }
    // 状态栏段 hb#（悬停背景——导出对象形态回归）
    for (auto& it : m_items) {
        if (it.model->props.type != "status-bar") continue;
        applyStatusBarItemsText(*it.model, u8"左段@b#11223344@hb#AABBCCDD\n右段");
    }
    // 菜单结构（P0-69：层级/快捷键/分隔符 + 顺序往返；P0-72① 面板级属性：字号/字体名/阴影半径）
    for (auto& it : m_items) {
        if (it.model->props.type != "menu-bar") continue;
        applyMenusText(*it.model,
                       u8"文件@@@12@harmonyos-sans-sc-regular@r#12\n>新建@Ctrl+N\n>--\n>退出\n帮助\n>关于");
        const auto& menus = it.model->props.extended.value("menus", nlohmann::json::array());
        check(menus.size() == 2 && menus[0].contains("font") &&
              menus[0]["font"].value("size", 0) == 12 &&
              menus[0]["font"].value("name", "") == "harmonyos-sans-sc-regular" &&
              menus[0].value("shadow-radius", 0.0f) == 12.0f,
              "menus: panel attrs (font/shadow-radius)");
        const std::string rt = menusText(*it.model);
        check(rt.find("r#12") != std::string::npos &&
              rt.find("harmonyos-sans-sc-regular") != std::string::npos,
              "menus: panel attrs round-trip text");
    }
    // StatusBar 颜色（回归：背景/悬停色须随导出携带 + 引擎应用）
    for (auto& it : m_items) {
        if (it.model->props.type != "status-bar") continue;
        writeColorSlotTo(it.model->props.id, "background", "background", UIColor{255, 0, 0, 255});
        writeColorSlotTo(it.model->props.id, "background.hover", "background", UIColor{0, 255, 0, 255});
    }
    // Image provider 路径（回归：加载不得被放置默认值覆盖——曾丢失 provider:test/xxx）
    for (auto& it : m_items) {
        if (it.model->props.type != "image") continue;
        writeStringPropTo(it.model->props.id, "image", "provider:test/bombBlock.jsonc");
    }
    // TabControl：页签 + 页内放置（方案 A：页 Panel 自管 + 容器语义）
    std::string tabsId;
    for (auto& it : m_items)
        if (it.model->props.type == "tab-control") tabsId = it.model->props.id;
    try {
        if (!tabsId.empty()) {
            if (ControlModel* tm = findModel(tabsId)) applyTabsText(*tm, u8"首页\n设置");
            m_placeTargetId = tabsId;              // 放置目标=TabControl → 当前页 Panel
            placeControl("tLabel", 0, 0, false);
            m_placeTargetId.clear();
        }
    } catch (const std::exception& e) {
        std::fflush(stdout);
    }
    // 回归（用户反馈）：指定页放置 + 页 Panel 背景色（模型 + 引擎回读）
    {
        if (!tabsId.empty()) {
            ControlModel* tm = findModel(tabsId);
            const nlohmann::json tabs = tm ? tm->props.extended.value("tabs", nlohmann::json::array())
                                           : nlohmann::json::array();
            if (tabs.is_array() && tabs.size() >= 2) {
                const std::string page2 = tabs[1].value("page-id", "");
                // 1) 指定第 2 页为放置目标（模拟控件树选中页节点）→ 放置应落入该页
                selectItem(page2);
                setPlaceTarget(page2);
                placeControl("tLabel", 0, 0, false);
                std::string placedInP2 = m_selectedId;
                check(!page2.empty() && !placedInP2.empty() &&
                      placedById(placedInP2) && placedById(placedInP2)->parentId == page2,
                      "tab: place into selected page (page 2)");
                // 2) 页 Panel 背景色：模型 + 引擎句柄回读
                writeColorSlotTo(page2, "background", "background", UIColor{0x11, 0x22, 0x33, 0xFF});
                ControlModel* pm = findModel(page2);
                PlacedControl* pp = placedById(page2);
                const std::string hex = pm ? pm->props.extended.value("colors", nlohmann::json::object())
                    .value("background", nlohmann::json::object()).value("normal", std::string())
                    : std::string();
                UIColor eng{0, 0, 0, 0};
                if (pp && pp->ctl.IsValid()) eng = pp->ctl.GetColor("background");
                check(hex == "#112233FF" && eng.r == 0x11 && eng.g == 0x22 && eng.b == 0x33,
                      "tab: page panel background color (model + engine)");
                // 3) 放置即用：新 TabControl 默认两页（放置后选中项即该 TabControl）
                placeControl("tTabs", 900, 500, false);
                const std::string t2 = m_selectedId;
                const nlohmann::json tabs2 = findModel(t2)
                    ? findModel(t2)->props.extended.value("tabs", nlohmann::json::array())
                    : nlohmann::json::array();
                check(tabs2.is_array() && tabs2.size() == 2, "tab: default pages on place");
                deleteSelected();   // 清理（连带页 Panel 子树）
                // 页签文字亮色（引擎页签底深色硬编码——深字不可读，用户实测）
                {
                    ControlModel* tcm = findModel(tabsId);
                    const std::string tn = tcm
                        ? tcm->props.extended.value("colors", nlohmann::json::object())
                              .value("text", nlohmann::json::object())
                              .value("normal", std::string())
                        : std::string();
                    check(tn == "#EBEBEBFF", "tab: light text color for readability");
                }
                // 4) 树选页 → 所属 TabControl 当前页同步切换（所见即所编/所放）
                selectItem(page2);
                syncTabCurrentToPage(page2);
                PlacedControl* tp2 = placedById(tabsId);
                check(tp2 && tp2->ctl.IsValid() &&
                      tp2->ctl.GetInt(PropertyNames::kTabCurrentIndex) == 1,
                      "tab: tree-select page switches current index");
                // 5) 用户流复现：树选页 1 → 落点在该 Tab 矩形内 → 应落页 1（非引擎当前页/末页）
                {
                    const std::string page1 = tabs[0].value("page-id", "");
                    const SRect tr = findModel(tabsId)->props.rect;
                    selectItem(page1);
                    syncTabCurrentToPage(page1);
                    placeControl("tLabel", tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f, false);
                    const std::string newId = m_selectedId;   // placeControl 选中新控件
                    PlacedControl* np = placedById(newId);
                    const std::string landedParent = np ? np->parentId : "";
                    std::printf("[tab-test] page1=%s current=%d new=%s parent=%s\n",
                                page1.c_str(),
                                tp2 && tp2->ctl.IsValid()
                                    ? tp2->ctl.GetInt(PropertyNames::kTabCurrentIndex) : -99,
                                newId.c_str(), landedParent.c_str());
                    check(!page1.empty() && np && np->parentId == page1,
                          "tab: drop over tab lands in tree-selected page");
                    // 清理测试放置（保持既有夹具计数：页 1 恰 1 个 label）
                    if (placedById(newId)) {
                        selectItem(newId);
                        deleteSelected();
                    }
                }
                if (!placedInP2.empty() && placedById(placedInP2)) {
                    selectItem(placedInP2);
                    deleteSelected();
                }
                // 6) 删除含控件页（确认放行）：被删页与其控件真正移除，其余页保 id
                {
                    const std::string pFirst = tabs[0].value("page-id", "");
                    const std::string pSecond = tabs[1].value("page-id", "");
                    selectItem(pSecond);
                    setPlaceTarget(pSecond);
                    placeControl("tLabel", 0, 0, false);
                    const std::string extra = m_selectedId;
                    check(placedById(extra) && placedById(extra)->parentId == pSecond,
                          "tab: control placed into page 2 for delete test");
                    m_tabDeleteConfirmed = true;   // 放行确认门（弹窗交互另行验证）
                    applyTabsText(*findModel(tabsId), u8"首页");   // 删"设置"（含控件）
                    m_tabDeleteConfirmed = false;
                    const nlohmann::json t5 = findModel(tabsId)->props.extended.value(
                        "tabs", nlohmann::json::array());
                    check(t5.is_array() && t5.size() == 1 &&
                          t5[0].value("page-id", "") == pFirst &&
                          !placedById(pSecond) && !placedById(extra),
                          "tab: delete page removes it + its controls (title-matched)");
                    applyTabsText(*findModel(tabsId), u8"首页\n设置");   // 还原两页夹具
                }
                // 7) 确认门：含控件页删除未确认时暂缓（弹窗路径；取消=状态不变）
                {
                    const nlohmann::json t6 = findModel(tabsId)->props.extended.value(
                        "tabs", nlohmann::json::array());
                    const std::string pFirst2 = t6[0].value("page-id", "");
                    m_tabDeleteConfirmed = false;
                    applyTabsText(*findModel(tabsId), u8"设置");   // 删含控件的"首页"
                    const nlohmann::json t7 = findModel(tabsId)->props.extended.value(
                        "tabs", nlohmann::json::array());
                    check(t7.is_array() && t7.size() == 2 && !m_pendingTabsText.empty() &&
                          t7[0].value("page-id", "") == pFirst2,
                          "tab: delete with controls deferred to confirm dialog");
                    std::printf("[tab-test] defer: size=%d pending=%d first=%s\n",
                                t7.is_array() ? (int)t7.size() : -1,
                                m_pendingTabsText.empty() ? 0 : 1, pFirst2.c_str());
                    if (m_tabDeleteDlg.IsValid()) m_tabDeleteDlg.SetBool(PropertyNames::kVisible, 0);
                    m_pendingTabsId.clear();       // 模拟取消（状态不变）
                    m_pendingTabsText.clear();
                    rebuildDynamicRows();
                }
                // 8) 页序调整（P0-91③ TabMovePage）：模型序 + 引擎序同步
                {
                    const nlohmann::json t8 = findModel(tabsId)->props.extended.value(
                        "tabs", nlohmann::json::array());
                    const std::string pa = t8[0].value("page-id", "");
                    const std::string pb = t8[1].value("page-id", "");
                    check(moveTabPage(tabsId, 0, 1), "tab: moveTabPage ok (engine TabMovePage)");
                    const nlohmann::json t9 = findModel(tabsId)->props.extended.value(
                        "tabs", nlohmann::json::array());
                    const bool modelOk = t9.is_array() && t9.size() == 2 &&
                        t9[0].value("page-id", "") == pb && t9[1].value("page-id", "") == pa;
                    selectItem(pa);
                    syncTabCurrentToPage(pa);
                    PlacedControl* tp4 = placedById(tabsId);
                    const bool engOk = tp4 && tp4->ctl.IsValid() &&
                        tp4->ctl.GetInt(PropertyNames::kTabCurrentIndex) == 1;
                    check(modelOk && engOk, "tab: page reorder syncs model + engine order");
                    moveTabPage(tabsId, 1, 0);   // 还原（首页在前）
                }
                // 9) 池往返回归（P0-91④ 引擎修复）：Remove→Add→SetPage 合法序不再崩溃
                {
                    PlacedControl* tp5 = placedById(tabsId);
                    const nlohmann::json t10 = findModel(tabsId)->props.extended.value(
                        "tabs", nlohmann::json::array());
                    const std::string pB = t10.is_array() && t10.size() > 1
                        ? t10[1].value("page-id", "") : "";
                    PlacedControl* pgB = pB.empty() ? nullptr : placedById(pB);
                    bool ok = tp5 && tp5->ctl.IsValid() && pgB && pgB->ctl.IsValid();
                    if (ok) {
                        m_viewport->TabRemovePage(tp5->ctl, 1);
                        m_viewport->TabAddPage(tp5->ctl, u8"设置");
                        m_viewport->TabSetPage(tp5->ctl, 1, pgB->ctl);
                        ok = pgB->ctl.IsValid();
                    }
                    check(ok, "tab: pool roundtrip (remove+add+setpage) safe");
                }
                // 10) 页签配色（P0-90）：写入 + 模型/引擎读回
                {
                    writeTabColorTo(tabsId, PropertyNames::kTabBackground,
                                    UIColor{0x11, 0x22, 0x33, 0xFF});
                    ControlModel* tm10 = findModel(tabsId);
                    PlacedControl* tp10 = placedById(tabsId);
                    const std::string hx = tm10 ? tm10->props.extended.value(
                        PropertyNames::kTabBackground, std::string()) : "";
                    UIColor engc{0, 0, 0, 0};
                    if (tp10 && tp10->ctl.IsValid())
                        engc = tp10->ctl.GetColor(PropertyNames::kTabBackground);
                    check(hx == "#112233FF" && engc.r == 0x11 && engc.g == 0x22 && engc.b == 0x33,
                          "tab: strip color write (model + engine)");
                    // 保留特征色用于 save/open/导出 往返断言（P0-92）
                    writeTabColorTo(tabsId, PropertyNames::kTabBackground,
                                    UIColor{0x44, 0x55, 0x66, 0xFF});
                    // P0-94：模型置可重排（复刻用户勾选"可重排"——预览/导出应携带）
                    findModel(tabsId)->props.extended[PropertyNames::kReorderable] = true;
                }
                // 11a) P0-94 拖拽属性探针（引擎通道；Phase 1 只读验证 + 设计态约束）
                {
                    PlacedControl* tp12 = placedById(tabsId);
                    if (tp12 && tp12->ctl.IsValid()) {
                        tp12->ctl.SetBool(PropertyNames::kDraggable, 1);
                        const bool d1 = tp12->ctl.GetBool(PropertyNames::kDraggable);
                        tp12->ctl.SetFloat(PropertyNames::kDragThreshold, 6.0f);
                        const float th = tp12->ctl.GetFloat(PropertyNames::kDragThreshold);
                        const bool act = tp12->ctl.GetBool(PropertyNames::kDragActive);
                        tp12->ctl.SetBool(PropertyNames::kDraggable, 0);
                        check(d1 && th >= 5.9f && th <= 6.1f && !act,
                              "drag: properties roundtrip (draggable/threshold/active)");
                    }
                }
                // 11) UI 流模拟（注入鼠标源 + tickInput 驱动）：点击项/空白/拖动排序
                {
                    PlacedControl* tabPc = placedById(tabsId);
                    if (tabPc && tabPc->ctl.IsValid()) {
                        auto savedMouse = m_mouseSource;
                        struct FakeMouse { bool down = false; float wx = 0, wy = 0; };
                        FakeMouse fm;
                        m_mouseSource = [&fm](float& x, float& y) { x = fm.wx; y = fm.wy; return fm.down; };
                        auto toScreen = [this](float cx2, float cy2) {
                            float sx = 0, sy = 0;
                            CanvasToScreen(cx2, cy2, sx, sy);
                            return std::pair<float, float>{ sx, sy };
                        };
                        float tx0 = 0, ty0 = 0;
                        globalOriginOf(*tabPc, tx0, ty0);
                        UIRect r0{}, r1{};
                        check(m_viewport->TabGetItemRect(tabPc->ctl, 0, r0) &&
                              m_viewport->TabGetItemRect(tabPc->ctl, 1, r1),
                              "ui-sim: TabGetItemRect readable");
                        // 页签带命中回归：item0 中心 hitTest 应解析为 Tab（页模型 rect 曾
                        // 覆盖页签带 → 命中页而非 Tab，Tab 点击逻辑被绕过）
                        {
                            std::string hitChk;
                            const bool hitOk = hitTest(tx0 + r0.x + r0.w * 0.5f,
                                                       ty0 + r0.y + r0.h * 0.5f, hitChk);
                            std::printf("[ui-sim] strip-hit: %s (want %s)\n",
                                        hitChk.c_str(), tabsId.c_str());
                            check(hitOk && hitChk == tabsId,
                                  "ui-sim: strip hit resolves to tab (not page)");
                        }
                        // ① 点击页签项 0 → 选中页 0
                        {
                            const auto s = toScreen(tx0 + r0.x + r0.w * 0.5f,
                                                    ty0 + r0.y + r0.h * 0.5f);
                            fm.wx = s.first; fm.wy = s.second; fm.down = false; tickInput();
                            fm.down = true; tickInput();
                            const nlohmann::json tj = findModel(tabsId)->props.extended.value(
                                "tabs", nlohmann::json::array());
                            const std::string want = tj.is_array() && !tj.empty()
                                ? tj[0].value("page-id", "") : "";
                            check(!want.empty() && m_selectedId == want,
                                  "ui-sim: click tab item selects its page");
                            fm.down = false; tickInput();
                        }
                        // ② 点击空白页签（末项右侧 +10）→ 选中整个 Tab
                        {
                            const auto s = toScreen(tx0 + r1.x + r1.w + 10.0f,
                                                    ty0 + r0.y + r0.h * 0.5f);
                            fm.wx = s.first; fm.wy = s.second; fm.down = false; tickInput();
                            fm.down = true; tickInput();
                            check(m_selectedId == tabsId,
                                  "ui-sim: click blank strip selects tab");
                            fm.down = false; tickInput();
                        }
                        // ③ 拖动页签项 0 → 项 1（排序）：按下 → 移动到项 1 → 释放
                        {
                            const auto s0 = toScreen(tx0 + r0.x + r0.w * 0.5f,
                                                     ty0 + r0.y + r0.h * 0.5f);
                            const auto s1 = toScreen(tx0 + r1.x + r1.w * 0.5f,
                                                     ty0 + r1.y + r1.h * 0.5f);
                            const nlohmann::json before = findModel(tabsId)->props.extended.value(
                                "tabs", nlohmann::json::array());
                            const std::string p0 = before[0].value("page-id", "");
                            const std::string p1 = before[1].value("page-id", "");
                            fm.wx = s0.first; fm.wy = s0.second; fm.down = false; tickInput();
                            fm.down = true; tickInput();
                            check(m_tabDragGhost.IsValid(),
                                  "ui-sim: drag ghost created on press (visible feedback)");
                            fm.wx = s1.first; fm.wy = s1.second; tickInput();
                            fm.down = false; tickInput();
                            check(!m_tabDragGhost.IsValid(),
                                  "ui-sim: drag ghost destroyed on release");
                            const nlohmann::json after = findModel(tabsId)->props.extended.value(
                                "tabs", nlohmann::json::array());
                            check(after.is_array() && after.size() == 2 &&
                                  after[0].value("page-id", "") == p1 &&
                                  after[1].value("page-id", "") == p0,
                                  "ui-sim: drag tab item reorders pages");
                        }
                        // ④ 用户流复现：放置全新 Tab → 点击其页签项 0 → 画布选中该页且
                        //    控件树同步（selected-id 读回——用户实测"两次才同步"）
                        {
                            placeControl("tTabs", 950, 520, false);
                            const std::string t2 = m_selectedId;   // placeControl 选中该 Tab
                            PlacedControl* tp2b = placedById(t2);
                            check(tp2b && tp2b->ctl.IsValid(), "ui-sim: fresh tab placed");
                            if (tp2b && tp2b->ctl.IsValid()) {
                                float fx = 0, fy = 0;
                                globalOriginOf(*tp2b, fx, fy);
                                UIRect fr{};
                                m_viewport->TabGetItemRect(tp2b->ctl, 0, fr);
                                {
                                    std::string hprobe;
                                    const bool hok = hitTest(fx + fr.x + fr.w * 0.5f,
                                                             fy + fr.y + fr.h * 0.5f, hprobe);
                                    std::printf("[ui-sim] fresh-tab strip hit: %s (want %s)\n",
                                                hprobe.c_str(), t2.c_str());
                                    (void)hok;
                                }
                                const auto fs = toScreen(fx + fr.x + fr.w * 0.5f,
                                                         fy + fr.y + fr.h * 0.5f);
                                fm.wx = fs.first; fm.wy = fs.second; fm.down = false; tickInput();
                                fm.down = true; tickInput();
                                const nlohmann::json tj2 = findModel(t2)->props.extended.value(
                                    "tabs", nlohmann::json::array());
                                const std::string want2 = tj2.is_array() && !tj2.empty()
                                    ? tj2[0].value("page-id", "") : "";
                                fm.down = false; tickInput();
                                const std::string treeSel =
                                    m_ui->TreeViewGetSelectedId(m_canvasTree);
                                std::printf("[ui-sim] fresh-click: sel=%s want=%s tree=%s\n",
                                            m_selectedId.c_str(), want2.c_str(), treeSel.c_str());
                                check(!want2.empty() && m_selectedId == want2,
                                      "ui-sim: fresh tab click selects its page");
                                check(treeSel == want2,
                                      "ui-sim: fresh tab click syncs tree selection");
                            }
                            selectItem(t2);
                            deleteSelected();   // 清理
                        }
                        // ⑤ 手柄让区重叠复现：先选中 Tab（激活手柄）→ 点页签项上缘
                        //   （2px，落在 resize 区）→ 仍应选中该项的页
                        {
                            PlacedControl* tabPc3 = placedById(tabsId);
                            if (tabPc3 && tabPc3->ctl.IsValid()) {
                                selectItem(tabsId);
                                float gx3 = 0, gy3 = 0;
                                globalOriginOf(*tabPc3, gx3, gy3);
                                UIRect rr{};
                                m_viewport->TabGetItemRect(tabPc3->ctl, 0, rr);
                                const auto s3 = toScreen(gx3 + rr.x + rr.w * 0.5f,
                                                         gy3 + rr.y + 2.0f);   // 上缘 2px
                                fm.wx = s3.first; fm.wy = s3.second; fm.down = false; tickInput();
                                fm.down = true; tickInput();
                                const nlohmann::json tj3 = findModel(tabsId)->props.extended.value(
                                    "tabs", nlohmann::json::array());
                                const std::string w3 = tj3.is_array() && !tj3.empty()
                                    ? tj3[0].value("page-id", "") : "";
                                fm.down = false; tickInput();
                                check(!w3.empty() && m_selectedId == w3,
                                      "ui-sim: item top-edge click (handle zone) selects page");
                            }
                        }
                        // ⑥ 工具拖放（画布侧）：候选 + 画布内释放 → 放置（用户报障回归）
                        {
                            const size_t nBefore = m_items.size();
                            m_pendingPlaceTool = "tBtn";
                            const auto ds = toScreen(100.0f, 600.0f);   // 画布空白区（避开页签项）
                            fm.wx = ds.first; fm.wy = ds.second; fm.down = true; tickInput();
                            fm.down = false; tickInput();   // 画布内释放 → placeControl
                            const bool ok6 = m_items.size() == nBefore + 1 &&
                                             m_pendingPlaceTool.empty();
                            std::printf("[ui-sim] toolbox drop: placed=%d\n", ok6 ? 1 : 0);
                            check(ok6, "ui-sim: toolbox pending tool drops to canvas");
                            if (m_items.size() == nBefore + 1) {
                                selectItem(m_items.back().model->props.id);
                                deleteSelected();
                            }
                        }
                        m_mouseSource = savedMouse;
                        // 还原顺序（拖动测试若已换位——按"首页"标题复位）
                        {
                            const nlohmann::json after = findModel(tabsId)->props.extended.value(
                                "tabs", nlohmann::json::array());
                            int idxHome = -1;
                            if (after.is_array())
                                for (int i2 = 0; i2 < (int)after.size(); ++i2)
                                    if (after[(size_t)i2].value("title", "") == u8"首页")
                                        idxHome = i2;
                            if (idxHome == 1) moveTabPage(tabsId, 1, 0);
                        }
                    }
                }
                m_placeTargetId.clear();
            }
        }
    }
    const int n0 = (int)m_items.size();   // 全部构建完成后（含页 Panel/页内控件）
    const int childCount0 = (int)std::count_if(m_items.begin(), m_items.end(),
        [](const PlacedControl& p) { return !p.parentId.empty(); });
    const std::string csd = dir + u8"/自测.csd";   // 中文文件名：覆盖宽字符路径
    const std::string js = dir + u8"/自测.json";
    check(!listId.empty() && !childId.empty(), "setup: list-view & container child placed");
    // 控件 id 重命名（用户需求回归：模型/子引用/拒绝非法/导出携带）
    {
        check(renameControlId(panelId, "myPanel"), "rename: panel id");
        bool idOk = false, refOk = false;
        for (auto& it : m_items) {
            if (it.model->props.id == "myPanel") idOk = true;
            if (it.model->props.id == childId && it.parentId == "myPanel") refOk = true;
        }
        check(idOk, "rename: model id updated");
        check(refOk, "rename: child parentId updated");
        check(!renameControlId("myPanel", ""), "rename: empty rejected");
        check(!renameControlId("myPanel", childId), "rename: duplicate rejected");
    }
    // 连续改名无残留（模拟逐键改名的树同步——中间 id 不应残留节点）
    {
        const std::string b0 = "myPanel", b1 = b0 + "A", b2 = b1 + "B", b3 = b2 + "C";
        check(renameControlId(b0, b1), "rename-seq: 1");
        check(renameControlId(b1, b2), "rename-seq: 2");
        check(renameControlId(b2, b3), "rename-seq: 3");
        if (m_canvasTree.IsValid() && m_ui) {
            check(!m_ui->TreeViewSelectNode(m_canvasTree, b0), "rename-seq: original node gone");
            check(!m_ui->TreeViewSelectNode(m_canvasTree, b1), "rename-seq: intermediate-1 gone");
            check(!m_ui->TreeViewSelectNode(m_canvasTree, b2), "rename-seq: intermediate-2 gone");
            check(m_ui->TreeViewSelectNode(m_canvasTree, b3), "rename-seq: final node present");
        }
        check(renameControlId(b3, b0), "rename-seq: restore");
        for (auto& it : m_items) it.parentId = (it.parentId == b3) ? b0 : it.parentId;
    }
    // 事件面板（F4）：绑定/校验/去重/建议命名
    {
        std::string bId;
        for (auto& it : m_items)
            if (it.model->props.type == "button") bId = it.model->props.id;
        check(!bId.empty(), "events: button found");
        check(addEventBinding(bId, "onClick", "onBtn1_Click"), "events: bind onClick");
        check(!isValidIdentifier("1abc") && isValidIdentifier("_a1"), "events: identifier rules");
        check(!addEventBinding(bId, "onTextChanged", "onX"), "events: non-applicable event rejected");
        check(!addEventBinding(bId, "onClick", "onBtn1_Click"), "events: duplicate handler rejected");
        const std::string sug = suggestHandlerName(bId, "onClick");
        check(sug.rfind("on", 0) == 0 && sug.find("Click") != std::string::npos,
              "events: suggest name");
    }
    // 画布（Bench）属性往返：背景（alpha=00 透明——游戏叠加场景）/缩放模式/id/标题
    m_benchBg = UIColor{0x11, 0x22, 0x33, 0x00};
    m_benchScaleMode = 1;   // fit
    m_benchId = "mainCanvas";
    m_benchTitle = u8"游戏 UI";
    selectBench();
    check(m_benchSelected && m_selectedId.empty(), "bench: select (rows built)");
    {
        // 画布 id/标题行存在且值正确（读回验证——用户"无法添加"回归）
        int idRows = 0, titleRows = 0;
        for (auto& r : m_dynamicRows) {
            if (!r.IsValid()) continue;
            std::string tx = r.GetString(PropertyNames::kTextContent);
            if (tx.empty()) tx = r.GetString(PropertyNames::kCaption);   // 值显示行为 Label
            if (tx == m_benchId) ++idRows;
            if (!m_benchTitle.empty() && tx == m_benchTitle) ++titleRows;
        }
        check(idRows >= 1 && titleRows >= 1, "bench: id/title rows present with values");
    }
    selectItem("");
    check(!m_benchSelected, "bench: cleared by control-selection path");
    check(SaveProject(csd), "SaveProject");
    NewProject();
    std::fflush(stdout);
    check(m_items.empty(), "NewProject clears canvas");
    check(OpenProject(csd), "OpenProject");
    std::fflush(stdout);
    // P0-92：tab 配色 save/open 往返（schema 白名单透传 + 解析/回放全链）
    {
        bool tcOk = false;
        for (auto& it2 : m_items)
            if (it2.model->props.type == "tab-control") {
                const std::string v = it2.model->props.extended.value(
                    PropertyNames::kTabBackground, std::string());
                UIColor engc{0, 0, 0, 0};
                if (it2.ctl.IsValid())
                    engc = it2.ctl.GetColor(PropertyNames::kTabBackground);
                if (v == "#445566FF" && engc.r == 0x44 && engc.g == 0x55 && engc.b == 0x66)
                    tcOk = true;
            }
        check(tcOk, "open: tab strip color roundtrip (model + engine)");
    }
    check((int)m_items.size() == n0, "open: control count preserved");
    const int childCount1 = (int)std::count_if(m_items.begin(), m_items.end(),
        [](const PlacedControl& p) { return !p.parentId.empty(); });
    check(childCount1 == childCount0, "open: parentId relationships preserved");
    check((int)m_designW == 1440 && (int)m_designH == 900, "open: canvas size from metadata");
    {
        bool evOk = false;
        for (auto& it : m_items)
            if (it.model->props.type == "button") {
                const auto& ev = it.model->props.extended.value("events", nlohmann::json::object());
                evOk = ev.is_object() && ev.value("onClick", "") == "onBtn1_Click";
            }
        check(evOk, "open: event binding preserved");
    }
    check(m_benchBg.r == 0x11 && m_benchBg.g == 0x22 && m_benchBg.b == 0x33 && m_benchBg.a == 0x00,
          "open: bench background (transparent) preserved");
    check(m_benchScaleMode == 1, "open: bench scale-mode preserved");
    check(m_benchId == "mainCanvas", "open: bench id preserved");
    check(m_benchTitle == u8"游戏 UI", "open: bench title preserved");
    for (auto& it : m_items) {
        if (it.model->props.type != "list-view") continue;
        const auto& ex = it.model->props.extended;
        check(ex.value("font-size", 0) == 15, "open: font-size preserved");
        check(ex.contains("colors") && ex["colors"].contains("background") &&
              ex["colors"]["background"].contains("normal") &&
              ex["colors"]["background"]["normal"].get<std::string>() == "#112233FF",
              "open: colors preserved");
        check(ex.contains("columns") && ex["columns"].size() == 1 &&
              ex["columns"][0].value("header-font-name", "") == "quando-regular" &&
              ex["columns"][0].value("header-size", 0) == 16,
              "open: columns header style preserved");
        check(ex.contains("rows") && ex["rows"][0]["cells"].size() == 2 &&
              ex["rows"][0]["cells"][1].value("text", "") == "B1" &&
              ex["rows"][0]["cells"][1].value("hover-background-color", "") == "#0000FFFF",
              "open: rows object cells preserved");
        // 引擎应用验证：字号读回
        int fs = it.ctl.GetInt(PropertyNames::kFontSize);
        check(fs == 15, "open: engine font-size applied");
    }
    for (auto& it : m_items) {
        if (it.model->props.type != "tree-view") continue;
        const auto& ex = it.model->props.extended;
        check(ex.contains("items") && ex["items"].size() == 2 &&
              ex["items"][0].value("label", "") == "A" &&
              ex["items"][1].value("label", "") == "B",
              "open: tree root order preserved");
        check(ex["items"][0].contains("children") && ex["items"][0]["children"].size() == 2 &&
              ex["items"][0]["children"][0].value("label", "") == "A1" &&
              ex["items"][0]["children"][1].value("label", "") == "A2",
              "open: tree sibling order preserved");
    }
    for (auto& it : m_items) {
        if (it.model->props.type != "menu-bar") continue;
        const auto& ex = it.model->props.extended;
        check(ex.contains("menus") && ex["menus"].size() == 2 &&
              ex["menus"][0].value("caption", "") == "文件" &&
              ex["menus"][1].value("caption", "") == "帮助",
              "open: menus top-level preserved");
        const auto& mi = ex["menus"][0]["items"];
        check(mi.size() == 3 && mi[0].value("caption", "") == "新建" &&
              mi[0].value("shortcut", "") == "Ctrl+N" &&
              mi[1].value("type", "") == "separator" &&
              mi[2].value("caption", "") == "退出",
              "open: menu items order/shortcut/separator preserved");
        check(ex["menus"][1]["items"][0].value("caption", "") == "关于",
              "open: second menu items preserved");
    }
    for (auto& it : m_items) {
        if (it.model->props.type != "tab-control") continue;
        const auto& ex = it.model->props.extended;
        check(ex.contains("tabs") && ex["tabs"].size() == 2 &&
              ex["tabs"][0].value("title", "") == "首页" &&
              ex["tabs"][1].value("title", "") == "设置",
              "open: tabs titles preserved");
        const std::string p0 = ex["tabs"][0].value("page-id", "");
        check(!p0.empty() && !ex["tabs"][1].value("page-id", "").empty(),
              "open: tab page ids present");
        int inPage = 0;
        for (auto& it2 : m_items)
            if (it2.parentId == p0 && it2.model->props.type == "label") ++inPage;
        check(inPage == 1, "open: control inside tab page preserved");
    }
    check(ExportLayout(js), "ExportLayout");
    // P0-92：布局导出携带 tab 配色键（schema 白名单透传——导出应用经引擎解析生效）
    {
        std::ifstream jf(utf8Path(js), std::ios::binary);
        const std::string jt((std::istreambuf_iterator<char>(jf)),
                             std::istreambuf_iterator<char>());
        check(jt.find("\"tab-background\"") != std::string::npos,
              "export: tab color key carried");
        check(jt.find("\"reorderable\": true") != std::string::npos,
              "export: reorderable carried (preview/runtime drag)");
        // P0-96 已闭环（引擎自含）：不再自动补 draggable/droppable（过渡耦合已回收）
        check(jt.find("\"droppable\": true") == std::string::npos &&
              jt.find("\"draggable\": true") == std::string::npos,
              "export: no draggable/droppable auto-add (P0-96 self-contained)");
    }
    // P0-94 设计态约束联测：向导出布局注入 reorderable=true → 导入后
    // 模型保留该键、引擎侧被强制关闭（宿主拖动接管编辑交互）
    {
        std::ifstream jf2(utf8Path(js), std::ios::binary);
        std::string jt2((std::istreambuf_iterator<char>(jf2)),
                        std::istreambuf_iterator<char>());
        jf2.close();
        const auto p2 = jt2.find("\"type\": \"tab-control\"");
        if (p2 != std::string::npos) {
            jt2.insert(p2, "\"reorderable\": true, ");
            std::ofstream jw(utf8Path(js), std::ios::binary);
            jw << jt2;
        }
    }
    NewProject();
    check(ImportLayout(js), "ImportLayout");
    check((int)m_items.size() == n0, "import: control count preserved");
    // P0-94 设计态约束（续）：模型保留 reorderable、引擎侧强制关闭
    {
        bool modelKept = false, engineOff = false, foundTab = false;
        for (auto& it2 : m_items)
            if (it2.model->props.type == "tab-control") {
                foundTab = true;
                modelKept = it2.model->props.extended.value(PropertyNames::kReorderable, false);
                if (it2.ctl.IsValid())
                    engineOff = !it2.ctl.GetBool(PropertyNames::kReorderable) &&
                                !it2.ctl.GetBool(PropertyNames::kDraggable) &&
                                !it2.ctl.GetBool(PropertyNames::kDroppable);
            }
        check(foundTab && modelKept, "import: reorderable kept in model");
        check(engineOff, "import: drag flags forced off in engine (design canvas)");
    }
    bool listOk = false;
    for (auto& it : m_items)
        if (it.model->props.type == "list-view" && it.model->props.extended.contains("rows"))
            listOk = true;
    check(listOk, "import: structured data preserved");
    // 引擎级：StatusBar 颜色已应用（背景 normal/hover 与模型一致）
    bool sbColorOk = false;
    for (auto& it : m_items) {
        if (it.model->props.type != "status-bar" || !it.ctl.IsValid()) continue;
        UIColor c = it.ctl.GetColor(PropertyNames::kBackground);
        sbColorOk = (c.r == 255 && c.g == 0 && c.b == 0);
    }
    check(sbColorOk, "import: status-bar colors applied to engine");
    // 回归：Image 的 provider 路径经保存/打开保持（不得被放置默认值覆盖）
    bool imgPathOk = false;
    for (auto& it : m_items)
        if (it.model->props.type == "image")
            imgPathOk = it.model->props.extended.value("image", "") == "provider:test/bombBlock.jsonc";
    check(imgPathOk, "import: image provider path preserved");
    // 导出 JSON 文件级校验：StatusBar 颜色携带（背景 normal/hover）
    {
        std::ifstream jf(utf8Path(js), std::ios::binary);
        std::string jt((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
        bool colorsOk = false, renameOk = false, benchOk = false;
        nlohmann::json jj;
        try {
            jj = nlohmann::json::parse(jt);
            if (jj.contains("viewport") && jj["viewport"].is_object()) {
                const auto& vp = jj["viewport"];
                benchOk = vp.value("background", "") == "#11223300" &&
                          vp.value("scale-mode", "") == "fit" &&
                          vp.value("id", "") == "mainCanvas";
            }
            std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n) {
                if (n.is_object()) {
                    if (n.value("type", "") == "status-bar" && n.contains("colors")) {
                        const auto& c = n["colors"];
                        if (c.contains("background") &&
                            c["background"].value("normal", "") == "#FF0000FF" &&
                            c["background"].value("hover", "") == "#00FF00FF")
                            colorsOk = true;
                    }
                    if (n.value("id", "") == "myPanel") renameOk = true;
                    if (n.contains("controls") && n["controls"].is_array())
                        for (auto& x : n["controls"]) walk(x);
                    if (n.contains("children") && n["children"].is_array())
                        for (auto& x : n["children"]) walk(x);
                }
            };
            walk(jj);
        } catch (...) {}
        check(colorsOk, "export: status-bar colors carried");
        check(renameOk, "export: renamed id carried");
        check(benchOk, "export: bench viewport carried (bg alpha/scale/id)");
        // 树/状态栏项 hb# → 独立 hover-background-color 键（引擎 P0-75 已支持；模型 1:1）
        bool hbOk = false, hbTreeOk = false;
        std::function<void(const nlohmann::json&)> walkHb = [&](const nlohmann::json& n) {
            if (n.is_object()) {
                if (n.value("type", "") == "status-bar" && n.contains("items") && n["items"].is_array() &&
                    !n["items"].empty()) {
                    const auto& it0 = n["items"][0];
                    if (it0.value("background-color", "") == "#11223344" &&
                        it0.value("hover-background-color", "") == "#AABBCCDD" &&
                        !it0["background-color"].is_object())
                        hbOk = true;
                }
                if (n.value("type", "") == "tree-view" && n.contains("items") && n["items"].is_array() &&
                    !n["items"].empty()) {
                    const auto& it0 = n["items"][0];
                    if (it0.value("hover-background-color", "") == "#00112233" &&
                        !it0.contains("background-color"))
                        hbTreeOk = true;   // 仅 hover：不引入常态背景
                }
                for (const char* k : { "controls", "children" }) {
                    if (n.contains(k) && n[k].is_array())
                        for (auto& x : n[k]) walkHb(x);
                }
            }
        };
        walkHb(jj);
        check(hbOk, "export: status-bar item hb# standalone key");
        check(hbTreeOk, "export: tree item hb# standalone key");
        // 菜单面板属性（P0-72①）导出携带
        bool menuOk = false;
        std::function<void(const nlohmann::json&)> walkMenu = [&](const nlohmann::json& n) {
            if (n.is_object()) {
                if (n.value("type", "") == "menu-bar" && n.contains("menus") && n["menus"].is_array() &&
                    !n["menus"].empty()) {
                    const auto& m0 = n["menus"][0];
                    if (m0.value("shadow-radius", 0.0f) == 12.0f &&
                        m0.contains("font") && m0["font"].value("size", 0) == 12)
                        menuOk = true;
                }
                for (const char* k : { "controls", "children" }) {
                    if (n.contains(k) && n[k].is_array())
                        for (auto& x : n[k]) walkMenu(x);
                }
            }
        };
        walkMenu(jj);
        check(menuOk, "export: menu panel attrs carried");
        // F4.3：事件列表文件（与导出同名基）
        {
            std::string base = js;
            const auto dot = base.find_last_of('.');
            base = (dot == std::string::npos) ? base : base.substr(0, dot);
            std::ifstream hf(utf8Path(base + "_events.h"), std::ios::binary);
            std::string ht((std::istreambuf_iterator<char>(hf)), std::istreambuf_iterator<char>());
            std::ifstream lf(utf8Path(base + "_event_list.json"), std::ios::binary);
            std::string lt((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
            check(ht.find("void onBtn1_Click(UIControlHandle ctl, void* user);") != std::string::npos,
                  "export: events.h signature");
            check(lt.find("onBtn1_Click") != std::string::npos &&
                  lt.find("\"event\": \"onClick\"") != std::string::npos,
                  "export: event_list.json entry");
        }
    }
    // F4.4/F7.2 代码骨架导出：目录内四件套
    {
        const std::string codeDir = dir + "/自测_代码";
        check(ExportCode(codeDir), "code: export ok");
        auto readText = [&](const std::string& p2) {
            std::ifstream f(utf8Path(p2), std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        check(readText(codeDir + "/layout.json").find("\"controls\"") != std::string::npos,
              "code: layout.json written");
        check(readText(codeDir + "/events.h").find(
                  "void onBtn1_Click(UIControlHandle ctl, void* user);") != std::string::npos,
              "code: events.h declaration");
        {
            const std::string ec = readText(codeDir + "/events.c");
            check(ec.find("#include \"events.h\"") != std::string::npos &&
                  ec.find("void onBtn1_Click(UIControlHandle ctl, void* user) {") != std::string::npos,
                  "code: events.c skeleton");
        }
        check(readText(codeDir + "/event_list.json").find("onBtn1_Click") != std::string::npos,
              "code: event_list.json written");
        // F7.3 宿主工程（可直接编译）
        {
            const std::string mc = readText(codeDir + "/main.c");
            check(mc.find("#include \"events.h\"") != std::string::npos &&
                  mc.find("fnRegisterAction(inst, \"onBtn1_Click\", onBtn1_Click, NULL);") != std::string::npos,
                  "code: main.c host + handler registration");
        }
        check(readText(codeDir + "/CMakeLists.txt").find("add_executable") != std::string::npos,
              "code: CMakeLists.txt written");
        check(readText(codeDir + "/build_debug.bat").find("--config Debug") != std::string::npos &&
              readText(codeDir + "/build_release.bat").find("--config Release") != std::string::npos,
              "code: build scripts (debug/release)");
        check(readText(codeDir + "/CMakeLists.txt").find(
                  "WIN32_EXECUTABLE \"$<CONFIG:Release>\"") != std::string::npos,
              "code: release no-console (WIN32_EXECUTABLE)");
        check(readText(codeDir + "/include/UICornerstoneAPI.h").find(
                  "UICornerstone_CreateInstanceFromPlugin") != std::string::npos,
              "code: C ABI header copied");
        {
            std::error_code ecH;
            check(std::filesystem::exists(utf8Path(codeDir + "/UICornerstone.dll"), ecH) &&
                  std::filesystem::exists(utf8Path(codeDir + "/UIBackend_sdl3.dll"), ecH),
                  "code: runtime DLLs copied");
            check(std::filesystem::exists(utf8Path(codeDir + "/assets/fonts"), ecH),
                  "code: assets copied");
        }
        // F7.4 C++（C++ Binding）工程导出
        {
            const std::string cppDir = dir + "/自测_代码_cpp";
            check(ExportCodeCpp(cppDir), "codec++: export ok");
            const std::string mcp = readText(cppDir + "/main.cpp");
            check(mcp.find("#include \"UICornerstone.h\"") != std::string::npos &&
                  mcp.find("ui->RegisterAction(\"onBtn1_Click\", [](Control c) {") != std::string::npos,
                  "codec++: main.cpp binding host + registration");
            check(readText(cppDir + "/CMakeLists.txt").find("add_subdirectory(binding") != std::string::npos,
                  "codec++: CMakeLists binding subdir");
            std::error_code ecB;
            check(std::filesystem::exists(utf8Path(cppDir + "/binding/CMakeLists.txt"), ecB) &&
                  std::filesystem::exists(utf8Path(cppDir + "/binding/src/UICornerstone.cpp"), ecB),
                  "codec++: binding package copied");
            check(std::filesystem::exists(utf8Path(cppDir + "/include/PropertyNames.h"), ecB),
                  "codec++: PropertyNames.h copied");
            check(std::filesystem::exists(utf8Path(cppDir + "/build_release.bat"), ecB),
                  "codec++: build script present");
        }
    }
    // F2.5 撤销/重做：放置 / 属性（颜色）/ 删除 三步往返
    {
        const size_t n0 = m_items.size();
        placeControl("tBtn", 200, 200, false);
        check(m_items.size() == n0 + 1, "undo: place applied");
        const std::string pid = m_items.back().model->props.id;
        check(Undo() && m_items.size() == n0, "undo: place undone");
        check(Redo() && m_items.size() == n0 + 1, "redo: place redone");
        {
            // 文本色回放保真：撤销/重建后宿主 + 内部 Label 文本色均为样式色 #333333
            //（回归：曾因宿主状态色未同步，重建后 caption 用缺省色渲染为灰）
            PlacedControl* pcB = placedById(pid);
            UIColor hostT{0, 0, 0, 0}, labT{0, 0, 0, 0};
            void* capH2 = nullptr;
            if (pcB && pcB->ctl.IsValid()) {
                hostT = pcB->ctl.GetColor("text");
                capH2 = pcB->ctl.GetPtr(PropertyNames::kCaptionLabel);
            }
            if (capH2) labT = m_viewport->FromHandle(capH2).GetColor("text");
            check(hostT.r == 0x33 && hostT.g == 0x33 && hostT.b == 0x33,
                  "undo: host text color #333333 after rebuild");
            check(labT.r == 0x33 && labT.g == 0x33 && labT.b == 0x33,
                  "undo: caption label text color #333333 after rebuild");
        }
        auto bgNormalOf = [&](const std::string& id) -> std::string {
            const ControlModel* mm = findModel(id);
            if (!mm) return std::string();
            return mm->props.extended.value("colors", nlohmann::json::object())
                .value("background", nlohmann::json::object())
                .value("normal", std::string());
        };
        const std::string oldC = bgNormalOf(pid);
        writeColorSlotTo(pid, "background", "background", UIColor{0x11, 0x22, 0x33, 0xFF});
        const std::string newC = bgNormalOf(pid);
        check(!newC.empty() && newC != oldC, "undo: color write applied");
        check(Undo(), "undo: color undone (flag)");
        check(bgNormalOf(pid) == oldC, "undo: color restored");
        check(Redo(), "redo: color redone (flag)");
        check(bgNormalOf(pid) == newC, "redo: color restored");
        selectItem(pid);
        deleteSelected();
        check(m_items.size() == n0, "undo: delete applied");
        check(Undo() && m_items.size() == n0 + 1, "undo: delete undone");
        check(Redo() && m_items.size() == n0, "redo: delete redone");
    }
    // F2.5 动态行覆盖：X/Y/W/H NUD（applyModelRect）与 caption（applyCaptionTo）可撤销
    {
        placeControl("tBtn", 300, 300, false);
        const std::string pid2 = m_items.back().model->props.id;
        ControlModel* mm2 = findModel(pid2);
        const SRect r0 = mm2->props.rect;
        mm2->props.rect.x = r0.x + 40.0f;
        applyModelRect(mm2);
        check(Undo(), "undo: NUD x change undoable");
        ControlModel* after = findModel(pid2);
        check(after != nullptr && after->props.rect.x == r0.x, "undo: NUD x restored");
        const std::string oldCap = after != nullptr
            ? after->props.extended.value("caption", std::string()) : std::string();
        applyCaptionTo(pid2, u8"新标题");
        check(Undo(), "undo: caption edit undoable");
        ControlModel* after2 = findModel(pid2);
        check(after2 != nullptr && after2->props.extended.value("caption", std::string()) == oldCap,
              "undo: caption restored");
    }
    // F2.5 动态行覆盖：结构化文本行（tree items）可撤销
    {
        const std::string tid = SelfTestIdOfType("tree-view");
        if (!tid.empty()) {
            ControlModel* tm2 = findModel(tid);
            const nlohmann::json oldItems =
                tm2->props.extended.value("items", nlohmann::json::array());
            applyTreeItemsText(*tm2, u8">X@#AABBCCDD");
            check(Undo(), "undo: structured tree items undoable");
            ControlModel* after3 = findModel(tid);
            check(after3 != nullptr &&
                  after3->props.extended.value("items", nlohmann::json::array()) == oldItems,
                  "undo: tree items restored");
        }
    }
    // 长路径（>260 字符）保存/打开往返（Windows MAX_PATH 规避——utf8Path 的 \\?\ 前缀）
    {
        std::string longDir = dir + "/";
        for (int i = 0; i < 9; ++i) longDir += "selftest-long-20260716T084504/";
        const std::string longPath = longDir + "long project.csd";
        std::printf("[SELFTEST-IO] long path len=%zu\n", longPath.size());
        if (longPath.size() > 260) {
            std::error_code ec;
            std::filesystem::create_directories(utf8Path(longDir), ec);
            const size_t nBefore = m_items.size();
            bool saveOk = SaveProject(longPath);
            NewProject();
            bool openOk = OpenProject(longPath);
            check(saveOk && openOk && m_items.size() == nBefore,
                  "long-path: save/open roundtrip (>260 chars)");
        } else {
            check(false, "long-path: fixture path length < 261 (unexpected)");
        }
    }
    NewProject();
    std::printf("[SELFTEST-IO] pass=%d fail=%d\n", pass, fail);
    return fail == 0;
}

// auto=3 冒烟：放置少量样例控件（预览窗口多实例测试用）
void CanvasPane::SelfTestPlaceSample() {
    NewProject();
    placeControl("tBtn", 200, 150, false);
    placeControl("tLabel", 420, 150, false);
    placeControl("tList", 300, 350, false);
    placeControl("tStatus", 200, 500, false);
    placeControl("tImage", 700, 150, false);
    placeControl("tAni", 700, 350, false);
    placeControl("tMenu", 200, 60, false);
    placeControl("tTabs", 900, 500, false);   // P0-96 预览拖拽旗标联测（模型置可重排）
    // 预览冒烟数据（用户场景回归）：StatusBar **仅设 hover**（部分对象语义——引擎
    // 曾把未给出的 normal 重置为黑，构造缺省 VSCode 蓝丢失；P0-74④ 修复后应保持蓝）
    for (auto& it : m_items) {
        if (it.model->props.type == "status-bar")
            writeColorSlotTo(it.model->props.id, "background.hover", "background", UIColor{0, 255, 0, 255});
    }
    // P0-96：Tab 模型置可重排（序列化自动补 draggable/droppable——预览端到端联测）
    for (auto& it : m_items)
        if (it.model->props.type == "tab-control")
            it.model->props.extended[PropertyNames::kReorderable] = true;
    // 菜单栏 pressed 色（用户问题 2：预览态生效回归）
    for (auto& it : m_items)
        if (it.model->props.type == "menu-bar")
            writeColorSlotTo(it.model->props.id, "background.pressed", "background",
                             UIColor{255, 128, 0, 255});
    // 资源 provider 挂载回归（用户实测口径：test=引擎样例目录；svg 图 + jsonc 动画。
    // 帧注册覆盖两种 src：资产相对键（先资源根）与 jsonc 相对（次 jsonc 目录）——
    // 决定性用例（src 仅 jsonc 目录存在）曾验证通过，用例文件在 Temp/ani_test）
    m_resourceProviders.push_back(
        { "test", "D:/GitSpace/UIControls/subModules/assets/animations/bombBlock" });
    refreshResourceList();
    mountResourceProviders();   // 设计器画布同步挂载（编辑态 provider 引用回归）
    for (auto& it : m_items) {
        if (it.model->props.type == "image")
            writeStringPropTo(it.model->props.id, "image", "provider:test/bomb.svg");
        if (it.model->props.type == "animation")
            writeStringPropTo(it.model->props.id, "path", "provider:test/bombBlock.jsonc");
    }
    // 画布背景灰（#C0C0C0FF——用户问题 1 实测值）
    m_benchBg = UIColor{0xC0, 0xC0, 0xC0, 255};
    applyBenchBackground();
}

std::string CanvasPane::SelfTestIdOfType(const std::string& type) const {
    for (auto& it : m_items)
        if (it.model->props.type == type) return it.model->props.id;
    return std::string();
}

// 新建（清空画布 + 解绑项目路径 + 缺省画布尺寸 + 清资源）
void CanvasPane::NewProject() {
    clearCanvas();
    SetCanvasSize(1280.0f, 800.0f);
    m_resourceProviders.clear();
    refreshResourceList();
    mountResourceProviders();
    m_projectPath.clear();
    resetUndo();   // F2.5：新建 = 新撤销基线
    setStatusHint(u8"已新建项目");
}

// ── 资源面板（布局级 resourceProviders）──

void CanvasPane::SetResourcePanel(Control list) {
    m_resList = list;
    if (m_resList.IsValid())
        m_resList.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event&) {
            std::string n, p;   // 选中行 → 状态栏显示全路径（列宽有限）
            if (SelectedResourceProvider(n, p)) setStatusHint(u8"资源：" + n + " = " + p);
        });
    refreshResourceList();
    mountResourceProviders();
}

// 列表重填（列已在 main_layout.json 声明：名称/路径；行 id rp_i）。
// 注：C ABI RemoveRow 恒返回 1（不反映越界）——按记账行数移除，禁用 while 探测。
void CanvasPane::refreshResourceList() {
    if (!m_resList.IsValid() || !m_ui) return;
    for (int i = m_resRows - 1; i >= 0; --i) m_ui->ListViewRemoveRow(m_resList, i);
    for (size_t i = 0; i < m_resourceProviders.size(); ++i) {
        std::vector<std::string> cells = { m_resourceProviders[i].first,
                                           m_resourceProviders[i].second };
        m_ui->ListViewAddRow(m_resList, "rp_" + std::to_string(i), cells);
    }
    m_resRows = (int)m_resourceProviders.size();
}

// 插入（App 弹窗收集名称+目录后调用）：非空 + 名称唯一校验 → 入列 + 刷新 + 挂载
bool CanvasPane::AddResourceProvider(const std::string& name, const std::string& path) {
    if (name.empty() || path.empty()) {
        setStatusHint(u8"资源添加失败：名称/路径为空");
        return false;
    }
    for (const auto& rp : m_resourceProviders)
        if (rp.first == name) {
            setStatusHint(u8"资源名重复：" + name + u8"（未添加）");
            return false;
        }
    m_resourceProviders.push_back({ name, path });
    refreshResourceList();
    mountResourceProviders();
    setStatusHint(u8"已添加资源：" + name + " = " + path);
    std::printf("[CanvasPane] resource provider added: %s = %s\n", name.c_str(), path.c_str());
    return true;
}

// 选中行（App 弹窗回填用）
bool CanvasPane::SelectedResourceProvider(std::string& name, std::string& path) const {
    if (!m_resList.IsValid()) return false;
    const int idx = m_resList.GetInt(PropertyNames::kSelectedIndex);
    if (idx < 0 || idx >= (int)m_resourceProviders.size()) return false;
    name = m_resourceProviders[(size_t)idx].first;
    path = m_resourceProviders[(size_t)idx].second;
    return true;
}

// 修改选中行：非空 + 名称唯一（排除自身）→ 更新 + 刷新 + 挂载
bool CanvasPane::UpdateSelectedResourceProvider(const std::string& name, const std::string& path) {
    if (!m_resList.IsValid() || !m_ui) return false;
    const int idx = m_resList.GetInt(PropertyNames::kSelectedIndex);
    if (idx < 0 || idx >= (int)m_resourceProviders.size()) {
        setStatusHint(u8"请先在资源列表中选择要修改的条目");
        return false;
    }
    if (name.empty() || path.empty()) {
        setStatusHint(u8"资源修改失败：名称/路径为空");
        return false;
    }
    for (size_t i = 0; i < m_resourceProviders.size(); ++i)
        if ((int)i != idx && m_resourceProviders[i].first == name) {
            setStatusHint(u8"资源名重复：" + name + u8"（未修改）");
            return false;
        }
    m_resourceProviders[(size_t)idx] = { name, path };
    refreshResourceList();
    mountResourceProviders();
    setStatusHint(u8"已修改资源：" + name + " = " + path);
    return true;
}

// 删除（列表选中行）
void CanvasPane::RemoveSelectedResourceProvider() {
    if (!m_resList.IsValid() || !m_ui) return;
    const int idx = m_resList.GetInt(PropertyNames::kSelectedIndex);
    if (idx < 0 || idx >= (int)m_resourceProviders.size()) {
        setStatusHint(u8"请先在资源列表中选择要删除的条目");
        return;
    }
    const std::string name = m_resourceProviders[(size_t)idx].first;
    m_resourceProviders.erase(m_resourceProviders.begin() + idx);
    refreshResourceList();
    mountResourceProviders();
    setStatusHint(u8"已删除资源：" + name);
}

// 设计态挂载：经 LoadLayout 顶层 resourceProviders（引擎 mountPath 懒加载；
// 无独立运行时挂载 API——布局挂载路径无 controls 亦执行）
void CanvasPane::mountResourceProviders() {
    if (!m_viewport) return;
    nlohmann::json j;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rp : m_resourceProviders)
        arr.push_back({ { "name", rp.first }, { "path", rp.second } });
    j["resourceProviders"] = std::move(arr);
    m_viewport->LoadLayout(j.dump());
}

// 状态栏右段：缩放/设计尺寸/控件数/网格（缩放/画布尺寸/网格开关/增删控件时刷新）。
void CanvasPane::refreshStatusRight() {
    std::string txt = u8"缩放 " + std::to_string((int)std::lround(m_zoom * 100.0f)) + "%"
                    + u8"  |  画布 " + std::to_string((int)m_designW) + u8"×"
                    + std::to_string((int)m_designH)
                    + u8"  |  控件 " + std::to_string(m_items.size())
                    + u8"  |  网格 " + (m_gridVisible ? u8"开" : u8"关");
    writeStatusItem("statusRight", txt);
}

// P1 预览模式：设计器交互整体关闭（控件交互由引擎子视口事件驱动——ProcessViewportEvents
// 常开）；进入清选择/去网格线/清放置与拖动状态；退出恢复网格并重写放置目标提示。
void CanvasPane::SetPreviewMode(bool on) {
    if (m_previewMode == on) return;
    m_previewMode = on;
    if (on) {
        selectItem("");                       // 清选择（手柄/次级外框隐藏）
        if (m_alignV.IsValid()) m_alignV.SetBool(PropertyNames::kVisible, 0);
        if (m_alignH.IsValid()) m_alignH.SetBool(PropertyNames::kVisible, 0);
        m_pendingPlaceTool.clear();
        if (m_dragGhost.IsValid()) { m_dragGhost.Destroy(); m_dragGhost = Control(); }
        m_dragId.clear();
        m_dragParentId.clear();
        m_dragCtl = Control();
        m_groupDrag = false;
        m_groupEntries.clear();
        m_bandActive = false;
        if (m_bandRect.IsValid()) m_bandRect.SetBool(PropertyNames::kVisible, 0);
        m_mouseWasDown = false;
        m_gridVisibleSaved = m_gridVisible;
        SetGridVisible(false);                // 白纸无网格线（复用网格开关瓦片机制）
        setStatusHint(u8"预览模式（F5 或工具栏退出）");
    } else {
        SetGridVisible(m_gridVisibleSaved);
        refreshPlaceTargetHint();
    }
}

// 画布控件增删/选中 → 树同步（编程式，触发 select 回调被 m_treeSync 抑制）。
// tvTree 在主实例，经 m_ui 成员方法操作。
void CanvasPane::syncTreeAdd(const std::string& parentId, const std::string& id,
                             const std::string& label) {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    // P1-a 层级：容器子控件挂到父控件节点下（顶层挂画布根节点）
    m_ui->TreeViewAddNode(m_canvasTree, parentId.empty() ? kBenchTreeNode : parentId, id, label);
    m_treeSync = false;
}

void CanvasPane::syncTreeRemove(const std::string& id) {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    m_ui->TreeViewRemoveNode(m_canvasTree, id);
    m_treeSync = false;
}

void CanvasPane::syncTreeRename(const std::string& id, const std::string& label) {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    m_ui->TreeViewSetNodeLabel(m_canvasTree, id, label);
    m_treeSync = false;
}

// 控件 id 重命名（用户需求：代码引用）——模型/引用/引擎注册/锚定父/控件树 全链更新。
// 引擎 id 表：空串注册=移除该控件全部 id（CABI SetControlId 语义）→ 再注册新 id。
// 控件 id 行：提交待改名（切换选中/回车/空闲到期调用）；失败还原行显示
void CanvasPane::commitPendingIdRename() {
    if (!m_idRenamePending) return;
    m_idRenamePending = false;
    const std::string target = m_idRenameTarget;
    const std::string text = m_idRenameText;
    m_idRenameTarget.clear();
    m_idRenameText.clear();
    ControlModel* m = findModel(target);
    if (!m || text == m->props.id) return;
    if (!renameControlId(target, text)) {
        setStatusHint(u8"id 未修改（空/重复/含空白/保留键）：" + text);
        rebuildDynamicRows();   // 行刷新还原为当前 id
    } else {
        commitUndo();   // F2.5：重命名为独立撤销步
    }
}

bool CanvasPane::renameControlId(const std::string& oldId, std::string newId) {
    if (newId == oldId) return true;
    const auto b = newId.find_first_not_of(" \t");
    const auto e2 = newId.find_last_not_of(" \t");
    newId = (b == std::string::npos) ? std::string() : newId.substr(b, e2 - b + 1);
    if (newId.empty()) return false;
    if (newId == kBenchTreeNode) return false;   // 树根节点内部键保留
    for (char c : newId)
        if (std::isspace((unsigned char)c)) return false;   // 含内部空白不允许（代码引用友好）
    ControlModel* m = findModel(oldId);
    PlacedControl* pc = placedById(oldId);
    if (!m || !pc) return false;
    for (auto& it : m_items)
        if (it.model && it.model->props.id == newId) return false;   // 重复
    // 锚定/底拉伸父容器：child-id 若指向旧 id → 换新（需在改 id 前判断）
    Control anchorPar = anchorParentOf(*pc);
    const bool anchorWas = anchorPar.IsValid() &&
        anchorPar.GetString(PropertyNames::kChildTargetId) == oldId;
    // 1) 模型 id + 引用（子 parentId / tabs page-id）
    m->props.id = newId;
    for (auto& it : m_items) {
        if (it.parentId == oldId) it.parentId = newId;
        auto& ex = it.model->props.extended;
        if (ex.contains("tabs") && ex["tabs"].is_array())
            for (auto& t : ex["tabs"])
                if (t.is_object() && t.value("page-id", "") == oldId) t["page-id"] = newId;
    }
    if (m_placeTargetId == oldId) m_placeTargetId = newId;
    if (m_selectedId == oldId) m_selectedId = newId;
    for (auto& s : m_selIds) if (s == oldId) s = newId;
    // 2) 引擎 id 注册（清旧 + 注册新；子视口实例表——与放置注册同一通道）
    if (m_viewport && pc->ctl.IsValid()) {
        m_viewport->SetControlId(pc->ctl, "");
        m_viewport->SetControlId(pc->ctl, newId);
    }
    // 3) 锚定父 child-id 同步
    if (anchorWas) anchorPar.SetString(PropertyNames::kChildTargetId, newId);
    // 4) 控件树：节点 id 即控件 id——移除根节点（连带子树）→ 按原顺序重建
    if (m_canvasTree.IsValid() && m_ui) {
        const std::string rootParent = pc->parentId;
        std::vector<const PlacedControl*> sub;
        sub.push_back(pc);
        std::function<void(const std::string&)> collect = [&](const std::string& pid) {
            for (auto& it : m_items)
                if (it.parentId == pid) {
                    sub.push_back(&it);
                    collect(it.model->props.id);
                }
        };
        collect(newId);
        m_treeSync = true;
        m_ui->TreeViewRemoveNode(m_canvasTree, oldId);
        for (const PlacedControl* sp : sub) {
            const std::string cap = sp->model->props.extended.value("caption", std::string());
            const std::string par = sp == pc ? rootParent : sp->parentId;
            m_ui->TreeViewAddNode(m_canvasTree, par.empty() ? kBenchTreeNode : par,
                                  sp->model->props.id, cap + " " + sp->model->props.id);
        }
        m_treeSync = false;
        syncTreeSelection();
    }
    // 5) 序号防撞：新 id 形如 "xxx_N" 时抬高生成序号（避免后续放置再撞）
    {
        const auto us = newId.find_last_of('_');
        if (us != std::string::npos && us + 1 < newId.size()) {
            try {
                const int n = std::stoi(newId.substr(us + 1));
                if (n > m_seq) m_seq = n;
            } catch (...) {}
        }
    }
    setStatusHint(u8"控件 id 已改为：" + newId);
    return true;
}

// ─────────────────────────── 事件面板（F4） ───────────────────────────
bool CanvasPane::isValidIdentifier(const std::string& name) const {
    if (name.empty()) return false;
    if (!(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

bool CanvasPane::isHandlerNameUnique(const std::string& name) const {
    for (const auto& it : m_items) {
        const auto& ev = it.model->props.extended.value("events", nlohmann::json::object());
        if (!ev.is_object()) continue;
        for (auto e = ev.begin(); e != ev.end(); ++e)
            if (e.value().is_string() && e.value().get<std::string>() == name) return false;
    }
    return true;
}

std::string CanvasPane::suggestHandlerName(const std::string& ctrlId,
                                           const std::string& jsonEvent) const {
    std::string base = "on";   // on + Capitalize(id) + "_" + 事件后缀（onClick → Click）
    bool up = true;
    for (char c : ctrlId) {
        if (!std::isalnum((unsigned char)c)) { up = true; continue; }
        base += up ? (char)std::toupper((unsigned char)c) : c;
        up = false;
    }
    base += "_";
    base += (jsonEvent.size() > 2) ? jsonEvent.substr(2) : jsonEvent;
    if (isHandlerNameUnique(base)) return base;
    for (int n = 2;; ++n) {
        const std::string cand = base + std::to_string(n);
        if (isHandlerNameUnique(cand)) return cand;
    }
}

bool CanvasPane::addEventBinding(const std::string& ctrlId, const std::string& jsonEvent,
                                 const std::string& handler) {
    const EventSpec* spec = findEventSpecByJson(jsonEvent);
    if (!spec) return false;
    if (!isValidIdentifier(handler)) return false;
    if (!isHandlerNameUnique(handler)) return false;
    PlacedControl* pc = placedById(ctrlId);
    if (!pc) return false;
    const std::string typeSet = std::string(",") + spec->types + ",";
    if (typeSet.find("," + pc->model->props.type + ",") == std::string::npos)
        return false;   // 事件不适用于该控件类型
    pc->model->props.extended["events"][jsonEvent] = handler;
    // 引擎实演：桥接回调（设计期打点；导出后由宿主 RegisterAction 接管）
    if (pc->ctl.IsValid()) {
        const EventSpec* sp = spec;
        const std::string cid = ctrlId;
        pc->ctl.SetCallback(sp->bind, [cid, handler](const Event&) {
            std::printf("[Events] %s: %s\n", cid.c_str(), handler.c_str());
        });
    }
    commitUndo();   // F2.5：事件绑定为独立撤销步
    return true;
}

bool CanvasPane::removeEventBinding(const std::string& ctrlId, const std::string& jsonEvent) {
    PlacedControl* pc = placedById(ctrlId);
    if (!pc) return false;
    auto& ev = pc->model->props.extended["events"];
    if (!ev.is_object() || !ev.contains(jsonEvent)) return false;
    ev.erase(jsonEvent);
    if (ev.empty()) pc->model->props.extended.erase("events");
    if (pc->ctl.IsValid()) {
        if (const EventSpec* sp = findEventSpecByJson(jsonEvent))
            pc->ctl.SetCallback(sp->bind, nullptr);   // P0-86 落地：空回调=解绑（binding 直传 nullptr→引擎 erase）
    }
    commitUndo();   // F2.5：事件解绑为独立撤销步
    return true;
}

std::vector<CanvasPane::EventBinding> CanvasPane::collectEventBindings() const {
    std::vector<EventBinding> out;
    for (const auto& it : m_items) {
        const auto& ev = it.model->props.extended.value("events", nlohmann::json::object());
        if (!ev.is_object()) continue;
        for (auto e = ev.begin(); e != ev.end(); ++e)
            if (e.value().is_string())
                out.push_back({ it.model->props.id, e.key(), e.value().get<std::string>() });
    }
    return out;
}

// 事件面板接线：存句柄 + 隐藏样例行 + 绑定交互（下拉/建议/添加/实时校验）
void CanvasPane::SetEventPanel(Control typeCombo, Control handlerEdit, Control suggestBtn,
                               Control addBtn, Control errLabel, Control listHost) {
    if (!m_ui) return;
    m_evType = typeCombo;
    m_evHandler = handlerEdit;
    m_evSuggest = suggestBtn;
    m_evAdd = addBtn;
    m_evErr = errLabel;
    m_evHost = listHost;
    if (auto row = m_ui->FindControl("bd_row"); row.IsValid())
        row.SetBool(PropertyNames::kVisible, false);   // 样例行：仅作列宽参考
    // 事件类型下拉：选择即预填建议命名
    if (m_evType.IsValid())
        m_evType.SetCallback(PropertyNames::kEventSelectionChanged, [this](const Event&) {
            if (!m_evHandler.IsValid() || m_selectedId.empty()) return;
            const int idx = m_evType.GetInt(PropertyNames::kSelectedIndex);
            if (idx < 0 || idx >= (int)m_evTypeItems.size()) return;
            if (m_evHandler.GetString(PropertyNames::kTextContent).empty())
                m_evHandler.SetString(PropertyNames::kTextContent,
                                      suggestHandlerName(m_selectedId, m_evTypeItems[(size_t)idx]));
        });
    // handler 实时校验
    if (m_evHandler.IsValid())
        m_evHandler.SetCallback(PropertyNames::kEventTextChanged, [this](const Event& e) {
            if (!m_evErr.IsValid()) return;
            const std::string v = sanitizePropValue("caption", e.GetTextChanged());
            if (v.empty()) { m_evErr.SetString(PropertyNames::kCaption, ""); return; }
            if (!isValidIdentifier(v))
                m_evErr.SetString(PropertyNames::kCaption, u8"handler 非 C 标识符（A-Za-z_ 开头）");
            else if (!isHandlerNameUnique(v))
                m_evErr.SetString(PropertyNames::kCaption, u8"handler 已存在（须唯一）");
            else
                m_evErr.SetString(PropertyNames::kCaption, "");
        });
    if (m_evSuggest.IsValid())
        m_evSuggest.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            if (m_selectedId.empty() || !m_evHandler.IsValid()) return;
            const int idx = m_evType.GetInt(PropertyNames::kSelectedIndex);
            const std::string json = (idx >= 0 && idx < (int)m_evTypeItems.size())
                                         ? m_evTypeItems[(size_t)idx] : std::string();
            if (json.empty()) return;
            m_evHandler.SetString(PropertyNames::kTextContent,
                                  suggestHandlerName(m_selectedId, json));
            if (m_evErr.IsValid()) m_evErr.SetString(PropertyNames::kCaption, "");
        });
    if (m_evAdd.IsValid())
        m_evAdd.SetCallback(PropertyNames::kEventClick, [this](const Event&) {
            if (m_selectedId.empty()) {
                setStatusHint(u8"请先选中一个控件再绑定事件");
                return;
            }
            const int idx = m_evType.GetInt(PropertyNames::kSelectedIndex);
            const std::string json = (idx >= 0 && idx < (int)m_evTypeItems.size())
                                         ? m_evTypeItems[(size_t)idx] : std::string();
            const std::string handler = m_evHandler.IsValid()
                ? sanitizePropValue("caption", m_evHandler.GetString(PropertyNames::kTextContent))
                : std::string();
            if (json.empty()) { setStatusHint(u8"无可用事件类型（该控件已全部绑定）"); return; }
            if (!isValidIdentifier(handler)) {
                if (m_evErr.IsValid()) m_evErr.SetString(PropertyNames::kCaption,
                                                         u8"handler 非 C 标识符（A-Za-z_ 开头）");
                setStatusHint(u8"事件绑定失败：handler 名不合法");
                return;
            }
            if (!isHandlerNameUnique(handler)) {
                if (m_evErr.IsValid()) m_evErr.SetString(PropertyNames::kCaption,
                                                         u8"handler 已存在（须唯一）");
                setStatusHint(u8"事件绑定失败：handler 重名");
                return;
            }
            if (addEventBinding(m_selectedId, json, handler)) {
                setStatusHint(u8"已绑定：" + m_selectedId + " " + json + " → " + handler);
                refreshEventPanel();
            }
        });
}

// 刷新事件面板：事件下拉（按类型过滤、扣除已绑定）+ 建议命名预填 + 已绑定动态行
void CanvasPane::refreshEventPanel() {
    if (!m_ui || !m_evType.IsValid()) return;
    for (auto& r : m_evRows)
        if (r.IsValid()) r.Destroy();
    m_evRows.clear();
    m_evTypeItems.clear();
    m_ui->ComboBoxClearItems(m_evType);
    const ControlModel* m = findModel(m_selectedId);
    const bool hasSel = (m != nullptr);
    m_evType.SetBool(PropertyNames::kEnabled, hasSel);
    if (m_evHandler.IsValid()) {
        m_evHandler.SetBool(PropertyNames::kEnabled, hasSel);
        if (!hasSel) m_evHandler.SetString(PropertyNames::kTextContent, "");
    }
    if (m_evSuggest.IsValid()) m_evSuggest.SetBool(PropertyNames::kEnabled, hasSel);
    if (m_evAdd.IsValid()) m_evAdd.SetBool(PropertyNames::kEnabled, hasSel);
    if (m_evErr.IsValid()) m_evErr.SetString(PropertyNames::kCaption, "");
    if (auto note = m_ui->FindControl("evtNote"); note.IsValid())
        note.SetBool(PropertyNames::kVisible, true);
    if (!hasSel) return;
    const std::string& tp2 = m->props.type;
    const auto& ev = m->props.extended.value("events", nlohmann::json::object());
    for (const auto& sp : kEventSpecs) {
        const std::string types = std::string(",") + sp.types + ",";
        if (types.find("," + tp2 + ",") == std::string::npos) continue;
        if (ev.is_object() && ev.contains(sp.json)) continue;   // 一事件一绑定
        m_ui->ComboBoxAddItem(m_evType, sp.json, sp.json);
        m_evTypeItems.push_back(sp.json);
    }
    if (!m_evTypeItems.empty()) {
        m_evType.SetInt(PropertyNames::kSelectedIndex, 0);
        if (m_evHandler.IsValid() && m_evHandler.GetString(PropertyNames::kTextContent).empty())
            m_evHandler.SetString(PropertyNames::kTextContent,
                                  suggestHandlerName(m->props.id, m_evTypeItems[0]));
    }
    // 已绑定列表（动态行：控件 id / 事件 / handler / 删除）
    if (ev.is_object() && !ev.empty()) {
        if (auto note = m_ui->FindControl("evtNote"); note.IsValid())
            note.SetBool(PropertyNames::kVisible, false);   // 有绑定则不显示空态注释
        for (auto e = ev.begin(); e != ev.end(); ++e) {
            if (!e.value().is_string()) continue;
            const std::string json = e.key();
            const std::string handler = e.value().get<std::string>();
            Control row = m_ui->CreatePanel(0, 0, 464, 32);
            if (!row.IsValid()) continue;
            row.SetEnum(PropertyNames::kLayout, "h-flow");
            row.SetBool(PropertyNames::kBorderVisible, false);
            row.SetBool(PropertyNames::kTransparent, true);
            auto mk = [&](const std::string& caption, float w) {
                Control lb = m_ui->CreateLabel(caption, 16.0f, 0, 0, w, 32);
                if (lb.IsValid()) {
                    lb.SetColor(PropertyNames::kText, UIColor{0xE0, 0xE0, 0xE0, 255});
                    row.AddChild(lb);
                }
            };
            mk(m->props.id, 60.0f);
            mk(json, 90.0f);
            Control hb = m_ui->CreateLabel(handler, 16.0f, 0, 0, 200, 32);
            if (hb.IsValid()) {
                hb.SetColor(PropertyNames::kText, UIColor{0x8F, 0xC8, 0xFF, 255});
                hb.SetFloat(PropertyNames::kChildFlowWeight, 1.0f);
                row.AddChild(hb);
            }
            const std::string cid = m->props.id;
            Control del = m_ui->CreateButton(u8"删除", 0, 0, 56, 32);
            if (del.IsValid()) {
                del.SetCallback(PropertyNames::kEventClick, [this, cid, json](const Event&) {
                    if (removeEventBinding(cid, json)) {
                        setStatusHint(u8"已解除绑定：" + cid + " " + json);
                        refreshEventPanel();
                    }
                });
                row.AddChild(del);
            }
            m_evHost.AddChild(row);
            m_evRows.push_back(row);
        }
    }
}

void CanvasPane::syncTreeSelection() {
    if (!m_canvasTree.IsValid() || !m_ui) return;
    m_treeSync = true;
    if (m_selectedId.empty()) m_ui->TreeViewClearSelection(m_canvasTree);
    else {
        // 展开祖先链：父节点收起时选中节点不可见（用户实测"点两次才在控件树中选中"）
        for (PlacedControl* p = placedById(m_selectedId); p && !p->parentId.empty();) {
            m_ui->TreeViewExpandNode(m_canvasTree, p->parentId);
            p = placedById(p->parentId);
        }
        m_ui->TreeViewSelectNode(m_canvasTree, m_selectedId);
    }
    m_treeSync = false;
}

// 删除选中控件：销毁视口控件 + 移除模型 + 取消选中（清空属性面板/隐藏选中框）。
void CanvasPane::deleteSelected() {
    if (m_selIds.empty()) return;
    // 收集后代 id（含自身）——容器删除连带子树（P1-a 层级）；P2 多选：全部选中项
    std::vector<std::string> toRemove;
    std::function<void(const std::string&)> collect = [&](const std::string& pid) {
        if (std::find(toRemove.begin(), toRemove.end(), pid) != toRemove.end()) return;
        toRemove.push_back(pid);
        for (auto& it : m_items)
            if (it.parentId == pid) collect(it.model->props.id);
    };
    for (const auto& id : m_selIds) collect(id);
    // 反向（后代先删）：控件销毁（视口内）+ 模型条目移除 + 树节点移除
    for (auto rit = toRemove.rbegin(); rit != toRemove.rend(); ++rit) {
        if (PlacedControl* pc = placedById(*rit)) {
            if (pc->ctl.IsValid()) pc->ctl.Destroy();
        }
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it->model->props.id == *rit) { m_items.erase(it); break; }
        }
        syncTreeRemove(*rit);
    }
    if (m_dragId == m_selectedId || !m_dragParentId.empty()) {   // 拖动状态清理
        m_dragId.clear();
        m_dragParentId.clear();
        m_dragCtl = Control();
    }
    selectItem("");   // 取消选中 → 属性面板清零 + 选中框隐藏
    refreshPlaceTargetHint();   // P1-b：目标容器被删 → 提示回画布根
    refreshStatusRight();       // 状态栏控件数同步
    commitUndo();               // F2.5：删除为独立撤销步
}

void CanvasPane::SetToolboxTree(Control tree) {
    m_toolboxTree = tree;
    if (!m_toolboxTree.IsValid()) return;
    m_toolboxTree.SetCallback(PropertyNames::kEventSelect, [this](const Event& e) {
        const std::string id = e.GetNodeId();
        std::printf("[Toolbox] select node=%s\n", id.c_str());
        if (id.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (id == m_lastToolId) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastToolTime).count();
            m_lastToolId.clear();
            if (ms < 500) {
                // 双击：画布中央放置（逻辑坐标）；清放置待定（双击流程第一击已设 pending）
                std::printf("[Place] dblclick tool=%s center=(%.0f,%.0f)\n",
                            id.c_str(), m_vpW / (2 * m_zoom), m_vpH / (2 * m_zoom));
                placeControl(id, m_vpW / (2 * m_zoom), m_vpH / (2 * m_zoom));
                m_pendingPlaceTool.clear();
                return;
            }
        }
        // 拖放候选：按下工具项即候选（松开在画布→放置；在工具箱内松开→单击不动作）；
        // 双击：画布中央放置（既有习惯，清候选防松开误放置）
        m_pendingPlaceTool = id;
        m_lastToolId = id;
        m_lastToolTime = now;
    });
}

void CanvasPane::TickInput() {
    tickInput();
}

}  // namespace Designer