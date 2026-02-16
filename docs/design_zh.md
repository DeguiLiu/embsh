# embsh 设计文档

> 版本: v0.1.0 | 更新: 2026-02-16

---

## 1. 项目定位

embsh 是一个面向 ARM-Linux 工业嵌入式平台的 C++17 纯头文件调试 Shell 库。提供三种 I/O 后端 (TCP telnet / Console / UART) 的统一命令行接口，零外部依赖，零堆分配热路径。

**适用场景**: 激光雷达、机器人控制器、边缘计算网关等需要运行时诊断能力的嵌入式设备。

**设计目标**:

| 目标 | 实现方式 |
|------|----------|
| 零外部依赖 | 纯头文件，仅依赖 POSIX + C++17 标准库 |
| 零堆分配热路径 | 固定大小缓冲区 (line_buf, history, 命令表) |
| 多后端统一 | 函数指针 I/O 抽象，共享命令注册表和行编辑逻辑 |
| 嵌入式友好 | 兼容 `-fno-exceptions -fno-rtti`，固定宽度整数 |
| RT-Thread 源码兼容 | `MSH_CMD_EXPORT` 宏映射到 `EMBSH_CMD` |

---

## 2. 架构总览

### 2.1 模块依赖图

```
platform.hpp  ────────────────────  (无依赖，纯宏)
    |
types.hpp  ────────────────────────  (expected, function_ref, ShellError)
    |
command_registry.hpp  ─────────────  (CommandRegistry, ShellSplit, EMBSH_CMD, ShellPrintf)
    |
line_editor.hpp  ──────────────────  (Session, I/O 抽象, ProcessByte, History, IAC, ESC)
    |
    ├── telnet_server.hpp  ────────  (TelnetServer: TCP 多会话 + 认证)
    ├── console_shell.hpp  ────────  (ConsoleShell: stdin/stdout + termios)
    └── uart_shell.hpp  ───────────  (UartShell: 串口 + 波特率配置)
```

### 2.2 三后端架构

```
        +---------------------------------------------------+
        |               共享层 (line_editor.hpp)              |
        |  editor::ProcessByte  <-- FilterIac <-- ESC FSM    |
        |  editor::ExecuteLine  TabComplete  PushHistory      |
        |  SessionWrite / SessionWriteN                       |
        |  WriteFn / ReadFn 函数指针抽象                       |
        +--------+----------------+----------------+----------+
                 |                |                |
        +--------v-----+  +------v-------+  +-----v--------+
        | TelnetServer  |  | ConsoleShell |  |  UartShell   |
        | TCP telnet    |  | stdin/stdout |  | /dev/ttyS*   |
        | IAC 协商      |  | termios raw  |  | 8N1 termios  |
        | 多会话 (8)    |  | poll(200ms)  |  | poll(200ms)  |
        | 可选认证      |  | 同步/异步    |  | PTY 测试     |
        +---------------+  +--------------+  +--------------+
```

---

## 3. 模块详解

### 3.1 platform.hpp -- 平台检测

编译器和平台检测宏，断言，属性标注。

| 宏 | 用途 |
|----|------|
| `EMBSH_ASSERT(cond)` | 调试断言 (Release 空操作) |
| `EMBSH_LIKELY(x)` / `EMBSH_UNLIKELY(x)` | 分支预测提示 |
| `EMBSH_UNUSED` | 抑制未使用变量警告 |

### 3.2 types.hpp -- 词汇类型

**ShellError 枚举**:

```cpp
enum class ShellError : uint8_t {
  kOk = 0,
  kRegistryFull,       // 命令表满 (>= EMBSH_MAX_COMMANDS)
  kDuplicateName,      // 重复注册
  kAuthFailed,         // 认证失败
  kPortInUse,          // TCP 端口被占用
  kAlreadyRunning,     // 重复 Start()
  kNotRunning,         // 未启动时调用 Stop()
  kDeviceOpenFailed,   // UART 设备打开失败
  kInvalidArgument     // 无效参数
};
```

**expected&lt;V, E&gt;**: 轻量 Result 类型，`aligned_storage` 实现，无异常。

**function_ref&lt;R(Args...)&gt;**: 非拥有可调用引用，零开销类型擦除。

### 3.3 command_registry.hpp -- 命令注册表

**核心类型**:

```cpp
using CmdFn = int (*)(int argc, char* argv[], void* ctx);

struct CmdEntry {
  const char* name;   // 命令名 (static storage)
  const char* desc;   // 描述
  CmdFn fn;           // 回调
  void* ctx;          // 用户上下文 (可选)
};
```

**CommandRegistry**: Meyer's 单例，mutex 保护注册，固定容量 (`EMBSH_MAX_COMMANDS`)。

| 方法 | 说明 |
|------|------|
| `Register(name, fn, ctx, desc)` | 注册命令 (线程安全) |
| `Find(name)` | 精确查找 (只读，无锁) |
| `AutoComplete(prefix, out, size)` | Tab 补全 (最长公共前缀) |
| `ForEach(visitor)` | 遍历所有命令 |
| `Count()` | 已注册命令数 |

