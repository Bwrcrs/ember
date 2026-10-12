// ============================================================
//  main.cpp —— 子命令分发 + 参数解析
//
//  ★ 这个文件里**没有一行 Win32**，一次都没包含过 windows.h。
//    需要平台能力的两件事（控制台切 UTF-8、取宽字符命令行）
//    都在 platform_win32.cpp 里，这里只调函数。
//
//    这不是洁癖，是 build.bat 里那条 lint 强制的：
//    一旦 main.cpp 开始包含 windows.h，
//    "内核不碰平台"这条线就从入口处断了，往后每个文件都
//    可以找理由碰一下，最后测试没法在无窗口环境下跑。
//    （连注释里都得写成不带尖括号的 windows.h——lint 是
//      字面量匹配，故意不做"这行是不是注释"的判断。）
//
//  ★ 为什么只有一个 exe？因为 build.bat 把 src\*.cpp 全链一起。
//    所以多工具靠**子命令**区分，不是多个可执行文件：
//
//        ember run / screen / trace / dis / asm / selftest
//
//  ★ 输出流的分工（这个很重要，黄金文件靠它）：
//    标准输出 = **只有数据**（屏幕转储、追踪行、反汇编行）
//    标准错误 = 人看的旁白（跑了多少条、PC 停在哪、警告）
//    这样 tests\run_tests.ps1 直接拿 stdout 就能比对，
//    不会被"跑了 400 条指令"这种话污染。
//
//  退出码：0 正常 / 1 用户输入有错 / 2 用法或环境有问题
// ============================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "disassembler.hpp"
#include "display.hpp"
#include "error.hpp"
#include "font.hpp"
#include "hexfmt.hpp"
#include "machine.hpp"
#include "opcodes.hpp"
#include "platform_win32.hpp"
#include "quirks.hpp"
#include "render_ansi.hpp"
#include "rom.hpp"
#include "trace.hpp"

namespace {

// ============================================================
//  一、参数
// ============================================================

// 固定的默认种子。见 machine.hpp 里那段说明：
// 随机数必须可复现，否则 Cxnn 的黄金文件永远对不上。
constexpr std::uint32_t DEFAULT_SEED = 0x00C0FFEEu;

// --poke 的一次写入。
struct Poke {
    std::uint16_t address;
    std::uint8_t value;
};

// 整个命令行解析出来的东西。
//
// 【为什么所有字段都给默认值】因为不同的子命令用不同的字段，
// 给默认值之后，"用户没写这个开关"和"这个开关对这条子命令不适用"
// 就是同一件事，不用再区分。判断"用户写没写"靠 -1 这种哨兵值
// （cycles / steps），或者靠 0 表示"自动"（scale / bytes）。
struct Options {
    std::string command;
    std::string input;    // ROM 或汇编源码的路径
    std::string output;   // -o

    long long cycles = -1;   // screen：跑多少条指令。-1 = 用户没写
    long long steps = -1;    // trace：印多少行。-1 = 用户没写
    long long offset = 0;    // dis：从 ROM 的第几个字节开始
    long long bytes = 0;     // dis：看多少字节。0 = 到末尾

    int ips = Clock::DEFAULT_IPS;
    int hz = Clock::DEFAULT_TIMER_HZ;
    int scale = 0;

    std::uint32_t seed = DEFAULT_SEED;
    Quirks quirks = Quirks::modern();
    std::vector<Poke> pokes;

    bool deltas = true;         // trace：印不印寄存器变化
    bool fontOnly = false;      // selftest --font
    bool quirksTable = false;   // selftest --quirks-table
    bool ansi = false;          // screen --ansi：用半块 + 颜色渲染
    bool help = false;
};

void printUsage() {
    std::fputs(
        "Ember —— 从零手写的 CHIP-8 模拟器\n"
        "\n"
        "用法:\n"
        "  ember play   <rom.ch8>                 在终端里玩（键盘：1234/QWER/ASDF/ZXCV，Esc 退出）\n"
        "  ember run    <rom.ch8>                 开窗口玩（阶段 7，还没做）\n"
        "  ember screen <rom.ch8> [--cycles N]    不开窗口，把显存 dump 成字符画\n"
        "  ember trace  <rom.ch8> [--steps N]     逐条打印指令和寄存器变化\n"
        "  ember dis    <rom.ch8> [--offset N] [--bytes N]    反汇编\n"
        "  ember asm    <prog.asm> [-o prog.ch8]  汇编器（阶段 5）\n"
        "  ember selftest [--font|--quirks-table]   自检 / 打印字模 / 打印开关表\n"
        "\n"
        "通用参数:\n"
        "  --ips N        每秒指令数（默认 700）\n"
        "  --hz N         定时器频率（默认 60）\n"
        "  --seed N       随机数种子，十进制或 0x 开头十六进制（默认 0xC0FFEE）\n"
        "  --poke A=V     往地址 A 塞一个字节，可重复，两个都是十六进制\n"
        "  --quirks NAME  行为开关预设，默认 modern\n"
        "  --no-deltas    trace 时只印控制流，不印寄存器变化\n"
        "  --ansi         screen 时用半块 + 颜色渲染（64x16，和 play 看到的一样）\n"
        "\n"
        "退出码: 0 正常 / 1 用户输入有错 / 2 用法或环境有问题\n",
        stdout);
}

// ---- 数字解析 ----
// 都带上"是谁要的数"，这样报错能指出是哪个开关写错了，
// 而不是干巴巴一句"不是合法的数字"。

bool isHexLiteral(const std::string& text) {
    return text.size() > 2 && text[0] == '0' &&
           (text[1] == 'x' || text[1] == 'X');
}

long long parseDecimal(const std::string& text, const std::string& what) {
    if (text.empty()) {
        throw EmberError(what + " 需要一个十进制整数，但值是空的");
    }
    try {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used, 10);
        if (used != text.size()) {
            throw EmberError(what + " 需要十进制整数，\"" + text +
                             "\" 后面有多余的字符");
        }
        return value;
    } catch (const EmberError&) {
        throw;
    } catch (const std::exception&) {
        // std::stoll 抛的是 invalid_argument / out_of_range，
        // 都不是 EmberError，得翻译一下，否则退出码会变成 2（环境错），
        // 而用户只是把开关写错了，应该是 1。
        throw EmberError(what + " 不是合法的十进制整数: \"" + text + "\"");
    }
}

// 十六进制，允许写 0x 前缀也允许裸写（0x1FF 和 1FF 都行）。
// 十六进制在日常里写惯了不写前缀，所以这里两种都收。
std::uint32_t parseHex(const std::string& text, const std::string& what) {
    std::string digits = isHexLiteral(text) ? text.substr(2) : text;
    if (digits.empty()) {
        throw EmberError(what + " 需要一个十六进制数，但值是空的");
    }
    try {
        std::size_t used = 0;
        const unsigned long value = std::stoul(digits, &used, 16);
        if (used != digits.size()) {
            throw EmberError(what + " 需要十六进制数，\"" + text +
                             "\" 里有不是 0-9A-F 的字符");
        }
        return static_cast<std::uint32_t>(value);
    } catch (const EmberError&) {
        throw;
    } catch (const std::exception&) {
        throw EmberError(what + " 不是合法的十六进制数: \"" + text + "\"");
    }
}

// --seed 收十进制和 0x 十六进制两种。其他数字开关只收十进制，
// 因为 --cycles 400 该是 400 而不是 0x400，那是两个差很远的数。
std::uint32_t parseSeed(const std::string& text) {
    if (isHexLiteral(text)) {
        return parseHex(text, "--seed");
    }
    const long long value = parseDecimal(text, "--seed");
    if (value < 0 || value > 0xFFFFFFFFLL) {
        throw EmberError("--seed 要在 0 到 4294967295 之间，给的是 " +
                         std::to_string(value));
    }
    return static_cast<std::uint32_t>(value);
}

