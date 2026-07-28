# Gleam 第七轮复审修复报告

- 修复日期：2026-07-28
- 当前代码基线：`916dd04afd2c80e656ca7c293928053c24aa4bc9`
- 被修复基线：`c57d0ce`
- 提交 tree：`87c25bb40351647aee681299a43c4b3ce21fa3d7`
- 正式日志：`ci_logs/916dd04_20260728_181209/`

## 结论

第六轮复审提出的 **1 高 + 4 中 + 1 低共 6 项**的行为缺陷全部修复，并有定向自动化留证。修复过程中另外定位到 **2 个引擎缺陷**（均不在原评审条目内，见下节）。

**一处验收标准未完全满足**：safe-step 恢复路径一项的验收标准 2（"对 wait、continue、resume 失败以及 `DetachAndBreak` 增加可注入测试"）**未实现**，理由与现状见"未闭合的验收条件"一节。该项的标准 1、3 已满足。

## 已修复问题

| 级别 | 问题 | 修复 | 验收留证 |
|---|---|---|---|
| 高 | **`ignore` 绕过 stepout 内部断点状态处理。** ignore-count 在所有权处理之前提前返回，而引擎在回调之后无条件消费一次性断点（`GleeBug/Debugger.Loop.Exception.cpp:76-77`），于是 `mStepOutActive` 为真而物理断点已不存在，目标静默跑到退出。 | 内部断点簿记抽为 `handleStepOutBreakpoint()`，作为 `cbBreakpoint` 的**第一条语句**执行，任何用户态处理（ignore/rule）都不可能在它之前返回；`mStepOutBpOurs = false` 提到 owner/non-owner 分支之上，任何线程命中即失去删除权。位置：`Gleam/GleamDebugger.cpp:615-665`。 | **W14e**（新增）：owner 与 non-owner 两档各叠加 `ignore 1`，断言 `stepout return` 恰好 1 次、簿记行出现在 `event ignored` 之前、`bl` 无残留、目标正常退出；owner 档连跑 5 轮逐轮判定。修复前该场景表现为出现 `event ignored` 且完全没有 `stepout return`。 |
| 中 | **`quit`、`restart` 未走统一中止入口**，`resetTransientState()` 也漏清 5 个 per-operation 字段。 | 两个命令均调用 `abortStepOut("quit"/"restart")`（幂等，常规路径不产生多余输出）；`resetTransientState()` 清空全部 per-operation 字段，仅刻意保留单调的 `mStepOutGen`（附注释说明理由）。位置：`Gleam/GleamCommands.Control.cpp:179-207`、`Gleam/GleamDebugger.cpp:537-562`。 | **W14d**（新增）：在 non-owner 停止点分别执行 quit / restart / owner 线程退出。quit 与 restart 各恰好 1 次统一中止、0 次再武装；restart 后新会话 `no breakpoints`、0 次 `stop reason=stepout`、目标跑完；owner 退出档恰好 1 次 `stepout aborted (thread exit)`、0 次幻影 step 停止、干净退出。 |
| 中 | **safe-step 两个恢复分支绕开统一检查**（owner 线程退出、single-step 到达处直接 `ResumeThread` 并忽略返回值）。 | 抽出带返回值检查的 `resumeSuspendedThreads()`（失败输出 TID + 系统错误码），两处裸恢复改为调用它；`cleanupSuspensions()` 在其之上叠加停步标志清理。**不能直接复用 `cleanupSuspensions`**：single-step 到达分支之后 `exceptionEvent()` 仍需要 `isInternalStepping` 来判别该事件（`GleeBug/Debugger.Loop.Exception.cpp:82`），已在代码注释中标注。位置：`GleeBug/Debugger.Loop.cpp:37-77、143-148、219-227`。 | **部分留证（验收条件 2 未满足，见下）**：新增全套件扫描断言"任何场景都不得出现 `event error msg=`"（`run_tests.sh` 末尾），四轮正式日志 89×4 个场景全部 0 次——该扫描正是发现引擎缺陷 2 的手段，此前没有任何断言检查内部错误输出。V4e 的 detach 后目标自行完成断言继续有效。 |
| 中 | **W14 自动判定可能误通过，组合矩阵未补齐。**（两类证据不要求来自同一轮） | W14 改为**逐轮判定**：每轮同时要求 `ec=0`、non-owner ≥ 1、`stepout return` 恰好 1、正常退出恰好 1，失败轮原始日志另存 `gleam_W14_fail_$i.txt`，逐轮结构化行进 `pressure.log`。新增 **W14b** 执行控制矩阵（`g`/`step`/`stepover`/`tgo`/`until`/新 `ret`）与 **W14c** 用户断点矩阵。 | W14 10/10 逐轮全绿、无幻影 step 停止；W14b 6 档各断言 ec=0 + 到达 non-owner 停止 + `bl` 无 ` once` 残留。 |
| 中 | **W15“无诱饵绑定”断言检查错对象**（绑定日志打印逻辑名 `late`，`module=noexp` 永远不会出现）。 | W15 重写为**事件配对**判定：单遍 awk 把每次绑定与它打印所在的那次加载事件配对，用 `modules` 快照给基址命名，输出 9 个字段。纯几何（地址区间）判定在此不成立——探针证实诱饵 NoExp **复用了 Late#1 刚释放的同一基址与同一大小**，Late#1 的合法绑定本就落在 NoExp 区间内，已在脚本注释中记录。位置：`run_tests.sh:1388` 起。 | W15 11 条断言：2 个互异 Late 基址、绑定恰好 2 次、2 次均配对到 Late 加载事件、**配对到诱饵加载事件 0 次**、诱饵活跃区间内绑定/命中均 0、2 次命中一一对应绑定地址；另加"诱饵映像确实被识别"守卫（否则两条区间断言会**空洞通过**）；`selftest modid 2/2` 标注 `(aux)` 仅作补充。实测配对：load#1 Late@7FFCE4CF0000→绑定，load#2 **NoExp@7FFCE4CF0000（同址同大小）→无绑定**，load#3 Late@7FFCE4B40000→绑定。 |
| 低 | **W13 重复执行并覆盖同一日志。** | 删除较短的重复块，保留完整边界矩阵，并注释说明旧块写的是同一个日志文件。 | 断言不再重复计数。 |

