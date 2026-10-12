// ============================================================
//  PlatformWin32 —— 见 platform_win32.hpp
//
//  【关于 <windows.h> 的位置】它是本文件里第一个 include，
//  但必须排在**所有项目头文件之后**——因为 <windows.h> 会把
//  TRUE / FALSE / NULL / min / max 定义成宏，谁在它后面
//  写 #include "machine.hpp" 谁就被污染。
//  Nova 和 Acorn 都在这个坑上留过记录，这里照旧。
//
//  【注意】这个文件里的东西分两层：
//
//    无条件编译的：setupConsole / commandLineArgs
//      —— 这两个和"有没有窗口"无关。headless 构建**仍然是 Windows 程序**，
//         仍然需要把控制台切到 UTF-8、仍然需要正确地取命令行参数。
//         把"headless"理解成"不是 Windows"是个常见误解。
//
//    #ifdef EMBER_GUI 的：runWithWindow
//      —— 只有这个才真的和窗口有关。
// ============================================================

// ★ 这一行必须在**任何**头文件之前，包括下面这个自己的头文件。
//
//   TDM-GCC 9.2.0 默认的 _WIN32_WINNT 比 Vista 还老，于是
//   <windows.h> 里根本**不声明** GetTickCount64（以及一批
//   别的 2007 年之后的 API）。报错是"has not been declared"，
//   看起来像是函数名打错了或者少了库，其实只是版本门槛。
//
//   0x0600 就是 Vista。这个项目跑在 Windows 10 上，
//   声明出来不会有任何兼容性问题。
#define _WIN32_WINNT 0x0600

#include "platform_win32.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32

// <windows.h> 必须最先。
#include <windows.h>
// <shellapi.h> 必须在 <windows.h> 之后——它依赖 windows.h 里的类型。
// Acorn 的 README 里记过这个顺序问题。
#include <shellapi.h>

#else

#include <cstdlib>

#endif

#include "machine.hpp"

// ============================================================
//  控制台编码
// ============================================================

#ifdef _WIN32

void setupConsole() {
    // 输出的中文别乱码，输入的中文也别乱码。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
}

#else

void setupConsole() {
    // 其他平台上终端本来就是 UTF-8，不用管。
    // 但函数必须存在，因为 main.cpp 无条件调它——
    // 让调用方去 #ifdef 会把平台细节漏进 main.cpp。
}

#endif

// ============================================================
//  命令行参数
// ============================================================

#ifdef _WIN32

std::vector<std::string> commandLineArgs() {
    // ★ 为什么要绕这一圈？为什么不直接用 main 的 argv？
    //
    //   main(int argc, char** argv) 里的 argv 是**系统 ANSI 代码页**的，
    //   这台机器上是 GBK。而程序内部一路处理 UTF-8。
    //   于是命令行上传一个中文路径：
    //
    //       命令行参数 "D:\我的ROM\游戏.ch8"
    //         -> 系统给的 argv 已经是本机代码页的字节
    //         -> 我们把它当 UTF-8 用 -> 乱码 -> 文件找不到
    //
    //   而且它**不一定报错**！乱码如果碰巧解成一个合法文件名，
    //   你打开的是完全不相干的文件。
    //
    //   解法是绕开 argv：直接问系统要**宽字符**版本的原生命令行
    //   (GetCommandLineW)，用 Windows 的规则切开
    //   (CommandLineToArgvW)，再自己转成 UTF-8。
    //   全程不经过 ANSI 代码页，中文路径就对了。
    //
    //   【注意】CommandLineToArgvW 返回的是 LocalAlloc 出来的内存，
    //   必须 LocalFree 掉。它不在 C 运行时的堆上，free() 会崩。
    int wideArgc = 0;
    LPWSTR* wideArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wideArgc);
    if (wideArgv == nullptr) {
        return {};
    }

    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(wideArgc));

    for (int k = 0; k < wideArgc; ++k) {
        // 宽字符 -> UTF-8 的两段式调用：
        // 第一遍传 nullptr 问出需要多少字节，第二遍才真的转换。
        const int needed = ::WideCharToMultiByte(
            CP_UTF8, 0, wideArgv[k], -1, nullptr, 0, nullptr, nullptr);

        if (needed <= 0) {
            result.emplace_back();
            continue;
        }

        // needed 把结尾的 '\0' 也算进去了，所以下面转换完之后
        // 字符串里会多一个 '\0'。std::string 用 (ptr, len) 构造时
        // 要减去它，否则字符串里藏着一个空字符，
        // 打印出来看着对，比长度就错了。
        std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wideArgv[k], -1, &utf8[0], needed,
                              nullptr, nullptr);
        result.push_back(std::move(utf8));
    }

    ::LocalFree(wideArgv);
    return result;
}