Poke parsePoke(const std::string& text) {
    const std::size_t eq = text.find('=');
    if (eq == std::string::npos) {
        throw EmberError("--poke 要写成 地址=值，比如 --poke 1FF=01，给的是 \"" +
                         text + "\"");
    }

    const std::uint32_t address = parseHex(text.substr(0, eq), "--poke 的地址");
    const std::uint32_t value = parseHex(text.substr(eq + 1), "--poke 的值");

    // 报错要回显**用户打的字**，不是解析出来的数。
    //
    // ★ 这里原来是 hex3(address & 0xFFFF)，而 hex3 自己又会 & 0x0FFF，
    //   于是 `--poke 2000=01` 报的是"给的是 0x000"——
    //   用户输入的那个数被两层掩码吃干净了。
    //   报错消息里最有用的就是引起报错的那个值，丢了它
    //   等于逼用户自己回头数自己打了什么。
    //   （同一类毛病之前出在 UNKNOWN 指令印 0xFFF 上，见 disassembler.cpp。）
    if (address > 0x0FFF) {
        throw EmberError("--poke 的地址要在 0x000 到 0xFFF 之间，给的是 \"" +
                         text.substr(0, eq) + "\"");
    }
    if (value > 0xFF) {
        throw EmberError("--poke 的值要在一个字节以内（0x00 到 0xFF），给的是 \"" +
                         text.substr(eq + 1) + "\"");
    }

    return Poke{static_cast<std::uint16_t>(address),
                static_cast<std::uint8_t>(value)};
}

// 哪些开关后面要跟一个值。
// 写成一张显式的表而不是"试着解析失败就当开关"——
// 后者会把 ember screen a.ch8 b.ch8 里的 b.ch8 也吃掉。
bool flagTakesValue(const std::string& name) {
    return name == "--cycles" || name == "--steps" || name == "--offset" ||
           name == "--bytes" || name == "--ips" || name == "--hz" ||
           name == "--scale" || name == "--seed" || name == "--quirks" ||
           name == "--poke" || name == "-o";
}

void applyFlag(Options& opts, const std::string& name, const std::string& value) {
    if (name == "--cycles") {
        opts.cycles = parseDecimal(value, name);
    } else if (name == "--steps") {
        opts.steps = parseDecimal(value, name);
    } else if (name == "--offset") {
        opts.offset = parseDecimal(value, name);
    } else if (name == "--bytes") {
        opts.bytes = parseDecimal(value, name);
    } else if (name == "--ips") {
        opts.ips = static_cast<int>(parseDecimal(value, name));
    } else if (name == "--hz") {
        opts.hz = static_cast<int>(parseDecimal(value, name));
    } else if (name == "--scale") {
        opts.scale = static_cast<int>(parseDecimal(value, name));
    } else if (name == "--seed") {
        opts.seed = parseSeed(value);
    } else if (name == "--quirks") {
        opts.quirks = Quirks::fromName(value);
    } else if (name == "--poke") {
        opts.pokes.push_back(parsePoke(value));
    } else if (name == "-o") {
        opts.output = value;
    } else if (name == "--no-deltas") {
        opts.deltas = false;
    } else if (name == "--font") {
        opts.fontOnly = true;
    } else if (name == "--quirks-table") {
        opts.quirksTable = true;
    } else if (name == "--ansi") {
        opts.ansi = true;
    } else if (name == "--help" || name == "-h" || name == "-?") {
        opts.help = true;
    } else {
        throw EmberError("不认识的参数: " + name + "（用 ember --help 看用法）");
    }
}

Options parseCommandLine(const std::vector<std::string>& args) {
    Options opts;

    // 从 1 开始：第 0 个是程序自己的路径。
    for (std::size_t k = 1; k < args.size(); ++k) {
        const std::string arg = args[k];
        if (arg.empty()) {
            continue;
        }

        if (arg[0] == '-') {
            std::string name = arg;
            std::string value;
            bool hasValue = false;

            // 支持 --cycles=400 和 --cycles 400 两种写法。
            // 两种都收是因为：写脚本时用 = 更清楚，
            // 手敲时用空格更顺，没有理由逼人选一种。
            if (arg.size() > 2 && arg[1] == '-') {
                const std::size_t eq = arg.find('=');
                if (eq != std::string::npos) {
                    name = arg.substr(0, eq);
                    value = arg.substr(eq + 1);
                    hasValue = true;
                }
            }

            if (!hasValue && flagTakesValue(name)) {
                if (k + 1 >= args.size()) {
                    throw EmberError(name + " 后面要跟一个值");
                }
                value = args[++k];
                hasValue = true;
            }

            applyFlag(opts, name, value);
            continue;
        }

        // 位置参数：第一个是子命令，第二个是输入文件。
        if (opts.command.empty()) {
            opts.command = arg;
        } else if (opts.input.empty()) {
            opts.input = arg;
        } else {
            throw EmberError("多出来一个参数: \"" + arg +
                             "\"（一条命令最多带一个输入文件）");
        }
    }

    return opts;
}

// ============================================================
//  二、装载
// ============================================================

// 装好 ROM、pokes 也打完的机器。
//
// 【为什么做成返回值而不是一个 Session 结构体】
// 因为调用方要的就只有机器本身。包一层结构体只是为了
// "看起来整齐"，代价是每个用到的地方都得多写一次 .machine。
Machine loadMachine(const Options& opts) {
    Machine machine(opts.quirks, opts.seed);
    machine.loadRom(Rom::fromFile(opts.input));

    // poke 放在 loadRom 之后：这样 poke 可以改 ROM 自己的字节，
    // 也就是"打补丁"。反过来的话补丁会被 ROM 覆盖掉。
    for (std::size_t k = 0; k < opts.pokes.size(); ++k) {
        machine.poke(opts.pokes[k].address, opts.pokes[k].value);
    }

    return machine;
}

// 跑完之后给人看的一行旁白，走 stderr。
// 它不进黄金文件，因为黄金文件比的是 stdout。
void reportRun(const Machine& machine, long long instructions) {
    std::fprintf(stderr, "[跑了 %lld 条指令  PC=%s  I=%s  DT=%s ST=%s]\n",
                 instructions, hex3(machine.pc()).c_str(),
                 hex3(machine.i()).c_str(),
                 hex2(machine.delayTimer()).c_str(),
                 hex2(machine.soundTimer()).c_str());

    if (machine.lastStep().status == StepStatus::UNKNOWN_OPCODE) {
        // 这一条值得单独喊一声：跑飞的程序最常见的表现就是
        // PC 走到数据区、读到不是指令的字节。
        std::fprintf(stderr,
                     "[注意] 最后一条 %s 不是合法指令，已经当空操作跳过\n",
                     hex4(machine.lastStep().opcode).c_str());
    }
}

// ============================================================
//  三、各个子命令
// ============================================================

int commandRun(const Options& opts) {
    Machine machine = loadMachine(opts);
    return runWithWindow(machine, opts.ips, opts.hz, opts.scale);
}

// ---- 在终端里玩 ----
//
// 【这个循环的形状，以及为什么是这个形状】
//
//   每一轮做四件事，顺序是有讲究的：
//
//     1. 收按键        —— 先把输入拿到，这一帧的输入就是最新的
//     2. 跑指令        —— 按"上一帧到现在过了多少毫秒"跑对应条数
//     3. 画一帧        —— 画的是刚跑完的状态
//     4. 睡掉剩下的时间 —— 把帧率压到 60 左右
//
//   第 2 步用的是**真实流逝的毫秒**，不是"每帧固定跑 N 条"。
//   这一点是承重的：如果按固定条数跑，那么机器忙的时候
//   游戏会变慢（因为帧率掉了），而 CHIP-8 的游戏逻辑
//   （重力、倒计时、移动速度）全挂在定时器上，
//   变慢的表现就是"游戏像慢动作"。按真实时间跑，
//   帧率可以掉，但游戏速度不变——这正是 Clock::advanceByMs
//   存在的意义，也是它内部要钳制 MAX_CATCHUP_MS 的原因。
//
//   第 4 步减去"这一帧已经花掉的时间"，而不是无脑 sleep 16ms。
//   无脑睡的话，渲染本身花 5ms 的话实际帧长就是 21ms，
//   帧率掉到 47。这不影响游戏速度（见上），但画面会明显抖。
constexpr double PLAY_FRAME_MS = 1000.0 / 60.0;

