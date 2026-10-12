// ============================================================
//  Display —— 显存和画精灵
//  见 display.hpp 的说明。
// ============================================================

#include "display.hpp"

void Display::clear() {
    pixels_.fill(0);
    dirty_ = true;
}

bool Display::drawSprite(int x, int y, const std::uint8_t* sprite, int rows,
                         bool verticalWrap) {
    // rows 是 0 到 15 的半字节。0 表示"什么都不画"。
    //
    // 【注意】n = 0 时这里直接返回 false，而**不是**画 16 行。
    // 有些模拟器把 n = 0 当成 16 来处理，那是个常见的误读：
    // Dxyn 的 n 就是行数，0 就是 0 行。
    if (rows <= 0) {
        return false;
    }

    // ★ 起点坐标在这里回绕，**不推给调用方**。
    //
    //   CHIP-8 里坐标来自 8 位寄存器，最大 255，而屏幕只有 64 宽。
    //   真实硬件是直接丢掉高位——"x = 200" 等于 "x = 8"。
    //
    //   这条规则属于**屏幕的坐标系**，所以由屏幕自己执行：
    //   调用方不需要知道"坐标要先 & 63"这回事，也就不会漏做。
    //   放在调用方的话，"起点回绕"和"横向裁剪"这两半规则
    //   会分别住在两个文件里，谁读其中一个都看不全。
    //
    //   【负数也要处理】C++ 的 % 跟着被除数的符号走，
    //   负 x 会算出负余数，所以要再加一轮再取模。
    //   现在的调用方只可能传 0..255，但一个"只对合法输入正确"
    //   的函数是个迟早会炸的陷阱，多两行换掉它很划算。
    const int startX = ((x % WIDTH) + WIDTH) % WIDTH;
    const int startY = ((y % HEIGHT) + HEIGHT) % HEIGHT;

    bool collision = false;

    for (int row = 0; row < rows; ++row) {
        int py = startY + row;

        if (py >= HEIGHT) {
            if (!verticalWrap) {
                // 超出底部，这行丢掉。
                // 用 continue 而不是 break：verticalWrap 为 true 时
                // 后面的行还要绕回去画，所以这里不能跳出循环。
                continue;
            }
            py %= HEIGHT;  // 绕回顶部
        }

        const std::uint8_t bits = sprite[row];
        if (bits == 0) {
            continue;  // 整行都是空的，省掉 8 次内层循环
        }

        for (int col = 0; col < 8; ++col) {
            // 最高位是最左边的像素。
            // 0x80 >> col 依次是 10000000、01000000、00100000 ...
            if ((bits & (0x80 >> col)) == 0) {
                continue;
            }

            const int px = startX + col;

            // 横向**永远裁剪**，不绕。起点已经回绕到 0..63 了，
            // 但 startX + col 最大能到 70。
            if (px >= WIDTH) {
                continue;
            }

            const std::size_t index =
                static_cast<std::size_t>(py) * WIDTH + px;

            if (pixels_[index] != 0) {
                // 原本亮着 -> 熄灭，并且记下碰撞。
                // ★ 碰撞的定义是"至少有一个点从亮变灭"，不是"有重叠"。
                //   画在已经亮着的点上有重叠但不算碰撞——它确实变了。
                pixels_[index] = 0;
                collision = true;
            } else {
                pixels_[index] = 1;
            }
        }
    }

    // 只要有像素被翻转就标脏。哪怕最终画面和原来一样
    // （比如画两次同样的精灵会抵消），中间过程也是变化，
    // 标脏最多多贴一次图，不标脏可能会漏掉一次重绘。
    dirty_ = true;

    return collision;
}

std::string Display::toAscii() const {
    std::string out;
    out.reserve(static_cast<std::size_t>(HEIGHT) * (WIDTH + 1));

    for (int y = 0; y < HEIGHT; ++y) {
        if (y > 0) {
            out += '\n';
        }
        for (int x = 0; x < WIDTH; ++x) {
            out += pixel(x, y) ? '#' : '.';
        }
    }

    return out;
}
