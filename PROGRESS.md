# 进度文档

> 交接用：让下一个 AGENT 不用翻聊天记录就能继续。每次完成有意义的工作后更新本文件。
> 目标与验证标准见 `PROJECT.md`（总纲），本文件只记录"做到哪了、怎么继续"。

## 当前状态：第八轮复审 SYM-1/SYM-2/C3-R5-R/C3-R6 修复完成并通过全部测试

最后更新：2026-07-30

**第八轮复审中的 4 项问题（SYM-1/SYM-2/C3-R5-R/C3-R6）已全部修复并通过测试**，提交 `793dbd3` + `cbba0e1` + `3b7dd5a`。

- **SYM-1（高）：统一符号消歧，pending DLL 绑定不再绕过**：`resolveModuleSymbol` 改返回 `SymbolResult` 三态（Found/NotFound/Ambiguous）；`resolvePdbSymbol` 使用完整 ILT 消歧逻辑，不再直接调 `SymFromName`；`bindModuleBreakpoints` 遇歧义明确拒绝并从 pending 删除。Late.dll 新增 `ambig_a.cpp`/`ambig_b.cpp` 双翻译单元提供"活跃 ambig + 僵尸 ambig"的测试形状
- **SYM-2（中）：移除跨命令污染的全局歧义状态**：删除 `mSymbolAmbiguous` 布尔值，歧义通过返回值传递；`exprParseAtom`/`bp` 命令按结果分支，`eval ZombieTarget!inner` 后 `bp module+rva` 保持正确 pending 语义。修复歧义错误重复打印（`eval` 不再重复打印 `resolveModuleSymbol` 已打印的详细错误）
- **C3-R5-R（中）：stub 发布前写入失败保留页面地址**：`ensureBreakInStub` 的 `WriteProcessMemory` + `VirtualFreeEx` 双失败时将页面登记到 `mBreakInStubPage`，后续清理可重试
- **C3-R6（中）：hide on 下 stub ResumeThread 失败可恢复**：`ResumeThread` 失败后，非 hide on 时使用 `DebugBreakProcess` fallback（保持原始行为）；hide on 下明确告知不可用并延迟到下一自然事件

**新增测试**：SYM-1（pending DLL 歧义拒绝）、SYM-2（跨命令污染回归）

**测试结果**：**PASS=399 FAIL=0**（116 场景，Debug 模式全绿）

**累计提交**：
- `793dbd3`: 主要修复（符号消歧统一化、歧义状态移除、stub 页面/恢复修复）
- `cbba0e1`: 测试修复（歧义错误去重、测试调整）
- `3b7dd5a`: 修复总结文档

---

## 第八轮复审 C3-R5/C3-R6/C3-T3a-R/SYM-1 全部落地，门禁绑定 6818206

最后更新：2026-07-30

**复审方复核 `1b0d0ce` 后提出的 SYM-1（高，符号僵尸体）、C3-R5（释放失败丢地址）、C3-R6（stub 终止契约）、C3-T3a-R（低，TID 语法）已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/6818206_20260730_115920/`。提交 `6818206`。

- **C3-R6 stub 终止契约**：stub 改 16 字节纯 int3，彻底移除 `ExitThread` 解析依赖（`mExitThreadAddr` 删除；hide-on 下不再需要 `DebugBreakProcess`）；识别改页范围匹配——终止失败的线程活到下一个 int3 会被再次识别重试；`TerminateThread`/`ResumeThread` 全检查，未确认死亡前不关句柄/清 TID/放页面。W17 三档 gleam 侧注入（stubresume 回退暂停可用、terminate 下一 int3 重试成功、vfree 拒绝后重试释放成功）
- **C3-R5 释放失败保地址**：`freeBreakInStubPage()` 成功才清地址；detach 统一为"先非阻塞清理 → 线程未确认则延迟到 EXIT_THREAD 事件 → 仅页面失败才当场拒绝"。**过程中挖出更深的 hang**：`exitThreadEvent` 在 `cbPostDebugEvent` 前清空 `mThread`，EXIT_THREAD 事件永远进不了命令循环——拒绝路径改 `forceBreakIn` 主动唤醒（与 `cbDetachRefused` 一致），消灭"暂停中等待死亡"的整个故障类别
- **SYM-1 符号歧义（高）**：`resolveModuleSymbol` 改全量枚举 + ILT 校验消歧——多记录时只有被 ILT thunk 指向的函数体可采纳，无法唯一消歧则明确拒绝；**歧义符号不再注册 pending**（`mSymbolAmbiguous` 硬拒绝，否则后续误绑或逐事件刷错误）。固化样本 `ZombieTarget`（双翻译单元同名静态 `inner` → 确定性双记录；增量链接僵尸样本尝试未果——PDB 更新行为不确定，同名静态是等价且确定的形状）。`selftest symdis` 4 例决策矩阵
- **C3-T3a-R**：TID 列表 `[0-9]+( [0-9]+)*` + 空/纯空格/双空格/尾随空格负例
- **R6/R7 断言修正**：quitting 开始后零注入改区间判定（stub 自包含后早期 pause 的合法注入不再偶然失败）；detach 后目标完成输出加等待消竞态

**累计**：116 场景 389 断言，Debug×3 + Release×1 全绿。

---

## 第八轮复审 C3-R4/C3-T3a/C3-T3b 全部落地，门禁绑定 1b0d0ce

最后更新：2026-07-30

**复审方复核 `ea51f41` 后提出的 C3-R4（stub 页面泄漏）、C3-T3a（白名单子串匹配）、C3-T3b（PID 检查假绿）已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/1b0d0ce_20260730_015821/`。提交 `96fda37`（主体）+ `71ac990` + `1b0d0ce`（replylater 结构性修复）。

- **C3-R4 stub 页面泄漏（我上轮自己引入的真泄漏）**：detach 在暂停中同步等 stub 线程死（持有事件期间全进程冻结，等待必超时），超时分支丢句柄留页面。现：stub 线程在 int3 处即终止（**不执行依赖解析地址的 `call ExitThread`**——该地址在 attach 会话可能错误，实测曾引发 137 万行异常循环），句柄保留，**EXIT_THREAD 事件作为死亡确认**；detach 遇活 stub 一律延迟（`detach deferred`），确认死亡 → `VirtualFreeEx` → 才 `Detach()`，释放失败则拒绝并保持附加。W16/attach：`wait=0x102` 零容忍 + powershell `VirtualQueryEx` 外部验证 MEM_FREE
- **C3-T3a**：白名单改全行锚定（含 TID 列表语法与固定结尾）+ 负向元测试（合法前缀+垃圾必拒、干净样本必收）
- **C3-T3b**：resume 档 PID 检查统一 `pid_state()`，并绑定真实 WPID 做 `TASKLIST=false` 强制失败验证

**replylater 注入的三层根因（门禁连挂两次换来的）**：① deferral 是内核调度产物无法强制，改有界重试并如实注明"测的是处理不是竞态"；② **搭档线程寿命**——busyWorker 仅 12 次调用约 1ms 退出，之后主线程独舞约 100 万次命中零 deferral，TestTarget 新增 `mtl` 模式（busyWorkerLong 5000 次）；③ **探针僵尸体**——增量链接下 PDB 可能保留 inner 的僵尸体，`!inner` 符号解析不稳定，据此下的断点曾把真实函数 `sub rsp` 立即数改写导致主线程栈损坏（AV）；ILOOP 探针改为跟随目标打印的 ILT thunk 到真实函数体 + `cmp 0x989680` 自校验。**遗留**：gleam 符号解析在增量链接下的僵尸体问题本身未修（超出本轮范围，下一轮可决策）。

**累计**：110 场景 353 断言，Debug×3 + Release×1 全绿。

---

## 第八轮复审 C3-R3/C3-T3 全部落地，门禁绑定 ea51f41

最后更新：2026-07-29

**复审方复核 `903d59d` 后提出的 C3-R3（detach 拒绝后命令控制依赖事件恢复）与 C3-T3（W16/attach 白名单缺失 + PID 检查假绿）已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/ea51f41_20260729_201247/`。提交 `ea51f41`。

- **C3-R3 同步回传（真活性 bug）**：拒绝 detach 发生在引擎循环迭代内，但 Gleam 此前只能在**下一个 debug event** 的 `cbPostDebugEvent` 发现——静默目标上队列命令与 `pause` 都被 `mQuitting` 永久卡死（旧 W16/attach 靠持久断点持续命中蒙混）。现引擎新增 `cbDetachRefused` 虚回调在拒绝分支**同步**调用；Gleam 在回调里立即清 `mQuitting`、置 `mWantsPause` 并 `forceBreakIn` 主动制造事件重进命令循环。上一版的 `mDetachInFlight` + 事件检测已撤销（回调是明确回传通道）。W16/attach 改**一次性断点**：拒绝后目标完全静默，off → 二次 detach 仍须完成——唤醒只能来自拒绝回传本身
- **C3-T3 门禁**：attach 档每条 `event error msg=` 必须命中白名单（注入的 `ResumeThread error 5` 或预期的拒绝文本）且拒绝恰好 1 条，白名单外任何错误立即判负；`pid_state()` 三态（alive/gone/error）让 `chk_pid_gone` 不再把 tasklist 查询失败当"目标已退出"，并加两个元断言（强制查询失败必须判 error、死 PID 必须判 gone）证明门禁自身不假绿

**累计**：108 场景 345 断言，Debug×3 + Release×1 全绿。

---

## 第八轮复审 C3-R2/C3-T2 全部落地，门禁绑定 903d59d

最后更新：2026-07-29

**复审方复核 `87e1b81` 后提出的 C3-R2（持续恢复失败的终态处理）与 C3-T2（W16 清理证据不全）已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/903d59d_20260729_180513/`。提交 `903d59d`。

