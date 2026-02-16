/**
 * @file uart_shell.hpp
 * @brief UART (serial port) shell backend with termios configuration.
 */

#ifndef EMBSH_UART_SHELL_HPP_
#define EMBSH_UART_SHELL_HPP_

#include "embsh/line_editor.hpp"

#include <atomic>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace embsh {

/**
 * @brief UART serial port shell backend.
 *
 * Opens a serial device (e.g., /dev/ttyS0, /dev/ttyUSB0) and provides
 * interactive command-line access over a serial connection.
 */
class UartShell final {
 public:
  struct Config {
    const char* device;
    uint32_t baudrate;
    const char* prompt;
    int override_fd;

    Config() noexcept
        : device("/dev/ttyS0"),
          baudrate(115200),
          prompt("embsh> "),
          override_fd(-1) {}
  };

  explicit UartShell(const Config& cfg = Config{}) : cfg_(cfg) {
    detail::RegisterHelpOnce();
  }

  ~UartShell() { Stop(); }

  UartShell(const UartShell&) = delete;
  UartShell& operator=(const UartShell&) = delete;

  /// @brief Start the UART shell.
  inline expected<void, ShellError> Start() noexcept;

  /// @brief Stop the UART shell.
  inline void Stop() noexcept;

  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

 private:
  Config cfg_;
  Session session_ = {};
  std::thread thread_;
  std::atomic<bool> running_{false};
  int uart_fd_ = -1;
  bool owns_fd_ = false;

  inline void RunLoop() noexcept;

  static inline speed_t BaudToSpeed(uint32_t baud) noexcept {
    switch (baud) {
      case 9600:   return B9600;
      case 19200:  return B19200;
      case 38400:  return B38400;
      case 57600:  return B57600;
      case 115200: return B115200;
      case 230400: return B230400;
      case 460800: return B460800;
      case 921600: return B921600;
      default:     return B115200;
    }
  }
};

// ============================================================================
// UartShell implementation
// ============================================================================

inline expected<void, ShellError> UartShell::Start() noexcept {
  if (running_.load(std::memory_order_relaxed)) {
    return expected<void, ShellError>::error(ShellError::kAlreadyRunning);
  }

  if (cfg_.override_fd >= 0) {
    uart_fd_ = cfg_.override_fd;
    owns_fd_ = false;
  } else {
    uart_fd_ = ::open(cfg_.device, O_RDWR | O_NOCTTY);
    if (uart_fd_ < 0) {
      return expected<void, ShellError>::error(ShellError::kDeviceOpenFailed);
    }
    owns_fd_ = true;

    // Configure termios.
    struct termios tty = {};
    if (::tcgetattr(uart_fd_, &tty) != 0) {
      ::close(uart_fd_);
      uart_fd_ = -1;
      return expected<void, ShellError>::error(ShellError::kDeviceOpenFailed);
    }

    speed_t spd = BaudToSpeed(cfg_.baudrate);
    (void)::cfsetispeed(&tty, spd);
    (void)::cfsetospeed(&tty, spd);

    // 8N1, no flow control.
    tty.c_cflag = (tty.c_cflag & ~static_cast<tcflag_t>(CSIZE)) | CS8;
    tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
    tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CRTSCTS);

    // Raw mode.
    tty.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY |
                                           ICRNL | INLCR | IGNCR);
    tty.c_oflag &= ~static_cast<tcflag_t>(OPOST);

    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(uart_fd_, TCSANOW, &tty) != 0) {
      ::close(uart_fd_);
      uart_fd_ = -1;
      return expected<void, ShellError>::error(ShellError::kDeviceOpenFailed);
    }
  }

  session_.read_fd = uart_fd_;
  session_.write_fd = uart_fd_;
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

inline void UartShell::Stop() noexcept {
  if (!running_.load(std::memory_order_relaxed)) return;
  running_.store(false, std::memory_order_release);
  session_.active.store(false, std::memory_order_release);

  if (thread_.joinable()) {
    thread_.join();
  }

  if (owns_fd_ && uart_fd_ >= 0) {
    ::close(uart_fd_);
    uart_fd_ = -1;
  }
}

inline void UartShell::RunLoop() noexcept {
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

#endif  // EMBSH_UART_SHELL_HPP_
