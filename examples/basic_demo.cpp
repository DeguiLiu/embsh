/**
 * @file basic_demo.cpp
 * @brief Minimal TCP telnet shell demo.
 *
 * Build and run:
 *   cmake -B build -DEMBSH_BUILD_EXAMPLES=ON
 *   cmake --build build
 *   ./build/examples/embsh_basic_demo
 *
 * Connect from another terminal:
 *   telnet localhost 2323
 */

#include "embsh/telnet_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

static std::atomic<bool> g_running{true};

static void SignalHandler(int /*sig*/) {
  g_running.store(false, std::memory_order_relaxed);
}

// ============================================================================
// Example commands
// ============================================================================

static int cmd_hello(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  embsh::ShellPrintf("Hello from embsh!\r\n");
  return 0;
}

static int cmd_uptime(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  static auto start = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
  embsh::ShellPrintf("Uptime: %ld seconds\r\n", secs);
  return 0;
}

static int cmd_echo(int argc, char* argv[], void* /*ctx*/) {
  for (int i = 1; i < argc; ++i) {
    embsh::ShellPrintf("%s ", argv[i]);
  }
  embsh::ShellPrintf("\r\n");
  return 0;
}

// Auto-register commands.
EMBSH_CMD(cmd_hello, "Say hello");
EMBSH_CMD(cmd_uptime, "Show uptime");
EMBSH_CMD(cmd_echo, "Echo arguments");

int main() {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  embsh::ServerConfig cfg;
  cfg.port = 2323;
  cfg.prompt = "demo> ";
  cfg.banner = "\r\n=== embsh basic demo ===\r\n\r\n";

  embsh::TelnetServer server(cfg);
  auto r = server.Start();
  if (!r.has_value()) {
    std::fprintf(stderr, "Failed to start server (error %d)\n", static_cast<int>(r.error_value()));
    return 1;
  }

  std::printf("Telnet server listening on port %u\n", cfg.port);
  std::printf("Connect with: telnet localhost %u\n", cfg.port);
  std::printf("Press Ctrl+C to stop.\n");

  while (g_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::printf("\nShutting down...\n");
  server.Stop();
  return 0;
}