- **C3-R2 detach 拒绝**：恢复失败条目保留后，detach 先做有界重试（3×50ms），仍无法恢复则**拒绝 detach**——致命错误列出 TID、清 `mDetach`、会话保持附加（attach 目标不再可能 detach 后遗留冻结线程）；循环结束仍有未恢复条目时终报 TID。Gleam 侧新增 `mDetachInFlight`：detach 被拒后检测（`mDetach` 已清但会话仍在）重新武装 `mQuitting`，用户恢复对附加目标的控制。**产品取舍**：选"保持调试状态"而非"致命后继续 detach"——留下冻结进程比拒绝 detach 更糟
- **C3-R2 测试基建**：`selftest failapi resume always`（持续失败）+ `selftest failapi off`；TestTarget 新增 `wait` 模式（主线程自旋可随时单步、worker 沉睡，30s 窗口）；attach 场景全链路：bp `kernel32!GetTickCount` → 断点重执行内部步挂起 worker → resume 持续失败 → detach 被拒 → 命令循环重臂 → off → 二次 detach 成功 → 目标存活后被测试清理。**场景教训（已固化在脚本注释）**：用户 `step` 走 `StepInto`（isSingleStepping）不触发挂起，挂起来自断点重执行的内部步（isInternalStepping）——attach 场景必须先让断点命中再 step，首个暂停就 detach 会因 `!mDetach` 守卫完全跳过挂起
- **C3-T2**：W16 全档锚定正则（API 名 + `error 5` + pid/tid 格式，resume 明确断言错误码）；launch 各档新增目标 PID 残留检查（`chk_pid_gone`，gleam 退出后 launch 目标必须消失）；attach 档断言拒绝链顺序、两次 detach 尝试、目标存活

**累计**：106 场景 341 断言，Debug×3 + Release×1 全绿。

---

## 第八轮复审 C3-R/C3-T/C3-L 全部落地，门禁绑定 87e1b81

最后更新：2026-07-29

**复审方复核 `4c28f0c` 后提出的 C3-R（resume 失败丢恢复记录）、C3-T（W16 假绿）、C3-L（hook 跨 restart 污染）已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/87e1b81_20260729_144403/`。提交 `87e1b81`。

- **C3-R（真引擎 bug）**：`resumeSuspendedThreads` 此前在 `ResumeThread` 失败后仍 `SuspendedThreads.clear()`——恢复记录丢失、线程永久冻结、挂起计数不配平。现只删恢复成功或确认已死的条目，失败条目保留并在下一恢复点重试（单步完成/detach 清理/循环结束）。验证方式是**线程级**的：TestTarget busyWorker 末尾打印 `BUSYWORKER_DONE=1`（`ExitProcess` 会连冻结线程一起杀，进程退出无区分度）；W16/resume = 一次性注入 + step（失败上报）+ detach（重试成功）→ BUSYWORKER_DONE 出现 + 目标 PID 自行退出
- **C3-L**：四个 failapi hook 改为触发即自清；`resetTransientState()` 统一复位三个 hook（restart 路径 `main.cpp:165` 必经）——武装但未触发的 hook 不再污染新会话。W16/restart：武装 → restart → 新会话 step 做真实 safe-step 挂起/恢复，0 条注入错误
- **C3-T**：W16 五档全部断言 `event error msg=` 的**精确总数**（注入档恰 1 条、restart 档恰 0 条）+ 允许集合，额外 cleanup/handle/resume 错误无法再利用整文件排除蒙混；resume 档不再用 `quit` 收尾（进程终止掩盖冻结线程）

**累计**：102 场景 330 断言，Debug×3 + Release×1 全绿。

---

## 第八轮复审 C3 故障注入 + T2 owner/认领门禁全部落地，门禁绑定 4c28f0c

最后更新：2026-07-29

**复审方复核 `413bb49` 后保留的 C3（故障注入验收缺口）与 T2（owner 路径 + 内部认领门禁）已全部修复，本轮无遗留**，ci 门禁全阶段通过，正式日志：`ci_logs/4c28f0c_20260729_123205/`。提交 `4c28f0c`。

- **C3 引擎故障注入**：引擎新增 3 个休眠钩子 `Debugger::mTestHook{WaitForDebugEvent,ContinueDebugEvent,ResumeThread}`（`Debugger.h`），循环内全部 wait/continue/reply-later/resume 调用点改走钩子（正常路径一次空指针检查，Debug/Release 行为一致，只能显式武装）；Gleam 新增 `selftest failapi wait|continue|replylater|resume`（注入 `ERROR_ACCESS_DENIED`）。W16 四场景：wait/continue 注入后错误行精确 + 会话受控结束；resume 失败只上报不中断循环；**replylater 确定性触发**——新增 ILOOP 探针（inner 慢循环体 = 第一个后跳目标，反汇编定位），主线程在慢循环上以 ignore 高速产生断点异常，busyWorker 命中 INNER 暂停时主线程异常已在队列，`step` 后置 TBP 必然 deferral → 注入失败（实测 4/4）
- **T2 owner/认领门禁**：W14c/sameaddr 窗口内要求用户 once 恰好命中 1 次且禁止 `hit by non-owner` 认领记录；W14c/abort 新增 **owner tid 断言**（命中线程 == 非 owner 停止线程 == 新 ret 的 owner）+ abort 后 bl 无 once 残留。**关键语义事实（已固化在脚本注释）**：引擎 `SetBreakpoint` 同址拒绝（`Debugger.Process.Breakpoint.cpp:9`），"内部断点持有时用户下同址断点"在命令模型里不可达——任何命令窗口都意味着内部断点已被消费，owner/非 owner 命中只能在所有权丢失后区分；sameaddr（非 owner + 继续）与 abort（owner + 中止）凑齐 2×2
- **清扫例外**：套件级引擎内部错误清扫（`event error msg=`）显式排除 W16 日志（有意注入），其余场景照旧零容忍

**累计**：98 场景 320 断言，Debug×3 + Release×1 全绿。（回归中出现过 1 次压测类抖动失败，重跑及门禁 4 轮均未复现。）

---

## 第八轮复审 C3 补齐 / C2-R / T1-T2 断言去污染，门禁绑定 413bb49

最后更新：2026-07-29

**复审方复核 `1a8afdf` 后提出 C3（前半漏修）、T1/T2（断言仍可被非 owner 停止污染）、C2-R（低，代次校验位置），已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/413bb49_20260729_102046/`。提交 `413bb49`。

- **C3 补齐**：`Debugger.Loop.cpp` 的 `ContinueDebugEvent(DBG_REPLY_LATER)` 失败分支此前仍静默 `break`（上一轮只修了普通 continue 分支）——现与普通分支一致上报错误码+PID/TID
- **C2-R 代次校验统一**：`handleStepOutBreakpoint` 的 generation 比较提到 owner/non-owner 分支**之前**——陈旧代次命中（任何线程）直接 `stepOutFinish("error")` 收束，不再可能进入 deferred re-arm 给已死代次重写 int3、污染新代次的 `mStepOutBpOurs`
- **T1**：W14b/until 档此前搜任意 `stop reason=breakpoint`，non-owner 停止即可满足——新增 `LADDR` 探针（`eval TestTarget!looper`），断言精确地址命中
- **T2**：W14c/sameaddr 与 /abort 的"用户 once 触发"全文搜索会被设置前的 non-owner 停止（同文本）污染——改 awk 区间判定：sameaddr 要求命中落在"one-shot breakpoint set"与下一次"re-armed"之间（该窗口内物理断点只能是用户 once），abort 要求命中出现在 once 设置之后
- **DetachAndBreak N/A 备案**：grep 确认 Gleam 产品侧无任何置位 `mDetachAndBreak` 的入口，已在 `GLEAM_P0_REVIEW_VERDICT.md` 末尾"不适用项（N/A）备案"表正式标记（此前连续两轮复审要求正式记录）

**累计**：94 场景 309 断言，Debug×3 + Release×1 全绿。

---

## 第七轮复审 C1-C3 修复 + T1-T4 假绿清除，门禁绑定 1a8afdf

最后更新：2026-07-29

