/**
 * @file console_shell.hpp
 * @brief Console (stdin/stdout) shell backend with termios raw mode.
 */

#ifndef EMBSH_CONSOLE_SHELL_HPP_
#define EMBSH_CONSOLE_SHELL_HPP_

#include "embsh/line_editor.hpp"

#include <atomic>
#include <thread>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace embsh {

/**
 * @brief Interactive console shell using stdin/stdout.
 *
 * Configures the terminal to raw mode for character-by-character input.
 * Restores the original terminal settings on Stop().
 */
class ConsoleShell final {
 public:
  struct Config {
    const char* prompt;
    int read_fd;
    int write_fd;
    bool raw_mode;

    Config() noexcept
        : prompt("embsh> "),
          read_fd(STDIN_FILENO),
          write_fd(STDOUT_FILENO),
          raw_mode(true) {}
  };

  explicit ConsoleShell(const Config& cfg = Config{}) : cfg_(cfg) {
    detail::RegisterHelpOnce();
  }

  ~ConsoleShell() { Stop(); }

  ConsoleShell(const ConsoleShell&) = delete;
  ConsoleShell& operator=(const ConsoleShell&) = delete;

  /// @brief Start the console shell.
  inline expected<void, ShellError> Start() noexcept;

  /// @brief Stop the console shell and restore terminal.
  inline void Stop() noexcept;

  /// @brief Run the shell synchronously (blocking).
  inline void Run() noexcept;

  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

 private:
  Config cfg_;
  Session session_ = {};
  std::thread thread_;
  std::atomic<bool> running_{false};
  struct termios orig_termios_ = {};
  bool termios_saved_ = false;

  inline void SetRawMode() noexcept;
  inline void RestoreTermios() noexcept;
  inline void RunLoop() noexcept;
};

// ============================================================================
// ConsoleShell implementation
// ============================================================================

inline void ConsoleShell::SetRawMode() noexcept {
  if (!cfg_.raw_mode) return;
  if (::tcgetattr(cfg_.read_fd, &orig_termios_) != 0) return;
  termios_saved_ = true;

  struct termios raw = orig_termios_;
  raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | ICRNL | INLCR | IGNCR);
  raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  (void)::tcsetattr(cfg_.read_fd, TCSANOW, &raw);
}

inline void ConsoleShell::RestoreTermios() noexcept {
  if (termios_saved_) {
    (void)::tcsetattr(cfg_.read_fd, TCSANOW, &orig_termios_);
    termios_saved_ = false;
  }
}

inline expected<void, ShellError> ConsoleShell::Start() noexcept {
  if (running_.load(std::memory_order_relaxed)) {
    return expected<void, ShellError>::error(ShellError::kAlreadyRunning);
  }

  SetRawMode();

  session_.read_fd = cfg_.read_fd;
  session_.write_fd = cfg_.write_fd;
  session_.write_fn = io::PosixWrite;
  session_.read_fn = io::PosixRead;
  session_.telnet_mode = false;
  session_.line_pos = 0;
  session_.hist_browsing = false;
  session_.esc_state = Session::EscState::kNone;
  session_.active.store(true, std::memory_order_release);

  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { RunLoop(); });

  return expected<void, ShellError>::success();
}

inline void ConsoleShell::Stop() noexcept {
  if (!running_.load(std::memory_order_relaxed)) return;
  running_.store(false, std::memory_order_release);
  session_.active.store(false, std::memory_order_release);

  if (thread_.joinable()) {
    thread_.join();
  }
  RestoreTermios();
}

inline void ConsoleShell::Run() noexcept {
  SetRawMode();

  session_.read_fd = cfg_.read_fd;
  session_.write_fd = cfg_.write_fd;
  session_.write_fn = io::PosixWrite;
  session_.read_fn = io::PosixRead;
  session_.telnet_mode = false;
  session_.line_pos = 0;
  session_.active.store(true, std::memory_order_release);
  running_.store(true, std::memory_order_release);

  RunLoop();

  RestoreTermios();
  running_.store(false, std::memory_order_release);
}

inline void ConsoleShell::RunLoop() noexcept {
  auto& s = session_;
  SessionWrite(s, cfg_.prompt);

  while (running_.load(std::memory_order_relaxed) &&
         s.active.load(std::memory_order_acquire)) {
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr == 0) continue;
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    uint8_t byte;
    ssize_t n = s.read_fn(s.read_fd, &byte, 1);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }

    if (editor::ProcessByte(s, byte, cfg_.prompt)) {
      editor::ExecuteLine(s);
      s.line_pos = 0;
      s.line_buf[0] = '\0';
      if (s.active.load(std::memory_order_acquire)) {
        SessionWrite(s, cfg_.prompt);
      }
    }
  }
}

}  // namespace embsh

#endif  // EMBSH_CONSOLE_SHELL_HPP_
