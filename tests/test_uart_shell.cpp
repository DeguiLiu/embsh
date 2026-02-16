/**
 * @file test_uart_shell.cpp
 * @brief Unit tests for UartShell using PTY (pseudo-terminal).
 */

#include <catch2/catch_test_macros.hpp>

#include "embsh/uart_shell.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <unistd.h>

// ============================================================================
// Helper: PTY pair for testing.
// ============================================================================

struct PtyPair {
  int master = -1;
  int slave = -1;

  PtyPair() {
    if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
      master = -1;
      slave = -1;
    }
  }

  ~PtyPair() {
    if (master >= 0) ::close(master);
    if (slave >= 0) ::close(slave);
  }

  void SendToSlave(const char* str) {
    (void)::write(master, str, std::strlen(str));
  }

  std::string ReadFromSlave(int timeout_ms = 500) {
    std::string result;
    char buf[256];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      struct pollfd pfd;
      pfd.fd = master;
      pfd.events = POLLIN;
      int pr = ::poll(&pfd, 1, 50);
      if (pr > 0) {
        ssize_t n = ::read(master, buf, sizeof(buf) - 1);
        if (n > 0) {
          buf[n] = '\0';
          result += buf;
        }
      }
    }
    return result;
  }
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("UartShell: start and stop with PTY", "[uart_shell]") {
  PtyPair pty;
  REQUIRE(pty.slave >= 0);

  embsh::UartShell::Config cfg;
  cfg.override_fd = pty.slave;
  cfg.prompt = "uart> ";

  embsh::UartShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());
  CHECK(shell.IsRunning());

  // Should receive prompt via master side.
  std::string out = pty.ReadFromSlave(300);
  CHECK(out.find("uart>") != std::string::npos);

  shell.Stop();
  CHECK_FALSE(shell.IsRunning());
}

TEST_CASE("UartShell: command execution via PTY", "[uart_shell]") {
  static bool uart_cmd_ran = false;
  uart_cmd_ran = false;
  auto cmd_fn = [](int /*argc*/, char* /*argv*/[], void* /*ctx*/) -> int {
    uart_cmd_ran = true;
    return 0;
  };
  embsh::CommandRegistry::Instance().Register("uart_test", cmd_fn,
                                              "uart test");

  PtyPair pty;
  REQUIRE(pty.slave >= 0);

  embsh::UartShell::Config cfg;
  cfg.override_fd = pty.slave;

  embsh::UartShell shell(cfg);
  auto r = shell.Start();
  REQUIRE(r.has_value());

  // Wait for prompt.
  (void)pty.ReadFromSlave(200);

  // Send command via master.
  pty.SendToSlave("uart_test\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  CHECK(uart_cmd_ran == true);

  shell.Stop();
}

TEST_CASE("UartShell: start is idempotent", "[uart_shell]") {
  PtyPair pty;
  REQUIRE(pty.slave >= 0);

  embsh::UartShell::Config cfg;
  cfg.override_fd = pty.slave;

  embsh::UartShell shell(cfg);
  auto r1 = shell.Start();
  REQUIRE(r1.has_value());

  auto r2 = shell.Start();
  CHECK_FALSE(r2.has_value());

  shell.Stop();
}

TEST_CASE("UartShell: invalid device returns error", "[uart_shell]") {
  embsh::UartShell::Config cfg;
  cfg.device = "/dev/nonexistent_serial_port_xyz";

  embsh::UartShell shell(cfg);
  auto r = shell.Start();
  CHECK_FALSE(r.has_value());
  CHECK(r.error_value() == embsh::ShellError::kDeviceOpenFailed);
}

TEST_CASE("UartShell: stop when not running is safe", "[uart_shell]") {
  embsh::UartShell::Config cfg;
  cfg.override_fd = -1;

  embsh::UartShell shell(cfg);
  shell.Stop();  // Should not crash.
  CHECK_FALSE(shell.IsRunning());
}