**复审方复核 `367e9c4` 后提出 C1-C3（代码问题）+ T1-T4（测试假绿），已全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/1a8afdf_20260729_011007/`。提交 `1a8afdf`。

- **C1 re-arm 暂停时机**：`cbPostDebugEvent` 命令循环入口移到 re-arm 之后——re-arm 失败产生的 stop 在同一事件内进入暂停，不再出现"记录已发但事件已继续、RIP 与上下文不一致"
- **C2 归属校验**：`handleStepOutBreakpoint` 入口加入 `mStepOutBpOurs` 物理持有条件——非 owner 消费后同址用户 once 不再被误判为内部断点（此前被静默消费）；stale-generation 分支改 `stepOutFinish("error")`
- **C3 引擎 API 失败上报**：`WaitForDebugEvent` 按 `GetLastError` 区分正常超时与真错误（报错退出）；`ContinueDebugEvent` 失败报错误码+PID/TID——此前这些静默失败连内部错误清扫都抓不到
- **T1-T4 假绿清除**：re-arm 补真实事件行（W14d 曾搜不存在的字符串，计数 0 永真）；W14b 每档语义断言（g→stepout return、step→step stop 等）；W14c 同址 once 两档（不被内部认领 + abort 不误删）；W14e 逐轮强制全部字段不允许默认值

**累计**：94 场景 309 断言，Debug×3 + Release×1 全绿。

---

## 第七轮（并行代理，基线 c57d0ce）：6 项修复，门禁日志绑定提交 367e9c4

最后更新：2026-07-28（第七轮）

**第七轮复审（基线 `c57d0ce`）1 高 + 4 中 + 1 低共 6 项全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/367e9c4_20260728_204024/`。提交 `2c5e9dc`（修复）+ `916dd04`（W14e 验收）+ `367e9c4`（全套件内部错误清扫）。详见 `GLEAM_CODE_REVIEW.md`。

- **`ignore` 绕过 stepout 内部断点（高）**：引擎在回调之后**无条件**消费一次性断点，所以任何在簿记之前的提前返回都会让 `mStepOutActive` 为真而物理断点已消失（目标静默跑到退出）。内部断点簿记抽为 `handleStepOutBreakpoint()` 并作为 `cbBreakpoint` **第一条语句**执行，用户态处理（ignore/rule）不可能抢在它前面；`mStepOutBpOurs = false` 提到 owner/non-owner 分支之上
- **quit/restart 统一中止（中）**：两命令均走 `abortStepOut()`（幂等）；`resetTransientState()` 清空全部 per-operation 字段，仅刻意保留单调 `mStepOutGen`
- **safe-step 恢复路径（中，引擎）**：裸 `ResumeThread` 两处改走带检查的 `resumeSuspendedThreads()`（失败报 TID+错误码）。**注意**：single-step 到达分支不能改用 `cleanupSuspensions()`——其后 `exceptionEvent()` 仍需 `isInternalStepping` 判别该事件。**该项验收标准 2（对 wait/continue/resume 失败与 `DetachAndBreak` 增加可注入测试）未实现**——引擎里没有故障注入缝，`DetachAndBreak` 也没有 Gleam 调用方，给共享引擎加测试钩子属于设计变更，留到下一轮决策；本轮以"全套件不得出现 `event error msg=`"扫描作为部分措施
- **W14 门禁收紧 + 矩阵补齐（中）**：逐轮判定（`ec=0` ∧ non-owner ≥ 1 ∧ return 恰好 1 ∧ 正常退出恰好 1）；新增 W14b（执行控制矩阵）、W14c（用户断点矩阵）、W14d（quit/restart/owner 退出）、W14e（ignore 叠加，高优先项的验收条件）
- **W15 判定重写（中）**：改为绑定↔加载事件配对判定（单遍 awk），并加"诱饵映像确实被识别"守卫
- **W13 重复块（低）**：删除较短的重复块，断言不再重复计数

**本轮另修 2 个引擎缺陷（不在评审条目内）**：① `Debugger::Init` 有命令行时把 `lpApplicationName` 置空，CreateProcessW 自行解析路径对**正斜杠相对路径**失败（`Gleam.exe bin/Debug/x64/TestTarget.exe dll4` 起不来，不带参数却能起）——既有缺陷非本轮回归；CI 一直绿是因为 `%BASH% -c` 转发时 Git Bash 把未加引号的 `$TARGET` 重写成了反斜杠路径。② 新的带检查恢复在正常 teardown 误报 `ERROR_INVALID_HANDLE`——`SuspendedThreads` 存的是系统句柄，continue 越过 EXIT_THREAD/EXIT_PROCESS 即失效，现只恢复仍在进程线程表中的条目。

**测试设计教训（已固化在脚本注释中）**：① 断言必须能区分"正确删除"与"错误删除"——W14c 初版把一次性断点设在 `rip` 上，随后的 `step` 会**合法**消费它，"从 bl 消失"无法判别对错；改为把一次性断点放到单步到不了的地址。② "零命中"类断言必须配"对象确实存在"的守卫，否则解析失败会让断言**空洞通过**（W15 诱饵识别守卫）。③ 纯几何区间判定在 W15 不成立：诱饵 NoExp 会**复用 Late#1 刚释放的同一基址与同一大小**。

**累计**：89 场景 297 断言，Debug×3 + Release×1 全绿。第 297 条是套件末尾新增的**全套件内部错误清扫**（任何场景都不得出现 `event error msg=`）——它正是发现引擎缺陷 ② 的手段，此前没有任何断言检查引擎内部错误输出。

**第六轮复审（基线 `fd7533d`）5 项未解决问题全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/c57d0ce_20260728_114407/`。提交 `c57d0ce`。

- **stepout 中止状态（高）**：`abortStepOut` 此前不清 `mStepArmed`，中止后排队单步异常形成意外 `stop reason=step`——统一入口同步清 `mStepArmed/mStepOverArmed`，`step/stepover/tgo/until`/新 ret 全部走该入口，两个执行控制状态机不再并存
- **内部断点所有权（高）**：不再仅凭地址删除——新增"物理武装且属于本代"标志（`mStepOutBpOurs`），任何线程命中即失去删除权；非 owner 消费后用户在同址下断不再被误删。W14 确定性矩阵（busyWorker + gate 对齐主线程慢 inner）：非 owner 消费 → 延迟再武装 → owner 正常完成
- **DLL 身份验证（高）**：修复"符号可解析即身份"的自我实现漏洞（resolvePdbSymbol 会把缓存文件按任意基址重定位加载）——匿名事件身份回退改为比对远程模块与缓存文件的 **CodeView GUID+Age+SizeOfImage**（含节表 RVA→文件偏移转换、`\??\` 路径规格化），符号加载与身份判断彻底分离；`mModulePaths` 随 restart 清理。W15：NoExp 诱饵不绑定、Late 换基址重载绑定命中、`selftest modid 2/2`
- **detach 恢复统一化（中，引擎）**：safe-step 恢复抽为统一清理函数，detach 分支、自然循环结束、错误 break 全部调用；`ResumeThread` 失败报 TID+错误码
- **验收矩阵（中）**：`protect` 新增 `n`（PAGE_NOACCESS）档；W13 补代理对跨块与页保护往返；V4a 恢复双 g 基线（运行态 pause 由 V4e 证明）；V4c 逐轮独立日志

**场景工程教训（已固化）**：非 owner 命中窗口的本质是"owner 的 call 在飞时间"，用 gate（`InterlockedExchange` 于 inner 入口 + busyWorker 自旋）可确定性对齐，不要靠概率；主线程慢 inner 只慢主线程（按 TID 判别），否则两个线程互相等对方窗口。

**累计**：86 场景 261 断言，Debug×3 + Release×1 全绿。

**第五轮复审（基线 `9231755`）4 项未解决问题全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/fd7533d_20260727_231523/`。提交 `fd7533d`。

- **detach 遗留挂起线程（高，引擎）**：safe-step 内部单步挂起其他线程，detach 只清 TF 不恢复——detach 迭代现在清 `isInternalStepping/isSingleStepping`、恢复 `SuspendedThreads` 全部条目，且 detach 期间不再新挂。V4e 30 次运行态 pause→detach，目标全部自行完成（完成状态按 PID 存活判定——detach 后目标输出随 gleam 退出不可见）
- **stepout 新 ret 遗留（高）**：`ret` 入口统一 `abortStepOut`（删内部断点+清两阶段再武装），W10 验证运行中 pause 中止后 bl 无遗留、新 ret 正常完成
- **CI 留证（中）**：`HEAD^{tree}` 加引号（cmd 吃 `^`）、git 命令失败即失败
- **验收矩阵（中）**：W11（延迟断点 unload/reload）、W11b（伪符号永不绑定）、W13（UTF-16 下一页不可读/前缀 partial/奇数地址/代理对）

