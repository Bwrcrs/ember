# 第三方测试 ROM（不提交进仓库）

这个目录**故意是空的**，`.gitignore` 里也**故意忽略了它**。

## 为什么

`Timendus/chip8-test-suite` 是 **GPLv3**。把那些 `.ch8` 提交进这个仓库，
会让整个项目的分发沾上 GPL 条款。这个项目是 MIT，作者从没打算做那个决定。

所以：**检查能力留在这台机器上，许可证决定不做**。
文件在的时候 `run_tests.ps1` 会跑它们；不在就整组跳过（打印 `[SKIP]`）。

## 怎么装

去 https://github.com/Timendus/chip8-test-suite 下载 ROM，
把 `.ch8` 文件放进这个目录。`run_tests.ps1` 找的是这几个名字：

| 文件 | 它验什么 |
|---|---|
| `1-chip8-logo.ch8` | 最基本的取指/绘图链路 |
| `2-ibm-logo.ch8` | 同上，图案更复杂 |
| `3-corax+.ch8` | **主要正确性判据**：全套指令，错了画叉 |
| `4-flags.ch8` | **主要正确性判据**：所有 8xy_ 的标志位 |
| `5-quirks.ch8` | 报告它**检测到**的行为，不是通过/失败 |
| `6-keypad.ch8` | 键盘（阶段 6 之后才有意义） |
| `7-beep.ch8` | 蜂鸣器（阶段 7 之后才有意义） |

## 为什么这几个 ROM 的地位和黄金文件不一样

`tests\run\*.screen` 和 `*.trace` 是**回归测试**：它们记录模拟器**做过**什么，
`-Update` 会把当前行为（包括 bug）固化成"期望值"。

`3-corax+` 和 `4-flags` 是**正确性判据**：它们存在的全部意义就是
"你错了就画个叉"。这是这个项目里唯一两个不依赖"和上次一样"的判据
（另一个是 `run_tests.ps1` 里手写且永不重新生成的那些 invariant）。

## 3-corax+ 和 4-flags 只能靠人看

`run_tests.ps1` 对这两个只断言"跑完了"和"屏幕上画了东西"——
因为**自动判断一屏对勾和叉的区别**，要么写一堆脆弱的模板匹配，
要么就得假装看懂了。所以它们通过时打印的是：

```
[PASS] 3-corax+ ran and drew something (read the screen yourself)
```

括号里那句是字面意思：**自己看**。

```bat
ember screen ..\..\tests\external\3-corax+.ch8 --cycles 50000
```

## 5-quirks 要手工跑，因为它的输出不是通过/失败

```bat
ember run external\5-quirks.ch8 --poke 1FF=01 --quirks=vip
ember run external\5-quirks.ch8 --poke 1FF=02 --quirks=schip
```

它会**报告它检测到的**行为，所以要看的是"每一行是否和你选的目标自洽"。
