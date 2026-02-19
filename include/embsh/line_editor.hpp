/**
 * @file line_editor.hpp
 * @brief Interactive line editor with history navigation, tab completion,
 *        and ESC sequence handling.
 */

#ifndef EMBSH_LINE_EDITOR_HPP_
#define EMBSH_LINE_EDITOR_HPP_

#include "embsh/command_registry.hpp"
#include "embsh/platform.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef EMBSH_LINE_BUF_SIZE
#define EMBSH_LINE_BUF_SIZE 256
#endif

#ifndef EMBSH_HISTORY_SIZE
#define EMBSH_HISTORY_SIZE 16
#endif

namespace embsh {

// ============================================================================
// I/O function pointer types
// ============================================================================

/// @brief Write function: ssize_t write(int fd, const void* buf, size_t len).
using WriteFn = ssize_t (*)(int fd, const void* buf, size_t len);

/// @brief Read function: ssize_t read(int fd, void* buf, size_t len).
using ReadFn = ssize_t (*)(int fd, void* buf, size_t len);

// ============================================================================
// Built-in I/O backends
// ============================================================================

namespace io {

/// @brief TCP backend: send() with MSG_NOSIGNAL.
inline ssize_t TcpWrite(int fd, const void* buf, size_t len) {
  return ::send(fd, buf, len, MSG_NOSIGNAL);
}

/// @brief TCP backend: recv().
inline ssize_t TcpRead(int fd, void* buf, size_t len) {
  return ::recv(fd, buf, len, 0);
}

/// @brief POSIX backend: write() for stdin/stdout/UART.
inline ssize_t PosixWrite(int fd, const void* buf, size_t len) {
  return ::write(fd, buf, len);
}

/// @brief POSIX backend: read() for stdin/UART.
inline ssize_t PosixRead(int fd, void* buf, size_t len) {
  return ::read(fd, buf, len);
}

}  // namespace io

// ============================================================================
// Session - Per-connection state
// ============================================================================

/// @brief Session state shared by all backends.
struct Session {
  int read_fd = -1;
  int write_fd = -1;
  WriteFn write_fn = nullptr;
  ReadFn read_fn = nullptr;

  // Line editing
  char line_buf[EMBSH_LINE_BUF_SIZE] = {};
  uint32_t line_pos = 0;

  // History
  char history[EMBSH_HISTORY_SIZE][EMBSH_LINE_BUF_SIZE] = {};
  uint32_t hist_count = 0;
  uint32_t hist_write = 0;     ///< Next write index (ring).
  uint32_t hist_nav = 0;       ///< Current navigation index.
  bool hist_browsing = false;  ///< True while navigating history.

  // Telnet IAC state
  bool telnet_mode = false;

  // Authentication
  bool auth_required = false;
  bool authenticated = false;
  uint8_t auth_attempts = 0;
  char auth_user_buf[64] = {};  ///< Buffer for username input.
  uint32_t auth_user_pos = 0;

  // ESC sequence state
  enum class EscState : uint8_t { kNone = 0, kEsc, kBracket };
  EscState esc_state = EscState::kNone;

  // IAC state
  enum class IacState : uint8_t { kNormal = 0, kIac, kNego, kSub };
  IacState iac_state = IacState::kNormal;

