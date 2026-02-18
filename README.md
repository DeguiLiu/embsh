**English** | [中文](README_zh.md)

# embsh

[![CI](https://github.com/DeguiLiu/embsh/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/embsh/actions/workflows/ci.yml)
[![Code Coverage](https://github.com/DeguiLiu/embsh/actions/workflows/coverage.yml/badge.svg)](https://github.com/DeguiLiu/embsh/actions/workflows/coverage.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Modern C++17 header-only embedded debug shell with multi-backend I/O (TCP telnet / Console / UART), IAC protocol, authentication, arrow-key history, and tab completion.

## Features

- **Header-only**: Single CMake INTERFACE library, C++17, zero external dependencies
- **Multi-backend I/O**: TCP telnet, stdin/stdout console, UART serial -- all sharing the same command registry
- **Telnet protocol**: IAC negotiation FSM, WILL/WONT/DO/DONT handling
- **Authentication**: Optional username/password with password masking
- **Arrow-key history**: Up/Down navigation through command history (16 entries)
- **Tab completion**: Single-match auto-fill, multi-match longest common prefix
- **Context pointer**: `int (*)(int argc, char* argv[], void* ctx)` -- bind stateful objects without closures
- **RT-Thread MSH compatible**: `MSH_CMD_EXPORT` macro for source-level portability
- **Compatible with `-fno-exceptions -fno-rtti`**: Suitable for resource-constrained embedded Linux

## Quick Start

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
  // ... application runs ...
  server.Stop();
}
```

Connect with: `telnet localhost 2323`

## Modules

| Header | Description |
|--------|-------------|
| `platform.hpp` | Platform detection, assertion macro, compiler hints |
| `types.hpp` | `expected<V,E>`, `function_ref`, `ShellError` enum |
| `command_registry.hpp` | Global command table (64 slots), `ShellSplit`, `EMBSH_CMD` macro |
| `line_editor.hpp` | Line editing, history, tab completion, ESC/IAC FSM |
| `telnet_server.hpp` | TCP telnet backend (8 sessions, authentication) |
| `console_shell.hpp` | stdin/stdout backend (termios raw mode) |
| `uart_shell.hpp` | UART serial backend (configurable baud rate) |

## Build

```bash
cmake -B build -DEMBSH_BUILD_TESTS=ON -DEMBSH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Compile-Time Configuration

| Macro | Default | Description |
|-------|---------|-------------|
| `EMBSH_MAX_COMMANDS` | 64 | Maximum registered commands |
| `EMBSH_MAX_SESSIONS` | 8 | Maximum concurrent TCP sessions |
| `EMBSH_LINE_BUF_SIZE` | 256 | Line buffer size (bytes) |
| `EMBSH_HISTORY_SIZE` | 16 | History entries per session |
| `EMBSH_MAX_ARGS` | 32 | Maximum arguments per command |
| `EMBSH_DEFAULT_PORT` | 2323 | Default TCP listen port |

## Examples

- `basic_demo.cpp` -- Minimal TCP telnet server
- `console_demo.cpp` -- Interactive console shell
- `multi_backend.cpp` -- TCP + Console running simultaneously

## License

MIT -- See [LICENSE](LICENSE)