#else

std::vector<std::string> commandLineArgs() {
    // 非 Windows 平台直接读 /proc/self/cmdline 更原汁原味，
    // 但那边路径本来就是字节串，main 的 argv 直接能用。
    // 这个项目只在 Windows 上跑，所以这里留一个空实现，
    // 让代码能编过、能被看懂，而不是假装支持。
    return {};
}

#endif

// ============================================================
//  终端游戏模式（按键 / 计时 / ANSI）
// ============================================================
//
// 【和上面两节的区别】上面两节（控制台编码、命令行参数）在
// headless 构建里也照样编译。这一节不是——它只在**真的要在
// 终端里玩游戏**时才用得上，而且它**只在 Windows 上有意义**。
// 不加任何宏，因为它在两种构建里都该存在：终端版不需要窗口，
// 阶段 7 的窗口版是另外一条路（GUI 宏只管那一条）。

#ifdef _WIN32

namespace {

// ---- 保存下来的控制台状态 ----
//
// 这几个是文件级变量而不是局部变量，因为它们要在
// beginTerminalInput / endTerminalInput / Ctrl+C 处理器
// 三个地方共用，而这三个地方没有共同的调用栈。
//
// 全是 POD，没有构造顺序问题（这个文件里也没有别的全局对象）。
HANDLE gInput = INVALID_HANDLE_VALUE;
HANDLE gOutput = INVALID_HANDLE_VALUE;

bool  gInputSaved = false;
DWORD gSavedInputMode = 0;

// ANSI（VT）模式是我们开的吗？如果进来的时候就已经开着
// （比如用户用的是 Windows Terminal 的某些配置），
// 退出时就不该替人家关掉。只还原自己改过的东西。
bool  gAnsiOwned = false;
DWORD gSavedOutputMode = 0;

bool  gHandlerInstalled = false;

// enableTerminalAnsi 失败时，具体是为什么。见头文件里的说明。
std::string gAnsiFailureReason;

// 把输入模式还回去，并且把替代屏幕退掉。
// **幂等**：重复调用没关系。这一点是故意的——
// 正常退出路径和 Ctrl+C 处理器会各调一次，而正常退出时
// 也可能已经调过了。让"还原"这件事幂等，比让两条路径
// 互相知道对方做没做过要简单得多。
void restoreConsole() {
    if (gInputSaved) {
        ::SetConsoleMode(gInput, gSavedInputMode);
        gInputSaved = false;
    }

    if (gAnsiOwned) {
        ::SetConsoleMode(gOutput, gSavedOutputMode);
        gAnsiOwned = false;
    }
}

// Ctrl+C / 关窗口 / 注销的兜底。
//
// ★ 为什么要有它，明明我们已经把 ENABLE_PROCESSED_INPUT 关掉了？
//   关掉之后 Ctrl+C 确实变成一个普通按键、由我们的循环正常处理。
//   但还有几种情况**绕过**那个循环：
//     * 用户直接点窗口右上角关掉控制台
//     * 注销 / 关机
//     * 别的进程往这个控制台发 CTRL_C_EVENT
//   这些时候进程会被直接结束，控制台模式就留在"无回显"状态。
//   用户下次开终端敲什么都看不见，而且重启之前都不会好。
//
// ★ 返回值是 FALSE：我们只做清理，**不接管**这个信号。
//   返回 TRUE 的话进程会继续活着，而一个已经开始关机的
//   系统里留着一个不死的模拟器不是好事。
BOOL WINAPI emberCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        restoreConsole();

        // 用 WriteConsoleA 而不是 fputs：这个处理器跑在
        // 另一个线程上，而主线程可能正卡在 stdio 的锁里。
        // 走控制台 API 就绕开了整个 stdio。
        if (gOutput != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const char* leave = "\x1b[?25h\x1b[?1049l\x1b[0m";
            ::WriteConsoleA(gOutput, leave,
                            static_cast<DWORD>(std::strlen(leave)), &written,
                            nullptr);
        }
    }
    return FALSE;
}

