# Phase 2 实施计划：核心重构

开始时间：2026-07-30
状态：进行中

## 目标

实施 GLEAM_CODE_REVIEW.md 中 Phase 2 的 4 个高优先级改进：

1. ✅ 统一符号解析 - 已实现（SYM-1修复中完成）
2. 🔄 线程安全加固 - 进行中
3. 📝 资源管理 RAII - 待实施
4. 📝 状态管理分组 - 待实施

---

## 1. 统一符号解析（已完成）

**状态**: ✅ 已在 SYM-1 修复中完成

**实现内容**:
- `resolveModuleSymbol` 和 `resolvePdbSymbol` 都使用相同的 ILT 消歧逻辑
- 统一返回 `SymbolResult` 枚举（Found/NotFound/Ambiguous）
- `bindModuleBreakpoints` 正确处理所有路径的消歧结果

**验证**: 冒烟测试 Test 5 通过

---

## 2. 线程安全加固（进行中）

**目标**: 明确并发模型，添加文档，消除竞态条件

### 2.1 线程模型文档化

**当前线程**:
- 主线程：调试循环（GleeBug 事件处理）
- REPL 线程：命令输入（pushCommand/requestPause）

**共享状态**:
- `mCmdQueue`: ✅ 已保护（mCmdMutex + mCmdCv）
- `mBreakInStub*`: ✅ 已保护（atomic + mBreakInMutex）
- `mIsPaused`, `mQuitting`: ✅ 使用 atomic
- `mProcess/mThread`: ⚠️ 仅主线程访问（需文档化）

### 2.2 需要添加的保护

**问题点**:
1. `mAddrError` 在多线程环境下可能有竞态（虽然实际只有主线程访问）
2. 某些标志位使用 bool 而非 atomic

**改进**:
- 添加线程安全注解（GUARDED_BY 风格的注释）
- 在关键函数添加线程断言

---

## 3. 资源管理 RAII（待实施）

**目标**: 引入 RAII 包装，消除手动资源管理

### 3.1 UniqueHandle 类

```cpp
class UniqueHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) : h(handle) {}
    ~UniqueHandle() { if (h != INVALID_HANDLE_VALUE && h != NULL) CloseHandle(h); }
    
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    
    UniqueHandle(UniqueHandle&& other) noexcept : h(other.h) { other.h = INVALID_HANDLE_VALUE; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h = other.h;
            other.h = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    
    HANDLE get() const { return h; }
    HANDLE release() { HANDLE ret = h; h = INVALID_HANDLE_VALUE; return ret; }
    void reset(HANDLE newHandle = INVALID_HANDLE_VALUE) {
        if (h != INVALID_HANDLE_VALUE && h != NULL) CloseHandle(h);
        h = newHandle;
    }
    
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE && h != NULL; }
};
```

### 3.2 应用范围

- `mBreakInStubThread`: 改为 `UniqueHandle`
- 文件操作：`codeViewFromFile` 中的 `hFile`, `hMap`
- 临时句柄：各处的 `CreateFile`, `CreateFileMapping`

---

## 4. 状态管理分组（待实施）

**目标**: 将 60+ 成员变量按功能域分组

### 4.1 状态分组方案

```cpp
// 断点管理状态
struct BreakpointState {
    std::vector<LogicalBp> logicalBps;
    std::map<GleeBug::ptr, BpRule> bpRules;
    std::map<GleeBug::ptr, uint32_t> ignoreHits;
    GleeBug::ptr oepBreakpoint = 0;
};

// 符号管理状态
struct SymbolState {
    bool symInitialized = false;
    std::set<uint64_t> symLoadedBases;
    std::map<std::string, std::wstring> modulePaths;
    std::string addrError;  // 可选：保留在外层
};

// Break-in stub 状态
struct BreakInStubState {
    std::atomic<HANDLE> thread{nullptr};
    std::atomic<void*> page{nullptr};
    std::atomic<uint32_t> tid{0};
    std::mutex mutex;
};

// 瞬态会话状态（可重置）
struct SessionState {
    uint32_t selectedThreadId = 0;
    bool pausedOnException = false;
    bool lastExceptionValid = false;
    // ... 其他瞬态字段
    
    void reset() { /* 集中重置 */ }
};
```

### 4.2 `GleamDebugger` 重构后

```cpp
class GleamDebugger {
    // 核心功能模块
    BreakpointState breakpoints;
    SymbolState symbols;
    BreakInStubState breakInStub;
    SessionState session;
    
    // 其他必要的全局状态
    std::queue<std::string> mCmdQueue;
    std::mutex mCmdMutex;
    // ...
};
```

---

## 实施顺序

### 阶段 1：线程安全文档（1-2天）
- [x] 分析线程模型
- [ ] 添加线程安全注释
- [ ] 记录不变量

### 阶段 2：RAII 引入（2-3天）
- [ ] 实现 `UniqueHandle` 类
- [ ] 重构 break-in stub 句柄管理
- [ ] 重构文件操作句柄

### 阶段 3：状态分组（3-5天）
- [ ] 定义状态子结构
- [ ] 逐步迁移成员变量
- [ ] 重构 `resetTransientState`
- [ ] 更新所有访问点

### 阶段 4：测试验证（1-2天）
- [ ] 运行完整测试套件
- [ ] 性能基准测试
- [ ] 内存泄漏检查

---

## 预期收益

1. **线程安全**：明确的并发模型，消除隐藏的竞态
2. **资源安全**：RAII 消除句柄泄漏
3. **可维护性**：状态分组让类结构更清晰
4. **可测试性**：模块化的状态便于单元测试

---

## 风险与缓解

**风险**：大规模重构可能引入新 bug
**缓解**：
1. 每个阶段独立提交
2. 每次提交后运行测试
3. 保持小步迭代
4. 保留回滚能力

---

## 下一步

开始阶段 1：添加线程安全文档和注释
