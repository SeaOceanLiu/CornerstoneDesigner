# RGBA8888 Pixel Format (CRITICAL)

`Surface::create(w, h)` creates an RGBA8888 surface. On **little-endian x86** (the only relevant target), RGBA8888 memory layout is:

| Byte offset | 0 (LSB) | 1 | 2 | 3 (MSB) |
| ----------- | ------- | - | - | ------- |
| Channel     | A       | B | G | R       |

When writing pixels as `uint32_t`, the value MUST be formatted as:

```cpp
uint32_t pixel = (R<<24) | (G<<16) | (B<<8) | A;
// Example: opaque black = 0x000000FF, opaque red = 0xFF0000FF
```

Common mistakes:

- `0xFF000000` = R=255, G=0, B=0, A=0 = **transparent red** (NOT opaque black!)
- `0x000000FF` = R=0, G=0, B=0, A=255 = **opaque black** (correct for black)
- `0xFFFFFFFF` = R=255, G=255, B=255, A=255 = opaque white (correct)
- `0x00000000` = R=0, G=0, B=0, A=0 = transparent (correct)

TL;DR: **Alpha goes in the lowest byte (bits 0-7), Red in the highest byte (bits 24-31).**

## JSON 颜色格式

UICornerstone 布局 JSON 中的颜色为 **8 位十六进制 `#RRGGBBAA`**（如 `"#282828FF"`，见 `test_layout.json`）。设计器序列化时：

- 输出：一律 `#RRGGBBAA`（RGBA8888 与上表字节序对应）
- 输入兼容：接受 6 位 `#RRGGBB`（导出时补 `FF`）与对象格式 `{ "r":…, "g":…, "b":…, "a":… }`