// 虚拟键码 -> CHIP-8 键号。认不出来的返回 KEY_NONE。
//
// CHIP-8 的键盘是 4x4 的：
//
//      1 2 3 C
//      4 5 6 D
//      7 8 9 E
//      A 0 B F
//
// 而真实键盘是 QWERTY，所以要把左上角那块 4x4 映射过去：
//
//      1 2 3 4       ->   1 2 3 C
//      Q W E R       ->   4 5 6 D
//      A S D F       ->   7 8 9 E
//      Z X C V       ->   A 0 B F
//
// 这个布局是模拟器里的通用做法（叫"cosmac 布局"或"1234 布局"）。
// 选它是因为**手指的位置**和原始 CHIP-8 键盘的形状对得上，
// 而不是因为字母本身有什么含义——那个 4 上写着 C、
// C 上写着 B，纯粹是映射，不是密码。
constexpr int KEY_NONE = -2;

int mapKey(unsigned long virtualKey) {
    switch (virtualKey) {
        // 第一行
        case '1': return 0x1;
        case '2': return 0x2;
        case '3': return 0x3;
        case '4': return 0xC;
        // 第二行
        case 'Q': return 0x4;
        case 'W': return 0x5;
        case 'E': return 0x6;
        case 'R': return 0xD;
        // 第三行
        case 'A': return 0x7;
        case 'S': return 0x8;
        case 'D': return 0x9;
        case 'F': return 0xE;
        // 第四行
        case 'Z': return 0xA;
        case 'X': return 0x0;
        case 'C': return 0xB;
        case 'V': return 0xF;
        // 退出
        case VK_ESCAPE: return KEY_QUIT;
        default: return KEY_NONE;
    }
}

}  // namespace

bool enableTerminalAnsi() {
    gAnsiFailureReason.clear();

    gOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (gOutput == INVALID_HANDLE_VALUE || gOutput == nullptr) {
        gAnsiFailureReason = "拿不到标准输出的句柄";
        return false;
    }

    DWORD mode = 0;
    if (!::GetConsoleMode(gOutput, &mode)) {
        // ★ 这里必须**问清楚是哪种情况**，不能一律说"被重定向了"。
        //
        //   因为最常见的用法恰恰是"在 Git Bash 里跑"：
        //   MinTTY（Git Bash 的终端）**不是** Windows 控制台，
        //   它是拿一个隐藏控制台 + 管道模拟出来的 pty。
        //   于是 GetConsoleMode 照样失败，但原因和"输出重定向到文件"
        //   完全不是一回事——而且 MinTTY 对 ANSI 的支持好得很。
        //
        //   报错说"你的终端不支持 ANSI"，用户会去查终端设置；
        //   真相是他需要换个地方跑（cmd / PowerShell），
        //   或者用 winpty 包一层。报错指错方向
        //   比不报错还费时间。
        const DWORD type = ::GetFileType(gOutput);
        if (type == FILE_TYPE_PIPE) {
            gAnsiFailureReason =
                "标准输出是**管道**——要么输出被重定向了，"
                "要么这个终端不是 Windows 控制台"
                "（Git Bash / MinTTY 就是这种）";
        } else if (type == FILE_TYPE_DISK) {
            gAnsiFailureReason = "标准输出是**文件**——输出被重定向进了文件";
        } else {
            gAnsiFailureReason = "标准输出不是 Windows 控制台";
        }
        return false;
    }

    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        // 已经开着。不动它，也不记成"我们开的"。
        return true;
    }

    if (!::SetConsoleMode(gOutput, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        // 老系统上这一位可能不被支持。这不是致命错误——
        // 调用方会退到纯 ASCII 渲染。
        gAnsiFailureReason =
            "这个 Windows 版本不支持 ENABLE_VIRTUAL_TERMINAL_PROCESSING"
            "（Windows 10 1511 之后才有）";
        return false;
    }

    gSavedOutputMode = mode;
    gAnsiOwned = true;
    return true;
}

std::string terminalAnsiFailureReason() {
    if (gAnsiFailureReason.empty()) {
        return "没有记录到原因";
    }
    return gAnsiFailureReason;
}

int terminalColumns() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr ||
        !::GetConsoleScreenBufferInfo(out, &info)) {
        return 0;
    }
    // ★ 用 srWindow（可见窗口）而不是 dwSize（缓冲区）。
    //   缓冲区经常比窗口高得多（默认 300 行），
    //   拿它当"有多少行能画"会算出几百行，然后画面就滚了。
    return info.srWindow.Right - info.srWindow.Left + 1;
}