**本轮实际复现的关键 bug（不在原评审条目内）**：DLL 卸载事件里引擎 `DeleteBreakpoint` 恢复字节失败（**卸载时只保证可读**），断点表残留导致 reload 永远绑不上——现失败时直接删引擎簿记；reload 事件 `hFile` 可能为 NULL 且无导出 DLL 无法命名——新增 `mModulePaths`（按模块名缓存首次真实路径）+ 符号身份回退（能在此基址解析即视为该模块）。W11 验证两次绑定两次命中。

**场景工程教训（已固化）**：stepout 的 1M tick 长忙窗口是获得"运行中 pause"的确定性手段；TestTarget looper 调用两次支撑重入场景；Release 下 DllMain 断点命中次数因配置而异，场景用 quit 收尾不数 g。

**累计**：83 场景 248 断言，Debug×3 + Release×1 全绿。

**第四轮复审（基线 `a29562d`）5 项未解决问题全部修复**，ci 门禁全阶段通过，正式日志：`ci_logs/9231755_20260727_153940/`。提交 `9231755`。

- **符号加载生命周期（高）**：`mSymLoadedBases` 原只在 DLL 卸载时清理，restart 后新进程复用同基址会跳过 `SymLoadModuleExW` 错过首次绑定；现 `closeSymSession`（逐个 `SymUnloadModule64` 配套）与 `resetTransientState` 均清空。W7b 验证 restart 第二会话仍在 DllMain 首次调用前绑定并命中（`event bp bound` 先于 `LATE_LOADED=1`）
- **stepout 再武装时序（高）**：非 owner 命中后引擎的内部单步尚未补执行原指令，同一事件写回 int3 导致二次命中——改两段式（命中登记 → 下一事件写回），写回失败报错终止；`cbStep` 加 owner TID 门控
- **跨页 code unit（中）**：页边界只是读取失败后的裁剪策略而非语义边界——先整块读（允许跨已提交页），失败才裁页尾；W8 断言 `read u16` 与 `read utf16` 对同一可读跨页地址结果一致
- **测试留证（中）**：`TDIR` 每轮独立目录（旧工件不串场）；压力循环逐次结构化日志；ci 开头记录 HEAD/tree hash/工作树状态；失败现场不再丢
- **V4（中）**：V4c 用 `Start-Process -PassThru` 记录本轮确切 PID（不再误杀用户 notepad），清理前校验映像路径；V4a 的 pause 改为运行中延迟发送（启动期 deferred pause 会因符号未解析丢失）

**降级记录**：ASLR 重试验收项降级为"引擎路径未启用，不适用"（gleam 无 `mDisableAslr` 入口，测它等于测产品不用的引擎路径），理由与修复思路见 `GLEAM_CODE_REVIEW.md` 附录。

**累计**：78 场景 235 断言，Debug×3 + Release×1 全绿。

**第三轮复审（基线 `570f7fb`）5 项未解决问题全部修复**，ci 门禁全阶段通过（clean rebuild Debug+Release、Debug 套件 3/3、Release 套件 1/1），正式日志：`ci_logs/a29562d_20260727_131653/`。提交 `a29562d`。

- 延迟断点（高）：PDB-only 符号在加载事件现场用 `SymLoadModuleEx` 按真实路径直接读符号绑定——此前靠"每个事件重试"原理上赶不上 DllMain 首次调用（load 事件 continue 与 DllMain 之间无调试事件）；显式加载在卸载时 `SymUnloadModule64` 配套防重载串符号。W7 场景裸跑 `bp Late!LateInternal`+`g` 命中首次调用
- stepout 所有权（高）：非 owner 线程命中内部断点时引擎消费一次性断点导致空等——现事件后重新武装、命中按普通暂停上报；代次参与命中校验；owner 线程退出中止
- 字符串页尾（中）：UTF-16 起始在页尾最后 1 字节凑不齐一个 code unit 时正确报错/报 partial，不再误报"无 NUL"（W8）
- frames 标注（中）：帧来源四档 `context/unwind/leaf/untrusted`，启发式帧不再冒充 unwind，与 best-effort 决策口径一致（W9，BoundaryTarget RWX 伪栈验证）
- 测试门禁（中）：二进制路径全参数化（GLEAM/TARGET/ATARGET/BTARGET）；ci 检查 Clean 退出码并归档每次运行全部场景输出；V4 加到 20 次 restart+后台退出码、5 次 attach/detach、Init 失败；V4a 暂停移到断点停止后发送（规避启动期 ExitThread 未解析的丢暂停竞态）

**累计**：77 场景 230 断言，Debug×3 + Release×1 全绿。

**第二轮复审（基线 `09183e5`）7 项问题全部修复**：

- stepout：call 后地址有用户断点时拒绝跳过并报错（不再单步进被调函数误报返回）；每 tick 严格计一次；`ret` 多余参数拒绝；内部断点登记 地址+TID+代次
- 延迟断点：绑定时刻的映像大小改为 `moduleBase` 直读 PE（不再依赖加载器列表，修复 `bp version+0` 类合法 pending 被误删）；"确定越界"才拒绝、"无法确认"保持 pending；`rebindPendingBreakpoints` 先收集再绑定（消除遍历删除失效）；重复 pending 规格 upsert 去重
- 异常策略修正为 verdict/x64dbg 语义：二次异常只有 `never+pass` 可放行（可能致死、显式），其余一律暂停+默认 swallow；first chance 暂停的 disposition 继承过滤器 handledBy。selftest 真值表更新为 16/16
- 地址错误不再跨命令粘滞（`mAddrError` 成功路径清理），`until/free/protect` 等补具体错误输出
- 字符串读取失败状态与地址分离（`read ansi 0` 不再误报）；`savemem` 用 `bytesRead` 保留成功前缀
- 子寄存器补全（r8d-r15d/w/b、sil/dil/bpl/spl）；raw DR 改为按 TID 登记，DR6 有 B0-B3 置位才报 `raw-hardware`，线程退出/restart/DR7=0 清理所有权
- frames（P0-2）按决策 (b) 定案：保留 best-effort 行为，README/help 改为诚实表述（非模块内存帧不可信，用 stackscan），verdict 正式废止四态门禁

**测试方法改进（"为什么没测出来"的整改）**：新场景一律从 verdict 矩阵直接派生（不再从实现派生）；每个功能必须带负例（失败路径/边界/非法参数）；跨命令序列状态单列场景（W3 错误串台）；新增 W1-W6 定向场景。

**测试目标库**：新增 `NoExp.dll`（无导出表，身份走 hFile 真实路径）、`Late.dll`（PDB-only 符号，重试路径）；TestTarget 新增 `dll2`（加载上述两 DLL）与 `av`（未处理访问违例，一/二次异常链）路径。注意：PE 头页是只读映射，rva=0 永远下不了软件断点，RVA 断点测试要用 ≥0x1000 的代码页偏移。

**本地 CI 门禁**：`ci_local.bat`——clean rebuild Debug+Release x64 → Debug 套件连跑 3 次 → Release 套件 1 次，日志存 `ci_logs/<commit>_<时间戳>/`。`run_tests.sh` 支持 `GLEAM`/`TARGET` 环境变量覆盖（Release 用）。Defender 已排除工作目录。指令定位（MRET/MCALLNEXT）改为反汇编探针，不再用固定偏移。

当前套件：74 场景 223 断言，Debug 全绿（PASS=223 FAIL=0）。



**验收口径**：`GLEAM_CODE_REVIEW.md`（2026-07-26 复审）暴露的 12 项问题（P0-1~P0-7、S0-1~S0-4、S1-1）已全部修复并逐场景验证；`run_tests.sh` 固定 RVA 已消除（全部运行时解析），clean rebuild 换链接布局后套件照常通过——复审的测试门禁要求已满足。74 场景 202 断言（增量构建下全绿；S5 高负载偶发 1/100 抖动，单独重跑 100/100，属环境噪声）。

**验收口径**：`GLEAM_CODE_REVIEW.md`（2026-07-26 复审）暴露的 12 项问题（P0-1~P0-7、S0-1~S0-4、S1-1）已全部修复并逐场景验证；`run_tests.sh` 固定 RVA 已消除（全部运行时解析），clean rebuild 换链接布局后套件照常通过。74 场景 202 断言（增量构建下全绿；S5 高负载偶发 1/100 抖动，单独重跑 100/100，属环境噪声）。`GLEAM_P0_REVIEW_VERDICT.md` 是验收口径的修订记录。

**修复轮遗留的平台事实（重要）**：DR 写在初始系统断点处会被内核在 continue 时抹掉（引擎 hbp 能工作是因为它在用户代码停止态逐事件重写 DR）——raw DR 必须在用户代码停止态设置；deferred pause 请求会被 restart 的 quitting 转换丢弃（pause 要在 restart 之后发）。

**两项有意的现状决策**（详见 `GLEAM_CODE_REVIEW.md`）：P0-2 frames 保持现状（检查类命令、与 x64dbg 同行为、理由成文）；ret 停止时机保持"执行 ret 后停在调用者"（与 x64dbg rtr 的 ret 前停不同，契约已写清）。stepout 已删除全部后向跳转快进（S0-2）。

