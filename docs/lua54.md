# 把 Lua 5.4 编译成 GPT 语

用 Windows x64 打开 `dist/TicTacToing/TicTacToing.exe`，按 Enter，两个人轮流输入行和列，例如 `2,3`。想看执行的英文，直接用十六进制编辑器打开同一个 EXE，跳到文件偏移 `0x600`。从这里开始，连续 260,321 个字节都是 UTF-8 英文指令。

## 编译自己的程序

`gptc.exe` 是可单独使用的 C++ 编译器驱动。它静态链接 Lua 5.4.9 的官方 C 语法前端，用 C++ 把指令和操作数写成英文段落，再生成 Windows PE。选 `.exe` 生成控制台程序，选 `.dll` 生成标准签名为 `int(lua_State*)` 的 Lua 模块。启动产物时，把发行目录中的 `gptlua54.dll` 和 `lua54.dll` 放在宿主 EXE 旁边。游戏的两种产物各自含有完整正文。

在仓库根目录运行以下 PowerShell 命令即可重编游戏。第一行指定了入口脚本与模块根目录。

```powershell
$entry = '.\examples\tic-tac-toing\tic-tac-toing.lua'
$root = Split-Path $entry
.\dist\gptc.exe compile $entry --root $root -o .\dist\TicTacToing\TicTacToing.exe
.\dist\gptc.exe compile $entry --root $root -o .\dist\TicTacToing\TicTacToing.dll
.\dist\gptc.exe inspect .\dist\TicTacToing\TicTacToing.exe | Select-Object -First 12
```

单文件只需 `gptc compile main.lua -o app.exe`。传入 `--root` 后，编译器递归收集目录内的 `.lua`，以相对路径记录。需要一起交付的纯 Lua 依赖可通过可重复的 `--modules` 挂到相同的虚拟根目录，例如 `--modules C:\rocks\share\lua\5.4`。两个不同文件占用同一个虚拟路径会给出编译错误。建议指向待发布源码目录。选中的目录中每个 Lua 文件都要能通过 5.4 语法检查。

EXE 将 Windows 命令行转换为 UTF-8，程序路径放在 `arg[0]`，用户参数同时传给 `arg[1..n]` 和主块的 `...`。DLL 运行在调用方传入的 Lua 状态，接收模块加载器的参数并返回主块的结果。它默认导出 `luaopen_<模块名>`，也可用 `--symbol` 指定完整的 ASCII 导出名。应用程序可通过 `LoadLibraryW` 和 `GetProcAddress` 获取这个函数，把它压入已经初始化的 Lua 状态后用 `lua_pcall` 调用。普通 Lua 宿主直接用 `require` 即可。以下命令假定库和构建出的官方命令行宿主位于同一个目录。

```powershell
.\lua-host.exe -e 'arg={}; require("TicTacToing")'
```

## 支持范围

| 层面 | 当前行为 |
| --- | --- |
| 语言 | 使用 PUC-Lua 5.4.9 的解析器和 VM，覆盖全部 83 种指令，包括嵌套函数、闭包、元方法、协程、尾调用、可变参数和 `<close>`。 |
| 纯 Lua 包 | `require` 先查标准 preload，再查包内模块，最后交给宿主的文件及原生搜索器。包内支持相对的 `package.path` 模板及常见的 `name.lua`、`name/init.lua`。标准 `package.loaded` 缓存有效。多个 GPT 模块共享同一 Lua 状态时，也共享模块名空间。 |
| 加载和调试 | 标准库环境中的 `loadfile`、`dofile` 可按相对路径打开包内脚本，包括自定义环境。每次加载都产生新闭包。调试信息保留文件名、行号、局部变量名。运行时生成的 `load` 字符串以及外部脚本仍走 Lua 自身的加载器。 |
| 原生与资源 | Lua 5.4 x64 原生扩展经 `package.cpath` 加载，保持原生 DLL 形态。图片、声音以及用 `io.open` 读取的数据按普通文件随应用部署。LÖVE 等框架还需要各自的宿主和资源，本次交付针对标准 Lua 5.4 的 Windows x64 环境。LuaJIT 的 FFI、Lua 5.1、5.5 及其他 ABI 各有自己的实现要求。 |

## PE 文件里的东西

| 区域 | 游戏 EXE 中的内容 |
| --- | --- |
| `.text` | 24 字节，准备参数，计算本 PE 的基址，跳到独立 GPT 运行时。 |
| `.gptil` | 1,110 段英文，覆盖四个源文件里的 26 个函数原型。空行分隔段落。EXE 与 DLL 的这一节逐字节相同。 |
| `.gptmeta` | 标识 `GPTLUA54`、文法版本、入口、导出名、模块路径、原型结构、常量、捕获变量和调试信息。 |
| Windows 目录 | EXE 通过 PE 入口调用 `gpt_lua_start`。DLL 通过 `luaopen_TicTacToing` 调用 `gpt_lua_module`。两者各自只向 `gptlua54.dll` 导入一个函数，CLR 目录为空。 |

