# Gleam

无界面、命令驱动的 Windows 调试器，基于 [GleeBug](https://github.com/x64dbg/GleeBug) 引擎（x64dbg 官方调试引擎）。

Gleam 的定位类似 idalib 之于 IDA：调试能力以库/命令的形式存在，没有图形界面，完全由命令驱动，供脚本和自动化系统（LLM / MCP）使用。适用于逆向分析场景。

## 特性

- 启动 / 附加进程调试（`gleam <exe>` / `gleam -a <pid>`）
- 三类断点：软件断点（含一次性）、硬件断点（DR0-3 槽位）、内存断点（页防护）
- 完整执行控制：继续、单步、步过、执行到返回、运行中暂停、脱离
- 状态读写：寄存器读写、内存读写、反汇编（Zydis）
- 进程信息：内存映射、模块列表、线程列表、栈回溯、内存模式搜索
- 符号支持（dbghelp）：导入表枚举（含延迟导入）、导出表枚举（通配过滤）
- 异常过滤、断点命中忽略计数
- 中文路径目标调试（UTF-16 命令行入口）
- 静态链接，单一 exe，无外部 DLL 依赖（CRT 也是静态）

## 构建

环境：Visual Studio 2022（v143 工具集）、Windows 10 SDK。

直接用 VS 打开仓库根目录的 `GleeBug.sln`，或命令行：

```bat
MSBuild GleeBug.sln -p:Configuration=Debug -p:Platform=x64 -m
```

产物：`bin\Debug\x64\Gleam.exe`

注意：本机若装有 vcpkg 全局集成，工程已设置 `<VcpkgEnabled>false</VcpkgEnabled>` 防止其自动链接的库与内置 Zydis 冲突，新建工程时请保留此项。

## 使用

```bat
gleam <target.exe> [args...]   :: 启动并调试
gleam -a <pid>                 :: 附加到运行中的进程
```

命令从标准输入读取（可交互、可管道脚本化）。命令协议要点：

- 目标程序因事件挂起时（断点命中、单步完成、异常等）进入暂停态，此时逐条执行队列中的命令
- 只有 `pause` 能在目标运行时生效（注入远程 int3 stub 线程断入）
- `detach`/`quit` 对运行中的目标不立即生效，先 `pause` 再执行
- 目标与调试器共享控制台输出，便于脚本断言
- `pause` 通过注入远程 int3 stub 线程实现（不用 `DebugBreakProcess`——它会检查 PEB.BeingDebugged，与 `hide` 冲突）

### 平台

仅 x64。Win32 构建配置已移除（GleeBug 引擎源码仍支持 x86，本项目暂未启用）。

### 命令一览

```
执行控制:
  g                       继续
  step                    单步（进入）
  stepover                步过调用
  tgo <reg><op><v> [max] [log]  条件跟踪（单步循环直到条件满足）
  ret                     执行到当前函数返回
  pause                   中断运行中的目标
  detach                  脱离调试（先 pause）
  quit                    终止目标并退出（先 pause）

断点:
  bp <hexaddr> [once]     软件断点（once 为一次性）
  bp <addr> if <r><op><v> 条件断点（op: == != < >，寄存器条件）
  bp <addr> do <命令>     命中时执行命令（恢复类命令则不暂停）
  trace <addr>            追踪点（命中记录 trace 行并自动继续）
  rbp <hexaddr>           删除软件断点
  hbp <hexaddr> [x|w|rw] [1|2|4|8]  硬件断点
  hbpd <hexaddr>          删除硬件断点
  mbp <hexaddr> <hexsize> [a|r|w|x] 内存断点
  mbpd <hexaddr>          删除内存断点
  bl                      断点列表
  ignore <hexaddr> <n>    忽略断点接下来的 n 次命中

检查:
  regs                    寄存器转储
  setreg <name> <hexval>  写寄存器（rax..r15, rip）
  read <hexaddr> <size>   读内存（十六进制转储）
  write <hexaddr> <b...>  写内存（十六进制字节）
  disasm [hexaddr] [n]    反汇编（默认 rip 起 8 条）
  maps                    已提交内存区域
  modules                 已加载模块
  find <addr> <size> <pat>  内存搜索（支持 ?? 通配）
  find <addr> <size> ascii|utf16 <text>  字符串搜索
  patch <addr> <b...>     补丁（自动记录原始字节）
  patches                 补丁列表
  restore <addr>          还原补丁
  stackscan [n]           栈扫描（标注疑似返回地址并解析符号）
  bt                      栈回溯（RBP 链）
  exinfo                  最近异常信息
  threads                 线程列表
  thread [tid]            查看/选择命令作用的线程
  thread [tid] suspend|resume  挂起/恢复线程（省略 tid 为事件线程）

内存管理:
  alloc <hexsize> [rwx]   在目标中分配内存
  free <addr>             释放内存
  protect <addr> <size> <prot>  修改页保护属性

代码扫描:
  xref <addr>             查找指向地址的引用（call/jmp）
  findasm <text>          按指令文本搜索代码

符号:
  imports [module]        模块导入表（默认主模块，含延迟导入）
  exports <module> [pat]  模块导出表，可选通配过滤
  sym <addr>              地址反查符号
  until <addr>            运行到指定地址（一次性断点语法糖）

反反调试:
  hide [on|off]           隐藏调试器（PEB 标志、堆标志、
                          IsDebuggerPresent 等 API 补丁）

事件暂停:
  breakon [sw] [on|off]   暂停开关：entry/dll/thread/exception

异常:
  ignoreexc <hexcode>     将指定异常码交还目标处理
```

### 脚本示例

```bat
echo bp 140070EC9 & echo g & echo regs & echo read 140190000 10 & echo g | gleam TestTarget.exe
```

## 测试

仓库根目录的 `run_tests.sh`（Git Bash 运行）：

```bat
bash run_tests.sh
```

48 个场景 121 项断言，覆盖断点、执行控制、内存/寄存器读写、反汇编、符号枚举、异常过滤、脱离、跟踪、补丁全矩阵、argv 引用、跨块扫描、Release/FPO 栈展开及压力（100 连发 pause 注入、100 会话）。`TestTarget` 等工程是被调试目标，固定基址保证地址稳定可断言。脚本在任一断言失败时以非零退出码结束。

## 架构

```
Gleam/
├── main.cpp                       入口 + REPL 线程（stdin → 命令队列）
├── GleamDebugger.h/.cpp           调试器类：事件回调、暂停态命令循环
├── GleamCommands.cpp              命令分发、解析辅助、help
├── GleamCommands.Breakpoints.cpp  断点类命令
├── GleamCommands.Inspect.cpp      检查类命令（寄存器/内存/反汇编/maps/…）
├── GleamCommands.Control.cpp      执行控制类命令
└── GleamCommands.Symbols.cpp      符号命令（导入/导出，dbghelp）
```

线程模型：调试循环在主线程；REPL 线程读 stdin 入队；命令统一由调试线程在目标挂起时执行。

## 许可

GleeBug 引擎为 MIT 协议（见上游 `LICENSE`）。Gleam 部分同。
