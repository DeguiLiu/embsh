/**
 * @file console_demo.cpp
 * @brief Interactive console shell demo (stdin/stdout).
 *
 * Build and run:
 *   cmake -B build -DEMBSH_BUILD_EXAMPLES=ON
 *   cmake --build build
 *   ./build/examples/embsh_console_demo
 */

#include "embsh/console_shell.hpp"

#include <cstdio>

static int cmd_hello(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  embsh::ShellPrintf("Hello from console!\r\n");
  return 0;
}

static int cmd_add(int argc, char* argv[], void* /*ctx*/) {
  if (argc < 3) {
    embsh::ShellPrintf("Usage: cmd_add <a> <b>\r\n");
    return 1;
  }
  int a = std::atoi(argv[1]);
  int b = std::atoi(argv[2]);
  embsh::ShellPrintf("%d + %d = %d\r\n", a, b, a + b);
  return 0;
}

EMBSH_CMD(cmd_hello, "Say hello");
EMBSH_CMD(cmd_add, "Add two numbers");

int main() {
  std::printf("embsh console demo. Type 'help' for commands, 'exit' to quit.\n");

  embsh::ConsoleShell::Config cfg;
  cfg.prompt = "console> ";

  embsh::ConsoleShell shell(cfg);
  shell.Run();  // Blocking.

  std::printf("Goodbye.\n");
  return 0;
}
