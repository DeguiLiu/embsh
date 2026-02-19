/**
 * @file multi_backend.cpp
 * @brief Run TCP telnet + console shell simultaneously.
 *
 * Build and run:
 *   cmake -B build -DEMBSH_BUILD_EXAMPLES=ON
 *   cmake --build build
 *   ./build/examples/embsh_multi_backend
 *
 * Then connect via telnet from another terminal:
 *   telnet localhost 2323
 */

#include "embsh/console_shell.hpp"
#include "embsh/telnet_server.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>

static std::atomic<bool> g_running{true};

static void SignalHandler(int /*sig*/) {
  g_running.store(false, std::memory_order_relaxed);
}

// ============================================================================
// Shared commands (available on both backends)
// ============================================================================

static int cmd_info(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  embsh::ShellPrintf("embsh multi-backend demo\r\n");
  embsh::ShellPrintf("TCP port: 2323\r\n");
  embsh::ShellPrintf("Console: stdin/stdout\r\n");
  return 0;
}

static int cmd_version(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  embsh::ShellPrintf("embsh v0.1.0\r\n");
  return 0;
}

EMBSH_CMD(cmd_info, "Show backend information");
EMBSH_CMD(cmd_version, "Show version");

int main() {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  // Start TCP telnet server.
  embsh::ServerConfig tcp_cfg;
  tcp_cfg.port = 2323;
  tcp_cfg.prompt = "tcp> ";
  tcp_cfg.banner = "\r\n=== embsh multi-backend (TCP) ===\r\n\r\n";

  embsh::TelnetServer tcp_server(tcp_cfg);
  auto r = tcp_server.Start();
  if (!r.has_value()) {
    std::fprintf(stderr, "Failed to start TCP server.\n");
    return 1;
  }

  std::printf("TCP telnet server on port %u\n", tcp_cfg.port);
  std::printf("Console shell active. Type 'help' for commands.\n\n");

  // Run console shell in the main thread (blocking).
  embsh::ConsoleShell::Config con_cfg;
  con_cfg.prompt = "local> ";

  embsh::ConsoleShell console(con_cfg);
  console.Run();

  // Console exited -- shut down TCP server.
  tcp_server.Stop();
  std::printf("All backends stopped.\n");
  return 0;
}