**ShellSplit**: 原位 tokenizer，支持单引号/双引号字符串和反斜杠转义。

**ShellPrintf**: 线程局部 `SessionOutput` 路由，命令回调内自动输出到当前会话。

**自动注册**:

```cpp
static int my_cmd(int argc, char* argv[], void* ctx) { ... }
EMBSH_CMD(my_cmd, "description");        // embsh 宏
MSH_CMD_EXPORT(my_cmd, "description");   // RT-Thread 兼容宏
```

### 3.4 line_editor.hpp -- 行编辑器

**Session 结构** (per-connection):

```
Session (~4.4 KB per instance)
|-- read_fd, write_fd         : int x2         (8B)
|-- write_fn, read_fn         : 函数指针 x2    (16B)
|-- line_buf[256]             : 行缓冲         (256B)
|-- line_pos                  : uint32_t       (4B)
|-- history[16][256]          : 历史环形缓冲   (4096B)
|-- hist_count/write/nav      : uint32_t x3    (12B)
|-- hist_browsing             : bool           (1B)
|-- telnet_mode               : bool           (1B)
|-- auth_required/authenticated: bool x2       (2B)
|-- auth_attempts             : uint8_t        (1B)
|-- auth_user_buf[64]         : 认证缓冲       (64B)
|-- esc_state (EscState)      : uint8_t        (1B)
|-- iac_state (IacState)      : uint8_t        (1B)
|-- active (atomic<bool>)     : 原子标志       (1B)
```

**字节处理流水线**:

```
raw byte --> [FilterIac] --> [ESC FSM] --> [字符分类]
             (telnet 专用)   (方向键解析)   (Enter/BS/Tab/Ctrl/打印)
```

**IAC 协议过滤** (4 状态 inline FSM):

```
kNormal --0xFF--> kIac --0xFB~0xFE--> kNego --option--> kNormal
                  kIac --0xFA------> kSub  --0xFF----> kIac (等待 SE)
                  kIac --0xFF------> kNormal (literal 0xFF)
```

**ESC 序列解析** (3 状态):

```
kNone --0x1B--> kEsc --'['--> kBracket --'A'--> HistoryUp
                                        --'B'--> HistoryDown
                                        --'C'--> (预留: 右移)
                                        --'D'--> (预留: 左移)
```

**历史记录**: 环形缓冲 `history[EMBSH_HISTORY_SIZE][EMBSH_LINE_BUF_SIZE]`，`hist_write` 指向下一个写入位置，`hist_nav` 用于浏览导航，跳过连续重复条目。

**editor 命名空间函数** (无状态，操作 Session 引用):

| 函数 | 说明 |
|------|------|
| `ProcessByte(s, byte, prompt)` | 统一字节处理入口，返回 true 表示行就绪 |
| `ExecuteLine(s)` | 解析并执行当前行 (内置 exit/quit) |
| `FilterIac(s, byte)` | IAC 协议字节过滤 |
| `PushHistory(s)` | 保存到历史环形缓冲 |
| `HistoryUp(s)` / `HistoryDown(s)` | 方向键历史导航 |
| `TabComplete(s, prompt)` | Tab 自动补全 |
| `ReplaceLine(s, new_line)` | 清除当前行并替换 |

### 3.5 telnet_server.hpp -- TCP telnet 后端

```
                   TelnetServer
                       |
              +--------+--------+
              |                 |
         accept_thread     SessionSlot[0..7]
         (poll + accept)     |-- Session
                             |-- std::thread
                             |-- atomic<bool> in_use
```

**ServerConfig**:

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `port` | 2323 | TCP 侦听端口 |
| `max_sessions` | 8 | 最大并发会话 |
| `prompt` | `"embsh> "` | 命令提示符 |
| `banner` | `"=== embsh v0.1.0 ==="` | 连接后显示横幅 |
| `username` | nullptr | 认证用户名 (nullptr = 无认证) |
| `password` | nullptr | 认证密码 |

**会话生命周期**:

```
accept() --> SendIac(WILL SGA, WILL ECHO) --> Banner --> [RunAuth] --> prompt --> loop
                                                                                  |
poll(200ms) + recv(1 byte) --> ProcessByte --> ExecuteLine --> prompt -----------+
                                                                                  |
exit/quit 或 Ctrl+D --> close(fd) --> slot.in_use = false -----------------------+
```

**线程安全**:
- AcceptLoop: `poll(500ms)` + `accept()`，Stop() 时 `shutdown(SHUT_RDWR)` + `close()` 唤醒
- SessionLoop: `poll(200ms)` + `recv(1)`，Stop() 时 `shutdown(SHUT_RDWR)` 安全唤醒
- 各 SessionSlot 独立，无共享可变状态

**认证流程**: Username (回显) -> Password (星号掩码) -> 验证 -> 3 次失败断开。

### 3.6 console_shell.hpp -- Console 后端

stdin/stdout 交互，termios raw mode，支持同步 (`Run()`) 和异步 (`Start()`) 两种模式。

