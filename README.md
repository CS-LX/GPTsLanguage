# GPT 文风机器码的程序集设计与文风调研

## 直接玩一局 GPT 井字棋

下载并完整解压 [v2.0.0 Windows x64 主包](https://github.com/CS-LX/GPTsLanguage/releases/download/v2.0.0/GPTsLanguage-windows-x64.zip)，双击 `TicTacToing\TicTacToing.exe`，按 Enter 开始。两个人轮流输入行、英文逗号和列，例如 `1,2`。同目录的 `gptlua54.dll` 是项目自己的 C++ 虚拟机，移动游戏时请一起复制。

用十六进制编辑器打开同一个 EXE，跳到 `0x600`，就能读到它解释的英文反馈。四份 Lua 源码里的 1,110 项操作合成了 329 段。把落子计数那句里的 `one more` 改成等长的 `one less`，落子数就会倒着走，原本获胜的第五步也会继续请对手下棋。[游戏 DLL](dist/TicTacToing/TicTacToing.dll) 有相同的正文，并提供 Windows 原生导出 `gpt_run`。

主包根目录的 `gptc.exe` 可以把自己的 Lua 程序编译成 EXE 或 DLL，例如 `gptc compile main.lua -o app.exe`。把生成的程序和 `gptlua54.dll` 放在同一目录即可运行。依赖完整 Lua 标准库或 Lua C 扩展的项目可以用 `--runtime lua` 编译，并把兼容包内的两个 DLL 放在生成的程序旁。[可选 Lua 兼容包](https://github.com/CS-LX/GPTsLanguage/releases/download/v2.0.0/GPTsLanguage-lua-host-windows-x64.zip) · [编译命令、文法和支持范围](docs/independent-vm.md) · [游戏原作](https://github.com/la-zu-li/tic-tac-toing/tree/528eb90f4d7dec6ce463c8c2626fefdd1b9fd6fc)

此前发布的 [v1.0.0 Windows 版](https://github.com/CS-LX/GPTsLanguage/releases/tag/v1.0.0) 使用第一版英语文法。要用独立 VM 玩那一份，保留原版 EXE，换上本目录的新版 `gptlua54.dll` 即可。

## C/C++ 整数子集

GPT Assembly v1 已在 Windows x64 上通过验收。`sample.dll` 向 Windows 导出 `distance_u32` 和 `add_u32`，向 GPT 运行时提供 2,843 字节可直接阅读的英文指令。普通 C 程序能调用这两个函数；比较、分支、算术和返回都由正文决定。

## 看它怎样提前辩解

新样例 [caveats.c](examples/caveats.c) 里有一句 `return has_evidence && sources >= 2u;`。编译器把它写成七段英文。第二段先替第二个观点争取了一番发言权，然后才决定要不要听它说话。

> To be clear, the second point may be perfectly valid in its own right. It can be thoughtful. It can be nuanced. It can even be correct. But if the first premise is false, none of that changes the answer — record zero and resume at paragraph 7.

从项目根目录运行同一份 DLL，换一下第一个实参：

```powershell
.\dist\gptc.exe trace .\dist\caveats.dll worth_a_followup 0 99
.\dist\gptc.exe trace .\dist\caveats.dll worth_a_followup 1 99
```

第一条只执行第 1、2、7 段，返回 `0`。第二条读到第二个参数 `99`，七段全部执行，返回 `1`。同一 DLL 的 `enough_to_publish` 演示 `||`：第一个实参为真时，先认可你想听第二意见的心情，接着直接返回 `1`。

`trace` 从文件中读取原文，按解释器的实际执行顺序打印段号、操作后的栈和跳过的范围。短路句式使用英语文法 v2；普通算术样例继续输出文法 v1。两个版本都可通过 Windows 原生宿主调用。

## 在 Windows 上运行

从项目根目录执行。`inspect` 直接读取 DLL 文件中的原文，即使运行时未加载也能工作。最后一条命令用原生 Windows 接口调用同一个 DLL。

```powershell
.\dist\gptc.exe inspect .\dist\sample.dll
.\dist\gptc.exe compile .\examples\sample.c -o .\dist\demo.dll
.\dist\sample-host.exe .\dist\demo.dll distance_u32 9 4
```

构建工具本身需要 CMake、Ninja 与 x64 MinGW-w64。以下命令输出到 `build/bin`。验收使用 Python 3、GCC/G++ 及 Windows 自带的 .NET Framework 对照 DLL。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -V
```

示例源码见 [sample.c](examples/sample.c)，格式与完整指令文法见 [format-v1.md](docs/format-v1.md)，验收入口见 [acceptance.py](tests/acceptance.py)。

## 什么算第三类 DLL

在 Windows 的 PE 文件里，外层记录节和导出目录。原生程序按处理器的指令工作；典型 .NET 类库在 PE 中定义程序集元数据和 CIL 方法体，由 CLR 执行。普通 C# 类库的 public 方法也不自动成为 `GetProcAddress` 可见的原生导出。GPT 程序集沿用 PE 容器，自行定义函数如何登记、指令如何编码、谁来执行，以及读错文件时如何报告错误。这里的“第三类”指项目定义的程序集格式，其识别和执行由 GPT 工具链负责。[Microsoft PE/COFF 规范](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) · [.NET 程序集文件格式](https://learn.microsoft.com/en-us/dotnet/standard/assembly/file-format) · [ECMA-335](https://ecma-international.org/publications-and-standards/standards/ecma-335/) · [GetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)

对 Windows，`sample.dll` 是有原生导出的合法 PE；对 GPT 运行时，它是一份能独立识别、校验和执行的程序集。这两种身份可以同时成立。Windows 认识导出函数的地址，GPT 运行时认识函数体里的每一段英文。任何人仅凭格式规范和该文件，都应能找出函数、读出正文并复现运算。

### 文件里必须能读到什么

**程序集头和函数表。** `.gptmeta` 以 `GPTASM\r\n` 的实际八个字节开头，随后是格式版本、英语文法版本和方法表。每条记录包含方法 ID、函数名、签名，以及方法在 `.gptil` 中的字节范围。两节均为只读数据，CLR 数据目录为空。

**有执行含义的英文。** 每种合法句式规定操作、操作数和下一段的位置。换行与标点按文法处理，条件句控制跳转，正文中的参数名或编号决定取哪个值。段落以未压缩的 UTF-8 原文落盘，用支持 UTF-8 的十六进制查看器就能直接读到。函数的比较、运算、分支和返回都以这些段落为唯一依据。运行时可以在内存中缓存解析结果。

**同一文件的原生导出。** 宿主调用 `GetProcAddress(sample, "distance_u32")` 时拿到一个符合 Windows x64 调用约定的函数地址。这个入口只向 `gptrt.dll` 传递当前模块、方法编号和实参，再把结果交还宿主。解释器在函数被调用时解析并执行正文。`sample.dll` 与运行时放在一起分发，直接调用无需用户另写加载器。导出的转接代码里不编译源函数的算术和分支，样例也不附带可替代正文的原生、CIL 或 Wasm 函数体。[Windows 的动态链接调用方式](https://learn.microsoft.com/en-us/windows/win32/dlls/using-run-time-dynamic-linking)

## v1 交付物

1. **C++ 编译器 `gptc.exe`。** 编译、静态检查及离线求值都由这个程序完成。编译器自行解析受支持的 C/C++ 语法、生成段落并写出 PE；发布版可以单独使用。
2. **`sample.dll`、独立运行时 `gptrt.dll` 和普通 C 宿主 `sample-host.exe`。** 样例大小为 6,656 字节，唯一的原生代码共 48 字节。每个导出各用 24 字节整理参数和方法 ID，再跳到 `gpt_dispatch`。运行时在每次调用时验证模块并解释对应段落。
3. **格式说明和验收脚本。** 格式规范给出方法表布局、完整的英文指令文法、栈语义、分支规则和原生调用约定。验收脚本从源码重编样例，同时用常规 GCC/G++ 生成独立参考程序对照。

### 验收结果

`distance_u32` 走两个分支，返回较大参数减去较小参数的结果。`add_u32` 同时验证第二个方法的导出、分派和无符号回绕。首个函数的完整源代码如下。

```c
#include <stdint.h>

uint32_t distance_u32(uint32_t a, uint32_t b)
{
    if (a >= b) return a - b;
    return b - a;
}
```

1. **从源码重编。** 只给 `gptc.exe` 源文件和 Windows 系统路径即可产出逐字节相同的 `sample.dll`。另编一份 C++ 源码，覆盖改名、嵌套分支、比较与表达式。五个导出在 820 次调用中与 GCC/G++ 的结果一致。
2. **检查文件。** 独立读取 PE 得到 `distance_u32` 的 2,108 字节正文和 `add_u32` 的 735 字节正文。它们与 `gptc inspect` 的输出一致。检查器拒绝普通原生 DLL、实际 .NET Framework 程序集、早期加法原型，以及格式标识、签名或转接入口损坏的副本。
3. **直接调用。** 普通 C 宿主用 `LoadLibraryW` 和 `GetProcAddress` 调用同一个 `distance_u32`，输入 `(9, 4)`、`(4, 9)`、`(4, 4)` 分别得到 `5`、`5`、`0`。
4. **改正文。** 只在副本第七段把 `decrease` 改成等长的 `increase`。同一宿主和运行时重新加载后，三个输入分别得到 `13`、`5`、`8`。后两个结果还验证了反向输入走另一条路径、相等输入走 `>=` 路径。两个文件在被改词之外逐字节相同。
5. **拒绝无效指令。** 把第七段改成未知操作词，或者把第四段的目的地改成第零段，静态检查器和已加载的运行时都会报出函数名与对应段号。修改不合法的源码重新编译时，已存在的目标 DLL 也保持完整。

**当前语言范围。** 一个模块支持多个函数，每个函数接受两个 `uint32_t` 并返回 `uint32_t`。源语言支持整数字面量、参数、加减、六种比较、括号、逻辑非 `!`、短路 `&&` 与 `||`、`if`、`else`、块和 `return`。循环、指针和任意外部调用超出当前子集。

## 早期原型验证了什么

早期 `examples/add.dll` 验证了改正文能改变结果。它由 Python 驱动构建，且每个 DLL 内嵌三段式解释器。以下材料保留原始调研和小样；v1 的编译器、独立运行时与验收结果见上文。

## 哪些写法让读者觉得有 GPT 味

**一行中间点。** 做网页设计的 Genjutsu 把连续用 `·` 串联标签的写法叫作 *middle-dot runs*。Pitch Unfairly 举的幻灯片例子更直观：`Fast · Simple · Secure`。三个词把一行填满，却没告诉读者产品有多快。原型把这股味道留给开场白。法国《世界报》报道过另一个更出圈的标点梗：英文长破折号 `—` 被叫作“ChatGPT 破折号”，连一直用它的作者也被误认过。[Genjutsu 界面实例](https://genjutsu.athevon.dev/docs/tells/) · [Pitch Unfairly 幻灯片实例](https://www.unfairly.ai/pitch/guides/ai-deck-tells/) · [法国《世界报》](https://www.lemonde.fr/en/m-le-mag/article/2026/04/19/when-dashes-give-away-chatgpt-usage_6752585_117.html)

**先虚构一个反对意见，再驳倒它。** Ekin Ilseven 把这种句子称为 *phantom-opponent contrast*。经理突然强调「要更聪明地工作」，顺手批判「只会埋头苦干」，可会议室里根本没人主张后者。Reddit 上一位自述有 25 年经验的广告文案作者也抱怨，一次 500 词的 GPT 输出里，同一种「不只是……还……」出现了四次。原型里的第二个加数只想参与运算，却要先澄清自己的立场。[Ilseven 的文章](https://medium.com/@fractionalvortex/watch-out-for-the-phantom-opponent-contrast-42a20a5e399c) · [文案作者的原帖](https://www.reddit.com/r/ChatGPT/comments/1kna7jj/another_chatgpt_tick_not_just/)

**替每句话加免责条款。** OpenAI 开发者社区一位用户抱怨，GPT 动不动就补上“这并不意味着……”或“据此还不能断言……”。对方只好反复解释自己根本没下过那个结论。另一个人说，连园艺和音乐都聊出了被合规人员盘问的感觉。这类句式很适合写条件分支：将来可以让一整段保留意见负责一次真正的条件判断。[开发者社区讨论](https://community.openai.com/t/excessive-caveats-and-repetitive-disclaimers-make-chatgpt-conversations-exhausting/1410602)

**擅自安慰，顺便升华。** 一位 Reddit 用户说，自己只是问怎么烤软陶，GPT 却告诉他「你并不幼稚」。另一个帖子里，有人模仿 GPT 把普通问题拔高成“通往更深理解的大门”，随后提醒它：自己只问了 Markdown 语法。原型选了后一种味道，让一次函数返回也有一整段总结陈词。[软陶讨论](https://www.reddit.com/r/ChatGPT/comments/1r5eyj9/chatgpt_keeps_stating_youre_not_crazy_so_much_so/) · [Markdown 语法的吐槽](https://www.reddit.com/r/ChatGPT/comments/1kna7jj/another_chatgpt_tick_not_just/)

## 加法编译后长什么样

输入是一段普通 C。它的两个参数和返回值在此目标平台上都是 32 位无符号整数。

```c
unsigned int add(unsigned int left, unsigned int right)
{
    return left + right;
}
```

下面是 Windows 样件 `add.dll` 的 `.gptil` 节中完整的三段 UTF-8 指令，共 569 字节，不含末尾的零字节。

> Context · Inputs · Alignment. Before we go any further, it helps to establish a shared starting point. For the purposes of this discussion, the running total starts with argument one.

> To be clear, acknowledging a second contribution does not invalidate the first. With that distinction in mind — and this is where the nuance matters — the practical takeaway is to increase the running total by argument two.

> Ultimately, the caller came here with a specific question. The most helpful thing we can do at this point is close the loop and return the running total.

对输入 `(9, 4)`，第一段把累计值设为 `9`，第二段加上 `4`，第三段把 `13` 交还给调用方。解释器按固定语法识别整段。`increase` 和 `decrease` 恰好都是八个 ASCII 字节，因此改写这个样件时，节长度和后续数据的位置都不动。

## Windows 怎样调用段落

**文件。** 外层是标准 x64 PE32+ DLL。导出表里有 `add`，`.text` 里有执行段落的原生解释器，只读的 `.gptil` 里装着这个函数的程序。按此设计，CPU 执行解释器的机器码，解释器执行段落指令。所以“GPT 文风机器码”这个名字，从虚拟机的角度也说得通。典型 .NET 程序集则在 PE 中存放 CIL 与 CLR 使用的元数据。[Microsoft PE/COFF 规范](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) · [.NET 程序集文件格式](https://learn.microsoft.com/en-us/dotnet/standard/assembly/file-format)

**调用。** x64 C 宿主用 `LoadLibraryW` 加载 DLL，用 `GetProcAddress` 找到 `add`，按 `uint32_t (uint32_t, uint32_t)` 签名调用。导出地址指向原生解释器入口。段落求值在调用时发生，离线即可执行。这个样件把解释器编进了 DLL，调用方可以照常使用 Windows 的动态链接接口。[Microsoft 动态加载示例](https://learn.microsoft.com/en-us/windows/win32/dlls/using-run-time-dynamic-linking)

**编译。** 小样用 MinGW-w64 的 GCC/G++ 解析 C 和 C++，再从 GCC 的文本中间表示 GIMPLE 生成英文段落。打包时只链接解释器和段落数据。前端临时生成的原生目标文件不参与最终链接，所以宿主拿到的 `13` 必须经过段落求值。

## 原型能编译什么

本机验证环境为 Windows x64、MinGW-w64 GCC/G++ 16.1.0 和 Python 3.12。当前每个 DLL 支持一个导出函数，签名固定为两个 32 位无符号参数，返回同类型的和或差。实测覆盖了 C 的加法、C++ 的减法、交换参数、重复使用参数及无符号回绕。两个参数相乘会在生成 DLL 之前报错。当前编译器匹配 GCC 的 GIMPLE 文本布局，换 GCC 版本要重新验证。

从项目根目录可重建样件，再用原生宿主调用。三个命令在 PowerShell 中执行，要求 MinGW-w64 的 `gcc` 和 Python 已加入 PATH。

```powershell
python scripts/compile_prose.py examples/add.c examples/add.dll
gcc -municode examples/call.c -o examples/call.exe
.\examples\call.exe (Resolve-Path examples/add.dll).Path
```

## 后续可扩展的语言前端

莎士比亚编程语言让角色和台词承担变量与操作的职责。v1 的整数距离函数用了同一个思路：读者看到一段提前辩解的转折时，解释器也确实会据此选择分支。[莎士比亚编程语言的说明和示例](https://shakespearelang.com/)

等整数、比较和控制流的规则站稳后，可以用 WebAssembly 的整数子集接入更多前端。先让 C 和 Rust 各编译一个相同的纯函数，再把 Wasm 指令翻成段落，以两端输出一致为验收条件。难点会落在各语言真实需要的内存、导入函数和运行库上，尤其是字符串或带堆分配的代码。[WebAssembly 核心规范](https://www.w3.org/TR/wasm-core/) · [Rust 的 wasm32 目标说明](https://doc.rust-lang.org/rustc/platform-support/wasm32-unknown-unknown.html)

[Lua 编译与文件格式](docs/lua54.md) · [C/C++ 整数子集格式](docs/format-v1.md)