int commandPlay(const Options& opts) {
    Machine machine = loadMachine(opts);

    // ★ 三道"能不能玩"的检查**全部**排在接管屏幕之前。
    //
    //   顺序是有意的：进替代屏幕之后再发现不行的话，得先退出来
    //   才能报错，用户看到的是"屏幕闪了一下然后说不行"；
    //   更糟的是"进来之后才发现收不到按键"，那就成了一个
    //   按什么都没反应、只能关窗口的游戏。
    //   所以：先问清楚，再动手。

    // 第一道：终端认不认 ANSI 转义。
    if (!enableTerminalAnsi()) {
        // 原因交给平台层去问——它知道是管道、是文件、
        // 还是"这个终端根本不是 Windows 控制台"。
        // main.cpp 自己猜的话，最可能猜错成"你的终端不支持 ANSI"，
        // 而真相常常是"你在 Git Bash 里跑，换个地方就行"。
        std::fprintf(stderr,
                     "[画不了] 这个终端画不了 ANSI 画面。\n"
                     "         原因：%s\n"
                     "         在 cmd.exe 或 PowerShell 窗口里直接跑就能玩。\n"
                     "         想不开窗口看画面：ember screen <rom> --cycles N\n",
                     terminalAnsiFailureReason().c_str());
        return 2;
    }

    // 第二道：窗口放不放得下。
    // 太小的话半块画面会折行，看起来像是渲染器画错了，
    // 其实是放不下。与其让用户去猜，不如直接说。
    const int columns = terminalColumns();
    const int rows = terminalRows();
    const int needRows = Display::HEIGHT / 2;
    if (columns > 0 && (columns < Display::WIDTH || rows < needRows)) {
        std::fprintf(stderr,
                     "[放不下] CHIP-8 的画面要 %d 列 x %d 行，"
                     "这个窗口是 %d x %d。\n"
                     "         把窗口拉大一点再试。\n",
                     Display::WIDTH, needRows, columns, rows);
        return 2;
    }

    // 第三道：键盘在不在我们手里。
    // 标准输入被重定向（比如 `ember play rom.ch8 < 一个文件`）时
    // 一个按键也收不到，那种情况下进去就是个死局。
    if (!beginTerminalInput()) {
        std::fputs(
            "[收不到按键] 标准输入不是控制台，这个进程拿不到键盘。\n"
            "             常见原因是把输入重定向了（< 文件 或 管道）。\n"
            "             直接双击或者在终端里正常敲这条命令就行。\n",
            stderr);
        return 2;
    }

    Clock clock(opts.ips, opts.hz);

    std::fputs(ansi::ENTER_ALT_SCREEN, stdout);
    std::fputs(ansi::HIDE_CURSOR, stdout);
    std::fputs(ansi::CLEAR, stdout);
    std::fflush(stdout);

    bool running = true;
    bool buzzerWasOn = false;
    double lastFrameMs = nowMs();

    while (running) {
        // ---- 1. 收按键 ----
        // 一次全收掉，不是一帧收一个：一帧里可能按下又松开好几个键，
        // 只收一个的话剩下的积压到下一帧，手感就是慢半拍。
        const std::vector<KeyEvent> events = pollKeyEvents();
        for (std::size_t k = 0; k < events.size(); ++k) {
            if (events[k].key == KEY_QUIT) {
                running = false;
                break;
            }
            // 按下和松开都要送进去。只送"按下"的话，
            // 游戏里按住方向键走两步就停了。
            machine.setKey(events[k].key, events[k].down);
        }
        if (!running) {
            break;
        }

        // ---- 2. 跑指令 ----
        const double frameBeginMs = nowMs();
        const double elapsedMs = frameBeginMs - lastFrameMs;
        lastFrameMs = frameBeginMs;
        clock.advanceByMs(machine, elapsedMs);

        // ---- 3. 蜂鸣器 ----
        // 终端里没有方波，但控制台有一个"响一声"的字符 \a。
        //
        // ★ 只在**从静音变成响**的那一刻发一次。
        //   每帧都发的话是 60 次/秒的响铃：听起来糊成一片噪音，
        //   而且控制台会把它们排队，游戏退出之后还在响。
        //   边沿触发才是声音定时器本来的语义——它是"开始响了"，
        //   不是一个持续的音量。
        const bool buzzerOn = machine.soundActive();
        if (buzzerOn && !buzzerWasOn) {
            std::fputc('\a', stdout);
        }
        buzzerWasOn = buzzerOn;

        // ---- 4. 画 ----
        // HOME 归位之后整屏重画，不用清屏：64x16 个格子每次都被
        // 完整的 64x16 个格子盖掉，一个不留。清屏反而会闪。
        std::fputs(ansi::HOME, stdout);
        const std::string frame = ansi::renderHalfBlock(machine.display(), true);
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fflush(stdout);

        // ---- 5. 睡掉这一帧剩下的时间 ----
        const double spentMs = nowMs() - frameBeginMs;
        const double restMs = PLAY_FRAME_MS - spentMs;
        if (restMs > 0.0) {
            sleepMs(static_cast<int>(restMs));
        }
    }

    // ---- 收尾 ----
    // 这几行的顺序和进来时正好相反，而且**必须**都执行到：
    // 少了 SHOW_CURSOR 光标就永远不见了，
    // 少了 LEAVE_ALT_SCREEN 用户敲过的命令就再也回不来了。
    //
    // 【为什么还留着 endTerminalInput 之外的 Ctrl+C 处理器】
    // 上面这个循环不是唯一的出口——用户可以直接关掉控制台窗口。
    // 那条路径不会执行到这里，所以 platform 层还装着一个
    // 兜底的处理器。两条路径都会调 restoreConsole()，
    // 而它是幂等的。
    endTerminalInput();
    std::fputs(ansi::RESET, stdout);
    std::fputs(ansi::SHOW_CURSOR, stdout);
    std::fputs(ansi::LEAVE_ALT_SCREEN, stdout);
    std::fflush(stdout);

    reportRun(machine, clock.totalInstructions());
    return 0;
}

int commandScreen(const Options& opts) {
    Machine machine = loadMachine(opts);
    Clock clock(opts.ips, opts.hz);

    long long cycles = 0;
    if (opts.cycles < 0) {
        // 不给 --cycles 就一条都不跑。这比"随便挑个默认值"好：
        // 随便挑的话，看到一屏空白你分不清是"程序没画"
        // 还是"默认值太小了没跑到画的地方"。
        std::fputs("[没给 --cycles，一条指令都没跑，画面是初始状态]\n", stderr);
    } else {
        cycles = opts.cycles;
        clock.advanceInstructions(machine, cycles);
    }

    // ★ stdout 和 stderr 分工见文件头。
    //   这里只印 32 行 x 64 列的字符画，一个多余字符都没有，
    //   黄金文件才能直接拿来 diff。
    if (opts.ansi) {
        // 半块 + 颜色那条路（和 ember play 屏幕上看到的是同一个函数）。
        //
        // 这个开关存在的理由是**可测**：play 要一个真终端才能跑，
        // 于是"两个像素有没有画对格子"就只能靠肉眼看。
        // 走 screen --ansi 就能把同一份渲染结果当文本断言。
        // 转义序列是原样输出的，所以它不是给人读的，是给测试比的。
        const std::string frame = ansi::renderHalfBlock(machine.display(), true);
        std::fputs(frame.c_str(), stdout);
        std::fputc('\n', stdout);
    } else {
        std::fputs(machine.display().toAscii().c_str(), stdout);
        std::fputc('\n', stdout);
    }

    reportRun(machine, cycles);
    return 0;
}

int commandTrace(const Options& opts) {
    Machine machine = loadMachine(opts);
    Clock clock(opts.ips, opts.hz);
    Tracer tracer(opts.deltas);

    long long steps = 0;
    if (opts.steps < 0) {
        std::fputs("[没给 --steps，一行都没印]\n", stderr);
    } else {
        steps = opts.steps;
    }

    for (long long k = 0; k < steps; ++k) {
        const std::string line = tracer.step(machine, clock);
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);

        // TICK 单独一行，紧跟触发它的那条指令。
        // 用它自己的访问器问，而不是在这里重算一次比例——
        // 重算就会出现"两处取整不一致，TICK 错开一条"。
        if (tracer.ticked()) {
            const std::string tick = Tracer::tickLine();
            std::fputs(tick.c_str(), stdout);
            std::fputc('\n', stdout);
        }
    }

    reportRun(machine, steps);
    return 0;
}

