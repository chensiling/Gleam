# 全局状态审计报告

## 发现的全局变量

### 1. `g_logLevel` (Log.h/Log.cpp)
- **类型**: `LogLevel` (enum class)
- **用途**: 控制日志输出的最低级别过滤
- **使用位置**: `logImpl()` 中检查日志级别
- **依赖关系**: 所有日志函数（logDebug/logInfo/logWarn/logError）
- **问题**: 
  - 全局可变状态，多线程不安全
  - 测试时无法隔离不同的日志级别
  - 无法支持多个 GleamDebugger 实例使用不同日志级别

### 2. `g_perfStats` (Performance.h/Performance.cpp)
- **类型**: `PerfStats` (class)
- **用途**: 收集性能统计信息（计时、缓存命中率）
- **使用位置**: 
  - `PerfRecorder` 析构函数
  - `PERF_RECORD()` 宏
  - `PERF_CACHE_HIT()` 宏
- **依赖关系**: 
  - GleamCommands.Symbols.cpp 中的符号解析
  - 任何使用 PERF_RECORD 的代码
- **问题**:
  - 全局可变状态，多线程访问需要同步
  - 测试时统计数据会混合，无法隔离
  - 无法为不同调试会话收集独立的统计

### 3. 静态常量 (不是问题)
- `kStepOutMode` (GleamCommands.Control.cpp)
- `kMachineMode` (GleamCommands.Inspect.cpp)
- 这些是编译期常量，不需要消除

### 4. 静态局部变量 (需要评估)
- `static auto queryThread` (GleamCommands.Hide.cpp:58)
- 这是函数内静态变量，用于缓存 API 地址
- 需要检查是否线程安全

## 消除方案

### 方案 A: 注入到 GleamDebugger
将全局状态作为 GleamDebugger 的成员变量：

```cpp
class GleamDebugger : public GleeBug::Debugger {
private:
    LogLevel mLogLevel = LogLevel::Info;
    PerfStats mPerfStats;
    
public:
    void setLogLevel(LogLevel level) { mLogLevel = level; }
    LogLevel getLogLevel() const { return mLogLevel; }
    
    PerfStats& perfStats() { return mPerfStats; }
    const PerfStats& perfStats() const { return mPerfStats; }
};
```

**优点**:
- 每个 GleamDebugger 实例独立
- 支持多实例
- 易于测试

**缺点**:
- 需要传递 GleamDebugger* 到所有使用日志/性能统计的地方
- API 变更较大

### 方案 B: 创建 Context 对象
引入 `GleamContext` 持有所有可变状态：

```cpp
class GleamContext {
public:
    LogLevel logLevel = LogLevel::Info;
    PerfStats perfStats;
    
    // 可扩展：未来可添加更多状态
};

class GleamDebugger : public GleeBug::Debugger {
private:
    GleamContext mContext;
public:
    GleamContext& context() { return mContext; }
};
```

**优点**:
- 状态集中管理
- 扩展性好
- 可以独立测试 Context

**缺点**:
- 仍需要传递 Context 引用
- 引入新的抽象层

### 方案 C: Logger 和 PerfMonitor 对象
将功能封装为独立对象：

```cpp
class Logger {
private:
    LogLevel mLevel = LogLevel::Info;
public:
    void setLevel(LogLevel level) { mLevel = level; }
    void log(LogLevel level, const char* prefix, const char* fmt, ...);
    // ... logDebug/logInfo/logWarn/logError
};

class PerfMonitor {
private:
    PerfStats mStats;
public:
    PerfStats& stats() { return mStats; }
    PerfRecorder record(PerfEvent event, const std::string& details);
};

class GleamDebugger : public GleeBug::Debugger {
private:
    Logger mLogger;
    PerfMonitor mPerfMonitor;
public:
    Logger& logger() { return mLogger; }
    PerfMonitor& perfMonitor() { return mPerfMonitor; }
};
```

**优点**:
- 职责明确，单一职责原则
- Logger 和 PerfMonitor 可独立测试
- 可以方便地 mock

**缺点**:
- 需要修改所有调用点
- 代码改动量最大

## 推荐方案

**混合方案（C 的简化版本）**:
1. 创建 `Logger` 类封装日志状态和方法
2. 创建 `PerfMonitor` 类封装性能统计
3. GleamDebugger 持有这两个对象的实例
4. 提供静态的 "当前" 访问器用于过渡期

```cpp
class Logger {
private:
    LogLevel mLevel = LogLevel::Info;
public:
    void setLevel(LogLevel level);
    LogLevel getLevel() const;
    
    void debug(const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);
    void event(const char* fmt, ...);
    void stop(const char* reason, const char* details, uint64_t rip, uint32_t tid);
    
private:
    void logImpl(LogLevel level, const char* prefix, const char* fmt, va_list args);
};

// 全局默认实例（过渡期）
extern Logger* g_defaultLogger;

// 便捷宏（过渡期）
#define LOG_DEBUG(...) (g_defaultLogger ? g_defaultLogger->debug(__VA_ARGS__) : (void)0)
```

这样可以：
1. **短期**: 保持 API 兼容，使用全局指针指向 GleamDebugger 的实例
2. **中期**: 逐步重构调用点，传递 Logger/PerfMonitor 引用
3. **长期**: 移除全局指针，完全基于依赖注入

## 实施步骤

### 第一阶段：创建封装类
1. 创建 `Gleam/Logger.h` 和 `Gleam/Logger.cpp`
2. 创建 `Gleam/PerfMonitor.h` 和 `Gleam/PerfMonitor.cpp`
3. 将现有功能迁移到类中

### 第二阶段：集成到 GleamDebugger
1. 在 GleamDebugger 中添加成员变量
2. 提供访问器方法
3. 设置全局指针（过渡）

### 第三阶段：渐进式重构
1. 识别所有使用全局变量的位置
2. 逐步替换为对象方法调用
3. 最终移除全局变量

### 第四阶段：测试和验证
1. 为 Logger 和 PerfMonitor 编写单元测试
2. 验证多实例场景
3. 性能基准测试