## 修复过程中新发现的引擎缺陷（不在原评审条目内）

1. **`Debugger::Init` 带命令行时无法启动相对路径目标。** 有命令行时 `lpApplicationName` 被置 `nullptr`，交由 CreateProcessW 从 `"\"%s\" %s"` 里自行解析路径，对**使用正斜杠的相对路径**解析失败——`Gleam.exe bin/Debug/x64/TestTarget.exe dll4` 报 `failed to start debuggee`，而同一路径不带参数却能启动。已确认为**既有缺陷、非本轮回归**（用未改动的 11:45 Release 二进制复现）。CI 一直是绿的原因：`ci_local.bat` 经 `%BASH% -c` 转发，Git Bash 把未加引号的 `$TARGET` 重写成反斜杠路径，恰好绕开了该分支。修复：命令行存在时仍显式传 `szFilePath`（argv[0] 仍取自命令行，被调试方观察不到差异）。位置：`GleeBug/Debugger.cpp:35-45`。
2. **统一恢复路径在正常 teardown 误报内部错误。** `SuspendedThreads` 里存的是调试事件给的系统句柄，一旦 continue 越过该线程的 EXIT_THREAD（或进程的 EXIT_PROCESS）即失效；新增的带检查恢复会对已埋葬的线程调用 `ResumeThread` 并以 `ERROR_INVALID_HANDLE`（6）报错。实测日志出现 `Debugger: ResumeThread failed for tid 22740 (error 6)`。修复：仅恢复仍在 `mProcesses` 线程表中的条目，已消失的条目直接丢弃（无对象可解冻）。位置：`GleeBug/Debugger.Loop.cpp:42-77`。修复后同场景内部错误 0 次（修复前 1 次）。