int commandDis(const Options& opts) {
    const Rom rom = Rom::fromFile(opts.input);
    const std::vector<std::uint8_t>& data = rom.bytes();

    const long long size = static_cast<long long>(data.size());
    if (opts.offset < 0 || opts.offset > size) {
        throw EmberError("--offset " + std::to_string(opts.offset) +
                         " 超出 ROM 范围（这个 ROM 有 " + std::to_string(size) +
                         " 字节）");
    }

    long long available = size - opts.offset;
    long long count = (opts.bytes > 0) ? std::min(opts.bytes, available) : available;

    if (count % 2 != 0) {
        // 一条指令两个字节，所以奇数长度的尾巴没法反汇编。
        // 不报错，但要说明白——不然"最后一行怎么少了一条"
        // 会变成一个要查半天的问题。
        std::fprintf(stderr, "[注意] 有 1 个字节凑不成整条指令，已忽略\n");
    }
    count -= count % 2;

    for (long long k = 0; k + 1 < count; k += 2) {
        const std::size_t low = static_cast<std::size_t>(opts.offset + k);
        const std::size_t high = low + 1;

        // 取指用移位而不是指针强转：数组里第 4095 个字节处
        // 强转成 uint16_t* 是非对齐访问（未定义行为），
        // 而且结果取决于字节序。
        const std::uint16_t raw = static_cast<std::uint16_t>(
            (data[low] << 8) | data[high]);
        const std::uint16_t address = static_cast<std::uint16_t>(
            (Machine::ROM_BASE + opts.offset + k) & 0x0FFF);

        const std::string line =
            hex3(address) + "  " + hex4(raw) + "  " + disassemble(raw, opts.quirks);
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
    }

    return 0;
}

int commandAsm(const Options& opts) {
    (void)opts;
    std::fputs(
        "[还没做] 汇编器是阶段 5 的内容。\n"
        "         现在的进度：反汇编（dis）已经能用了，\n"
        "         但 assemble 方向的代码还没写。\n",
        stderr);
    return 2;
}

// ============================================================
//  四、自检
// ============================================================
//
//  【为什么要自检，而不是"靠测试"】
//  这里查的是那些**用 ROM 测不出来**的东西：
//  字模的字节、译码表的覆盖、随机数的可复现性、内存地址的掩码。
//  它们出错的表现往往是"某个游戏画面很奇怪"，
//  从那个现象倒推到"字模第 7 个字节写错了"要花一整天。
//  自检把这些三十秒就能查完的事压到三十秒。

int gPass = 0;
int gFail = 0;

void reportCheck(bool ok, const std::string& label) {
    if (ok) {
        ++gPass;
        std::printf("[OK]   %s\n", label.c_str());
    } else {
        ++gFail;
        std::printf("[FAIL] %s\n", label.c_str());
    }
}

// ---- 字模 ----

std::uint32_t fontChecksum() {
    std::uint32_t sum = 0;
    for (std::size_t k = 0; k < FONT_TOTAL_BYTES; ++k) {
        sum += FONT[k];
    }
    return sum;
}

// FNV-1a 32 位。比单纯求和敏感得多——两个字节写反了，
// 和是一样的，FNV 就不一样。
std::uint32_t fontHash() {
    std::uint32_t hash = 2166136261u;
    for (std::size_t k = 0; k < FONT_TOTAL_BYTES; ++k) {
        hash ^= FONT[k];
        hash *= 16777619u;
    }
    return hash;
}

void printFont() {
    std::printf("字模：%d 个字形 x %d 字节 = %d 字节，默认装在内存 0x%03X\n",
                FONT_GLYPH_COUNT, FONT_GLYPH_BYTES, FONT_TOTAL_BYTES,
                Quirks::modern().fontBase());
    std::printf("（VIP 预设装在 0x%03X，由 --quirks vip 决定）\n\n",
                Quirks::vip().fontBase());

    // 续行缩进 22 列 = 5 列前缀 + 15 列字节 + 2 列空隙。
    const std::string indent(22, ' ');

    for (int digit = 0; digit < FONT_GLYPH_COUNT; ++digit) {
        const std::uint8_t* glyph = fontSprite(static_cast<std::uint8_t>(digit));

        for (int row = 0; row < FONT_GLYPH_BYTES; ++row) {
            // 画的是**高** 4 位：bit 7 在最左边。
            //
            // ★ 这里第一次写成了 bit 3..0，于是 16 个字形的点阵
            //   全是 "...." ——因为字模的低 4 位本来就全是 0
            //   （那 4 位是给 SUPER-CHIP 的高清字体留的）。
            //   注意**校验和当时是对的**：字节本身没错，
            //   错的是"哪几位算像素"这个理解。
            //   这就是为什么光有校验和不够，还得有能看的东西。
            std::string art;
            for (int bit = 7; bit >= 4; --bit) {
                const bool on = ((glyph[row] >> bit) & 1) != 0;
                art += on ? '#' : '.';
            }

            if (row == 0) {
                std::printf("  %X: %02X %02X %02X %02X %02X  %s\n", digit,
                            glyph[0], glyph[1], glyph[2], glyph[3], glyph[4],
                            art.c_str());
            } else {
                std::printf("%s%s\n", indent.c_str(), art.c_str());
            }
        }
    }

    std::printf("\n校验和: 和=0x%04X  FNV1a=0x%08X\n", fontChecksum(),
                fontHash());
}

// ---- 开关对照表 ----
//
// 【为什么这张表值得占屏幕】因为它是唯一能让人**一眼看出
//  两个预设哪里不一样**的地方。10 个开关 x 4 组预设 = 40 个格子，
//  比读四段构造函数快得多，也比"至少有一处不同"那种断言有用得多——
//  后者只回答"两边像不像"，不回答"每边对不对"。
//
//  实际上 vip 预设的字模地址写反了就是这么发现的：
//  不是测试红了，是这张表里那一列看着别扭。
void printQuirksTable() {
    const Quirks presets[4] = {Quirks::modern(), Quirks::vip(),
                              Quirks::schipModern(), Quirks::schipLegacy()};
    const char* names[4] = {"modern", "vip", "schip", "schip-legacy"};

    std::printf("行为开关预设对照（%d 个开关 x 4 组预设）\n\n",
                Quirks::FLAG_COUNT);

    // ★ 表头和格子全用 ASCII。
    //   用"是/否"的话 printf 的 %-13s 是按**字节**补空格，
    //   而一个汉字是 3 个字节、显示只占 2 列——表会歪。
    //   要让它不歪就得自己数显示宽度，不值得为一张调试用的表做这件事。
    std::printf("  %-22s %-14s %-14s %-14s %-14s\n", "flag (0-9)", names[0],
                names[1], names[2], names[3]);

    for (int f = 0; f < Quirks::FLAG_COUNT; ++f) {
        char label[32];
        std::snprintf(label, sizeof(label), "%d %s", f, Quirks::flagName(f));
        std::printf("  %-22s", label);
        for (int p = 0; p < 4; ++p) {
            std::printf(" %-14s", presets[p].flagValue(f) ? "Y" : "-");
        }
        std::printf("\n");
    }

    // fontBase() 是算出来的，不是那 10 个 bool 之一，
    // 但它是这几行里唯一一个"值"而不是"开关"，所以单列一行。
    std::printf("  %-22s", "fontBase()");
    for (int p = 0; p < 4; ++p) {
        std::printf(" %-14s", hex3(presets[p].fontBase()).c_str());
    }
    std::printf("\n");
}

