/**
 * @file telnet_server.hpp
 * @brief TCP telnet debug server with IAC protocol, authentication,
 *        and multi-session support.
 */

#ifndef EMBSH_TELNET_SERVER_HPP_
#define EMBSH_TELNET_SERVER_HPP_

#include "embsh/line_editor.hpp"

#include <atomic>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef EMBSH_MAX_SESSIONS
#define EMBSH_MAX_SESSIONS 8
#endif

#ifndef EMBSH_DEFAULT_PORT
#define EMBSH_DEFAULT_PORT 2323
#endif

namespace embsh {

// ============================================================================
// ServerConfig
// ============================================================================

/// @brief TCP telnet server configuration.
struct ServerConfig {
  uint16_t port = EMBSH_DEFAULT_PORT;
  uint32_t max_sessions = EMBSH_MAX_SESSIONS;
  const char* prompt = "embsh> ";
  const char* banner = "\r\n=== embsh v0.1.0 ===\r\n\r\n";
  const char* username = nullptr;  ///< nullptr = no authentication.
  const char* password = nullptr;
};

// ============================================================================
// TelnetServer
// ============================================================================

/**
 * @brief Lightweight telnet debug server.
 *
 * Listens on a configurable TCP port and accepts up to max_sessions
 * concurrent telnet sessions. Each session runs in its own thread.
 */
class TelnetServer final {
 public:
  explicit TelnetServer(const ServerConfig& cfg = ServerConfig{}) : cfg_(cfg) { detail::RegisterHelpOnce(); }

  ~TelnetServer() { Stop(); }

  TelnetServer(const TelnetServer&) = delete;
  TelnetServer& operator=(const TelnetServer&) = delete;

  /// @brief Start the telnet server.
  inline expected<void, ShellError> Start() noexcept;

  /// @brief Stop the server and close all sessions.
  inline void Stop() noexcept;

  /// @brief Check if the server is running.
  bool IsRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

 private:
  struct SessionSlot {
    Session session;
    std::thread thread;
    std::atomic<bool> in_use{false};
  };

  ServerConfig cfg_;
  int listen_fd_ = -1;
  std::thread accept_thread_;
  SessionSlot slots_[EMBSH_MAX_SESSIONS] = {};
  std::atomic<bool> running_{false};

  inline void AcceptLoop() noexcept;
  inline void SessionLoop(SessionSlot& slot) noexcept;
  inline void RunAuth(Session& s) noexcept;

