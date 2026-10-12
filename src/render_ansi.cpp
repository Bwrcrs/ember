// ============================================================
//  RenderAnsi —— 见 render_ansi.hpp
//
//  提醒：这个文件里**没有**任何输出。它只造字符串。
//  没有 printf、没有 fputs、不碰任何操作系统头文件。
//  这是它能被测试的原因（见 render_ansi.hpp 开头那段）。
//
//  （别在这个文件里写出那个"Windows 平台头文件名"——
//    build.bat 的 lint 是字面搜索，连注释里出现都会构建失败。）
// ============================================================

#include "render_ansi.hpp"

#include "display.hpp"

namespace ansi {

const char* ENTER_ALT_SCREEN = "\x1b[?1049h";
const char* LEAVE_ALT_SCREEN = "\x1b[?1049l";
const char* HIDE_CURSOR      = "\x1b[?25l";
const char* SHOW_CURSOR      = "\x1b[?25h";
const char* HOME             = "\x1b[H";
const char* CLEAR            = "\x1b[2J\x1b[H";
const char* RESET            = "\x1b[0m";

namespace {

// ---- 颜色 ----
//
// CHIP-8 是**单色**的：一个像素要么亮要么灭，没有别的颜色。
// 所以这里只需要两个颜色：亮白和黑。
//
// 用亮白（97 / 107）而不是普通白（37 / 47）：
// 这台机器的控制台默认背景是黑的，普通白看起来发灰，
// 亮白才像"点亮的像素"。

const char* FG_ON  = "\x1b[97m";   // 前景 = 亮白
const char* FG_OFF = "\x1b[30m";   // 前景 = 黑
const char* BG_ON  = "\x1b[107m";  // 背景 = 亮白
const char* BG_OFF = "\x1b[40m";   // 背景 = 黑

// "上半块" U+2580，UTF-8 是三个字节。
// 它的上半格涂前景色、下半格涂背景色——这就是一个格子装两个像素的原理。
const char* UPPER_HALF_BLOCK = "\xE2\x96\x80";

}  // namespace

std::string renderHalfBlock(const Display& display, bool color) {
    // 半块渲染的前提是像素行数是**偶数**——每个文字行配一对。
    // 奇数的话最后一行没有配对的另一半，会被静默丢掉。
    // 这不会自己变，但写在这里下次改显存尺寸时会直接编译不过，
    // 而不是等到画面上少一行才发现。
    static_assert(Display::HEIGHT % 2 == 0,
                  "半块渲染要求像素行数是偶数（一行文字配两行像素）");

    std::string out;

    // 预留一点空间。一帧是 64x16 个格子，
    // 每个格子最少 1 字节（空格）、最多 3 字节（▀）加转义序列，
    // 估个 8KB 够用，避免边拼边长反复扩容。
    out.reserve(8192);

    // 一行文字 = 两行像素：topRow 是上半格，bottomRow 是下半格。
    for (int cellRow = 0; cellRow < Display::HEIGHT / 2; ++cellRow) {
        const int topRow = cellRow * 2;
        const int bottomRow = topRow + 1;

        // ---- 当前生效的颜色 ----
        // -1 表示"还不知道"，保证这一行第一个格子一定会发一次颜色。
        // 每行开头重置这两个值，而不是跨行保留：
        // 因为行尾发了 RESET，跨行记着就会漏发。
        int curFg = -1;
        int curBg = -1;

        for (int x = 0; x < Display::WIDTH; ++x) {
            const bool top = display.pixel(x, topRow);
            const bool bottom = display.pixel(x, bottomRow);

            // ---- 不支持颜色时的退路 ----
            // 上下两个像素只要有一个亮就算亮。图案会粗一档
            // （竖直方向的分辨率减半），但形状还认得出来。
            if (!color) {
                out += (top || bottom) ? '#' : ' ';
                continue;
            }

            // ---- 两个都灭 ----
            // 空格子。空格字符不画前景，整格都是背景色，
            // 所以只要背景是"灭色"就够了，前景是什么无所谓。
            // 省下 ▀ 的三个字节——屏幕大部分是黑的，这里省得最多。
            if (!top && !bottom) {
                if (curBg != 0) {
                    out += BG_OFF;
                    curBg = 0;
                }
                out += ' ';
                continue;
            }

            // ---- 至少一半亮 ----
            // 前景装上半格，背景装下半格。哪个亮给哪个上亮色。
            const int fg = top ? 1 : 0;
            const int bg = bottom ? 1 : 0;

            if (fg != curFg) {
                out += fg ? FG_ON : FG_OFF;
                curFg = fg;
            }
            if (bg != curBg) {
                out += bg ? BG_ON : BG_OFF;
                curBg = bg;
            }
            out += UPPER_HALF_BLOCK;
        }

        // 行尾复位。不复位的话背景色会跟着换行符一起
        // 把下一行的行首涂上颜色，看起来像画面左边多了一列。
        out += RESET;

        // 最后一行不发换行符。
        // 发了的话光标会跑到第 17 行去，而窗口只有 30 行——
        // 现在没事，但哪天窗口小了就会滚屏。
        // 不依赖"窗口够大"这种假设，能省则省。
        //
        // ★ 换行只写 '\n'，不写 "\r\n"。理由见 render_ansi.hpp：
        //   标准输出是文本模式，CRT 自己会把 '\n' 补成 "\r\n"，
        //   再手写一个 '\r' 会变成 "\r\r\n"，
        //   经过测试脚本的 Normalize 之后就是**两个**换行。
        if (cellRow + 1 < Display::HEIGHT / 2) {
            out += '\n';
        }
    }

    return out;
}

std::string renderAscii(const Display& display) {
    std::string out;
    out.reserve(static_cast<std::size_t>(Display::WIDTH) * Display::HEIGHT +
                Display::HEIGHT * 2);

    for (int y = 0; y < Display::HEIGHT; ++y) {
        for (int x = 0; x < Display::WIDTH; ++x) {
            out += display.pixel(x, y) ? '#' : ' ';
        }
        if (y + 1 < Display::HEIGHT) {
            out += '\n';
        }
    }

    return out;
}

}  // namespace ansi