**P0 七项原子功能（对应 `GLEAM_ATOMIC_FEATURE_GAPS.md` P0 全部）已实现**，实施顺序 1 表达式 → 4 延迟断点 → 2 frames → 3 异常 → 5 类型化内存 → 6 完整上下文 → 7 restart。设计参考：动手前对 x64dbg 源码（development 分支）做过 7 项逐一调研，参考源码克隆在 `C:\Users\14860\Desktop\Dev\_ref\`（本地，不入库）。

**1. 地址表达式求值**（新文件 `GleamCommands.Expr.cpp`）：

- 递归下降：`expr := unary (('+'|'-') unary)*`，`unary := '-' unary | '[' expr ']' | '(' expr ')' | atom`；atom = hex / 寄存器 / 模块基址 / `module!symbol`；`[expr]` 为 8 字节解引用
- `parseAddress` 改为调 `evalExpression`——**所有地址类命令一处改造全部受益**；新命令 `eval <expr>` 直接暴露求值（测试断言用）
- 语法决策：保留 Gleam 的 `module!symbol`（x64dbg 不支持这种写法，它的 `module:1234` 是序数不是 RVA），`module+rva` 经模块基址原子 + `+` 运算实现。裸 hex 名（`dead`）按 hex 解析
- 词法纪律：token 在解析层分类，不学 x64dbg 的字符串前缀猜测

**4. 模块相对延迟断点**（`LogicalBp`，Breakpoints.cpp + GleamDebugger.cpp）：

- `bp module!symbol` / `bp module+rva`：模块已加载立即绑定并登记逻辑项（重载可重绑）；未加载存 pending，DLL 加载事件绑定、卸载事件解绑（记录保留）、重载重绑。`bl` 显示 `pending`/`bound=0x...`；`rbp` 可按逻辑规格删 pending
- **关键坑（x64dbg 也没有的）**：DLL 加载事件发生时 `EnumProcessModules`/`GetModuleFileNameEx`/dbghelp 全都看不到新模块（加载器列表未链接）——绑定走**自读 PE 导出表**：`dllNameFromBase`（导出目录自带 DLL 名）+ `findExportByName`（导出表遍历），dbghelp 仅 fallback
- 主模块没有加载事件：`rebindPendingBreakpoints` 在系统断点/attach 断点处补绑
- `normalizeModuleName`（小写、剥路径、剥 .dll/.exe）为全局唯一归一化点，`findModule` 也用它

**2. frames 真实栈帧枚举**（Symbols.cpp，本轮最大的坑）：

- **StackWalk64 在 x64 不可用（实测）**：只填 `AddrReturn`，`AddrPC`/`AddrStack`/CONTEXT 全部不推进（换 dbghelp 原生回调也一样）→ 放弃，改用 **RtlVirtualUnwind**（ntdll_x64.lib 已链）
- RtlVirtualUnwind 本地运行、直接读本地指针 → 必须**镜像**：远程栈整段（一次，暂停态栈不变）+ 每帧函数代码+UNWIND_INFO 的 RVA span（假基址映射，ControlPc 重定基）。展开后 Rsp/Rbp 从镜像地址翻译回远程地址；Rbp 映射不到就原样透传（无帧指针函数不引用它，引用了也有 SEH 兜底）
- `RtlVirtualUnwind` 调用包 `__try/__except`（独立无对象函数，C2712 限制）——坏的远程展开记录不能崩调试器
- **UNWIND_INFO 字段**：flags = `hdr[0]>>3`、code 数 = `hdr[2]`（曾因错取 hdr[1]/hdr[3] 调了一轮）
- **链式展开信息**（UNW_FLAG_CHAININFO，CRT/系统 DLL 常见）：沿链逐段镜像全部 piece；**.pdata 间接表项**（`UnwindInfoAddress` 指回 .pdata 内部的真实记录，kernel32/ntdll 常见）在 `findRuntimeFunction` 里透明解析
- 输出 `frame #N rip=.. rsp=.. module=.. sym=..+0x.. source=unwind`；终止：rip=0 / 帧数上限(默认64硬顶256) / rip 不变 / rsp 下降；启发式不进 frames（`stackscan` 兜底，`bt` 保留）。`ret` 逻辑一行未动

**3. 异常处置与过滤器**（Control.cpp + GleamDebugger.cpp）：

- `ExFilter { breakOn: first/second/never, handledBy: pass/swallow }`，`mExFilters` 取代 `mIgnoredExceptions`（`ignoreexc` = `{never, pass}` 语法糖）
- `exception pass|handle`：当前异常停止的处置（pass=NOT_HANDLED 交还 SEH；handle=DBG_CONTINUE 吞掉，second chance 打警告）；`excfilter [add <code> [first|second|never] [pass|swallow] | del <code>]`，裸 `excfilter` 列表
- 过滤器命中且 chance 匹配时**覆盖 breakon 开关**强制暂停；`mPausedOnException` 标记当前暂停是否异常停止，任何恢复类命令清除
- x64dbg 经验：second chance 盲目 NOT_HANDLED = 杀进程，命令路径明确交给用户选择

**5. 类型化内存读取与 savemem**（Inspect.cpp）：

- `read u8|u16|u32|u64|ptr <addr>` 单值；`read ansi|utf16 <addr> [max]` 字符串（UTF-16 转 UTF-8）；指针链用表达式嵌套 `[[]]` 覆盖，不另设命令
- `savemem <addr> <size> <file>`：按 4KB 页流式写，失败页清零计洞（`holes=N`），上限 256MB；不像 x64dbg 一次性 malloc 全量还留未初始化垃圾

**6. 完整线程上下文**（Inspect.cpp）：

- `regs` 追加 DR0-7、MXCSR、XMM0-15 行；`setreg` 扩展 eflags/dr0-7（**dr4→dr6、dr5→dr7**，x64dbg 同款）/mxcsr/xmm0-15（32 hex 高位在前），经 `Registers::GetContext()` 改字段靠 RAII 析构写回
- **EFLAGS 写入有内核 sanitize**：IF 被强制置 1、保留位被清——测试断言要用不动点值（0x2D5），不要断言任意写入值
- YMM/ZMM（XSTATE）明确不做；32/16/8 位子寄存器不做

**7. 会话重启 restart**（main.cpp 会话循环 + `resetTransientState`）：

- `restart` = quit 的关闭路径（Stop + 清 stub）+ main.cpp 里 `takeRestartRequest()` 循环重新 `Init`+`Start`；GleamDebugger 对象不析构，状态天然跨会话
- 存活：逻辑断点（转 pending 随加载/系统断点重绑）、异常过滤、breakon、hide；清除：补丁（打印 `patches cleared on restart`，不自动重放——缺口文档的"校验后才应用"）、ignore 计数、线程选择、单步/trace/stepout 态
- 引擎侧已验证：`Debugger.Loop.cpp:270` 在 Start() 末尾清理 mProcesses/mProcess，二次 Init 可行
- attach 会话报 `restart requires a launched session`

**测试基建修复（与功能无关但阻塞验证）**：S1/S2/S4/S5 的 `Stop-Process -Name notepad` 会误杀下一轮新起的 notepad（powershell 启动慢，杀进程命令延迟落地）——全部改为从 gleam 输出提取 PID 精确杀。TestTarget 新增 `dll` argv 路径（LoadLibrary version.dll + 调 GetFileVersionInfoSizeW）；改 TestTarget 后地址常量未漂移（MARKER/INNER/GDATA/OEP 已重核）。

## 缺口文档（GLEAM_ATOMIC_FEATURE_GAPS.md）完成情况

最后核对：2026-07-25

**P0 基础原语：7/7 全部完成**

| # | 原子功能 | 状态 | 落点 |
|---|---|---|---|
| 1 | 地址表达式求值 | ✅ | `evalExpression` + `eval` 命令，所有地址类命令共用 |
| 2 | 真实栈帧枚举 | ✅ | `frames [tid] [n]`（RtlVirtualUnwind，非 StackWalk64；未复用到 `ret`——`ret` 刻意不看栈） |
| 3 | 当前异常处置 | ✅ | `exception pass\|handle` + `excfilter` add/del/list（first/second chance × pass/swallow） |
| 4 | 延迟模块断点 | ✅ | `bp module!symbol` / `bp module+rva`，pending/bound 全生命周期 |
| 5 | 类型化内存读取与导出 | ✅ | `read u8..ptr/ansi/utf16` + `savemem` |
| 6 | 完整线程上下文 | ✅ | GPR/EFLAGS/DR0-7/XMM0-15/MXCSR（YMM/ZMM 与 32/16/8 位子寄存器明确不做） |
| 7 | 会话重启 | ✅ | `restart`（逻辑断点/异常过滤/hide 存活，补丁按策略清除） |

**推荐实施顺序对照**：1-6、8 已完成；第 7 项（分析工作区 safe 持久化）未开始——restart 先于工作区落地，目前 restart 状态存活只靠 GleamDebugger 对象不析构，无跨进程持久化。

