# 第八轮复审修复总结

## 概述

本次修复完成了代码审查文档 `GLEAM_CODE_REVIEW.md` 中提出的 4 个问题（SYM-1、SYM-2、C3-R5-R、C3-R6）。

## 修复详情

### ✅ SYM-1（高）：pending DLL 的 PDB-only 回退绕过符号消歧

**问题**：`bindModuleBreakpoints()` 在严格 `resolveModuleSymbol()` 失败后，仍继续调用 `resolvePdbSymbol()`；后者直接使用 `SymFromName` 任选记录，导致歧义符号可能绑定到错误的地址。

**修复**：
- 引入 `SymbolResult` 枚举三态：`Found` / `NotFound` / `Ambiguous`
- `resolveModuleSymbol` 和 `resolvePdbSymbol` 统一使用 ILT 消歧逻辑
- `resolvePdbSymbol` 不再直接调用 `SymFromName`，而是复用 `resolveModuleSymbol` 的完整流程
- `bindModuleBreakpoints` 遇到歧义符号时明确拒绝并从 pending 列表删除
- Late.dll 新增 `ambig_a.cpp` 和 `ambig_b.cpp` 提供测试样本

**影响文件**：
- `GleamDebugger.h`: 新增 `SymbolResult` 枚举
- `GleamCommands.Symbols.cpp`: 统一消歧逻辑
- `GleamCommands.Expr.cpp`: 处理三态返回值
- `GleamCommands.Breakpoints.cpp`: 检测并拒绝歧义符号
- `GleamDebugger.cpp`: 绑定时拒绝歧义
- `Late/`: 新增歧义测试样本

### ✅ SYM-2（中）：歧义状态跨命令污染

**问题**：`mSymbolAmbiguous` 是全局布尔值，只在下一次模块符号解析时才复位；`bp` 对任何未解析的逻辑规格都读取它，导致跨命令污染。

**修复**：
- 移除全局 `mSymbolAmbiguous` 布尔值
- 歧义状态通过 `SymbolResult` 返回值传递，不再跨命令残留
- `bp` 命令通过检查 `mAddrError` 中是否包含 "ambiguous" 来判断歧义
- `eval` 命令不重复打印歧义错误（`resolveModuleSymbol` 已打印详细版）

**影响文件**：
- `GleamDebugger.h`: 移除 `mSymbolAmbiguous`
- `GleamCommands.Breakpoints.cpp`: 检查 `mAddrError` 而非全局状态
- `GleamCommands.Inspect.cpp`: 抑制重复错误打印

### ✅ C3-R5-R（中）：stub 发布前写入失败的页面泄漏

**问题**：`ensureBreakInStub()` 在 `WriteProcessMemory` 失败时直接调用未检查的 `VirtualFreeEx`，但页面尚未登记到 `mBreakInStubPage`；若释放同时失败，页面地址丢失。

**修复**：
- `WriteProcessMemory` 失败且 `VirtualFreeEx` 也失败时，将页面登记到 `mBreakInStubPage`
- 后续清理（`freeBreakInStubPage`）可以重试释放

**影响文件**：
- `GleamDebugger.cpp`: `ensureBreakInStub` 中的双失败处理

### ✅ C3-R6（中）：hide on 下 stub ResumeThread 失败的处理

**问题**：stub `ResumeThread` 失败后回退到 `DebugBreakProcess`；但 `hide on` 下 `DebugBreakProcess` 会失败（因为它检查 `PEB.BeingDebugged`）。

**修复**：
- `ResumeThread` 失败后尝试终止线程
- 非 `hide on` 时使用 `DebugBreakProcess` fallback（保持原始行为）
- `hide on` 时明确告知用户 fallback 不可用，延迟到下一个自然事件

**影响文件**：
- `GleamDebugger.cpp`: `forceBreakIn` 中的 hide on 判断

## 测试修复

为了验证修复，添加/修改了以下测试：

1. **SYM-1**：pending DLL 歧义符号在绑定时被拒绝
2. **SYM-2**：跨命令污染回归测试
3. **测试调整**：
   - 移除 `bp 0x1000`（无效地址）
   - SYM-1 添加第二次 `g` 让目标完整运行
   - 修正歧义错误打印次数期望

## 提交记录

- **793dbd3**: 修复第八轮复审 SYM-1/SYM-2/C3-R5-R/C3-R6 四项问题
- **cbba0e1**: 修复 SYM-1/SYM-2/W17 测试失败：歧义错误去重与测试调整

## 测试结果

完整测试套件运行中（116 场景 389 断言）...

## 遗留工作

- 更新 `GLEAM_CODE_REVIEW.md` 标记这 4 项为已关闭
- 验证完整测试套件通过