// ---- 各项检查 ----
void checkFontShape() {
    reportCheck(FONT_GLYPH_COUNT * FONT_GLYPH_BYTES == FONT_TOTAL_BYTES,
                "字模尺寸自洽（16 x 5 = 80）");

    // 两两不同的字形。有重复的话，屏幕上两个数字会长得一样——
    // 那是个"看见了但不一定想得到去查"的 bug。
    bool allDistinct = true;
    for (int a = 0; a < FONT_GLYPH_COUNT && allDistinct; ++a) {
        for (int b = a + 1; b < FONT_GLYPH_COUNT; ++b) {
            const std::uint8_t* ga = fontSprite(static_cast<std::uint8_t>(a));
            const std::uint8_t* gb = fontSprite(static_cast<std::uint8_t>(b));
            bool same = true;
            for (int k = 0; k < FONT_GLYPH_BYTES; ++k) {
                if (ga[k] != gb[k]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                allDistinct = false;
                break;
            }
        }
    }
    reportCheck(allDistinct, "16 个字形两两不同");

    // 空格子。全零的字形在屏幕上什么都不显示，
    // 表现出来是"数字 8 不见了"，但你会先去查绘图指令。
    bool allNonBlank = true;
    for (int digit = 0; digit < FONT_GLYPH_COUNT; ++digit) {
        const std::uint8_t* glyph = fontSprite(static_cast<std::uint8_t>(digit));
        bool blank = true;
        for (int k = 0; k < FONT_GLYPH_BYTES; ++k) {
            if (glyph[k] != 0) {
                blank = false;
                break;
            }
        }
        if (blank) {
            allNonBlank = false;
            break;
        }
    }
    reportCheck(allNonBlank, "没有空字形");

    // 校验和本身不重要，重要的是它**钉死不变**。
    // 检查项的名字里带着算出来的值，所以黄金文件一旦
    // 记下这个名字，以后谁碰坏了字模，这一行就会变红。
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer),
                  "字模校验和（和=0x%04X FNV1a=0x%08X）", fontChecksum(),
                  fontHash());
    reportCheck(fontChecksum() != 0 && fontHash() != 0, buffer);
}

void checkQuirks() {
    // 逐个试比去解析 presetNames() 返回的那个字符串简单可靠，
    // 而且这里要验的就是"这四个名字能被 fromName 认出来"。
    const char* presets[] = {"modern", "vip", "schip", "schip-legacy"};
    bool allParsed = true;
    for (int k = 0; k < 4; ++k) {
        try {
            Quirks::fromName(presets[k]);
        } catch (const EmberError&) {
            allParsed = false;
        }
    }
    reportCheck(allParsed, "4 个 quirks 预设名字都能解析");

    bool badNameRejected = false;
    try {
        Quirks::fromName("这个预设不存在");
    } catch (const EmberError&) {
        badNameRejected = true;
    }
    reportCheck(badNameRejected, "乱写的 quirks 名字会被拒绝");

    bool namesOk = true;
    for (int k = 0; k < Quirks::FLAG_COUNT; ++k) {
        const char* name = Quirks::flagName(k);
        if (name == nullptr || name[0] == '\0') {
            namesOk = false;
        }
    }
    reportCheck(namesOk, "10 个开关都有名字");

    // 每两个预设之间至少要有一个开关不同。都一样的话
    // 那就是白写了三个预设。
    const Quirks modern = Quirks::modern();
    const Quirks vip = Quirks::vip();
    bool differs = false;
    for (int k = 0; k < Quirks::FLAG_COUNT; ++k) {
        if (modern.flagValue(k) != vip.flagValue(k)) {
            differs = true;
            break;
        }
    }
    reportCheck(differs, "modern 和 vip 至少有一处不同");

    // ★ 字模地址单独查，因为"vip 的字模在哪"这件事
    //   曾经真的写反过（vip() 里 fontAtVipAddress 是 false），
    //   而上面那条"至少有一处不同"照样是绿的——
    //   它只回答"两边像不像"，不回答"每边对不对"。
    //   这就是为什么"至少不同"这类检查不能顶替具体断言。
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "vip 的字模在 0x000、modern 在 0x050（实际 %s / %s）",
                  hex3(vip.fontBase()).c_str(),
                  hex3(modern.fontBase()).c_str());
    reportCheck(vip.fontBase() == 0x000 && modern.fontBase() == 0x050, buffer);

    reportCheck(Quirks::schipModern().fontBase() == 0x050 &&
                    Quirks::schipLegacy().fontBase() == 0x050,
                "两组 schip 预设的字模都在 0x050");
}

void checkDecode() {
    // 关键：Fx__ 家族按**低字节**分派。Fx15（LD DT,Vx）
    // 和 Fx55（LD I,Vx）的低半字节都是 5，按半字节分派
    // 会把它们混成一个——这个错误编译能过、不报错、
    // 只是静默译错。
    reportCheck(decode(0xF015).op == Opcode::LD_DT_VX,
                "0xF015 译成 LD DT, Vx（低字节分派）");
    reportCheck(decode(0xF055).op == Opcode::LD_I_VX,
                "0xF055 译成 LD [I], Vx（低字节分派）");
    reportCheck(decode(0xF065).op == Opcode::LD_VX_I,
                "0xF065 译成 LD Vx, [I]");

    reportCheck(decode(0x00E0).op == Opcode::CLS, "0x00E0 译成 CLS");
    reportCheck(decode(0x00EE).op == Opcode::RET, "0x00EE 译成 RET");
    reportCheck(decode(0x1234).op == Opcode::JP, "0x1234 译成 JP");
    reportCheck(decode(0x2ABC).op == Opcode::CALL, "0x2ABC 译成 CALL");
    reportCheck(decode(0x6012).op == Opcode::LD_BYTE, "0x6012 译成 LD Vx, nn");
    reportCheck(decode(0x8014).op == Opcode::ADD_REG, "0x8014 译成 ADD Vx, Vy");
    reportCheck(decode(0x8015).op == Opcode::SUB, "0x8015 译成 SUB Vx, Vy");
    reportCheck(decode(0x8016).op == Opcode::SHR, "0x8016 译成 SHR");
    reportCheck(decode(0xF029).op == Opcode::LD_F_VX, "0xF029 译成 LD F, Vx");

    // 5xy0 / 9xy0 的最后一位必须是 0。不是 0 的就是非法指令，
    // 不能因为"高 12 位对上了"就当成合法的——那会让
    // 5xy1 这种垃圾字节变成一条能跑的条件跳过。
    reportCheck(decode(0x5010).op == Opcode::SE_REG, "0x5010 译成 SE Vx, Vy");
    reportCheck(!isDefined(decode(0x5011).op), "0x5011 不是合法指令（尾巴必须是 0）");
    reportCheck(decode(0x9010).op == Opcode::SNE_REG, "0x9010 译成 SNE Vx, Vy");
    reportCheck(!isDefined(decode(0x9011).op), "0x9011 不是合法指令");

    // 全 65536 个值扫一遍，数出多少是未定义。
    // 这个数字钉死之后，"不小心把某条指令的掩码改了"
    // 会让它动，检查就会红。
    long long defined = 0;
    for (int raw = 0; raw <= 0xFFFF; ++raw) {
        if (isDefined(decode(static_cast<std::uint16_t>(raw)).op)) {
            ++defined;
        }
    }
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer),
                  "65536 个值里 %lld 个是合法指令（钉死用）", defined);
    reportCheck(defined > 0 && defined < 65536, buffer);

    // 每个枚举值都得有名字，否则 trace 里会印出空的反汇编列。
    bool allNamed = true;
    const Opcode samples[] = {
        Opcode::SYS,       Opcode::CLS,      Opcode::RET,     Opcode::JP,
        Opcode::CALL,      Opcode::SE_BYTE,  Opcode::SNE_BYTE, Opcode::LD_BYTE,
        Opcode::ADD_BYTE,  Opcode::LD_REG,   Opcode::OR,      Opcode::AND,
        Opcode::XOR,       Opcode::ADD_REG,  Opcode::SUB,     Opcode::SHR,
        Opcode::SUBN,      Opcode::SHL,      Opcode::SE_REG,  Opcode::SNE_REG,
        Opcode::LD_ADDR,   Opcode::JP_V0,    Opcode::RND,     Opcode::DRW,
        Opcode::SKP,       Opcode::SKNP,     Opcode::LD_VX_DT, Opcode::LD_VX_K,
        Opcode::LD_DT_VX,  Opcode::LD_ST_VX, Opcode::ADD_I_VX, Opcode::LD_F_VX,
        Opcode::LD_B_VX,   Opcode::LD_I_VX,  Opcode::LD_VX_I};
    for (std::size_t k = 0; k < sizeof(samples) / sizeof(samples[0]); ++k) {
        if (!isDefined(samples[k])) {
            allNamed = false;
        }
    }
    reportCheck(allNamed, "35 条指令枚举值全部有定义");
}

