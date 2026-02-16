**中文** | [English](README.md)

# embsh

现代 C++17 纯头文件嵌入式调试 Shell，支持多后端 I/O (TCP telnet / 控制台 / UART)、IAC 协议、认证、方向键历史导航和 Tab 补全。

## 特性

- **纯头文件**: 单一 CMake INTERFACE 库，C++17，零外部依赖
- **多后端 I/O**: TCP telnet、stdin/stdout 控制台、UART 串口 -- 共享同一命令注册表
- **Telnet 协议**: IAC 协商 FSM，WILL/WONT/DO/DONT 处理
- **认证**: 可选的用户名/密码验证，密码星号掩码
- **方向键历史**: Up/Down 导航历史命令 (16 条)
- **Tab 补全**: 单匹配自动填充，多匹配最长公共前缀
- **Context 指针**: `int (*)(int argc, char* argv[], void* ctx)` -- 无闭包绑定有状态对象
- **RT-Thread MSH 兼容**: `MSH_CMD_EXPORT` 宏，源码级可移植
- **兼容 `-fno-exceptions -fno-rtti`**: 适配资源受限的嵌入式 Linux

## 快速开始

```cpp
#include "embsh/telnet_server.hpp"

static int cmd_hello(int argc, char* argv[], void* ctx) {
  embsh::ShellPrintf("Hello from embsh!\r\n");
  return 0;
}
EMBSH_CMD(cmd_hello, "Say hello");

int main() {
  embsh::ServerConfig cfg;
  cfg.port = 2323;
  embsh::TelnetServer server(cfg);
  server.Start();
  // ... 应用运行 ...
  server.Stop();
}
```

连接: `telnet localhost 2323`

## 模块

| 头文件 | 说明 |
|--------|------|
| `platform.hpp` | 平台检测、断言宏、编译器提示 |
| `types.hpp` | `expected<V,E>`、`function_ref`、`ShellError` 枚举 |
| `command_registry.hpp` | 全局命令表 (64 slots)、`ShellSplit`、`EMBSH_CMD` 宏 |
| `line_editor.hpp` | 行编辑、历史、Tab 补全、ESC/IAC FSM |
| `telnet_server.hpp` | TCP telnet 后端 (8 并发、认证) |
| `console_shell.hpp` | stdin/stdout 控制台后端 (termios raw mode) |
| `uart_shell.hpp` | UART 串口后端 (可配置波特率) |

## 构建

```bash
cmake -B build -DEMBSH_BUILD_TESTS=ON -DEMBSH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## 编译期配置

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `EMBSH_MAX_COMMANDS` | 64 | 最大命令数 |
| `EMBSH_MAX_SESSIONS` | 8 | TCP 最大并发 session |
| `EMBSH_LINE_BUF_SIZE` | 256 | 行缓冲区大小 |
| `EMBSH_HISTORY_SIZE` | 16 | 历史记录条数 |
| `EMBSH_MAX_ARGS` | 32 | 单条命令最大参数数 |
| `EMBSH_DEFAULT_PORT` | 2323 | TCP 默认端口 |

## 示例

- `basic_demo.cpp` -- 最简 TCP telnet 服务器
- `console_demo.cpp` -- 控制台交互 shell
- `multi_backend.cpp` -- TCP + 控制台同时运行

## 许可证

MIT -- 详见 [LICENSE](LICENSE)