int terminalRows() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr ||
        !::GetConsoleScreenBufferInfo(out, &info)) {
        return 0;
    }
    return info.srWindow.Bottom - info.srWindow.Top + 1;
}

double nowMs() {
    // GetTickCount64 从系统启动起算，单位毫秒，不会回绕
    // （32 位的 GetTickCount 49.7 天绕一次，绕的那一下
    //   时间差会变成负数，帧率就疯了）。
    return static_cast<double>(::GetTickCount64());
}

void sleepMs(int ms) {
    if (ms > 0) {
        ::Sleep(static_cast<DWORD>(ms));
    }
}

bool beginTerminalInput() {
    gInput = ::GetStdHandle(STD_INPUT_HANDLE);
    if (gInput == INVALID_HANDLE_VALUE || gInput == nullptr) {
        return false;
    }

    DWORD mode = 0;
    // ★ 这一句就是"能不能玩"的判据。失败 = 标准输入是文件或管道，
    //   键盘根本不在这个进程手里。返回 false 让调用方拒绝启动，
    //   比进了一个收不到按键的游戏要好得多。
    if (!::GetConsoleMode(gInput, &mode)) {
        return false;
    }

    gSavedInputMode = mode;
    gInputSaved = true;

    // ★ 兜底处理器要在**改模式之前**装上。
    //   反过来的话，"改完模式"到"装上处理器"之间那个窗口里
    //   用户按一下 Ctrl+C，进程直接死，模式就留在改过的状态。
    //   窗口只有几微秒，但没有任何理由把它留着。
    if (!gHandlerInstalled) {
        ::SetConsoleCtrlHandler(emberCtrlHandler, TRUE);
        gHandlerInstalled = true;
    }

    // ★ 一次清掉四位，每一位都对应一个具体的坑：
    //
    //   ENABLE_LINE_INPUT    不清的话要按回车才送到程序，
    //                        游戏里就是"按了半天没反应，
    //                        一按回车刚才那堆全涌进来"。
    //   ENABLE_ECHO_INPUT    不清的话按键会打印在屏幕上，
    //                        把画面顶得七零八落。
    //   ENABLE_PROCESSED_INPUT 不清的话 Ctrl+C 被系统吃掉
    //                        变成信号，程序直接死，来不及
    //                        还原控制台。
    //   ENABLE_MOUSE_INPUT   清掉，鼠标事件就别送来了。
    //                        （送来也能忽略，但少一类事件
    //                         就少一次分支。）
    DWORD next = mode;
    next &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
              ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT);

    // ★ QuickEdit 是"扩展标志"那一组里的，而 Windows 要求
    //   改这一组之前必须先置上 ENABLE_EXTENDED_FLAGS 这一位，
    //   否则设置会被**静默忽略**——不报错，就是不生效，
    //   然后你去查为什么点一下鼠标程序就卡住。
    next |= ENABLE_EXTENDED_FLAGS;
    next &= ~ENABLE_QUICK_EDIT_MODE;

    if (!::SetConsoleMode(gInput, next)) {
        // 设不上就退回原样，别留一个半改的状态。
        ::SetConsoleMode(gInput, gSavedInputMode);
        gInputSaved = false;
        return false;
    }

    return true;
}

void endTerminalInput() {
    restoreConsole();

    if (gHandlerInstalled) {
        ::SetConsoleCtrlHandler(emberCtrlHandler, FALSE);
        gHandlerInstalled = false;
    }
}