void checkRandom() {
    // 【这里查的是可复现性，不是随机性】
    // 黄金文件比的是字节。同一颗种子跑出同一串字节，
    // 是 trace 能当回归测试的前提。
    Machine a(Quirks::modern(), 12345);
    Machine b(Quirks::modern(), 12345);
    for (int k = 0; k < 2; ++k) {
        a.poke(static_cast<std::uint16_t>(0x200 + k), k == 0 ? 0xC0 : 0xFF);
        b.poke(static_cast<std::uint16_t>(0x200 + k), k == 0 ? 0xC0 : 0xFF);
    }
    for (int k = 0; k < 8; ++k) {
        a.step();
        b.step();
    }
    reportCheck(a.v(0) == b.v(0), "同种子 -> 同随机数");

    // reset() 必须重新播种。不重播的话同一个程序跑第二遍
    // 结果就换了，黄金文件从第二次运行开始全部对不上。
    Machine c(Quirks::modern(), 12345);
    c.poke(0x200, 0xC0);
    c.poke(0x201, 0xFF);
    c.step();
    const std::uint8_t first = c.v(0);
    c.reset();
    c.poke(0x200, 0xC0);
    c.poke(0x201, 0xFF);
    c.step();
    reportCheck(c.v(0) == first, "reset() 会重新播种，序列从头开始");

    // 不同种子应该给不同结果。用 8 个字节比：
    // 单个字节撞车的概率是 1/256，8 个字节全撞是 2^-64。
    Machine d(Quirks::modern(), 999);
    Machine e(Quirks::modern(), 1000);
    for (int k = 0; k < 2; ++k) {
        d.poke(static_cast<std::uint16_t>(0x200 + k), k == 0 ? 0xC0 : 0xFF);
        e.poke(static_cast<std::uint16_t>(0x200 + k), k == 0 ? 0xC0 : 0xFF);
    }
    bool differs = false;
    for (int k = 0; k < 8; ++k) {
        d.step();
        e.step();
        if (d.v(0) != e.v(0)) {
            differs = true;
        }
    }
    reportCheck(differs, "不同种子 -> 不同随机数");
}

void checkDisplay() {
    Display display;

    // 一行 8 个点全亮。
    const std::uint8_t solid[1] = {0xFF};
    display.clear();
    const bool hit = display.drawSprite(0, 0, solid, 1, false);
    bool eightLit = true;
    for (int x = 0; x < 8; ++x) {
        if (!display.pixel(x, 0)) {
            eightLit = false;
        }
    }
    reportCheck(!hit && eightLit, "画一条 8 像素的横线，8 个点都亮且无碰撞");

    // 同一个位置再画一次 -> 全灭 + 碰撞标志。
    // 碰撞是"有像素从亮变灭"，XOR 的自然结果。
    const bool second = display.drawSprite(0, 0, solid, 1, false);
    bool allDark = true;
    for (int x = 0; x < 8; ++x) {
        if (display.pixel(x, 0)) {
            allDark = false;
        }
    }
    reportCheck(second && allDark, "同一位置重画 -> 全灭且碰撞标志置位");

    // n = 0 什么都不画。
    // ★ 这个坑很隐蔽：写 for (row = 0; row < 16; ++row) 就会
    //   用 n=0 画出 16 行长条，而且只在 n=0 时错。
    display.clear();
    const std::uint8_t rows[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF};
    const bool zeroHit = display.drawSprite(0, 0, rows, 0, false);
    bool nothingDrawn = true;
    for (int y = 0; y < Display::HEIGHT; ++y) {
        for (int x = 0; x < Display::WIDTH; ++x) {
            if (display.pixel(x, y)) {
                nothingDrawn = false;
            }
        }
    }
    reportCheck(!zeroHit && nothingDrawn, "n=0 什么都不画（不是画 16 行）");

    // 起点坐标永远回绕，横向越界永远裁剪。
    // 这两条和"精灵身体纵向绕不绕"是三件不同的事，
    // 经常被混成一件。
    Display wrapped;
    wrapped.drawSprite(Display::WIDTH, 0, solid, 1, false);
    bool startWraps = true;
    for (int x = 0; x < 8; ++x) {
        if (!wrapped.pixel(x, 0)) {
            startWraps = false;
        }
    }
    reportCheck(startWraps, "起点 x=64 回绕到 0");

    Display clipped;
    clipped.drawSprite(Display::WIDTH - 4, 0, solid, 1, false);
    bool clippedRight = true;
    for (int x = Display::WIDTH - 4; x < Display::WIDTH; ++x) {
        if (!clipped.pixel(x, 0)) {
            clippedRight = false;
        }
    }
    // 前 4 个点亮在屏上，后 4 个被裁掉——裁剪不该绕回来
    // 把左边 4 个点亮。
    for (int x = 0; x < 4; ++x) {
        if (clipped.pixel(x, 0)) {
            clippedRight = false;
        }
    }
    reportCheck(clippedRight, "横向越界是裁掉，不是绕回");

    // 纵向：不绕时最后一行被裁，绕时它出现在第一行。
    const std::uint8_t two[2] = {0xFF, 0xFF};
    Display noWrap;
    noWrap.drawSprite(0, Display::HEIGHT - 1, two, 2, false);
    const bool clipBottom = !noWrap.pixel(0, 0) && noWrap.pixel(0, Display::HEIGHT - 1);
    reportCheck(clipBottom, "spriteVerticalWrap 关：底部越界裁掉");

    Display withWrap;
    withWrap.drawSprite(0, Display::HEIGHT - 1, two, 2, true);
    const bool wrapBottom = withWrap.pixel(0, 0) && withWrap.pixel(0, Display::HEIGHT - 1);
    reportCheck(wrapBottom, "spriteVerticalWrap 开：底部越界绕回顶部");

    reportCheck(Display::PIXEL_COUNT == Display::WIDTH * Display::HEIGHT,
                "显存大小 = 64 x 32");
}

void checkMemoryMasking() {
    // ★ 这个检查针对的是一个**静默而且不一定崩**的 bug：
    //   I = 0xFF8 时 Fx55 要写 I+0..I+15，也就是写到 0x1007，
    //   越过了 4096 个元素的数组。不掩码的话是未定义行为，
    //   debug 下可能什么都不发生，release 下可能踩坏别的东西。
    //
    //   程序给 V0..VF 分别装上可辨认的值，然后 Fx55 存回去。
    //   正确的话这 16 个字节会**绕着内存顶端折成两段**：
    //       V0..V7  ->  0xFF8..0xFFF   （前半段，正常落到高位）
    //       V8..VF  ->  0x000..0x007   （后半段，掩码后绕回低位）
    //   一个检查看前半段、一个看后半段，合起来才说明"折"对了。
    Machine machine(Quirks::modern(), 1);

    const std::uint16_t program[] = {
        0xAFF8,  // LD I, 0xFF8
        0x6010,  // LD V0, 0x10
        0x6111,  // LD V1, 0x11
        0x6212,  // LD V2, 0x12
        0x6313,  // LD V3, 0x13
        0x6414,  // LD V4, 0x14
        0x6515,  // LD V5, 0x15
        0x6616,  // LD V6, 0x16
        0x6717,  // LD V7, 0x17
        0x6801,  // LD V8, 0x01
        0x6902,  // LD V9, 0x02
        0x6A03,  // LD V10, 0x03
        0x6B04,  // LD V11, 0x04
        0x6C05,  // LD V12, 0x05
        0x6D06,  // LD V13, 0x06
        0x6E07,  // LD V14, 0x07
        0x6F08,  // LD VF, 0x08
        0xFF55,  // LD [I], Vx   （x = F，所以是全部 16 个）
    };
    const int programLength =
        static_cast<int>(sizeof(program) / sizeof(program[0]));

    std::uint16_t address = 0x200;
    for (int k = 0; k < programLength; ++k) {
        machine.poke(address, static_cast<std::uint8_t>(program[k] >> 8));
        machine.poke(static_cast<std::uint16_t>(address + 1),
                     static_cast<std::uint8_t>(program[k] & 0xFF));
        address = static_cast<std::uint16_t>(address + 2);
    }

    for (int k = 0; k < programLength; ++k) {
        machine.step();
    }

    reportCheck(machine.memory(0xFF8) == 0x10 && machine.memory(0xFFF) == 0x17,
                "I=0xFF8 时 Fx55 的前半段落在 0xFF8..0xFFF");

    // 后半段能落回 0x000..0x007 才是真正证明了掩码在做工——
    // 不做掩码的话这八个字节会写到数组外面去，
    // 而 0x000..0x007 会保持字模的原样，这个检查就会红。
    reportCheck(machine.memory(0x000) == 0x01 && machine.memory(0x007) == 0x08,
                "同一次 Fx55 的后半段被掩码绕回 0x000..0x007");
}

