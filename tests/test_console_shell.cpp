/**
 * @file test_console_shell.cpp
 * @brief Unit tests for ConsoleShell using pipe-based I/O.
 */

#include <catch2/catch_test_macros.hpp>

#include "embsh/console_shell.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include <unistd.h>

// ============================================================================
// Helper: pipe-based test fixture.
// ============================================================================

struct ConsolePipes {
  int input_pipe[2] = {-1, -1};   // [0]=read (shell reads), [1]=write (test writes)
  int output_pipe[2] = {-1, -1};  // [0]=read (test reads), [1]=write (shell writes)

  ConsolePipes() {
    (void)::pipe(input_pipe);
    (void)::pipe(output_pipe);
  }

  ~ConsolePipes() {
    if (input_pipe[0] >= 0)
      ::close(input_pipe[0]);
    if (input_pipe[1] >= 0)
      ::close(input_pipe[1]);
    if (output_pipe[0] >= 0)
      ::close(output_pipe[0]);
    if (output_pipe[1] >= 0)
      ::close(output_pipe[1]);
  }

  void SendInput(const char* str) { (void)::write(input_pipe[1], str, std::strlen(str)); }

  std::string ReadOutput(int timeout_ms = 500) {
    std::string result;
    char buf[256];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      struct pollfd pfd;
      pfd.fd = output_pipe[0];
      pfd.events = POLLIN;
      int pr = ::poll(&pfd, 1, 50);
      if (pr > 0) {
        ssize_t n = ::read(output_pipe[0], buf, sizeof(buf) - 1);
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

TEST_CASE("ConsoleShell: start and stop with pipes", "[console_shell]") {
  ConsolePipes pipes;

  embsh::ConsoleShell::Config cfg;
  cfg.read_fd = pipes.input_pipe[0];
  cfg.write_fd = pipes.output_pipe[1];
  cfg.raw_mode = false;  // Don't touch termios for pipes.
  cfg.prompt = "test> ";

  embsh::ConsoleShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());
  CHECK(shell.IsRunning());

  // Should receive prompt.
  std::string out = pipes.ReadOutput(300);
  CHECK(out.find("test>") != std::string::npos);

  shell.Stop();
  CHECK_FALSE(shell.IsRunning());
}

TEST_CASE("ConsoleShell: command execution via pipe", "[console_shell]") {
  static bool console_cmd_ran = false;
  console_cmd_ran = false;
  auto cmd_fn = [](int /*argc*/, char* /*argv*/[], void* /*ctx*/) -> int {
    console_cmd_ran = true;
    return 0;
  };
  embsh::CommandRegistry::Instance().Register("console_test", cmd_fn, "console test");

  ConsolePipes pipes;

  embsh::ConsoleShell::Config cfg;
  cfg.read_fd = pipes.input_pipe[0];
  cfg.write_fd = pipes.output_pipe[1];
  cfg.raw_mode = false;

  embsh::ConsoleShell shell(cfg);
  auto r = shell.Start();
  REQUIRE(r.has_value());

  // Wait for prompt.
  (void)pipes.ReadOutput(200);

  // Send command.
  pipes.SendInput("console_test\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  CHECK(console_cmd_ran == true);

  shell.Stop();
}

TEST_CASE("ConsoleShell: start is idempotent", "[console_shell]") {
  ConsolePipes pipes;

  embsh::ConsoleShell::Config cfg;
  cfg.read_fd = pipes.input_pipe[0];
  cfg.write_fd = pipes.output_pipe[1];
  cfg.raw_mode = false;

  embsh::ConsoleShell shell(cfg);
  auto r1 = shell.Start();
  REQUIRE(r1.has_value());

  auto r2 = shell.Start();
  CHECK_FALSE(r2.has_value());

  shell.Stop();
}

TEST_CASE("ConsoleShell: unknown command shows error", "[console_shell]") {
  ConsolePipes pipes;

  embsh::ConsoleShell::Config cfg;
  cfg.read_fd = pipes.input_pipe[0];
  cfg.write_fd = pipes.output_pipe[1];
  cfg.raw_mode = false;

  embsh::ConsoleShell shell(cfg);
  shell.Start();

  // Wait for prompt.
  (void)pipes.ReadOutput(200);

  // Send unknown command.
  pipes.SendInput("no_such_cmd\r");
  std::string out = pipes.ReadOutput(300);
  CHECK(out.find("unknown command") != std::string::npos);

  shell.Stop();
}