游戏里负责累计落子数的一段英文原样如下。

> Great question. The most straightforward way to honor that intent is to increase the register value by a signed immediate. For transparency, the operand ledger reads A 3 · B 3 · C 128 · selector 0. The takeaway? We can now move forward with confidence.

在这个句式中，寄存器 B 提供原值，寄存器 A 接收结果，C 以 127 为偏移表示有符号立即数。128 在这里就是加一。在测试副本里把这三个 ASCII 数字改为 127，计数便停在零。输入相同的五步棋，原件宣布 X 胜，修改件仍邀请 O 落子。回归测试还直接把另一个 DLL 的 `increase` 改为 `decrease`，确认运行结果随动词改变。

### 如何重建执行流

文件格式的 32 位字段均为小端。下表中的“字节串”先写一个 32 位长度，再写对应字节。元数据开头依次是八字节标识、32 位版本 1、入口路径字节串、导出名字节串和 32 位模块数。每个模块记录路径字节串和 32 位原型数。按官方序列化的先序顺序，每个原型写一个非代码片段字节串和 32 位指令数；最后有一个尾部非代码片段字节串。

编码时按官方原型布局剥离所有指令数组和 31 字节的二进制块头。`.gptil` 按相同次序每条指令写一段，再写两个 LF。动词短语选择操作，正文中的十进制数确定寄存器、常量索引、立即数或跳转距离。五种操作数布局及所有动词见 [lua_prose.cpp](../src/lua_prose.cpp)。

运行时先从段落还原每条指令，在内存中与常量及原型结构合并，再交给 Lua VM。读取器独立重新遍历原型，检查每个指令数组的位置和长度都与段落记录一致。编译器在落盘前还把从 PE 还原的每一个模块与官方编译器刚生成的块逐字节比较，包括调试信息。[包编码与读取](../src/lua_package.cpp) · [Lua 5.4 手册](https://www.lua.org/manual/5.4/)

随附的 `lua54.dll` 保留官方 C API，并增加一个仅供 GPT 块使用的加载函数，恢复源编译时同一模块内相同长字符串字面量共享对象的行为。宿主已经加载原版 `lua54.dll` 时，GPT 运行时会使用该库的标准二进制加载器，此时长字符串的指针身份与原版 `luac` 一致。

## 来源与复验

游戏源自 `la-zu-li/tic-tac-toing` 的提交 `528eb90`，按 MIT 许可证分发。移植时沿用了原有棋盘、胜负和再来一局的逻辑，调整了 Windows 首次清屏、缺少坐标时的输入处理、操作提示及命令行帮助。四个 Lua 文件均已入包。[游戏上游源码](https://github.com/la-zu-li/tic-tac-toing/tree/528eb90f4d7dec6ce463c8c2626fefdd1b9fd6fc)

仓库的 CTest 覆盖全部指令的操作数边界、生成 EXE 和 DLL、原生 Lua 模块、相同输入与官方解释器逐字节输出对照、二进制改字、损坏段落、编译失败后保留旧产物，以及旧版 C/C++ 样例。人工操作 Windows 控制台也已验证开局、逐手落子、连线、重玩提示和 Ctrl+Z 退出。[端到端测试](../tests/lua_acceptance.py)

另用官方 Lua 5.4.9 测试套件的 basic 模式（`_U=true`）验收。把 33 个上游 Lua 文件及入口全部编为一个 GPT EXE，从没有源码的目录运行，到达 `final OK !!!`。basic 会跳过平台专用、内部及部分高资源测试。复验时从官方页面下载并解压 `lua-5.4.9-tests.tar.gz`，SHA-256 为 `7d971845f545ffc09fbb3128a86b2c6524161c70d0fdf0154a16e8c00c343fca`，然后运行下列命令。[Lua 官方测试](https://www.lua.org/tests/)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGPT_LUA_TEST_SUITE=C:/src/lua-5.4.9-tests
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
```

生态兼容另用两个上游库做了联测。用 `--modules` 把纯 Lua 游戏工具库 lume 编入正文，在同一个 EXE 里 `require("lfs")` 加载以官方头文件编译的 LuaFileSystem 1.9.0 原生 DLL。测试用 lume 对 1、2、3 的平方求和，得到 14，再由 LuaFileSystem 读取该 EXE 的文件属性。[lume 固定版本](https://github.com/rxi/lume/tree/98847e7812cf28d3d64b289b03fad71dc704547d) · [LuaFileSystem 固定版本](https://github.com/lunarmodules/luafilesystem/tree/146ab458e821cd7617892099fe8e6e399b46185c)

`third_party/lua54` 收录官方 Lua 5.4.9 源文件与 MIT 许可证。源档 SHA-256 为 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`。[官方发行档](https://www.lua.org/ftp/)