void checkBcd() {
    // Fx33：把 Vx 拆成三位十进制。
    // ★ 经典错法是中间那一位写 v / 10 而不是 (v / 10) % 10，
    //   于是 255 变成 2, 25, 5。单看 9 以内的小数字查不出来，
    //   所以这里专门挑 255 / 137 / 100 这种。
    struct Case {
        std::uint8_t value;
        std::uint8_t hundreds;
        std::uint8_t tens;
        std::uint8_t ones;
    };
    const Case cases[] = {
        {255, 2, 5, 5}, {137, 1, 3, 7}, {100, 1, 0, 0},
        {99, 0, 9, 9},  {9, 0, 0, 9},   {0, 0, 0, 0},
    };

    bool allOk = true;
    std::string firstBad;

    for (std::size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        const Case& one = cases[k];
        Machine machine(Quirks::modern(), 1);

        const std::uint16_t program[] = {
            0xA300,                                              // LD I, 0x300
            static_cast<std::uint16_t>(0x6000 | one.value),       // LD V0, nn
            0xF033,                                              // LD B, V0
        };
        std::uint16_t address = 0x200;
        for (std::size_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i) {
            machine.poke(address, static_cast<std::uint8_t>(program[i] >> 8));
            machine.poke(static_cast<std::uint16_t>(address + 1),
                         static_cast<std::uint8_t>(program[i] & 0xFF));
            address = static_cast<std::uint16_t>(address + 2);
        }

        for (int i = 0; i < 3; ++i) {
            machine.step();
        }

        const bool ok = machine.memory(0x300) == one.hundreds &&
                        machine.memory(0x301) == one.tens &&
                        machine.memory(0x302) == one.ones;
        if (!ok) {
            allOk = false;
            if (firstBad.empty()) {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer),
                              "%d -> 期望 %d%d%d 实际 %d%d%d", one.value,
                              one.hundreds, one.tens, one.ones,
                              machine.memory(0x300), machine.memory(0x301),
                              machine.memory(0x302));
                firstBad = buffer;
            }
        }
    }

    reportCheck(allOk, firstBad.empty()
                           ? std::string("Fx33 十进制拆分 6 个用例全对")
                           : "Fx33 拆分错了：" + firstBad);
}

void checkTimers() {
    // 定时器不能下溢。uint8_t 减到 0 再减就是 255，
    // 于是延迟定时器"永生"，表现出来是游戏按了键没反应。
    Machine machine(Quirks::modern(), 1);
    for (int k = 0; k < 300; ++k) {
        machine.tickTimers();
    }
    reportCheck(machine.delayTimer() == 0 && machine.soundTimer() == 0,
                "多余的 tickTimers 不会让定时器下溢到 255");

    // 用 Fx15 设一个延迟值，再 tick 三次，应该刚好到 0。
    Machine timed(Quirks::modern(), 1);
    timed.poke(0x200, 0x60);  // LD V0, 0x02
    timed.poke(0x201, 0x02);
    timed.poke(0x202, 0xF0);  // LD DT, V0
    timed.poke(0x203, 0x15);
    timed.step();
    timed.step();
    reportCheck(timed.delayTimer() == 2, "Fx15 把 2 装进延迟定时器");

    timed.tickTimers();
    timed.tickTimers();
    const bool atZero = timed.delayTimer() == 0;
    timed.tickTimers();
    reportCheck(atZero && timed.delayTimer() == 0,
                "延迟定时器正好在两次 tick 后归零，且不会变 255");
}

void checkStackAndPcWrap() {
    // 栈是 16 层，用 4 位下标。压满 16 次之后下标绕回 0，
    // 也就是第 17 次会覆盖最老的那一层。这里只验证绕回本身。
    Machine machine(Quirks::modern(), 1);
    machine.poke(0x200, 0x22);  // CALL 0x200 —— 自己调自己
    machine.poke(0x201, 0x00);

    for (int k = 0; k < 16; ++k) {
        machine.step();
    }
    reportCheck(machine.sp() == 0 && machine.stackAt(0) == 0x202,
                "压栈 16 次之后栈指针绕回 0（4 位下标）");

    // 在 0xFFE 处执行指令：PC 会从 0x1000 掩码回 0x000。
    // 不掩的话 PC 就成了 0x1000，下一步 peekOpcode 会读
    // memory_[0x1000]——4096 个元素的数组**外面**。
    //
    // ★ 这里要算**两次**前进，我第一次写就漏了一次：
    //     step() 一进来就无条件 advancePc(2)（见【契约二】），
    //     然后跳过类指令再 advancePc(2)。所以
    //         普通指令    0xFFE -> 0x000   （只前进一次）
    //         会跳过的指令 0xFFE -> 0x002   （前进两次）
    //   两种都要查：只看一种的话，"跳过时用 pc_ += 4 而不是
    //   advancePc(2)"这个错法会漏过去（0xFFE + 4 掩码后是 0x002，
    //   恰好和正确结果一样——所以还得靠非跳过的那个用例把
    //   "越界后掩码回 0"这件事本身钉住）。
    Machine plain(Quirks::modern(), 1);
    plain.poke(0x200, 0x1F);  // JP 0xFFE
    plain.poke(0x201, 0xFE);
    plain.poke(0xFFE, 0x70);  // ADD V0, 0x01 —— 不改变控制流
    plain.poke(0xFFF, 0x01);
    plain.step();  // 跳到 0xFFE
    plain.step();  // 执行加法，PC 从 0x1000 掩码回 0x000
    const std::uint16_t plainPc = plain.pc();

    Machine edge(Quirks::modern(), 1);
    edge.poke(0x200, 0x1F);  // JP 0xFFE
    edge.poke(0x201, 0xFE);
    edge.poke(0xFFE, 0x30);  // SE V0, 0x00 —— V0 是 0，会跳过
    edge.poke(0xFFF, 0x00);
    edge.step();  // 跳到 0xFFE
    edge.step();  // 跳过，PC 再前进一次
    const std::uint16_t skipPc = edge.pc();

    // 标签里带上实际值：这两个检查一旦红了，
    // 输出直接告诉你 PC 停在哪，不用再去加打印重跑一遍。
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "0xFFE 处普通指令：PC 从 0x1000 掩码回 0x000（实际 %s）",
                  hex3(plainPc).c_str());
    reportCheck(plainPc == 0x000, buffer);

    std::snprintf(buffer, sizeof(buffer),
                  "0xFFE 处跳过：再前进一次到 0x002（实际 %s）",
                  hex3(skipPc).c_str());
    reportCheck(skipPc == 0x002, buffer);
}

