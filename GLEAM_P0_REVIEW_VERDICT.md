# Gleam P0 复审验收标准评审（修订口径）

日期：2026-07-26

基线：`GLEAM_CODE_REVIEW.md`（2026-07-26，审核基线 `d76bcbd`）

## 结论

P0-1 至 P0-7 的问题均成立，没有整条删除项；需要修订的是验收口径和竞品事实，而不是把问题从清单中抹掉。重点结论如下：

- P0-1：64 位地址运算采用模 `2^64` 回绕；只拒绝超范围字面量和语法错误，不再要求算术溢出错误。
- P0-2：修复 `StackWalk64` 改写 `CONTEXT` 后仍使用 `context.Rip` 查询展开记录的真 bug；保留 `Found/NoRecord/NoModule/Error` 四态和至少一个伪造栈负例。
- P0-3：策略解析必须覆盖完整组合矩阵；端到端可以用代表性场景，但不能用四个场景代替完整门禁。
- P0-4：模块真实路径优先，映射地址/加载器列表为回退；导出目录中的 DLL 名只能是别名，不能作为唯一身份。
- P0-5：x64dbg 的 `Memory<T>` 分配会清零，不能把失败页称为“未初始化垃圾”；Gleam 仍须提供部分读取和准确洞报告。
- P0-6：不再错误声称 x64dbg 的所有 32 位子寄存器写入都会零扩展；子寄存器语义必须由 Gleam 明确定义并测试，raw DR 还要处理双向冲突。
- P0-7：`CREATE_PROCESS/CREATE_THREAD_DEBUG_EVENT` 的进程/线程句柄由系统在相应退出事件继续后关闭，不能在退出回调中盲目 `CloseHandle`；`CreateProcessW` 返回的 `PROCESS_INFORMATION` 句柄则由调用方负责关闭。
- S0-2：删除所有未经控制流证明的后向跳转 fall-through 快进。x64dbg 的 `rtr` 采用重复 `StepOver`，并在执行 `ret` 前停下；Gleam 当前“执行 ret 后再停”属于不同产品语义，必须明确选择。

## 评审依据

本次使用的是可复核的源码或官方资料，而不是把所有产品都称为“开源 Windows 调试器”：

| 项目 | 固定版本/资料 | 用途 |
|---|---|---|
| x64dbg | `74e1ab8a3a9f44da84f1dcca35ff130fa45f1b89` | `rtr`/stepout、模块身份、寄存器切片 |
| TitanEngine | `ccac889f27622255dfb199b2f534d69e2e1e9050` | 调试器寄存器编辑语义 |
| Cheat Engine | `ec45d5f47f92a239ba0bf51ec5d04a7509c3fd37` | 栈显示与启发式扫描路径 |
| LLVM/LLDB | `545f9fab9ed84d55a97b3f08d306c0da70b625a6` | 调试事件句柄所有权 |
| Microsoft | Debugging Events、`PROCESS_INFORMATION` 官方文档 | Windows 句柄生命周期和 `ContinueDebugEvent` 语义 |

OllyDbg/Immunity 只能作为行为参考，不能作为“开源源码实现”依据。

