# Gleam 逆向工程原子能力缺口

盘点日期：2026-07-25

对应提交：`3749a80`

## 结论

Gleam 已覆盖启动/附加、执行控制、三类断点、基本寄存器与内存读写、符号、扫描和线程控制，能够完成基础动态调试闭环。

但从逆向工程及后续 MCP 自动化角度看，原子功能层尚未完整。除 `GLEAM_CODE_REVIEW.md` 已记录的 `pause`、`ret`、PE 和 REPL 问题外，还缺少 7 个基础原语及若干高价值辅助原语。

建议至少先完成地址表达式、真实栈帧、异常处置、延迟模块断点、类型化内存和完整线程上下文，再进入 MCP 封装。

## 原子功能定义

这里的“原子化功能”应满足：

- 单一职责，可以独立调用和测试；
- 输入、输出及失败语义明确；
- 失败时不产生未声明的会话状态变化；
- 可以被更高层任务组合，例如追调用链、分析异常、脱壳和定位内存修改；
- 不依赖 GUI，也不扩展到内核调试、crash dump 分析或 TTD。

## P0：基础原语

| 原子功能 | 当前缺口 | 最小能力与验收边界 |
|---|---|---|
| 地址表达式求值 | 地址只接受十六进制或 `module!symbol`：[GleamCommands.Symbols.cpp:228](Gleam/GleamCommands.Symbols.cpp#L228) | 支持寄存器、符号偏移、模块 RVA、括号、加减法和指针解引用，例如 `[rsp+28]`、`kernel32+1234`；所有地址类命令共用同一解析器；语法、溢出和读内存失败必须明确报错。 |
| 真实栈帧枚举 | `bt` 只遍历 RBP 链：[GleamCommands.Inspect.cpp:401](Gleam/GleamCommands.Inspect.cpp#L401) | 提供 `frames [tid]`，基于 `StackWalk64` 输出每帧 RIP/RSP、模块、符号和偏移；支持 RBP/FPO、叶函数和所选线程；同一展开能力供 `ret` 使用。 |
| 当前异常处置 | 只有永久性的 `ignoreexc <code>`：[GleamCommands.Control.cpp:336](Gleam/GleamCommands.Control.cpp#L336) | 当前异常暂停后可执行 `exception pass` 或 `exception handle`；过滤器支持添加、删除、列出以及 first/second chance；恢复时使用的 `DBG_CONTINUE`/`DBG_EXCEPTION_NOT_HANDLED` 必须可验证。 |
| 延迟模块断点 | DLL 未加载时 `module!symbol` 无法解析，DLL 事件只报告基址：[GleamDebugger.cpp:211](Gleam/GleamDebugger.cpp#L211) | 保存“模块名 + RVA/符号”的逻辑断点；模块加载时绑定、卸载时解绑；模块重载或 ASLR 改变后仍绑定到正确位置；列表同时显示逻辑状态和实际地址。 |
| 类型化内存读取与原始内存导出 | `read` 只有最多 64KB 的十六进制输出：[GleamCommands.Inspect.cpp:54](Gleam/GleamCommands.Inspect.cpp#L54) | 支持 `u8/u16/u32/u64/ptr`、ANSI/UTF-16 字符串和指针链读取；支持 `savemem <addr> <size> <file>` 分块导出并报告部分读取；此处只导出原始内存，不包含 crash dump 分析。 |
| 完整线程上下文 | Gleam 只展示 GPR/EFLAGS，只允许写 GPR/RIP：[GleamCommands.Inspect.cpp:22](Gleam/GleamCommands.Inspect.cpp#L22) | 能读写单个寄存器，并覆盖 EFLAGS、DR0-DR7、XMM0-XMM15 和 MXCSR；读取和写回必须作用于明确的所选线程；不同位宽写入语义必须有测试。 |
| 会话重启 | 当前只能退出 Gleam 后重新启动目标 | 提供 `restart`，复用原目标路径和 argv；明确逻辑断点、异常过滤、`hide`、用户补丁及所选线程的恢复/清除策略；连续重启不得遗留进程、句柄或旧地址状态。 |

## P1：高价值辅助原语

| 原子功能 | 最小能力与用途 |
|---|---|
| `meminfo <addr>` | 返回 allocation base、region size、state、保护属性、类型和所属模块，避免调用方从完整 `maps` 中自行匹配。 |
| `moduleinfo/sections` | 返回完整路径、映像大小、OEP、节区、TLS callbacks、异常目录 `.pdata` 和 Load Config/CFG；为入口断点、TLS 分析和栈展开提供基础数据。 |
| `peb`、`teb [tid]`、`tls [tid]` | 暴露进程/线程环境、TLS、LastError/LastStatus 和反调试相关状态；GleeBug 已保存线程 TEB 基址，但 Gleam 尚未提供查询命令。 |
| 断点启用/禁用/编辑 | 支持 enable、disable、修改条件/动作、命中计数和逻辑断点状态，不再依赖删除后重建。 |
| 段前缀地址表达式 | `gs:[expr]`（x64 用户态 = TEB 基址，引擎 `Thread::lpThreadLocalBase` 现成，典型如 `gs:[60]` 取 PEB）、`fs:[expr]`（x64 基址为 0，预留）、`ds:/es:/ss:/cs:[expr]`（平坦模型基址 0，剥掉前缀即可）。当前 `eval gs:[60]` 报 `unknown name 'gs:'`，`ds:[rcx+30]` 同样无法解析。改动集中在 `exprParseUnary` 一处。 |
| 段寄存器读写与打印 | `regs` 不显示 CS/SS/DS/ES/FS/GS（及 FS/GS base），`setreg` 不支持段寄存器。引擎 `Registers::R` 枚举含段寄存器项（GS/FS/ES/DS/CS/SS），TEB 基址在 `Thread::lpThreadLocalBase`。与上一条配合后可完整回答"指令里段寄存器操作数到底是多少"。 |
| 符号控制 | 支持 `sympath`、`symload`、`symreload` 和模块符号状态，使 DbgHelp 自动加载失败后可以恢复。 |
| `OutputDebugString` 事件 | GleeBug 已提供回调；Gleam 应支持记录内容、按需暂停和明确区分 ANSI/Unicode。 |
| 子进程跟随 | GleeBug 当前强制 `DEBUG_ONLY_THIS_PROCESS`：[Debugger.cpp:42](GleeBug/Debugger.cpp#L42)。应提供可选 follow-child，并在输出中携带 PID，支持启动器、壳和 dropper 场景。 |
| 有界指令跟踪 | 在现有 `tgo` 上提供 `stepn/trace n`，输出 RIP、指令及寄存器变化，支持上限和取消；不扩展为 TTD。 |

## P2：竞品调研补充层

盘点日期：2026-07-25（在 P0 全部完成之后）

调研范围：WinDbg 官方命令参考（learn.microsoft.com）、Cheat Engine wiki、Scylla/ScyllaHide 源码、IDA/OllyDbg/Immunity 官方文档，逐项核实。主场景约束：**附加（attach）已运行进程做逆向**，由 LLM 经 MCP 驱动。

### T0：attach 逆向刚需（建议最优先）

| 原子功能 | 最小能力与验收边界 |
|---|---|
| 快照差量内存扫描 | CE 初扫+续扫工作流：初扫存内存快照并筛选；续扫只在候选集上按 changed/unchanged/increased/decreased（含 by 增量）差量过滤，多轮收敛。当前 `find` 只能搜已知模式，"找到那个未知值"完全无解。实现=快照存储+差量比较，零新依赖；LLM 迭代闭环（改状态→续扫→收敛）天然适配。 |
| 通配批量断点 + 通配符号查询 | WinDbg `bm`/`x` 等价物：`bpm ws2_32!send*` 对整个 API 家族下断；`x module!*pat*` 通配列符号。exports 已有通配过滤，实现是"枚举×匹配→循环下断"纯增量。 |
| 内存区域用途分类 + 过滤搜索 | maps 增强：区域用途标注（Stack/Heap/Image/FileMap）；find 增强：按区域属性过滤（只在可写私有内存搜）；新增可打印字符串扫描（WinDbg `s -sa/-su`，无符号目标的字符串定位刚需）。 |
| 进程上下文速览 | P1 的 peb/teb/tls 扩展：命令行/环境变量（PEB）、LastError 解码（TEB.LastErrorValue）、线程 CPU 时间（GetThreadTimes，attach 后"哪个线程在干活"）。成本最低性价比最高。 |

### T1：差异化大杀器

| 原子功能 | 最小能力与验收边界 |
|---|---|
| Appcall（进程内函数调用） | `call <addr> <args...>`：暂停态保存上下文→伪造调用帧（参数+返回地址指向 int3 蹦床）→执行到返回→读回 rax→恢复上下文。用途：调内部解密函数直接拿明文、验证假设、注入 DLL。WinDbg `.call`/IDA appcall 有，x64dbg 没有——差异化最大。风险（死锁/状态破坏）按官方惯例标注。 |
| 内存快照 save/restore | 枚举可写区域读档，restore 写回+恢复保护。attach 场景 restart 无意义，快照是唯一"时光机"，让 LLM 敢做破坏性实验。 |
| `wt` 函数级跟踪统计 | 未知函数行为指纹：调用树+每函数指令数/调用次数统计。复用 tgo/stepout 单步循环设施，输出结构化适合 MCP。 |

### T2：覆盖率与指针

| 原子功能 | 最小能力与验收边界 |
|---|---|
| Hit trace 覆盖率标记 | "这个操作触发了哪些代码"：tgo 加记录模式，只存命中地址集合不存日志，输出适合 MCP 消费。 |
| 智能指针 dump（dps/dqs） | stackscan 思想泛化到任意地址：按指针大小读内存并逐值自动符号化（vftable、指针数组一眼可读）。 |
| 指针扫描（pointer scan） | 反向枚举到达目标地址的多级指针链（最深 N 级、最大偏移 M），产出 `模块+偏移→…→目标` 的稳定定位链，跨会话复用分析结论；与模块相对表达式体系契合。 |

### T3：按需补

| 原子功能 | 最小能力与验收边界 |
|---|---|
| 结构剖析（dissect） | 给结构体基址按偏移猜测字段类型（指针/int/float/字符串）；支持多实例并排差分（组内相同组间不同→字段语义线索）。 |
| ScyllaHide 高价值四钩子 | NtQueryInformationProcess 三件套（DebugPort/DebugFlags/DebugObjectHandle）、NtSetInformationThread(ThreadHideFromDebugger)、NtClose 无效句柄探测、Get/SetContextThread 的 DR 寄存器保护——正好是 hide 既有局限（NtQuery 类检测未覆盖）的补齐。 |
| 句柄表/堆枚举 | WinDbg `!handle`（需 NtQuerySystemInformation+对象命名）、`!heap`（堆块归属/统计）。 |

### 明确不做（调研确认超范围）

- Ultimap/DBVM（内核驱动/特定 CPU）、Scylla 脱壳三件套（OEP/IAT 重建/PE dump——非目标场景）
- WinDbg JS 脚本/NatVis（MCP 就是 Gleam 的脚本层，不需要进程内脚本引擎）
- 内核调试、dump 分析、TTD（PROJECT.md 既定排除项）

## MCP 前置条件


这不是新的逆向功能，但在 M3 前必须统一：

- 每个原子命令返回明确的成功/失败状态和稳定错误码；
- 列表、寄存器、内存、栈帧和事件使用结构化字段，不依赖自由文本解析；
- 每个结果携带必要的 PID、TID、地址宽度和会话状态；
- 大块内存及长列表支持分块或分页；
- MCP 工具应直接映射原子能力，不把多个隐含状态变化包装成一个不可验证操作。

## 本地分析工作区

Gleam 可以增加本地数据库，但应将其定义为“分析工作区”，而不是调试进程快照。

核心原则是：持久化能够跨运行重新定位和验证的逻辑状态，不恢复本次进程专属的瞬时状态。

### 状态分类

| 类型 | 建议记录的内容 | 恢复策略 |
|---|---|---|
| 分析配置 | 异常过滤、`breakon`、`hide`、符号路径、启动参数和工作目录 | 程序版本匹配时按恢复模式加载 |
| 逻辑断点 | 模块+RVA/符号、断点类型、大小、条件、动作和启用状态 | 模块加载后重新解析和绑定 |
| 分析标注 | 地址标签、注释、书签、关注表达式和字符串编码 | 自动恢复 |
| 补丁定义 | 模块+RVA、预期原字节、补丁字节和说明 | 校验程序指纹及原字节后，明确允许才应用 |
| 历史记录 | 调试时间、退出码、停止事件、命中统计和命令历史 | 只供查询，不写回新进程 |
| 瞬时状态 | PID/TID、HANDLE、模块基址、RIP、寄存器、当前异常和单步状态 | 不恢复 |

以下状态不能直接持久化并自动恢复：

- 绝对虚拟地址；
- 当前线程选择；
- `until`、`ret`、`stepover` 创建的临时断点；
- `ignore` 剩余次数；
- 当前 trace、pause 和 stub 状态；
- 远程分配得到的地址；
- 硬件断点槽位编号；
- 当前进程的句柄、模块基址和线程上下文。

### 程序身份

不能只使用文件路径，更不能使用 PID 判断“同一个程序”。建议建立两层身份：

```text
TargetFamily
  规范化 exe 路径
  产品名称或用户别名

BinaryRevision
  SHA-256
  PE Machine
  SizeOfImage
  PE TimeDateStamp
  PDB GUID + Age（存在时）
```

再次启动或附加时：

1. 通过 `QueryFullProcessImageNameW` 获取主程序路径。
2. 计算文件指纹并查找对应 `BinaryRevision`。
3. 指纹完全一致时打开原工作区。
4. 路径相同但指纹变化时创建新版本，不自动应用旧断点和补丁。
5. 可以继承注释等非侵入信息，但地址相关状态必须标记为待确认。

附加场景同样按可执行文件身份匹配，PID 只记录在本次 session 中。

### 地址持久化

数据库中的地址必须保存为可重新解析的逻辑引用：

```text
module fingerprint + RVA
module!symbol + offset
address expression
```

例如保存 `kernel32.dll + 0x1234`，不能保存本次运行中的 `0x7FFA...`。

堆对象和动态内存只能保存为可重新计算的表达式或指针链；无法重新定位的绝对地址只属于当前 session。这一能力依赖统一 `AddressRef` 和地址表达式原语。

### SQLite 数据模型

SQLite 适合此项目：可以把 amalgamation 静态编译进 Gleam，继续保持单一 EXE，并获得事务、查询和 schema migration 能力。

建议数据库位置：

```text
%LOCALAPPDATA%\Gleam\workspace.db
```

建议的最小数据表：

```text
meta                  schema 版本
targets               程序家族
binary_revisions      文件指纹和 PE 身份
profiles              argv、工作目录和调试配置
breakpoints           逻辑断点、条件和动作
exception_policies    异常处理策略
patches               原字节、补丁字节和应用策略
annotations           标签、注释和书签
watches               表达式、类型和编码
sessions              每次启动/附加记录
events                可选的停止事件和命中历史
```

核心字段应结构化存储；只对易扩展的条件、动作或显示选项使用带版本的 JSON。不能直接序列化 C++ 对象或保存裸指针。

### 恢复模式

建议提供三个等级：

| 模式 | 行为 |
|---|---|
| `off` | 不加载或写入分析工作区 |
| `safe` | 默认模式；恢复符号、逻辑断点、异常策略、注释和 watches，不自动应用补丁或高风险动作 |
| `full` | 仅程序指纹完全一致时允许恢复补丁、断点动作及其他会修改目标的状态 |

argv、环境变量和命令历史可能包含密钥或隐私数据，应支持不保存、字段脱敏或使用 Windows DPAPI 加密。

附加到现有进程时不应静默执行 `full` 恢复；调用方必须显式选择工作区和恢复模式。

### 保存与冲突规则

- 状态修改后立即使用事务保存，不能只在正常退出时写库。
- 数据库必须包含 `schema_version`，升级时使用可回滚的 migration。
- 二进制指纹变化后，地址相关记录进入 stale 状态，不静默重定位。
- 应用补丁前同时校验模块身份、RVA 范围和预期原字节。
- 持久化的 `bp do` 等动作只能在可信数据库、精确程序版本和允许的恢复模式下启用。
- session 命中次数可以累计用于历史分析，但一次运行中的 ignore 余量必须重新开始。

### 工作区实施顺序

1. 定义 `TargetKey`、`BinaryRevision`、`ModuleKey` 和统一 `AddressRef`。
2. 完成模块相对逻辑断点及 DLL 加载/卸载时的绑定。
3. 接入静态 SQLite、schema version、事务和 migration。
4. 先持久化 `safe` 状态：符号、逻辑断点、异常策略、注释和 watches。
5. 加入会话历史和结构化停止事件。
6. 最后实现补丁校验、危险动作和 `full` 恢复。
7. 提供工作区的 list/open/status/reset/export/import 原子接口，供命令层和 MCP 共用。

## 推荐实施顺序

1. 地址表达式求值。
2. `StackWalk64` 栈帧原语，并复用到 `bt` 和 `ret`。
3. 当前异常 pass/handle 与异常过滤器管理。
4. 模块相对的延迟断点。
5. 类型化内存读取、字符串读取和原始内存导出。
6. 完整线程上下文。
7. 本地分析工作区的 `safe` 持久化。
8. 会话重启，并与工作区恢复策略集成。
9. P1 辅助原语。
10. 统一结构化结果后进入 MCP 封装。
11. P2 按 T0→T3 推进：差量扫描 → 通配断点 → maps 分类/字符串扫描 → appcall → 内存快照 → wt 跟踪统计（前三个是性价比之王，可插队在 MCP 封装前；T2/T3 按实际需求）。