// ---- 实时路径 ----
//
// ★ 这一组查的是 Clock::advanceByMs —— 也就是 ember play 每一帧
//   走的那条路。它在这之前**一行测试都没有**：唯一调用它的是
//   终端播放循环，而那个循环要真终端、真键盘、真时钟才能跑，
//   没法自动化。于是"游戏速度对不对"这件事一直没人验过。
//
//   好在这条路径**本身是确定性的**——时间是从参数进来的，
//   不是从钟里读出来的。所以它能在这里被逐条钉住。
//   这正是 clock.hpp 那句"Clock 不知道墙钟"换来的东西：
//   把时间变成参数，实时路径就也变成了可测的。
void checkClockRealtime() {
    // 1. 钳制。拖窗口十秒会一次性报上来 10000ms，
    //    不钳的话这一帧要补 7000 条指令，窗口在这期间
    //    不处理任何消息，看起来就是卡死（死亡螺旋）。
    //    钳到 MAX_CATCHUP_MS = 100ms -> 70 条。
    {
        Machine machine(Quirks::modern(), 1);
        Clock clock(700, 60);
        const long long ran = clock.advanceByMs(machine, 100000.0);
        reportCheck(ran == 70, "advanceByMs 把 100000ms 钳到 100ms（= 70 条）");
    }

    // 负数当成 0：时钟回拨（对时、夏令时切换）时 elapsedMs
    // 会是负的，不挡掉的话 credit 会往回退。
    {
        Machine machine(Quirks::modern(), 1);
        Clock clock(700, 60);
        const long long ran = clock.advanceByMs(machine, -500.0);
        reportCheck(ran == 0, "advanceByMs 收到负数跑 0 条（时钟回拨）");
    }

    // 2. 一帧 10ms、跑 100 帧 = 正好 1 秒 = 正好 700 条。
    {
        Machine machine(Quirks::modern(), 1);
        Clock clock(700, 60);
        long long total = 0;
        for (int k = 0; k < 100; ++k) {
            total += clock.advanceByMs(machine, 10.0);
        }
        reportCheck(total == 700, "100 帧 x 10ms 一共 700 条指令（正好一秒）");
    }

    // 3. ★ 零头不能丢。这是这条路径上后果最隐蔽的一处：
    //    每帧 1ms、700ips 的话每帧只欠 0.7 条，够不到 1 条。
    //    如果 due <= 0 那条分支里顺手把 instructionCredit_ 清了，
    //    每帧就都从 0.7 重新开始，于是**一条指令都不会跑**。
    //    现象是"帧率越高跑得越慢"，最后游戏完全不动——
    //    而帧率是用户改不了的（由渲染速度决定），所以极难归因。
    {
        Machine machine(Quirks::modern(), 1);
        Clock clock(700, 60);
        long long total = 0;
        for (int k = 0; k < 10; ++k) {
            total += clock.advanceByMs(machine, 1.0);
        }
        // 理想是 7.0 条。0.7 在二进制里是无限循环小数，
        // 累加十次可能落在 6.999...，所以 6 和 7 都收。
        // 真正要钉住的是**不能是 0**——那才是零头被清掉的样子。
        reportCheck(total == 6 || total == 7,
                    "每帧 1ms 连跑 10 帧仍然跑了 6-7 条（零头没被清掉）");
    }

    // 4. 长时间不漂移：1000 帧 x 1ms = 1 秒，应该还是 700 条左右。
    //    这一条抓的是粗错（零头被清 -> 0；零头忘了减 -> 十几万），
    //    抓不到"取整方式"这种细错——四舍五入的平均值同样是 0.7/帧，
    //    只是每帧抖一下。所以别把它当成取整方式的判据。
    {
        Machine machine(Quirks::modern(), 1);
        Clock clock(700, 60);
        long long total = 0;
        for (int k = 0; k < 1000; ++k) {
            total += clock.advanceByMs(machine, 1.0);
        }
        reportCheck(total >= 695 && total <= 705,
                    "1000 帧 x 1ms 一共 695-705 条（长期不漂移）");
    }

    // 5. 定时器在实时路径上照样走，而且走的次数由**真实时间**决定。
    //    这一段是"游戏速度不随帧率变"的直接证据：
    //    同样是 1 秒，分成 100 帧还是一次报 1000ms（被钳成 100ms），
    //    前者该 tick 60 次，后者只该 tick 6 次。
    {
        Machine machine(Quirks::modern(), 1);
        machine.poke(0x200, 0x60);  // LD V0, 60
        machine.poke(0x201, 0x3C);
        machine.poke(0x202, 0xF0);  // LD DT, V0
        machine.poke(0x203, 0x15);

        Clock clock(600, 60);
        clock.advanceInstructions(machine, 2);  // 把 60 装进延迟定时器
        const int loaded = machine.delayTimer();

        // 半秒 = 300 条指令 = 30 次 tick
        for (int k = 0; k < 50; ++k) {
            clock.advanceByMs(machine, 10.0);
        }
        const int halfValue = machine.delayTimer();

        // 再半秒 = 另外 30 次 tick，定时器归零
        for (int k = 0; k < 50; ++k) {
            clock.advanceByMs(machine, 10.0);
        }
        const int fullValue = machine.delayTimer();

        // 把三个时刻的值都印出来：一旦红了，"是哪一段不对"
        // 不用再插打印重跑一遍。
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer),
                      "实时路径半秒 tick 30 次、一秒归零（装=%d 半秒后=%d 一秒后=%d）",
                      loaded, halfValue, fullValue);
        reportCheck(loaded == 60 && halfValue == 30 && fullValue == 0, buffer);
    }
}

void checkBlocking() {
    // Fx0A：没按键时返回 WAIT_KEY，PC 退回原处，
    // 而且**照样消耗一个周期**（定时器要在等待期间继续走）。
    Machine machine(Quirks::modern(), 1);
    machine.poke(0x200, 0xF0);  // LD V0, K
    machine.poke(0x201, 0x0A);

    const StepStatus first = machine.step();
    reportCheck(first == StepStatus::WAIT_KEY && machine.pc() == 0x200,
                "Fx0A 没按键时 PC 退回原地，状态是 WAIT_KEY");
    reportCheck(machine.cycle() == 1 && machine.lastStep().cycle == 1,
                "阻塞的指令照样消耗一个周期");

    // 按下键之后应该走通，并且拿到的是那个键的编号。
    machine.setKey(0x0B, true);
    const StepStatus second = machine.step();
    reportCheck(second == StepStatus::OK && machine.v(0) == 0x0B,
                "按下 B 之后 Fx0A 返回 0x0B");
    reportCheck(machine.pc() == 0x202, "等到了按键，PC 才前进");

    // 同时按多个键时取编号最小的那个。这个行为文档里没有，
    // 是这里定死的——不定死的话测试写不出确定的期望值。
    Machine multi(Quirks::modern(), 1);
    multi.poke(0x200, 0xF0);
    multi.poke(0x201, 0x0A);
    multi.setKey(0x07, true);
    multi.setKey(0x03, true);
    multi.setKey(0x0F, true);
    multi.step();
    reportCheck(multi.v(0) == 0x03, "同时按多个键取编号最小的");
}

void checkRunAll() {
    gPass = 0;
    gFail = 0;

    checkFontShape();
    checkQuirks();
    checkDecode();
    checkRandom();
    checkDisplay();
    checkMemoryMasking();
    checkBcd();
    checkTimers();
    checkClockRealtime();
    checkStackAndPcWrap();
    checkBlocking();

    std::printf("\n共 %d 项：%d 通过，%d 失败\n", gPass + gFail, gPass, gFail);
}

int commandSelftest(const Options& opts) {
    if (opts.fontOnly) {
        // 阶段 1 的里程碑就是这个：80 字节字模 + 一个固定的校验和。
        // 校验和的用处是——以后不小心碰坏了字模，它会立刻变。
        printFont();
        return 0;
    }

    if (opts.quirksTable) {
        printQuirksTable();
        return 0;
    }

    checkRunAll();
    return gFail == 0 ? 0 : 1;
}

}  // namespace

// ============================================================
//  入口
// ============================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setupConsole();

    // 用平台层拿参数，不用 argv。理由见 platform_win32.hpp：
    // argv 是本机代码页的，命令行上的中文路径会乱码。
    const std::vector<std::string> args = commandLineArgs();

    try {
        const Options opts = parseCommandLine(args);

        if (opts.help || opts.command.empty()) {
            printUsage();
            // 什么都没写就是"想知道怎么用"，这不是错误。
            // 但退出码给 2，因为脚本里 `ember` 打错了字应该能被发现。
            return opts.help ? 0 : 2;
        }

        if (opts.command == "run") {
            return commandRun(opts);
        }
        if (opts.command == "play") {
            return commandPlay(opts);
        }
        if (opts.command == "screen") {
            return commandScreen(opts);
        }
        if (opts.command == "trace") {
            return commandTrace(opts);
        }
        if (opts.command == "dis") {
            return commandDis(opts);
        }
        if (opts.command == "asm") {
            return commandAsm(opts);
        }
        if (opts.command == "selftest") {
            return commandSelftest(opts);
        }

        // 命令名打错了要给出全部合法名字，而不是只说"不认识"。
        throw EmberError("不认识的子命令: \"" + opts.command +
                         "\"\n可用的有: play run screen trace dis asm selftest");
    } catch (const EmberError& error) {
        std::fprintf(stderr, "错误: %s\n", error.what());
        return 1;
    } catch (const std::exception& error) {
        // 走到这里说明是程序自己的问题（不是用户输错），
        // 所以退出码是 2 而不是 1。
        std::fprintf(stderr, "内部错误: %s\n", error.what());
        return 2;
    }
}