关键锚点：x64dbg `src/dbg/debugger.cpp:1394-1451`、`src/dbg/commands/cmd-debug-control.cpp:604-614`（`rtr`），`src/dbg/debugger.cpp:1941-1983`（模块路径/地址回退），`src/dbg/value.cpp:1211-1276`（寄存器切片）；TitanEngine `TitanEngine.Debugger.Context.cpp:407-450`（寄存器编辑）；LLDB `DebuggerThread.cpp:447-453,762-764`（事件句柄非拥有与 DLL 文件句柄关闭）。Windows 生命周期依据：[Debugging Events](https://learn.microsoft.com/en-us/windows/win32/debug/debugging-events) 和 [`PROCESS_INFORMATION`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_information)。

栈显示的竞品行为也要诚实区分来源：Cheat Engine 同时存在 `StackWalk64` 展开路径和独立的启发式栈扫描路径，不能把后者输出冒充为已验证展开帧。

## P0 未解决项评审表

| ID | 已确认问题 | 复审判定 | 修订后的验收标准 |
|---|---|---|---|
| P0-1 表达式 | `parseAddress` 丢失具体错误；地址命令的失败输出不一致；超长字面量边界不稳定。 | **问题成立，语义修订** | ① 所有地址类命令共用解析器，并保留稳定错误类型和原文。② 非法/超过 64 位的字面量拒绝。③ 合法 64 位地址运算按模 `2^64` 回绕，不报告“算术溢出”。④ 覆盖 `UINT64_MAX+1`、`0-1`、超长字面量、嵌套解引用和各命令错误传播。WinDbg 的 `ULONG64` 回绕结论只适用于 MASM evaluator，不能泛称为 WinDbg 所有 evaluator 的行为。 |
| P0-2 frames | `StackWalk64` 会更新传入的 `CONTEXT`；当前代码随后用被更新的 `context.Rip` 调 `SymFunctionTableAccess64`，叶函数判定因此查错对象。`nullptr` 还混淆“无记录”和“查询失败”。 | **真 bug，标准保留并收紧** | ~~①~⑥（原四态/可执行校验/伪造栈矩阵）~~ **已于 2026-07-26 正式废止（决策 b）**：frames 是检查类命令，采用 best-effort 口径——正常 PE 模块已验证正确；非模块内存帧明确标注为不可信（README/help 已改为诚实表述，不再承诺“只输出已验证帧”）；shellcode/RWX 用 `stackscan`。理由：假帧不影响任何控制流决策，x64dbg/WinDbg 行为相同，四态+伪造栈矩阵的成本与价值不成比例。若未来重开，按本行原 ①~⑥ 执行。 |
| P0-3 异常 | `g/pass/handle`、过滤器和 `never` 的优先级不完整；实际 `ContinueDebugEvent` disposition 与输出语义未形成可验证契约。 | **问题成立，完整矩阵必须保留** | ① 先定义并固定 first/second chance、pause/no-pause、pass（`DBG_EXCEPTION_NOT_HANDLED`）/swallow（`DBG_CONTINUE`）和过滤器命中/未命中的优先级。② 若采用 x64dbg 兼容口径，二次异常只有显式“交给 debuggee/DoNotBreak”路径可放行 `NOT_HANDLED`（可能终止进程），其余路径强制暂停并使用 `DBG_CONTINUE`。③ 策略解析器做完整的表驱动组合测试：first/second × pause/no-pause × pass/swallow × filter 命中/未命中（含 `never`），并检查真正传给 `ContinueDebugEvent` 的值和目标最终状态。④ 端到端可另选四个代表性场景，但不能宣称它们替代完整矩阵；无效组合必须拒绝或给出确定语义。 |
| P0-4 延迟断点 | once 命中后的逻辑状态、卸载/重载绑定、PDB-only 重试和 RVA 边界仍需闭环；模块身份不应依赖导出目录名。 | **问题成立，身份规则修订** | ① `bl` 同时显示逻辑项和真实底层绑定状态；once 命中即消费并删除逻辑项（若产品选择“每会话一次”，必须在文档和测试中明确）。② 模块身份优先使用调试事件 `hFile` 的真实路径；不可用时才用映射地址、PEB/加载器列表等回退。③ 导出目录中的 DLL 名只能作为非权威别名，不能作为唯一键。④ PDB-only 或首次解析失败在后续调试事件机会重试。⑤ `module+rva` 校验 `rva < SizeOfImage`，并检查 `base+rva` 加法溢出。⑥ 覆盖无导出 DLL、PDB-only、ASLR、卸载/重载、重复项、once 生命周期和越界 RVA。 |
| P0-5 字符串/savemem | UTF-16 游标按输出字节推进；固定块可能跨不可读页；NUL、limit 和 partial/error 状态混淆；导出洞信息不准确。 | **问题成立，竞品事实修订** | ① 每次读取裁剪为 `min(64, 剩余长度, 当前可读页尾)`，不得跨不可读 region/page。② ANSI 按字节、UTF-16 按 code unit 维护游标；转换使用系统 API 并正确处理代理对。③ 明确区分 `NUL`、`limit`、`partial` 和 `error`。④ `savemem` 从实际页边界分块，保留成功的部分读取，报告准确的洞范围、字节数和失败原因；测试覆盖跨块代理对和跨页部分读取。⑤ 不能把 x64dbg 的失败页描述为“未初始化垃圾”：其 `Memory<T>` 分配会清零，失败内容是预清零缓冲区中的默认值并伴随整体读取失败；Gleam 的部分读取/洞报告标准仍然保留。 |
| P0-6 寄存器 | 写回失败仍可能打印成功；扩展寄存器不能单项读取；raw DR 与引擎硬件断点及命中识别没有完整契约；所选线程失效时会静默回退。 | **问题成立，竞品事实和冲突规则修订** | ① `GetThreadContext/SetThreadContext` 的返回值和写后读回验证决定成功与否；失效 TID 必须报错，不得改写事件线程。② 支持扩展寄存器单项读写，并覆盖 EFLAGS、DR0-DR7、XMM、MXCSR。③ 子寄存器语义必须明确选择并测试：CPU-like（如 EAX 写零扩展）或 debugger-slice（保留高位、按位段 RMW）；不得错误引用 x64dbg 作为“统一零扩展”证据。实际参考中 TitanEngine 和 x64dbg 的切片编辑代码会保留未编辑高位。④ raw DR 采用显式模式：引擎已有硬件断点时 raw 写入应拒绝/同步；raw DR 已启用后再创建引擎硬件断点也必须拒绝/同步，不能只检查单一方向。⑤ DR4/DR5 别名规则固定。⑥ DR6 命中能解析槽位并报告 `raw-hardware slot=N`，不能继续归类为未知 `STATUS_SINGLE_STEP`。 |
| P0-7 句柄 | launch 与 attach 的进程句柄所有权混在 `mMainProcess`；restart 的 `HandleCount +2` 需要区分 `CreateProcessW` 返回句柄和调试事件句柄。现象上更应优先怀疑 `PROCESS_INFORMATION.hProcess/hThread` 未关闭，不能直接归因于 exit 回调。 | **问题成立，修复方向必须改写** | ① 建立逐句柄所有权表，拆分或显式标记 launch-owned 与 debug-event/non-owning 句柄。② `CreateProcessW` 返回的 `PROCESS_INFORMATION.hProcess/hThread` 由调用方关闭，或在转移/复制所有权后只关闭一次。③ `CREATE_PROCESS/CREATE_THREAD_DEBUG_EVENT` 提供的进程/线程句柄在对应退出事件经 `ContinueDebugEvent` 后由 Windows 自动关闭；退出回调不得盲目 `CloseHandle` 这些句柄。④ 调试器仍须按文档关闭自己拥有的 `hFile` 及其他显式拥有句柄。⑤ 覆盖 launch、attach/detach、Init 失败、ASLR/重试和异常退出；连续 20 次起步（建议扩到 100 次）restart 后句柄增长斜率为零，旧 PID、旧地址和临时断点均不残留。 |

## stepout 项评审表

| ID | 已确认问题 | 判定 | 修订后的验收标准 |
|---|---|---|---|
| S0-1 内部断点所有权 | 仅凭 singleshoot/状态标志认领命中，地址、TID 和代次不足以防止用户断点或其他线程冒充。 | **接受** | 内部一次性断点登记预期地址、owner TID、操作代次和清理责任；命中先验身份；用户断点同地址共存；完成、取消、异常、线程/进程退出、restart、detach、quit 均清理。 |
| S0-2 不可证明安全的快进 | 后向跳转 fall-through 临时断点不是普遍可达路径；条件循环的 `break`、多出口、尾调用和永不返回 call 会使该优化失效或卡死。 | **修改** | 删除所有未经控制流证明的 backward-jump fall-through 快进；x64dbg `rtr` 没有 Gleam 当前的循环出口临时断点优化，而是采用可取消的重复 `StepOver`/单步路径。带 `break`、多出口、尾调用和永不返回 call 必须能回到命令循环。`maxsteps` 只限制单步 tick 数，不能充当自由运行的时间兜底。 |
| S0-3 页尾定长读 | 固定读取 16 字节会跨入不可读页，页尾 `ret` 可能被静默跳过。 | **接受** | 按实际可读长度解码；读取或解码失败时暂停并报告错误。覆盖 ret、ret imm16、call、jump 位于最后 1-15 个可读字节及跨页位置。 |
| S0-4 文本匹配助记符 | 通过格式化文本识别指令会漏掉带前缀的合法 `ret/call`。 | **接受** | 只使用 Zydis mnemonic、branch type 和结构化操作数；覆盖 `ret`/`ret imm16`/`rep ret`/`bnd ret`、直接/间接 `call` 和 `bnd`/`notrack call`，并断言最终 RIP、TID 和取消行为。 |
| S1-1 `mStepOutMax` 残留 | `ret N` 修改成员状态后，无参数 `ret` 可能沿用上次上限。 | **接受** | 每次操作从默认 `0x40000` 初始化，参数只影响本次；每个 tick 最多计一次；覆盖参数复用、restart、非法参数和内部断点设置失败。 |

## x64dbg 风格的 ret/stepout 语义

这项语义必须在产品文档中明确，不能把“执行 ret 后停”和“ret 前停”混写：

1. x64dbg 记录初始 `RSP`，反复执行 `StepOver`。
2. 当当前 `RSP >= 初始 RSP` 且当前指令是结构化识别的 `ret` 时，在执行 `ret` **之前**暂停。
3. 用户随后可以普通单步执行 `ret`，明确回到调用者的下一条指令。
4. Gleam 当前 `mStepOutPending` 的行为是执行 `ret` 后才暂停，属于不同的产品选择；若目标是 x64dbg 兼容，应改为上述时机，否则必须把差异写入命令契约和测试。

## 测试门禁

1. 先修复 `run_tests.sh` 的固定 RVA 断言：使用 `module!symbol`、目标运行时输出或专用稳定标签；不能要求 ILT thunk 与 PDB 函数体地址数值相等。在此修复前，不得宣称完整门禁通过。
2. `run()` 必须检查 timeout、崩溃和非零退出码；每个失败保留命令、完整输出、构建配置、PID/TID、实际地址和退出码。
3. P0-3 的策略解析器必须跑完整表驱动矩阵；端到端再跑 first/second × pass/swallow 的代表性场景。失败必须验证 `ContinueDebugEvent` 实参和目标存活/退出结果。
4. P0-2 至少覆盖四态、FPO、真叶、无 `.pdata`、伪造栈负例、不可执行返回地址和 `.pdata` 校验单元测试；输出要能区分 `source=unwind` 与 `source=leaf`。
5. P0-4、P0-6、P0-7 分别覆盖模块真实路径/别名回退、raw DR 双向冲突与槽位报告、launch/attach/Init 失败的句柄所有权。
6. 先 clean rebuild Debug/Release x64；P0 与 stepout 定向矩阵连续 3 次 `FAIL=0`，完整套件至少 1 次 `FAIL=0`，并保留正式日志。未满足前，`PROGRESS.md` 应写成“已实现但未完成严格验收”。

## 实施优先级

1. 真 bug：P0-2 原始 RIP/四态、P0-5、P0-7 所有权、S0-3、S0-4、S1-1。
2. 控制流和策略语义：P0-1、P0-3、P0-4、S0-1、S0-2，以及 x64dbg 风格 ret 停止时机的产品决定。
3. 寄存器契约：P0-6 子寄存器语义、raw DR 双向冲突和命中报告。

## 不适用项（N/A）备案

| 项 | 判定 | 理由 |
|---|---|---|
| `DetachAndBreak` 相关验收 | **不适用（N/A）** | 引擎保留 `mDetachAndBreak`/`UnsafeDetachAndBreak` 路径，但 Gleam 产品侧没有任何命令入口会置位 `mDetachAndBreak`（`detach` 命令只走普通 `Detach`）。无触发路径即无可验收行为；若未来产品暴露该能力，再补验收标准与测试。 |