## 未闭合的验收条件（本轮未做，需下一轮决策）

safe-step 那一项的验收标准共 3 条，本轮只满足第 1、3 条：统一恢复入口带返回值检查并输出 TID+错误码（第 1 条）、每个 debugger-owned suspend 都有对应恢复且 detach 后目标自行完成（第 3 条，V4e）。

**第 2 条"对 wait、continue、resume 失败以及 `DetachAndBreak` 增加可注入测试"未实现。** 现状与原因：

- 引擎里**不存在任何故障注入接缝**（无 test hook、无可替换的 API 间接层），`WaitForDebugEvent` / `ContinueDebugEvent` / `ResumeThread` 都是直接调用。要按字面实现，必须往共享引擎的事件循环里加测试专用钩子——这属于设计变更而非测试改动，不宜在一轮修复里单方面塞进去。
- `DetachAndBreak` **在 Gleam 侧没有任何调用点**（只有 `detach` 走 `Detach()`）。给一条产品不使用的引擎路径加注入测试，与第四轮"ASLR 重试"降级为"引擎路径未启用，不适用"的口径是同一类问题。
- 折中：本轮新增**全套件内部错误清扫**断言——`cbInternalError` 是引擎报告"依赖的 Windows API 失败"的唯一通道（`printf("event error msg=...")`），此前**没有任何断言检查它**，因此内部错误可以出现在每一份日志里而套件依然全绿。现在任一场景日志出现 `event error msg=` 即整套失败。已双向验证：干净日志通过、注入一条真实的修复前错误串即失败、空目录不误报。四套正式日志中该串出现次数为 0。位置：`run_tests.sh` 末尾。
- 该清扫**只覆盖"失败被报告出来"**，不覆盖"强制失败后引擎的恢复行为是否正确"。后者仍需上面的注入接缝，建议下一轮先就"是否给引擎加测试钩子"做决策，再决定实现或按不适用降级。

## 测试设计教训（已固化在脚本注释中）

- **断言必须能区分"正确删除"与"错误删除"。** W14c 初版把一次性用户断点设在 non-owner 停止点的 `rip` 上，随后的 `step` 会**合法**消费它，"从 `bl` 消失"于是既可能是正确行为也可能是错误删除，断言无法判别。改为：一次性断点放在单步到不了的地址（`inner`），同址场景交给不会被命中消费的普通断点。
- **"零命中"类断言必须配一条"对象确实存在"的守卫**，否则解析失败会让断言空洞通过（W15 的诱饵识别守卫）。

## 门禁结果

`ci_local.bat` 全阶段通过（`[ci] PASS: all stages green`）：

| 阶段 | 结果 |
|---|---|
| clean rebuild Debug x64 | 通过 |
| clean rebuild Release x64 | 通过 |
| Debug 套件 1/3 | 89 场景 296 断言，PASS=296 FAIL=0 |
| Debug 套件 2/3 | 89 场景 296 断言，PASS=296 FAIL=0 |
| Debug 套件 3/3 | 89 场景 296 断言，PASS=296 FAIL=0 |
| Release 套件 1/1 | 89 场景 296 断言，PASS=296 FAIL=0 |

四套日志中 `FAIL:` 行总数为 0；新增的 W14b/W14c/W14d/W14e 与重写后的 W15 在四套日志中均完整出现并全绿。

## 最终判定

6 项的行为缺陷全部关闭，另修 2 个引擎缺陷；**其中 safe-step 一项的验收标准 2（可注入失败测试）未实现**，需下一轮就"是否给共享引擎加测试钩子"做决策，不能视为该项验收条件全部满足。累计 89 场景 296 断言；上一轮正式日志实测为 86 场景 261 断言（`ci_logs/c57d0ce_20260728_114407/`），本轮净增 3 个场景块（新增 W14b/W14c/W14d/W14e 四块，删除重复的 W13 块一块）与 35 条断言，无场景丢失。
