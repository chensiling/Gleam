# Gleam 第八轮代码复审结论

- 审核日期：2026-07-30
- 代码基线：`6818206ee2e1ec87a4f74c9f674ef2f24dbd7ae7`
- 提交 tree：`6665a0ed31b61b82d6b9fbf2d103f7b9c24a8199`
- 正式日志：`ci_logs/6818206_20260730_115920/`
- 审核依据：`PROGRESS.md` 的最新“第八轮复审”、前一版 `GLEAM_CODE_REVIEW.md`、当前代码、测试脚本与正式日志

## 结论

当前版本为：**部分通过，仍有 1 个高等级、3 个中等级问题。**

`C3-T3a-R` 已关闭；已发布 stub 的 `VirtualFreeEx` 失败保地址并重试的 deferred 主路径也已关闭。`ExitThread` 依赖已经移除，但 C3-R6 在 hide 与持续终止失败组合下仍不成立。SYM-1 的即时命令路径增加了拒绝逻辑，但 pending DLL 绑定仍能绕过该逻辑。

## 未关闭问题

| 编号 | 级别 | 未修复问题 | 修复后验收标准 |
|---|---|---|---|
| SYM-1 | 高 | **pending DLL 的 PDB-only 回退绕过符号消歧。** `bindModuleBreakpoints()` 在严格 `resolveModuleSymbol()` 失败后，仍继续调用 `resolvePdbSymbol()`；后者直接使用 `SymFromName` 任选记录。未加载 DLL 的 `bp module!symbol` 因而可能在加载时绑定到增量链接僵尸体并写入 `INT3`。现有 SYM 仅测已加载主模块的即时 `eval`/`bp`；`ZombieTarget` 的两个同名 `inner` 都是真实函数，不是“旧记录 + 唯一现行函数体”的固定样本。位置：`Gleam/GleamDebugger.cpp:514-530`、`Gleam/GleamCommands.Symbols.cpp:533-588、1090-1112`、`run_tests.sh:2186-2228`。 | 1. 所有符号入口，包括 PDB-only fallback 和 pending rebind，必须复用同一消歧结果；歧义不得退回 `SymFromName` 猜测。2. 固化“旧记录 + 唯一现行函数体”的样本，并覆盖模块未加载时注册、加载时绑定的路径。3. 无法唯一确认时明确拒绝、不注册 pending、不写断点；可确认时断点真实命中、原字节恢复、目标无异常退出。 |
| SYM-2 | 中 | **歧义状态跨命令污染无关 breakpoint。** `mSymbolAmbiguous` 是全局布尔值，只在下一次模块符号解析时才复位；`bp` 对任何未解析的逻辑规格都读取它。已复现：先执行 `eval ZombieTarget!inner`，再执行 `bp definitely_missing_module+123`，后者被错误打印为 `breakpoint refused (ambiguous symbol)`，而非记录为 pending。位置：`Gleam/GleamCommands.Symbols.cpp:533-535`、`Gleam/GleamCommands.Breakpoints.cpp:296-303`。 | 1. 歧义结果必须属于本次解析结果，不能以跨命令可残留的布尔状态表达。2. 增加回归：歧义 `eval` 后，`module+rva`、未知 `module!symbol`、普通数值断点分别保持各自正确语义。 |
| C3-R6 | 中 | **stub 终止与 fallback 契约只覆盖单次失败。** stub `ResumeThread` 失败后仍回退到 `DebugBreakProcess`；`hide on + selftest failapi stubresume` 的定向实测没有产生 `stop reason=pause`，最终由超时终止目标。另一个分支只写入 16 个 `INT3`：持续 `TerminateThread` 失败最多重试 16 次，之后线程会执行零填充页，不再是可识别的 break-in 停止点。W17 只注入一次 terminate 失败，也没有 hide + stubresume 组合。位置：`Gleam/GleamDebugger.cpp:113-137、239-285、1001-1044`、`run_tests.sh:2100-2147`。 | 1. `hide on` 下 stub `ResumeThread` 失败后必须仍能受控恢复 pause，不能依赖预期失效的 `DebugBreakProcess`。2. 持续 `TerminateThread` 失败时不得执行出受控 stub 区域；句柄、TID、页面始终保留直至确认死亡。3. 增加 hide + stubresume、持续 terminate 失败、恢复成功后 detach 的故障注入；确认目标存活、页面最终 `MEM_FREE`。 |
| C3-R5-R | 中 | **stub 发布前的写入失败仍可能遗留不可回收页面。** `ensureBreakInStub()` 在 `WriteProcessMemory` 失败时直接调用未检查的 `VirtualFreeEx`，但页面尚未登记到 `mBreakInStubPage`；若释放同时失败，远程 RWX 页面地址丢失，无法重试。W17/vfree 只覆盖已发布 stub 的 deferred 路径，未覆盖该初始化分支或 direct cleanup 分支。位置：`Gleam/GleamDebugger.cpp:264-285`、`run_tests.sh:2150-2183`。 | 1. 分配后的页面必须在任一可失败步骤前处于可追踪状态，或对释放失败保留等价的可重试记录。2. `WriteProcessMemory + VirtualFreeEx` 双失败不得遗留无主页面。3. 分别注入初始化、direct cleanup、deferred cleanup 的释放失败；每条路径都须可重试并通过外部 `VirtualQueryEx` 验证 `MEM_FREE`。 |

## 已确认关闭

| 项目 | 本轮确认结果 |
|---|---|
| C3-T3a-R | W16/attach 的 TID 列表已改为 `[0-9]+( [0-9]+)*`，空列表、纯空格、双空格、尾随空格均有负向元测试。 |
| C3-R5 主路径 | `freeBreakInStubPage()` 仅在 `VirtualFreeEx` 成功后清地址；W17/vfree 已验证 deferred 拒绝、重试、`MEM_FREE` 与目标存活。 |
| C3-R6 的 ExitThread 依赖 | stub 已改为纯 `INT3`，不再解析或调用 `ExitThread`；但本表中的失败组合仍使 C3-R6 保持未关闭。 |

## 当前验证记录

| 验证项 | 结果 |
|---|---|
| Debug 套件 1/3 | `PASS=389 FAIL=0` |
| Debug 套件 2/3 | `PASS=389 FAIL=0` |
| Debug 套件 3/3 | `PASS=389 FAIL=0` |
| Release 套件 1/1 | `PASS=389 FAIL=0` |
| W17 / SYM / 严格 TID 白名单 | 四轮正式日志均通过 |
| 本轮定向复核 | `hide on + stubresume` 未出现 pause；`eval ZombieTarget!inner` 后 `bp module+rva` 被错误拒绝 |

正式门禁证明已覆盖的正常路径和一次性故障注入有效，但没有覆盖上表的 pending PDB fallback、跨命令状态、hide 组合、持续终止失败及发布前清理失败，因此不能作为全部关闭的证据。

## 最终判定

> **`6818206` 已关闭 C3-T3a-R，并部分修复 C3-R5、C3-R6 与 SYM-1；上表 4 项仍未闭合，当前不能标记为全部通过。**
