// ============================================================
//  Display —— 那块 64x32 的黑白屏幕
//
//  CHIP-8 的屏幕小得可怜：64 列 32 行，每个点只有亮/灭两种状态。
//  整个显存 2048 个点。这就是全部。
//
//  【设计取舍】每个点存 1 个字节，而不是 1 个 bit。
//  按理说 2048 个点用位压缩只要 256 字节，省 8 倍内存。
//  但这点内存在 2026 年根本不值一提，而位运算会带来两类 bug：
//  移位方向搞反（左边第一个像素是最高位还是最低位？），
//  以及"读-改-写"时把相邻的点也改了。
//  存成一个字节一个点，Dxyn 的逻辑就是"读这个点、翻转它"，
//  一眼能看懂，也能直接在调试器里看显存。
//  等真需要省内存的时候再说——而模拟 1977 年的游戏机，永远不需要。
//
//  ★ dirty_ 的所有权：**只有 Display 自己会置位，只有平台层会清除。**
//    内核负责"我画了东西"，平台层负责"我贴到屏幕上了"。
//    如果内核自己清（比如在 tickTimers 里顺手清了），
//    屏幕会在某些时序下静默停止更新——这种 bug 极难查。
//
//  这个文件**不碰任何 Win32**，也不产生 ANSI 转义码。
//  ASCII 转储（给测试比对用）在这里；彩色终端渲染在 render_ansi.cpp。
//  分开的理由：测试要的是字节精确、和终端无关的输出，
//  掺进 ANSI 转义码会让黄金文件比的是"终端色彩"而不是"画面对不对"。
// ============================================================

#pragma once

#include <array>
#include <cstdint>
#include <string>

class Display {
public:
    static constexpr int WIDTH = 64;
    static constexpr int HEIGHT = 32;
    static constexpr int PIXEL_COUNT = WIDTH * HEIGHT;

    Display() { clear(); }

    // 全部熄灭，并标脏（清屏是一次画面变化）。
    void clear();

    bool pixel(int x, int y) const {
        return pixels_[static_cast<std::size_t>(y) * WIDTH + x] != 0;
    }

    // 画一个精灵。这是 Dxyn 的实现，也是整个模拟器最核心的一段代码。
    //
    //   x, y          起点。**随便传什么值都行**，这里会先回绕到屏幕内。
    //   sprite        精灵数据，每字节一行，最高位是最左边的像素。
    //   rows          行数。Dxyn 里是 n，取 0 时**什么都不画**。
    //   verticalWrap  true = 越过底部绕回顶部（见 Quirks::spriteVerticalWrap）
    //
    //   返回 true 表示发生了碰撞：至少有一个原本亮着的点被熄灭了。
    //   Dxyn 要把这个值写进 VF。
    //
    // 【三条必须记住的规则】
    //   ① 起点坐标**在这里回绕**（x 取模 64，y 取模 32）。
    //      这是屏幕自己的坐标系规则，不推给调用方——
    //      否则"起点回绕"和"横向裁剪"这两半会分别住在两个文件里。
    //   ② 横向**永远裁剪**，不绕。任何版本的 CHIP-8 都是这样。
    //   ③ 纵向绕不绕由参数决定。注意"绕"和"裁"都在**行**这一级，
    //      不是像素级——一行的 8 个像素要么整行画，要么整行丢。
    //
    //  ①和②合起来的完整语义是："从哪儿开始画"会绕回屏幕内，
    //  "画出去的部分"直接丢掉。这两件事经常被混成一件。
    bool drawSprite(int x, int y, const std::uint8_t* sprite, int rows,
                    bool verticalWrap);

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // 把显存转成 32 行 x 64 列的 ASCII 文本。
    // 亮 = '#'，灭 = '.'，行之间用 \n，**末尾没有换行**。
    //
    // 用 ASCII 而不是 '█' 是因为这个输出要给测试当黄金文件比对：
    // 纯 ASCII 不存在编码问题，diff 出来也整齐。
    // 想在终端里看漂亮的画面请用 render_ansi。
    std::string toAscii() const;

private:
    // 一个点一个字节，非 0 即亮。
    std::array<std::uint8_t, PIXEL_COUNT> pixels_{};
    bool dirty_ = true;
};