**termios 配置**: 关闭 ECHO/ICANON/ISIG/IEXTEN/OPOST，关闭 IXON/IXOFF/ICRNL。

**管道测试**: `Config.read_fd` / `Config.write_fd` 支持 pipe fd 覆盖，CI 自动化测试。

### 3.7 uart_shell.hpp -- UART 串口后端

Linux termios 串口配置，8N1 模式，支持 8 种波特率 (9600 ~ 921600)。

**PTY 测试**: `Config.override_fd` 支持 PTY master fd 注入，无需真实串口硬件。

---

## 4. 与 newosp/shell.hpp 对比

| 维度 | embsh | newosp/shell.hpp |
|------|-------|------------------|
| 定位 | 独立库 | newosp 内部模块 |
| 文件组织 | 7 个头文件 (按职责拆分) | 单文件 (~1674 行) |
| 命令签名 | `int (*)(argc, argv, void* ctx)` | `int (*)(argc, argv)` |
| Context 指针 | 支持 (有状态命令) | 不支持 |
| Printf 路由 | SessionOutput (write + ctx) | thread_local Session* |
| 最大会话 | 8 (编译期配置) | 2 (运行时配置) |
| RT-Thread 兼容 | `MSH_CMD_EXPORT` 宏 | 无 |
| CRLF 处理 | `MSG_PEEK` recv 消费 | `skip_next_lf` 布尔标志 |
| 内置命令 | help + exit/quit | help |
| 命名空间 | `embsh::` | `osp::` / `osp::detail::` |

**关键设计差异**:

1. **Context 指针**: embsh 的 `CmdFn` 携带 `void* ctx`，允许无闭包绑定有状态对象。newosp 依赖 thread-local Session 指针。
2. **文件拆分**: embsh 按职责拆分 (types/registry/editor/backend)，各后端可独立包含。newosp 单文件包含全部功能。
3. **Printf 路由**: embsh 通过 `SessionOutput{write, ctx}` 间接调用，newosp 直接通过 `write_fn(write_fd, ...)` 调用。

---

## 5. 编译期配置

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `EMBSH_MAX_COMMANDS` | 64 | 全局命令表容量 |
| `EMBSH_MAX_SESSIONS` | 8 | TCP 最大并发会话 |
| `EMBSH_LINE_BUF_SIZE` | 256 | 行缓冲区大小 (字节) |
| `EMBSH_HISTORY_SIZE` | 16 | 历史记录条数 |
| `EMBSH_MAX_ARGS` | 32 | 单条命令最大参数数 |
| `EMBSH_DEFAULT_PORT` | 2323 | TCP 默认侦听端口 |

---

## 6. 资源预算

| 资源 | 大小 | 说明 |
|------|------|------|
| Session (per instance) | ~4.4 KB | line_buf(256) + history(16x256) + 控制字段 |
| TelnetServer (8 sessions) | ~35 KB | 8 x SessionSlot + listen_fd + accept_thread |
| CommandRegistry | ~2 KB | CmdEntry[64] (每条 ~32B) |
| ConsoleShell | ~4.4 KB | 1 Session + termios backup |
| UartShell | ~4.4 KB | 1 Session + uart_fd |
| ShellPrintf 栈缓冲 | 512 B | 每次调用临时分配 |

**线程数**:
- TelnetServer: 1 accept + N session (N <= max_sessions)
- ConsoleShell: 0 (同步 Run) 或 1 (异步 Start)
- UartShell: 1

---

## 7. 线程安全性

| 模块 | 保证 |
|------|------|
| CommandRegistry | mutex 保护注册；查找/遍历只读 (注册完成后) |
| Session | 各会话独立，无共享可变状态 |
| ShellPrintf | thread_local SessionOutput 路由，线程隔离 |
| TelnetServer::Stop() | shutdown(SHUT_RDWR) 安全唤醒 accept/recv |
| ConsoleShell::Stop() | atomic running_ + poll() 超时退出 |
| UartShell::Stop() | atomic running_ + poll() 超时退出 |

---

## 8. 测试覆盖

| 模块 | 测试文件 | 测试数 | 覆盖内容 |
|------|----------|--------|----------|
| ShellSplit | test_command_registry.cpp | 9 | 空输入/多词/引号/转义/tab |
| CommandRegistry | test_command_registry.cpp | 5 | 注册/查找/重复/满/自动补全 |
| LineEditor | test_line_editor.cpp | 18 | 字符/backspace/回车/ESC/tab/IAC |
| TelnetServer | test_telnet_server.cpp | 9 | 启停/连接/执行/认证/满 session |
| ConsoleShell | test_console_shell.cpp | 4 | 启停/幂等/执行/错误 |
| UartShell | test_uart_shell.cpp | 5 | 启停/幂等/执行/无效设备/PTY |
| **总计** | 5 文件 | **52** | Catch2 v3.5.2 |

---

## 9. 构建

```bash
cmake -B build -DEMBSH_BUILD_TESTS=ON -DEMBSH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

支持 `-DEMBSH_NO_EXCEPTIONS=ON` 编译模式。