  // Session control
  std::atomic<bool> active{false};
};

// ============================================================================
// Session I/O helpers
// ============================================================================

inline void SessionWrite(Session& s, const char* str) noexcept {
  if (s.write_fn != nullptr && str != nullptr) {
    size_t len = std::strlen(str);
    if (len > 0) {
      (void)s.write_fn(s.write_fd, str, len);
    }
  }
}

inline void SessionWriteN(Session& s, const char* buf, size_t len) noexcept {
  if (s.write_fn != nullptr && len > 0) {
    (void)s.write_fn(s.write_fd, buf, len);
  }
}

// ============================================================================
// LineEditor - History, completion, ESC sequences
// ============================================================================

/**
 * @brief Stateless line editing functions operating on a Session.
 *
 * All functions are inline and operate directly on Session state.
 */
namespace editor {

/// @brief Push a command line into the history ring buffer.
inline void PushHistory(Session& s) noexcept {
  if (s.line_pos == 0)
    return;
  // Skip duplicate of the last entry.
  if (s.hist_count > 0) {
    uint32_t last = (s.hist_write + EMBSH_HISTORY_SIZE - 1) % EMBSH_HISTORY_SIZE;
    if (std::strcmp(s.history[last], s.line_buf) == 0)
      return;
  }
  std::memcpy(s.history[s.hist_write], s.line_buf, s.line_pos);
  s.history[s.hist_write][s.line_pos] = '\0';
  s.hist_write = (s.hist_write + 1) % EMBSH_HISTORY_SIZE;
  if (s.hist_count < EMBSH_HISTORY_SIZE)
    ++s.hist_count;
}

/// @brief Clear the current line on the terminal and replace with new text.
inline void ReplaceLine(Session& s, const char* new_line) noexcept {
  // Erase current line.
  for (uint32_t i = 0; i < s.line_pos; ++i) {
    SessionWrite(s, "\b \b");
  }
  uint32_t len = static_cast<uint32_t>(std::strlen(new_line));
  if (len >= EMBSH_LINE_BUF_SIZE)
    len = EMBSH_LINE_BUF_SIZE - 1;
  std::memcpy(s.line_buf, new_line, len);
  s.line_buf[len] = '\0';
  s.line_pos = len;
  SessionWriteN(s, s.line_buf, s.line_pos);
}

/// @brief Navigate history up (older).
inline void HistoryUp(Session& s) noexcept {
  if (s.hist_count == 0)
    return;
  if (!s.hist_browsing) {
    s.hist_nav = s.hist_write;
    s.hist_browsing = true;
  }
  uint32_t prev = (s.hist_nav + EMBSH_HISTORY_SIZE - 1) % EMBSH_HISTORY_SIZE;
  // Check if we've gone past the oldest entry.
  uint32_t oldest = (s.hist_count < EMBSH_HISTORY_SIZE) ? 0 : s.hist_write;
  if (prev == s.hist_nav)
    return;  // Single entry case.
  // Don't go past oldest.
  uint32_t dist_from_write = (s.hist_write + EMBSH_HISTORY_SIZE - prev) % EMBSH_HISTORY_SIZE;
  if (dist_from_write > s.hist_count)
    return;

  s.hist_nav = prev;
  ReplaceLine(s, s.history[s.hist_nav]);
}

/// @brief Navigate history down (newer).
inline void HistoryDown(Session& s) noexcept {
  if (!s.hist_browsing)
    return;
  uint32_t next = (s.hist_nav + 1) % EMBSH_HISTORY_SIZE;
  if (next == s.hist_write) {
    // Back to current (empty) line.
    s.hist_browsing = false;
    for (uint32_t i = 0; i < s.line_pos; ++i) {
      SessionWrite(s, "\b \b");
    }
    s.line_pos = 0;
    s.line_buf[0] = '\0';
    return;
  }
  s.hist_nav = next;
  ReplaceLine(s, s.history[s.hist_nav]);
}

/// @brief Handle tab completion.
inline void TabComplete(Session& s, const char* prompt) noexcept {
  s.line_buf[s.line_pos] = '\0';
  char completion[64] = {};
  uint32_t matches = CommandRegistry::Instance().AutoComplete(s.line_buf, completion, sizeof(completion));

  if (matches == 1) {
    // Single match: replace line with completion + space.
    for (uint32_t i = 0; i < s.line_pos; ++i) {
      SessionWrite(s, "\b \b");
    }
    uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
    if (comp_len >= EMBSH_LINE_BUF_SIZE - 2) {
      comp_len = EMBSH_LINE_BUF_SIZE - 3;
    }
    std::memcpy(s.line_buf, completion, comp_len);
    s.line_buf[comp_len] = ' ';
    s.line_pos = comp_len + 1;
    s.line_buf[s.line_pos] = '\0';
    SessionWriteN(s, s.line_buf, s.line_pos);
  } else if (matches > 1) {
    // Show all matches.
    SessionWrite(s, "\r\n");
    CommandRegistry::Instance().ForEach([&s](const CmdEntry& cmd) {
      if (std::strncmp(cmd.name, s.line_buf, s.line_pos) == 0) {
        SessionWrite(s, cmd.name);
        SessionWrite(s, "  ");
      }
    });
    SessionWrite(s, "\r\n");
    SessionWrite(s, prompt);
    // Fill with longest common prefix.
    uint32_t comp_len = static_cast<uint32_t>(std::strlen(completion));
    if (comp_len >= EMBSH_LINE_BUF_SIZE - 1) {
      comp_len = EMBSH_LINE_BUF_SIZE - 2;
    }
    std::memcpy(s.line_buf, completion, comp_len);
    s.line_pos = comp_len;
    s.line_buf[s.line_pos] = '\0';
    SessionWriteN(s, s.line_buf, s.line_pos);
  }
}

/// @brief Filter IAC telnet protocol bytes.
/// @return The printable character, or '\0' if the byte was consumed.
inline char FilterIac(Session& s, uint8_t byte) noexcept {
  switch (s.iac_state) {
    case Session::IacState::kNormal:
      if (byte == 0xFF) {  // IAC
        s.iac_state = Session::IacState::kIac;
        return '\0';
      }
      return static_cast<char>(byte);

    case Session::IacState::kIac:
      if (byte >= 0xFB && byte <= 0xFE) {  // WILL/WONT/DO/DONT
        s.iac_state = Session::IacState::kNego;
        return '\0';
      }
      if (byte == 0xFA) {  // SB
        s.iac_state = Session::IacState::kSub;
        return '\0';
      }
      // IAC IAC = literal 0xFF, or unknown -> reset.
      s.iac_state = Session::IacState::kNormal;
      return (byte == 0xFF) ? static_cast<char>(0xFF) : '\0';

    case Session::IacState::kNego:
      // Option byte after WILL/WONT/DO/DONT: ignore and return to normal.
      s.iac_state = Session::IacState::kNormal;
      return '\0';

    case Session::IacState::kSub:
      // Consume until IAC SE (0xFF 0xF0).
      if (byte == 0xFF) {
        s.iac_state = Session::IacState::kIac;
      }
      return '\0';
  }
  s.iac_state = Session::IacState::kNormal;
  return '\0';
}

/// @brief Process one input byte in a session.
/// @return true if a complete line is ready for execution.
inline bool ProcessByte(Session& s, uint8_t byte, const char* prompt) noexcept {
  // IAC filtering for telnet mode.
  if (s.telnet_mode) {
    char ch = FilterIac(s, byte);
    if (ch == '\0')
      return false;
    byte = static_cast<uint8_t>(ch);
  }

  char ch = static_cast<char>(byte);

  // ESC sequence FSM.
  switch (s.esc_state) {
    case Session::EscState::kEsc:
      if (ch == '[') {
        s.esc_state = Session::EscState::kBracket;
        return false;
      }
      s.esc_state = Session::EscState::kNone;
      return false;  // Unknown ESC sequence, ignore.

    case Session::EscState::kBracket:
      s.esc_state = Session::EscState::kNone;
      switch (ch) {
        case 'A':
          HistoryUp(s);
          return false;  // Up arrow
        case 'B':
          HistoryDown(s);
          return false;  // Down arrow
        case 'C':
          return false;  // Right arrow (TODO: cursor move)
        case 'D':
          return false;  // Left arrow (TODO: cursor move)
        default:
          return false;
      }

    case Session::EscState::kNone:
      break;
  }

  // ESC starts escape sequence.
  if (byte == 0x1B) {
    s.esc_state = Session::EscState::kEsc;
    return false;
  }

  // Ctrl+C: cancel current line.
  if (byte == 0x03) {
    SessionWrite(s, "^C\r\n");
    s.line_pos = 0;
    s.line_buf[0] = '\0';
    s.hist_browsing = false;
    SessionWrite(s, prompt);
    return false;
  }

  // Ctrl+D: EOF.
  if (byte == 0x04) {
    if (s.line_pos == 0) {
      SessionWrite(s, "\r\nBye.\r\n");
      s.active.store(false, std::memory_order_release);
    }
    return false;
  }

  // Backspace.
  if (byte == 0x7F || byte == 0x08) {
    if (s.line_pos > 0) {
      --s.line_pos;
      SessionWrite(s, "\b \b");
    }
    return false;
  }

  // Tab: auto-complete.
  if (byte == '\t') {
    TabComplete(s, prompt);
    return false;
  }

  // Enter: execute line.
  if (ch == '\r' || ch == '\n') {
    SessionWrite(s, "\r\n");

    // Telnet \r\n: peek-and-consume trailing byte.
    if (s.telnet_mode && ch == '\r') {
      char next;
      ssize_t peek = ::recv(s.read_fd, &next, 1, MSG_PEEK);
      if (peek == 1 && (next == '\n' || next == '\0')) {
        (void)::recv(s.read_fd, &next, 1, 0);
      }
    }

    s.line_buf[s.line_pos] = '\0';
    s.hist_browsing = false;

    if (s.line_pos > 0) {
      PushHistory(s);
      return true;  // Line ready for execution.
    }

    SessionWrite(s, prompt);
    return false;
  }

  // Regular printable character.
  if (s.line_pos < EMBSH_LINE_BUF_SIZE - 1 && byte >= 0x20 && byte < 0x7F) {
    s.line_buf[s.line_pos++] = ch;
    SessionWriteN(s, &ch, 1);
  }

  return false;
}

/// @brief Execute the current line buffer of a session.
inline void ExecuteLine(Session& s) noexcept {
  char cmd_copy[EMBSH_LINE_BUF_SIZE];
  std::memcpy(cmd_copy, s.line_buf, s.line_pos + 1);

  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = ShellSplit(cmd_copy, s.line_pos, argv);

  if (argc <= 0)
    return;

  // Built-in: exit / quit.
  if (std::strcmp(argv[0], "exit") == 0 || std::strcmp(argv[0], "quit") == 0) {
    SessionWrite(s, "Bye.\r\n");
    s.active.store(false, std::memory_order_release);
    return;
  }

  const CmdEntry* cmd = CommandRegistry::Instance().Find(argv[0]);
  if (cmd != nullptr) {
    // Set up session output for ShellPrintf routing.
    auto write_adapter = [](const char* str, void* ctx) {
      auto* sess = static_cast<Session*>(ctx);
      SessionWrite(*sess, str);
    };
    detail::CurrentOutput().write = write_adapter;
    detail::CurrentOutput().ctx = &s;

    cmd->fn(argc, argv, cmd->ctx);

    detail::CurrentOutput().write = nullptr;
    detail::CurrentOutput().ctx = nullptr;
  } else {
    char msg[160];
    int n = std::snprintf(msg, sizeof(msg), "unknown command: %s\r\n", argv[0]);
    if (n > 0) {
      SessionWriteN(s, msg, static_cast<size_t>(n));
    }
  }
}

}  // namespace editor

}  // namespace embsh

#endif  // EMBSH_LINE_EDITOR_HPP_