std::vector<KeyEvent> pollKeyEvents() {
    std::vector<KeyEvent> events;

    if (gInput == INVALID_HANDLE_VALUE || gInput == nullptr) {
        return events;
    }

    // 一次读一条，直到队列空。用一个上限兜底：
    // 万一时钟/队列出了问题，"取空"这个条件永远不成立的话，
    // 这里会变成一个死循环，而现象是"游戏完全不刷新"——
    // 又是那种要查半天的症状。上限到了就下一帧接着取。
    for (int guard = 0; guard < 256; ++guard) {
        DWORD pending = 0;
        if (!::GetNumberOfConsoleInputEvents(gInput, &pending) || pending == 0) {
            break;
        }

        INPUT_RECORD record;
        DWORD read = 0;
        if (!::ReadConsoleInputW(gInput, &record, 1, &read) || read == 0) {
            break;
        }

        // ★ 不是按键的**也要读掉**。不读的话它会永远躺在队列里，
        //   于是 GetNumberOfConsoleInputEvents 永远大于 0，
        //   上面的循环就永远退不出来。窗口大小变化、
        //   焦点变化都会产生这种事件，所以这不是理论问题。
        if (record.EventType != KEY_EVENT) {
            continue;
        }

        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;

        // ★ Ctrl+C 单独处理成"退出"，不让它落到按键映射上。
        //
        //   我们在 beginTerminalInput 里关掉了 ENABLE_PROCESSED_INPUT，
        //   所以 Ctrl+C 不再产生信号，而是变成一条普通的按键记录：
        //   虚拟键码 'C'。而 'C' 在映射表里是 CHIP-8 的 0xB ——
        //   于是用户想退出，游戏里却"按了一下 B 键"，
        //   他只会觉得"Ctrl+C 不管用"，然后去点窗口的叉。
        //
        //   这里看 dwControlKeyState 里有没有按下 Ctrl，
        //   有就翻译成 KEY_QUIT。Esc 仍然是主推的退出方式，
        //   这一条只是别让最顺手的那个键做错事。
        if (key.bKeyDown != FALSE) {
            const DWORD ctrl = key.dwControlKeyState;
            if ((ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0 &&
                key.wVirtualKeyCode == 'C') {
                KeyEvent quit;
                quit.key = KEY_QUIT;
                quit.down = true;
                events.push_back(quit);
                continue;
            }
        }

        // wVirtualKeyCode 而不是 uChar：uChar 受键盘布局和
        // 输入法影响（中文输入法开着的时候 uChar 经常是 0），
        // 而虚拟键码就是物理键位，正是我们想要的。
        const int mapped = mapKey(key.wVirtualKeyCode);
        if (mapped == KEY_NONE) {
            continue;
        }

        KeyEvent event;
        event.key = mapped;
        event.down = (key.bKeyDown != FALSE);
        events.push_back(event);
    }

    return events;
}

#else  // 不是 Windows

// 这一节是 Windows 专用：按键走的是控制台输入记录，
// 计时走的是 GetTickCount64，都不是可移植的东西。
//
// 别的平台上要跑终端版，得用 termios 关掉 ICANON/ECHO、
// 用 clock_gettime 计时——那是另一份实现，不是这里
// 写几个空函数就能"支持"的。所以 enableTerminalAnsi()
// 老实返回 false，commandRun 会据此拒绝启动并说明原因。
// 这比给一个编译得过、跑起来什么都不做的桩子好：
// 后者会让人以为终端版在这个平台上是好的。

bool enableTerminalAnsi() {
    // 注意这里不碰 gAnsiFailureReason —— 那个变量在 #ifdef _WIN32
    // 的匿名命名空间里，这个分支看不见它。
    return false;
}
std::string terminalAnsiFailureReason() {
    return "这个平台还没有终端播放层的实现（只写了 Windows 的）";
}
int terminalColumns() { return 0; }
int terminalRows() { return 0; }
double nowMs() { return 0.0; }
void sleepMs(int ms) { (void)ms; }
bool beginTerminalInput() { return false; }
void endTerminalInput() {}
std::vector<KeyEvent> pollKeyEvents() { return {}; }

#endif  // _WIN32

// ============================================================
//  窗口
// ============================================================

#ifdef EMBER_GUI

// 窗口这部分要等阶段 7 才写。
// 现在先留一个明确的"还没做"而不是一个假实现——
// 假实现会让人以为窗口能开，然后花时间查为什么是黑的。
int runWithWindow(Machine& machine, int ips, int hz, int scale) {
    (void)machine;
    (void)ips;
    (void)hz;
    (void)scale;

    std::fprintf(stderr,
                 "[还没做] 窗口模式是阶段 7 的内容。\n"
                 "         现在可以用：\n"
                 "           ember screen <rom>   把画面 dump 成字符画\n"
                 "           ember trace  <rom>   逐条打印指令\n");
    return 2;
}

#else  // 没有 EMBER_GUI：headless 构建

int runWithWindow(Machine& machine, int ips, int hz, int scale) {
    (void)machine;
    (void)ips;
    (void)hz;
    (void)scale;

    std::fprintf(stderr,
                 "[没有窗口] 这次构建不带窗口（build.bat headless）。\n"
                 "           重新跑一次 build.bat 就有了。\n"
                 "           不带窗口也能用：screen / trace / dis / selftest\n");
    return 2;
}

#endif  // EMBER_GUI