**P1 高价值辅助原语：9/10 完成（2026-07-31）**

已完成：`meminfo`、`peb/teb/tls`、段前缀地址表达式、段寄存器打印、`moduleinfo`/`sections`、符号控制（`sympath`/`symload`/`symreload` + 模块符号状态）、`OutputDebugString` 事件、有界指令跟踪（`stepn`）、断点启用/禁用/编辑（`bpdisable`/`bpenable`/`bpedit`）。逐项能力与取舍见缺口文档 P1 表。

**未完成：子进程跟随（1 项）**。唯一需要改引擎的一项，`GleeBug/Debugger.cpp:54` 仍无条件 `DEBUG_ONLY_THIS_PROCESS`。不是加 flag 就行：引擎 `mMainProcess`/`mProcess`/`mThread` 是单进程假设，开启后 `CREATE_PROCESS_DEBUG_EVENT` 会为第二个进程再次触发，命令层"当前进程"语义需重新定义。建议独立评审后单独提交。

**实施中偏离原方案的三处（均为刻意选择，理由已写入缺口文档）**：

1. **断点禁用改为物理移除**，而非本文档原先设想的"硬件/内存用禁用集合+自动继续"。留着 int3 的"禁用"断点对自校验目标仍可见、每次命中仍付异常代价，且不交还稀缺的 DR 槽（套件 P1BPE 用"填满 4 槽→禁用 1 个→第 5 个必须装得下"证明了槽确实交还）。代价是重新启用可能失败，此时如实报告并保持 disabled。
2. **命名用 `bpdisable`/`bpenable`** 而非 `bpd`/`bpe`：`hbpd`/`mbpd` 已表示硬件/内存断点的**删除**，一个含义相反的三字母近邻是陷阱。
3. **有界跟踪叫 `stepn`** 而非文档原写的 `trace n`：`trace <addr>` 已是 tracepoint，参数形态会相撞。

**段寄存器"写"以"平台不可实现"关闭**：x64 内核在 `SetThreadContext` 时丢弃用户态线程的段选择子，两个 base 又是派生值。`setreg` 明确报错而非假装成功。原表述中"读写"的写一半不是待办。

**自查中发现并修掉的四处状态漂移（`mDisabledBps` 按地址存，`LogicalBp.disabled/disabledAddr` 按条目存，同一事实两份记录会走散）**：

1. **删除路径不清理禁用记录**。`rbp`/`hbpd`/`mbpd` 在引擎断点表里找不到已禁用的断点，于是报「删除失败」并留下保存的规格：`bl` 出现一条只有 restart 才能清掉的幽灵 DISABLED 行，随后一次 `bpenable` 会**复活用户已删除的断点**。新增 `forgetBreakpointState()` 统一丢弃 `mDisabledBps`/`mBpRules`/`mIgnoreHits`/`mBpHits` 四张按地址索引的侧表，三条删除路径都经由它，删除已禁用断点也如实报成功。
2. **命中计数不随断点消亡**。`mBpHits` 按地址索引且从不在删除时清除，于是在同一地址新建的断点会继承死断点的计数。同上由 `forgetBreakpointState()` 覆盖。
3. **模块卸载漏掉已禁用条目**。`unbindModuleBreakpoints` 只扫 `boundAddr`，而禁用条目把地址停在 `disabledAddr`，导致保存的规格比模块活得更久——之后 `bpenable` 会把 int3 写进已解除映射的内存，或写进恰好加载到该地址的另一个模块。为此 `disableBreakpointAt` 刻意**保留 `boundBase`**（这是停放条目回到其模块的唯一线索）。逻辑条目的 disabled 状态作为用户意图保留，重载后仍关闭。
4. **`bpedit` 污染同伴条目**。原判据 `lb.disabled && !physical` 命中列表里**每一个**已禁用逻辑条目，于是编辑一个会改写其余所有条目的规则。改为按地址精确匹配 `lb.disabledAddr == addr`。该污染只在**重新绑定**应用 `lb.rule` 时才显形，故套件用 `restart` 作观测点（restart 清空保存规格但保留逻辑条目及其规则）。

四条都不会被原有断言发现：它们要么让删除少做事，要么让编辑多做事，输出看起来都正常。套件新增 P1BPG/P1BPH/P1BPI/P1BPJ 四个场景专门盯这四条。

**顺带修掉的两处套件缺陷（都属于"悄悄放水"而不是"报错"，比一条挂掉的断言危险）**：

1. **`gleam_drive.py` 的 `RESUME` 集合漏了新命令**。凡是返回 `CmdResult::Resume` 的命令都必须登记，否则驱动不武装暂停闸门，**下一条**命令会在目标仍在跑时被推入并由 `pushCommand` 防线 #1 静默丢弃——命令凭空消失且不报错。`stepn` 正是如此：紧跟其后的 `bl` 完全没有输出，而当时没有断言覆盖它，于是"通过"了。反过来，参数被拒的命令返回 `Handled` 却同样武装了闸门，白等一整个 `--pause-wait`（默认 20s），故参数拒绝类用例单独开会话并传 `-p 2`。
2. **套件启动时必须清空 `${TDIR}/gleam_*.txt`**。结尾的内部错误扫描按该模式 glob，而 S1 只在失败迭代写 `gleam_S1_fail_<i>.txt` 且无人清理——一次失败的运行会让之后每次运行都因同样的陈旧文件而变红。本次就有 5 个上午留下的残留被读成新的 break-in stub 回归，而 S1 实际 25/25 通过。

**MCP 前置条件：未开始**。当前命令输出是"统一 key=value 风格"但不是结构化协议：无稳定错误码、无分页、无会话状态字段。这是 M3 前必须做的一轮统一。

**P2 竞品调研补充层：0/13，已盘点入库（2026-07-25）**。按 WinDbg/CE/IDA/OllyDbg 官方资料逐项核实，主场景锚定"附加已运行进程"。T0：快照差量内存扫描（最大能力空白，CE 初扫+续扫）、通配批量断点（`bm`）、maps 用途分类+可打印字符串扫描、进程上下文速览；T1：Appcall 进程内函数调用（x64dbg 都没有的差异化项）、内存快照 save/restore（attach 场景唯一时光机）、wt 函数级跟踪统计；T2：hit trace 覆盖率、dps/dqs 智能指针 dump、CE 指针扫描；T3：结构剖析、ScyllaHide 四钩子（补 hide 的 NtQuery 局限）、句柄/堆枚举。明细在缺口文档 P2 节。

**分析工作区（SQLite）：未开始**。逻辑断点表（`mLogicalBps`）已是工作区"逻辑断点持久化"的内存雏形；`normalizeModuleName` + 模块+RVA 寻址可直接映射到文档的 `AddressRef` 设计。

---



## 仓库与环境

- 本地目录：`C:\Users\14860\Desktop\Dev\GleeBug`（目录名历史遗留，仓库实为 Gleam 项目）
- 远程：`fork` → `https://github.com/chensiling/Gleam`（主仓库），`origin` → `https://github.com/x64dbg/GleeBug`（上游，只读同步用）
- 分支模型：开发主线 `gleam`；`vs2015` 保持干净跟随上游；同步上游用 `git fetch origin` → `git rebase origin/vs2015` → `git push fork gleam --force-with-lease`
- 上游基线：commit `6d37c95`，本地 10 个提交在其上（构建适配、GleeArchValue 修复、Zydis v4.1.1 替换、Gleam 全部功能）
- IDE：Visual Studio 2022 Professional（仅 v143 工具集）
- 命令行构建：`"C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" GleeBug.sln -p:Configuration=Debug -p:Platform=x64 -m`
- 提交规范：commit 信息用简体中文；MD 文档只提交 `Gleam/README.md`，`PROJECT.md`/`PROGRESS.md` 保持本地不跟踪；推送由用户手动执行

## 解决方案结构（6 个工程）

| 工程 | 类型 | 说明 |
|---|---|---|
| GleeBug | 静态库 | 上游引擎（本地有未提交修改，见下） |
| MyDebugger | exe | 上游示例，仅作代码参考，跑不了（硬编码路径） |
| StaticEngine / TitanEngineEmulator | dll | 上游兼容层，与项目目标无关 |
| **Gleam** | exe | **本项目产物**：命令驱动无界面调试器 |
| **TestTarget** | exe | M2 验证用被调试目标（固定基址无 ASLR） |

## M2 完成情况

`Gleam` 工程实现（按职责拆分）：

