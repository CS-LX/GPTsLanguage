# 独立 GPT 虚拟机与反馈文法 v2

双击 `dist/TicTacToing/TicTacToing.exe`，同目录里的 `gptlua54.dll` 负责执行。这个文件名沿用了 v1 的 PE 导入约定，所以旧 EXE 也能用新 VM。游戏进程需要的项目文件只有这两个。

## 正文怎样参与计算

莎士比亚语言里，台上站着谁决定了台词中的“你”指谁，名词前面的形容词会让它代表的数值翻倍。于是舞台和修辞本身就能保存程序状态。我们把这个办法用在反馈文体里，前一句得出的值可以由下一句的 *that last point* 指代，原先反复出现的参数表也有了具体的叙事对象。[原作者的语言说明](https://krayon.github.io/shakespearelang/docs/shakespeare.html) · [可运行的示例剧本](https://shakespearelang.com/1.0/)

游戏 EXE 的第二段实际上决定了 Windows 上使用哪种清屏命令。

> My starting recommendation for the clear screen facility would be “clear”. There is one caveat worth spelling out. If the Windows environment check comes back positive, I'd use “cls” instead. That keeps the general advice useful while giving the exception somewhere sensible to live.

“起始建议”写入默认值。“有一个例外”引入谓词调用、条件分支和覆盖赋值。读到这一个段落，VM 得到六项有先后关系的操作。编译器在其他程序遇到同样的结构时也使用这条规则。模块引用可以写成一段资料综述，闭包声明可以写成一串后续问题，比较和跳转可以写成同一句限定条件。动作、顺序、变量和常量由确定的英语文法解析，编译和游玩都可以离线进行。[反馈文法与解析器](../src/feedback.cpp) · [基本句式](../src/feedback_rules.inc)

井字棋的 26 个函数原型包含 1,110 项操作，编成 329 段。正文从文件偏移 `0x600` 开始，约占 EXE 的 91%。想单独查看前几段，可执行：

```powershell
.\dist\gptc.exe inspect .\dist\TicTacToing\TicTacToing.exe | Select-Object -First 12
```

## 运行时做了什么

Windows 将控制权交给 PE 里的 24 字节转接入口。C++ 加载器读取 UTF-8 正文和只读的原型、常量、局部变量及捕获信息。解析器在装载时把反馈解析为内存中的操作节点，由独立解释循环推进程序计数器。第一版文法也有自己的解析入口，两种文法最后使用同一套 C++ 调用帧、值、表、闭包、垃圾回收器和输入输出实现。游戏的清屏选择、轮流落子、胜负判定全部来自游戏文件。[加载器](../src/lua_package.cpp) · [C++ 解释循环](../src/prose_vm.cpp) · [独立运行库](../src/prose_library.cpp)

编译阶段保留 Lua 5.4.9 的官方前端。编译器落盘前从英文重新装配，并与官方前端的输出逐字节对照。独立执行路径直接从元数据和英文建立函数树。需要原生 Lua 模块互操作时，单独的兼容运行时可以重建 Lua 块交给宿主。

## 编译与部署

| 产物 | 编译方式和运行环境 |
| --- | --- |
| 独立 EXE | `gptc compile project/main.lua --root project -o app.exe`。入口必须位于指定根目录内。把 `app.exe` 和独立运行时 `gptlua54.dll` 放在一起。单文件可省略 `--root`。 |
| 独立 DLL | 将输出改成 `-o app.dll`。普通 Windows 程序用 `LoadLibraryW` 加载并调用 `int gpt_run(const char *const *argv)`。传入以空指针结束的 UTF-8 参数数组，首项是程序名。每次调用创建独立的 VM 状态，返回进程风格的退出码。 |
| 原生 Lua 宿主 | 对需要 Lua 5.4 C 扩展、协程、完整标准库或动态加载源码的项目，显式使用 `--runtime lua`。生成的 EXE 或 DLL 直接导入 `gptcompat.dll`。把 `dist/LuaHost` 内的 `gptcompat.dll` 和 `lua54.dll` 放在宿主旁。DLL 向 Lua 宿主导出标准的 `luaopen_<name>`。 |
| 同一 DLL 双入口 | 默认生成的 DLL 也保留 `luaopen_<name>`。宿主选择这个入口时，将 `gptcompat.dll` 放在 `gptlua54.dll` 旁，并提供兼容的 Lua 5.4 宿主状态；`gpt_run` 始终走独立 VM。 |

独立 VM 支持数值与位运算、分支、数值和泛型循环、表、多个返回值、可变参数、闭包、尾调用，以及常用的元方法。内置库覆盖包内 `require`、控制台读写、部分字符串与表操作等。Lua 模式匹配只覆盖实现列出的子集。协程、动态 `load`、外部 C 扩展和整套 Lua 标准库使用兼容后端。两种后端由构建选项决定，独立 EXE 遇到不支持的功能会报告错误。

## 验收

- 从测试夹具恢复 v1.0.0 已发布的原版 EXE，SHA-256 为 `fc3abbb3f106e169644ee834608404811eec858cd5f58652705999825e1d92cb`。在临时目录复制后实际删除 `lua54.dll`，只留下原版 EXE 和新版独立运行时，清理子进程 PATH。游戏显示开场后枚举 Windows 已加载模块，确认只加载了独立运行时，并完成胜局、和局、错误输入和重玩。用新文法重编的 EXE 重复上述测试，游戏输出相同。
- 在新 EXE 副本中把 `one more for your count of “moves”` 改成等长的 `one less for your count of “moves”`。原件第五手宣布获胜，改字版仍提示 O 落子。两个 EXE 都从只有独立运行时的目录启动。
- 通过原生导出 `gpt_run` 运行游戏 DLL，验证在没有 Lua 状态的进程内也能完成相同胜局。另一个独立程序覆盖捕获变量、元方法、计数循环、循环引用回收、尾调用和整数边界，其输出与官方解释器对照。
- 兼容模式另跑 Lua 5.4.9 官方 basic 套件及原生扩展测试。CTest 中的 `independent_vm` 对应独立路径，`lua_official_basic` 对应兼容路径。[独立 VM 验收脚本](../tests/independent_vm.py) · [编译器与双入口测试](../tests/lua_acceptance.py)

如需复跑全部五组测试，将官方测试档解压后传给 CMake。仓库自带的测试可以直接在支持 Windows x64 的 MinGW-w64 环境执行。[Lua 官方测试档](https://www.lua.org/tests/)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGPT_LUA_TEST_SUITE=C:/src/lua-5.4.9-tests
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
```