  inline int FindFreeSlot() noexcept {
    for (uint32_t i = 0; i < cfg_.max_sessions && i < EMBSH_MAX_SESSIONS; ++i) {
      if (!slots_[i].in_use.load(std::memory_order_acquire)) {
        // Join stale thread if needed.
        if (slots_[i].thread.joinable()) {
          slots_[i].thread.join();
        }
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  static inline void SendStr(int fd, const char* str) noexcept {
    if (fd >= 0 && str != nullptr) {
      size_t len = std::strlen(str);
      if (len > 0) {
        (void)::send(fd, str, len, MSG_NOSIGNAL);
      }
    }
  }

  static inline void SendIac(int fd, uint8_t cmd, uint8_t opt) noexcept {
    uint8_t buf[3] = {0xFF, cmd, opt};
    (void)::send(fd, buf, 3, MSG_NOSIGNAL);
  }
};

// ============================================================================
// TelnetServer implementation
// ============================================================================

inline expected<void, ShellError> TelnetServer::Start() noexcept {
  if (running_.load(std::memory_order_relaxed)) {
    return expected<void, ShellError>::error(ShellError::kAlreadyRunning);
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  int opt = 1;
  (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(cfg_.port);

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  if (::listen(listen_fd_, 4) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return expected<void, ShellError>::error(ShellError::kPortInUse);
  }

  running_.store(true, std::memory_order_release);
  accept_thread_ = std::thread([this]() { AcceptLoop(); });

  return expected<void, ShellError>::success();
}

inline void TelnetServer::Stop() noexcept {
  if (!running_.load(std::memory_order_relaxed))
    return;
  running_.store(false, std::memory_order_release);

  // Close the listen socket to unblock accept().
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  // Stop all active sessions.
  for (uint32_t i = 0; i < EMBSH_MAX_SESSIONS; ++i) {
    if (slots_[i].in_use.load(std::memory_order_acquire)) {
      slots_[i].session.active.store(false, std::memory_order_release);
      if (slots_[i].session.read_fd >= 0) {
        ::shutdown(slots_[i].session.read_fd, SHUT_RDWR);
      }
    }
    if (slots_[i].thread.joinable()) {
      slots_[i].thread.join();
    }
    slots_[i].in_use.store(false, std::memory_order_release);
  }
}

inline void TelnetServer::AcceptLoop() noexcept {
  while (running_.load(std::memory_order_relaxed)) {
    struct pollfd pfd;
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 500);
    if (pr <= 0)
      continue;

    struct sockaddr_in client_addr = {};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0)
      continue;

    int idx = FindFreeSlot();
    if (idx < 0) {
      SendStr(client_fd, "Too many connections.\r\n");
      ::close(client_fd);
      continue;
    }

    auto& slot = slots_[idx];
    slot.in_use.store(true, std::memory_order_release);

    // Initialize session.
    auto& s = slot.session;
    s.read_fd = client_fd;
    s.write_fd = client_fd;
    s.write_fn = io::TcpWrite;
    s.read_fn = io::TcpRead;
    s.telnet_mode = true;
    s.line_pos = 0;
    s.hist_browsing = false;
    s.esc_state = Session::EscState::kNone;
    s.iac_state = Session::IacState::kNormal;
    s.active.store(true, std::memory_order_release);

    // Authentication state.
    s.auth_required = (cfg_.username != nullptr && cfg_.password != nullptr);
    s.authenticated = !s.auth_required;
    s.auth_attempts = 0;
    s.auth_user_pos = 0;

    slot.thread = std::thread([this, &slot]() { SessionLoop(slot); });
  }
}

inline void TelnetServer::SessionLoop(SessionSlot& slot) noexcept {
  auto& s = slot.session;

  // Telnet negotiations: suppress go-ahead + echo.
  SendIac(s.write_fd, 0xFB, 0x03);  // WILL SGA
  SendIac(s.write_fd, 0xFB, 0x01);  // WILL ECHO

  // Banner.
  if (cfg_.banner != nullptr) {
    SessionWrite(s, cfg_.banner);
  }

  // Authentication.
  if (s.auth_required) {
    RunAuth(s);
    if (!s.authenticated) {
      SessionWrite(s, "Authentication failed.\r\n");
      ::close(s.read_fd);
      s.read_fd = -1;
      s.active.store(false, std::memory_order_release);
      slot.in_use.store(false, std::memory_order_release);
      return;
    }
  }

  // Main interactive loop.
  SessionWrite(s, cfg_.prompt);

  while (running_.load(std::memory_order_relaxed) && s.active.load(std::memory_order_acquire)) {
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr == 0)
      continue;
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    uint8_t byte;
    ssize_t n = s.read_fn(s.read_fd, &byte, 1);
    if (n <= 0)
      break;

    if (editor::ProcessByte(s, byte, cfg_.prompt)) {
      editor::ExecuteLine(s);
      s.line_pos = 0;
      s.line_buf[0] = '\0';
      if (s.active.load(std::memory_order_acquire)) {
        SessionWrite(s, cfg_.prompt);
      }
    }
  }

  if (s.read_fd >= 0) {
    ::close(s.read_fd);
    s.read_fd = -1;
  }
  s.active.store(false, std::memory_order_release);
  slot.in_use.store(false, std::memory_order_release);
}

inline void TelnetServer::RunAuth(Session& s) noexcept {
  static constexpr uint8_t kMaxAttempts = 3;
  enum class AuthPhase : uint8_t { kUser, kPass };
  AuthPhase phase = AuthPhase::kUser;
  char user_buf[64] = {};
  char pass_buf[64] = {};
  uint32_t buf_pos = 0;

  SessionWrite(s, "Username: ");

  while (s.active.load(std::memory_order_acquire) && s.auth_attempts < kMaxAttempts) {
    struct pollfd pfd;
    pfd.fd = s.read_fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 200);
    if (pr <= 0) {
      if (pr == 0)
        continue;
      break;
    }

    uint8_t byte;
    ssize_t n = s.read_fn(s.read_fd, &byte, 1);
    if (n <= 0)
      break;

    // IAC filtering.
    if (s.telnet_mode) {
      char ch = editor::FilterIac(s, byte);
      if (ch == '\0')
        continue;
      byte = static_cast<uint8_t>(ch);
    }

    char ch = static_cast<char>(byte);

    // Backspace.
    if (byte == 0x7F || byte == 0x08) {
      if (buf_pos > 0) {
        --buf_pos;
        if (phase == AuthPhase::kUser) {
          SessionWrite(s, "\b \b");
        } else {
          SessionWrite(s, "\b \b");
        }
      }
      continue;
    }

    // Enter.
    if (ch == '\r' || ch == '\n') {
      // Consume trailing \n.
      if (s.telnet_mode && ch == '\r') {
        char next;
        ssize_t peek = ::recv(s.read_fd, &next, 1, MSG_PEEK);
        if (peek == 1 && (next == '\n' || next == '\0')) {
          (void)::recv(s.read_fd, &next, 1, 0);
        }
      }

      SessionWrite(s, "\r\n");

      if (phase == AuthPhase::kUser) {
        user_buf[buf_pos] = '\0';
        phase = AuthPhase::kPass;
        buf_pos = 0;
        SessionWrite(s, "Password: ");
        // Turn off echo for password.
        continue;
      }

      // Password entered.
      pass_buf[buf_pos] = '\0';
      if (std::strcmp(user_buf, cfg_.username) == 0 && std::strcmp(pass_buf, cfg_.password) == 0) {
        s.authenticated = true;
        SessionWrite(s, "Login successful.\r\n");
        return;
      }

      ++s.auth_attempts;
      if (s.auth_attempts < kMaxAttempts) {
        SessionWrite(s, "Invalid credentials. Try again.\r\n");
        phase = AuthPhase::kUser;
        buf_pos = 0;
        SessionWrite(s, "Username: ");
      }
      continue;
    }

    // Printable character.
    if (byte >= 0x20 && byte < 0x7F) {
      if (phase == AuthPhase::kUser && buf_pos < sizeof(user_buf) - 1) {
        user_buf[buf_pos++] = ch;
        SessionWriteN(s, &ch, 1);
      } else if (phase == AuthPhase::kPass && buf_pos < sizeof(pass_buf) - 1) {
        pass_buf[buf_pos++] = ch;
        SessionWrite(s, "*");  // Mask password.
      }
    }
  }
}

}  // namespace embsh

#endif  // EMBSH_TELNET_SERVER_HPP_