- `Gleam\GleamDebugger.h`：`GleamDebugger` 类声明（继承 `GleeBug::Debugger`）
- `Gleam\GleamDebugger.cpp`：事件回调（`cbXxx`）+ 暂停态命令循环 `commandLoop()` + 线程选择 `currentThread()` + 断入管理（`requestPause`/`forceBreakIn`）
- `Gleam\GleamCommands.cpp`：命令分发（`executeCommand` → 五个 `try*Command` handler）、`parseHex`、`help`
- `Gleam\GleamCommands.Breakpoints.cpp`：`bp [once] [if] [do]`、`rbp`、`hbp`、`hbpd`、`mbp`、`mbpd`、`bl`、`ignore`、`trace`、`evalBpRule`/`parseCondition`/`evalCondition`
- `Gleam\GleamCommands.Inspect.cpp`：`regs`、`setreg`、`read`、`write`、`disasm`、`maps`、`modules`、`find`（含 ascii/utf16）、`bt`、`stackscan`、`patch`/`patches`/`restore`、`exinfo`、`threads`、`disasmOne`
- `Gleam\GleamCommands.Control.cpp`：`g`、`step`、`stepover`、`tgo`、`ret`、`until`、`detach`、`quit`、`thread`（含 suspend/resume）、`alloc`/`free`/`protect`、`ignoreexc`、`breakon`、`hide`
- `Gleam\GleamCommands.Symbols.cpp`：`imports`、`exports`、`sym`、`parseAddress`、`moduleEntryPoint`、`symNameByAddr`（dbghelp）
- `Gleam\GleamCommands.Hide.cpp`：反反调试 `applyHides`
- `Gleam\GleamCommands.Scan.cpp`：`xref`、`findasm` 线性反汇编扫描
- `Gleam\main.cpp`：入口（`gleam <exe>` 启动 / `gleam -a <pid>` 附加）+ REPL 线程

**停止记录统一格式（M3 的 JSON 基础）**：每次暂停输出单行 key=value：`stop reason=<原因> [细节] rip=0x... tid=<id>`。原因包括 `system`/`attach`/`entry`/`breakpoint`/`step`/`pause`/`exception`/`dll`/`thread`/`exit`。线程创建带 `start=0x... name=<符号>`；二次异常（chance=second）永远暂停不可关闭；信息行统一 `event ...` 前缀

**breakon 开关**（`breakon <entry|dll|thread|exception> [on|off]`，裸 `breakon` 查状态）：entry/dll/thread 默认 off，exception 默认 on（保持历史行为）。注意 OEP 断点是**惰性设置**的（`applyEntryBreakpoint`）——开关常在系统断点处打开，晚于进程创建事件，必须在开时补设

**逆向功能第二批**（63 项断言全过）：

- `hide`（`GleamCommands.Hide.cpp`）：PEB.BeingDebugged/NtGlobalFlag/堆标志清理 + `IsDebuggerPresent`/`CheckRemoteDebuggerPresent` API 补丁；系统断点处自动应用（`mHideOn`）。**局限**：NtQueryInformationProcess 类检测未覆盖（需 ntdll hook）
- `trace` + 条件断点：`BpRule`（`mBpRules`），命中时由 `evalBpRule` 在调试线程核心内判定（条件不满足自动继续 / tracepoint 打印 `trace address=...` 自动继续），不走暂停——高频路径必须留核心，勿移到 MCP
- `sym`（地址反查）、`until`（一次性断点语法糖）、`find ascii|utf16`（字符串搜索）
- `patch`/`patches`/`restore`：补丁管理，原始字节存 `mPatches`
- `stackscan`：扫 RSP 起 qword，落在可执行段的标注并用 dbghelp 解析符号（naive `bt` 的 FPO 补充）

**逆向功能第三批**（70 项断言全过）：

- `thread [tid] suspend|resume`：线程挂起/恢复（引擎 `Thread::Suspend/Resume` 现成；省略 tid 作用于事件线程——**测试里 tid 每次运行都变，不要写死**）
- `alloc`/`free`/`protect`：目标内存分配与页属性修改
- `xref`/`findasm`（`GleamCommands.Scan.cpp`）：线性反汇编扫描全部可执行区域。注意：指令目标从**原始字节**计算（E8/E9 rel32、FF /2 /4 rip 相对）——扫描类代码的纪律保留，不依赖库内部状态

**stepout 全面重做**（`ret` 命令）：核心内单步循环 + 三特判——`ret` 执行后停在**调用者**；`call` 用一次性断点跳过；后向跳转识别为循环回边，循环出口下一次性断点**原生速度快进**。不看栈、不看帧、不看展开数据，FPO/壳/shellcode 通用（100k 迭代实测 22 个事件完成）。设计说明见 `GLEAM_CODE_REVIEW.md`。`stackWalkReturn`/`checkUnwindRecord` 代码保留供 bt 升级复用，不再被 `ret` 调用

**逆向功能第四批**（75 项断言全过）：

- `tgo <reg><op><val> [max] [log]`（条件跟踪）：核心内单步循环，`cbStep` 里 `evalCondition` 判定，满足或到上限（默认 0x10000）才停，输出 `stop reason=trace condition|maxreached steps=N`。条件解析/求值与条件断点共用 `parseCondition`/`evalCondition`
- `bp <addr> do <命令>`：命中时由 `evalBpRule` 在暂停上下文执行任意命令；恢复类命令（g/step）不暂停直接继续——`trace` 是 `do g` 的特例

**代码审查修复批·第三轮（89 项断言全过）**：

- pause stub 断入识别改为**异常地址判定**（`ExceptionAddress == stub 页`），与簿记时序彻底解耦；stub 句柄原子化
- `ret` 支持 FPO/优化函数中段：栈扫描找"可执行地址且上一条指令是 call"的候选（`call rel32`/`call [rip]`/`call r/m` 三种前导形态），输出标注 `(stack scan)`；帧指针路径标注 `(frame)`
- 补丁桥接合并：`merged` 逐轮携带并集，多个旧记录被一个新补丁覆盖时原始字节不丢
- PE thunk 遍历按 `SizeOfImage` 裁剪，畸形 `FirstThunk`/IAT RVA 直接跳过
- REPL 退出用 `ExitProcess`（CRT 清理前终止全部线程）
- `run_tests.sh` 末尾 `[ "$FAIL" -eq 0 ]`，失败返回非零
- 新增场景：R4（Release/FPO 的 ret，需 Release 二进制，缺则跳过）、R5（桥接补丁）、S1（25 连发 pause 注入压力，notepad 真实进程）

**代码审查修复批·第二轮**：

- 断点规则/忽略计数在 `cbBreakpoint` 入口先快照再清理（一次性断点的 do g/trace/ignore 等早退路径也不再泄漏）；`rbp` 同步清 ignore
- `evalBpRule` 改为接收规则快照参数
- REPL 生命周期：`GleamDebugger` 堆分配有意泄漏，detach 的 REPL 线程不再访问栈上对象
- `ret` 的 `[rsp]` 回退同样做可执行校验，两个候选都不合法时报错而不是乱下断
- 参数重组实现完整 Windows 命令行引用规则（空参数、内嵌引号、结尾反斜杠）
- 补丁记录支持重叠/变长合并：旧记录的更早原始字节优先，新范围只补未覆盖部分
- 扫描每块多读 15 字节，跨块指令完整解码
- 导入遍历按数据目录 Size 定界（带硬上限兜底）
- `hide` 重复启用不再清空首次原始记录；`CheckRemoteDebuggerPresent` 补丁改为置假并返回 TRUE（原来返回 FALSE 等于 API 失败）

**代码审查修复批·第一轮**：

- `ignoreexc` 语义改正：不暂停并保持 `DBG_EXCEPTION_NOT_HANDLED` 交还目标 SEH（原来用 `DBG_CONTINUE` 是吞异常）
- REPL 退出卡死：`repl.detach()` 替代 `join()`；命令行参数加引号保真；`std::getline` 替代定长缓冲
- 扫描越界读修复（块尾按剩余长度解码）
- `ret` 栈帧感知：优先 `[rbp+8]`（帧链校验+可执行校验），回退 `[rsp]`
- 一次性断点命中后清理 `mBpRules`/`mIgnoreHits`；`breakon entry off` 撤销 OEP 断点
- `patch` 只记录首次原始字节；`find` 上限 256MB；`imports` 遍历上限
- `hide off` 恢复全部修改（`mHideOriginals` 反向回写）；dbghelp 会话 `SymRefreshModuleList` 同步运行时 DLL
- Win32 配置已从 sln 和全部工程删除（x64 only）
- **引擎行为备忘**：一次性断点命中后的同一暂停里，在相同地址重设断点会立即再次命中（被恢复的原指令还没执行，新断点自然捕获）——不是 bug，写测试时预期要算进去

架构要点：

- 调试循环（`Init`/`Attach` + `Start`）在主线程；REPL 线程从 stdin 读命令进队列（`std::queue` + 条件变量）
- 被调试程序因事件挂起时（系统断点/attach 断点/断点命中/单步/未处理异常），调试线程进入 `commandLoop()` 逐条执行队列命令；`g`/`step`/`stepover`/`ret`/`detach`/`quit` 退出暂停态恢复运行
- 目标进程以 `newConsole=false` 启动，其 stdout 与 Gleam 输出混在一起，方便脚本化验证
- `attach` 场景必须重写 `cbAttachBreakpoint` 并请求暂停——attach 不会触发 `cbSystemBreakpoint`，否则永远没有暂停点、命令永远无法执行
- **REPL 语义（重要）**：除 `pause` 外所有命令都进队列、只在挂起状态执行；运行中的目标要 detach/quit 必须先 `pause`。EOF 不自动 quit（脚本必须显式写 quit/detach 结尾）。不要尝试"运行中立即执行 detach/quit"——它和脚本顺序执行语义根本冲突，两次尝试（EOF 强断入、队空强断入）都造成了回归
- `requestPause` 采用 **set-then-check**：请求先发布（`mPauseAfterResume`），双端竞速 `exchange` 消费；`forceBreakIn` 在 `mBreakInMutex` 下执行并与清理/退出串行化；**`mProcess` 未就绪时请求重新入队而不是丢弃**（启动竞速的 1% 丢暂停根因）；`mQuitting` 为原子量，延迟请求消费点在 `cbPostDebugEvent` 末尾且先查 `mQuitting`
- `pause` 的断入通过**自注入 stub 线程**实现：会话级单页复用（会话结束才释放，杜绝"释放后线程还在页上执行"）；线程 `CREATE_SUSPENDED` 创建、发布页和句柄后才 `ResumeThread`（无发布竞态）；stub 为 `int3 + call ExitThread` 自我清理；断入用**异常地址**识别（stub 页 / `DbgUiRemoteBreakin` 地址 / 兜底标志，按精确度排序）；`ExitThread` 与 `DbgUiRemoteBreakin` 地址在调试线程解析缓存，未解析到时**每个事件机会重试**（日志限流）；清理顺序 `TerminateThread → WaitForSingleObject → CloseHandle → 确认后才 VirtualFreeEx`，等待超时保留页和句柄并打可见日志。**不要改回 `DebugBreakProcess`**（检查 `PEB.BeingDebugged`，hide 清零后失效）

## 测试

`run_tests.sh`（仓库根目录）：89 个场景 297 项断言，当前全过（含 3 次连续运行一致、100 连发 pause 注入、100 会话生命周期）。运行方式：`bash run_tests.sh`（Git Bash），失败返回非零退出码。**地址全部运行时解析**（TestTarget 自己打印 MARKER/INNER/GDATA，MRET/MCALLNEXT 走反汇编探针）——固定 RVA 是复审门禁明令禁止的，改 TestTarget 源码后无需同步任何脚本常量。可用 `GLEAM`/`TARGET`/`ATARGET`/`BTARGET` 覆盖二进制路径（Release 轮次即如此），`TDIR` 指定每轮独立工件目录。另有 ArgvTarget（argv 引用验证）和 BoundaryTarget（跨 1MB 块指令扫描，固定分配地址 `0x60000000`）两个专项测试工程。

dbghelp 使用要点（`GleamCommands.Symbols.cpp`）：

- `SymInitialize(hProcess, NULL, TRUE)` 必须 **invade**——否则 dbghelp 不知道目标进程里有哪些模块，`SymFromAddr`/`SymEnumSymbols` 全部静默失败
- 会话在首次符号命令时建立（懒加载），`cbExitProcessEvent` 里 `SymCleanup`
- 延迟导入描述符结构体在 MSVC 的 `delayimp.h`（`ImgDelayDescr`，字段 `rvaDLLName`/`rvaIAT`），注意 `grAttrs & 1` 区分 RVA/VA
- Gleam 链接依赖已含 `dbghelp.lib`

另用真实程序 `C:\Windows\notepad.exe` 做过人工验证：`modules`/`maps`/`threads`/`bt`/`disasm` 正常，`pause` 能中断空闲 GUI 进程，`pause`→`detach` 后 notepad 存活、`gleam` 干净退出。注意 `newConsole=false` 下目标继承 gleam 的 stdout，脚本里管道要等目标退出才会关闭（用 `timeout` 兜底）。

## 上游 GleeBug bug（已在本地修复，见 `git diff`）

0. **vendor Zydis 已整体替换为官方 v4.1.1**（用官方 `assets/amalgamate.py` 自行合并生成，枚举与解码表一致，`ZYDIS_MNEMONIC_CALL`=71 两头对齐）。生成物有两处本地微调：`#include <Zydis.h>` 改 `"Zydis.h"`、头文件顶部加 `#define ZYDIS_STATIC_BUILD`。重新生成步骤：`git clone --depth 1 -b v4.1.1 zyantific/zydis` → `git submodule update --init --depth 1` → `python assets/amalgamate.py` → 产物在 `amalgamated-dist/`
1. **Zydis 枚举错位**：已通过将 vendor Zydis 整体替换为官方 v4.1.1 根治（见第 0 条）。早期临时方案 `MnemonicIs()` 已**回退**，`StepOver`/`IsRepeated`/`StepInternal` 恢复标准枚举比较——目前 `Debugger.Process.cpp` 与上游的差异仅剩 `GleeArchValue` 两行。纪律保留：扫描类代码（xref/findasm）仍用原始字节判断指令目标，不依赖库内部状态
2. **`GleeArchValue` 参数写反**（同文件两个调用点）：已换成正确的 `(x32, x64)` 顺序，x64 构建现在用 64 位模式解码
3. Gleam 的 `stepover` 已换回引擎 `Process::StepOver`（自研绕行代码已删），测试证明修复生效
4. 这些修复值得回馈上游（给 x64dbg/GleeBug 提 PR）；`git pull` 前注意冲突

**vcpkg 坑**：本机有 vcpkg 全局集成，会自动把 `x64-windows-static\lib` 里所有 lib（含一个 Zydis.lib）链进每个 vcxproj，与 vendor 的 Zydis.obj 重复定义。已在 `Gleam.vcxproj` 和 `GleeBug.vcxproj` 全部配置加 `<VcpkgEnabled>false</VcpkgEnabled>`。**新建工程时必须加这一项**

## 关键事实（接手前必读）

- **链接 ntdll**：凡是要用到 `Debugger::Init/Start` 的工程，必须在链接器附加依赖里加 `psapi.lib` 和 `$(SolutionDir)GleeBug\ntdll_x64.lib`（Win32 用 `ntdll_x86.lib`）。上游 CMake（`cmake.toml`）就是这么配的，但 `.vcxproj` 没配——MyDebugger 不需要只是因为它的 `testDebugger()` 被注释掉、链接器没拉入 `Debugger.obj`
- **寄存器枚举**：`R` 是嵌套在 `GleeBug::Registers` 类内的枚举（头文件在类体内 `#include`），外部要用 `Registers::R`。通用读写用 `Registers::Get(R)/Set(R, ptr)`，但只对 64 位寄存器安全（32 位项会越界读写相邻字段）；读全部寄存器用类型化成员（`r.Rax()` 等）
- **单步语义**：`Thread::StepInto()` 触发后由 `cbStep()` 回调通知；GleeBug 内部恢复断点也会产生 `cbStep`，用户步进要用标志位（`mStepArmed`）区分
- **暂停模型**：所有命令必须在调试线程的事件回调上下文里执行（此时被调试进程已挂起），不能从 REPL 线程直接调 `mProcess` 方法
- `TestTarget` 用 `/FIXED /DYNAMICBASE:NO`（x64 配置），地址跨运行稳定，自动化断言依赖这一点
- GleeBug API 完整范例在 `MyDebugger\MyDebugger.h`（各类回调的签名和用法）

## 下一步

优先级排序（详见上方"缺口文档完成情况"）：

1. **MCP 前置条件统一**（M3 的硬门槛）：命令结果结构化——稳定错误码、PID/TID/会话状态字段、大块内存与长列表分页。不动功能语义，只统一输出协议。
2. **M3：MCP 封装**：在命令驱动层外包 MCP server（stdio 传输），tools 映射现有原子命令：`launch`、`attach`、`set_breakpoint`、`continue`、`step`、`read_registers`、`write_register`、`read_memory`、`write_memory`、`list_threads`、`pause` 等。参考用户已有 MCP 项目：`C:\Users\14860\Desktop\Dev\Windbg-MCP`、`C:\Users\14860\Desktop\Dev\x64dbg mcp`、`C:\Users\14860\Desktop\Dev\IDA-MCP`。验证标准见 `PROJECT.md` M3：真实 LLM 客户端自然语言驱动完成完整调试任务。
3. **P1 辅助原语**（按需插队在 M3 前做也行）：断点启用/禁用/编辑对 LLM 调试场景价值最高；`meminfo`、`peb/teb` 次之。
4. **P2 T0 三项**（可插队，性价比最高）：快照差量内存扫描（CE 初扫+续扫，当前最大能力空白）、通配批量断点（exports 通配→循环下断，纯增量）、maps 用途分类+可打印字符串扫描。T1 的 appcall/内存快照/wt 属差异化，视 MCP 进度排期。
5. **分析工作区（SQLite safe 持久化）**：缺口文档给了完整设计与实施顺序（先 `AddressRef`/`BinaryRevision` 地基，逻辑断点表已有内存雏形）。
