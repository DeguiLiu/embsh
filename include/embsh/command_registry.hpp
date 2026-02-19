/**
 * @file command_registry.hpp
 * @brief Global command registry with context pointer, auto-complete, and
 *        in-place command line tokenizer.
 *
 * Provides a fixed-capacity (EMBSH_MAX_COMMANDS) command table, thread-safe
 * registration, tab completion, and the EMBSH_CMD auto-registration macro.
 */

#ifndef EMBSH_COMMAND_REGISTRY_HPP_
#define EMBSH_COMMAND_REGISTRY_HPP_

#include "embsh/platform.hpp"
#include "embsh/types.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef EMBSH_MAX_COMMANDS
#define EMBSH_MAX_COMMANDS 64
#endif

#ifndef EMBSH_MAX_ARGS
#define EMBSH_MAX_ARGS 32
#endif

namespace embsh {

// ============================================================================
// Command types
// ============================================================================

/// @brief Unified command function signature with context pointer.
using CmdFn = int (*)(int argc, char* argv[], void* ctx);

/// @brief Descriptor for a registered shell command.
struct CmdEntry {
  const char* name = nullptr;  ///< Command name (must be static storage).
  const char* desc = nullptr;  ///< Human-readable description.
  CmdFn fn = nullptr;          ///< Callback to invoke.
  void* ctx = nullptr;         ///< User context passed to fn.
};

// ============================================================================
// ShellSplit - In-place command line tokenizer
// ============================================================================

/**
 * @brief Split a command line in-place into argc / argv.
 *
 * Replaces whitespace with NUL bytes and fills @p argv with pointers into
 * @p cmd.  Supports single-quoted, double-quoted strings and backslash escape.
 *
 * @param cmd    Mutable command line buffer (modified in-place).
 * @param length Length of @p cmd in bytes (excluding NUL terminator).
 * @param argv   Output array of argument pointers; must hold EMBSH_MAX_ARGS.
 * @return Number of arguments parsed (argc), or -1 on overflow.
 */
inline int ShellSplit(char* cmd, uint32_t length, char* argv[EMBSH_MAX_ARGS]) noexcept {
  int argc = 0;
  uint32_t i = 0;

  while (i < length && argc < EMBSH_MAX_ARGS) {
    // Skip leading whitespace.
    while (i < length && (cmd[i] == ' ' || cmd[i] == '\t')) {
      cmd[i] = '\0';
      ++i;
    }
    if (i >= length)
      break;

    if (cmd[i] == '"' || cmd[i] == '\'') {
      // Quoted argument.
      char quote = cmd[i];
      cmd[i] = '\0';
      ++i;
      if (i >= length)
        break;
      argv[argc++] = &cmd[i];
      while (i < length && cmd[i] != quote) {
        if (cmd[i] == '\\' && (i + 1) < length) {
          // Shift left to remove backslash.
          std::memmove(&cmd[i], &cmd[i + 1], length - i - 1);
          --length;
        } else {
          ++i;
        }
      }
      if (i < length) {
        cmd[i] = '\0';
        ++i;
      }
    } else {
      // Unquoted argument.
      argv[argc++] = &cmd[i];
      while (i < length && cmd[i] != ' ' && cmd[i] != '\t') {
        ++i;
      }
    }
  }

  return argc;
}

// ============================================================================
// CommandRegistry - Meyer's singleton command table
// ============================================================================

/**
 * @brief Global command registry.
 *
 * Thread-safe for registration (mutex-protected). Lookup and enumeration are
 * read-only after registration phase. Capacity: EMBSH_MAX_COMMANDS.
 */
class CommandRegistry final {
 public:
  static CommandRegistry& Instance() noexcept {
    static CommandRegistry reg;
    return reg;
  }

  /**
   * @brief Register a command.
   * @return success or ShellError on failure.
   */
  inline expected<void, ShellError> Register(const char* name, CmdFn fn, void* ctx, const char* desc) noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(cmds_[i].name, name) == 0) {
        return expected<void, ShellError>::error(ShellError::kDuplicateName);
      }
    }
    if (count_ >= EMBSH_MAX_COMMANDS) {
      return expected<void, ShellError>::error(ShellError::kRegistryFull);
    }
    cmds_[count_].name = name;
    cmds_[count_].desc = desc;
    cmds_[count_].fn = fn;
    cmds_[count_].ctx = ctx;
    ++count_;
    return expected<void, ShellError>::success();
  }

  /// @brief Convenience: register without context.
  inline expected<void, ShellError> Register(const char* name, CmdFn fn, const char* desc) noexcept {
    return Register(name, fn, nullptr, desc);
  }

  /// @brief Find a command by exact name.
  inline const CmdEntry* Find(const char* name) const noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(cmds_[i].name, name) == 0) {
        return &cmds_[i];
      }
    }
    return nullptr;
  }

  /**
   * @brief Auto-complete a command name prefix.
   * @return Number of matching commands.
   */
  inline uint32_t AutoComplete(const char* prefix, char* out_buf, uint32_t buf_size) const noexcept {
    if (buf_size == 0 || prefix == nullptr || out_buf == nullptr)
      return 0;

    const uint32_t prefix_len = static_cast<uint32_t>(std::strlen(prefix));
    uint32_t match_idx[EMBSH_MAX_COMMANDS];
    uint32_t match_count = 0;

    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strncmp(cmds_[i].name, prefix, prefix_len) == 0) {
        match_idx[match_count++] = i;
      }
    }

    if (match_count == 0) {
      out_buf[0] = '\0';
      return 0;
    }

    if (match_count == 1) {
      const char* full = cmds_[match_idx[0]].name;
      uint32_t len = static_cast<uint32_t>(std::strlen(full));
      uint32_t n = (len < buf_size - 1) ? len : (buf_size - 1);
      std::memcpy(out_buf, full, n);
      out_buf[n] = '\0';
      return 1;
    }

    // Multiple matches: longest common prefix.
    const char* first = cmds_[match_idx[0]].name;
    uint32_t common = static_cast<uint32_t>(std::strlen(first));
    for (uint32_t m = 1; m < match_count; ++m) {
      const char* other = cmds_[match_idx[m]].name;
      uint32_t j = 0;
      while (j < common && first[j] == other[j])
        ++j;
      common = j;
    }
    uint32_t n = (common < buf_size - 1) ? common : (buf_size - 1);
    std::memcpy(out_buf, first, n);
    out_buf[n] = '\0';
    return match_count;
  }

  /// @brief Iterate over all registered commands.
  inline void ForEach(function_ref<void(const CmdEntry&)> visitor) const noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      visitor(cmds_[i]);
    }
  }

  uint32_t Count() const noexcept { return count_; }

 private:
  CommandRegistry() = default;
  CommandRegistry(const CommandRegistry&) = delete;
  CommandRegistry& operator=(const CommandRegistry&) = delete;

  CmdEntry cmds_[EMBSH_MAX_COMMANDS] = {};
  uint32_t count_ = 0;
  mutable std::mutex mtx_;
};

// ============================================================================
// Built-in help command
// ============================================================================

namespace detail {

/// @brief Session write callback type (forward declaration for help command).
using SessionWriteFn = void (*)(const char* str, void* session_ctx);

/// @brief Thread-local session write context for Printf routing.
struct SessionOutput {
  SessionWriteFn write = nullptr;
  void* ctx = nullptr;
};

inline SessionOutput& CurrentOutput() noexcept {
  static thread_local SessionOutput out;
  return out;
}

}  // namespace detail

/// @brief Printf into the current session's output.
inline int ShellPrintf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

inline int ShellPrintf(const char* fmt, ...) {
  auto& out = detail::CurrentOutput();
  if (out.write == nullptr)
    return -1;

  char buf[512];
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (n > 0) {
    buf[sizeof(buf) - 1] = '\0';
    out.write(buf, out.ctx);
  }
  return n;
}

namespace detail {

inline int HelpCommand(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  CommandRegistry::Instance().ForEach(
      [](const CmdEntry& cmd) { ShellPrintf("  %-16s - %s\r\n", cmd.name, cmd.desc ? cmd.desc : ""); });
  return 0;
}

inline bool RegisterHelpOnce() noexcept {
  static const bool done = []() {
    CommandRegistry::Instance().Register("help", HelpCommand, "List all commands");
    return true;
  }();
  return done;
}

static const bool kHelpRegistered EMBSH_UNUSED = RegisterHelpOnce();

}  // namespace detail

// ============================================================================
// Auto-registration helpers
// ============================================================================

/// @brief Static auto-registration helper.
class CmdAutoReg {
 public:
  CmdAutoReg(const char* name, CmdFn fn, void* ctx, const char* desc) {
    CommandRegistry::Instance().Register(name, fn, ctx, desc);
  }
  CmdAutoReg(const char* name, CmdFn fn, const char* desc) { CommandRegistry::Instance().Register(name, fn, desc); }
};

/**
 * @brief Register a function as a shell command.
 *
 * Usage:
 * @code
 *   static int reboot(int argc, char* argv[], void* ctx) { ... }
 *   EMBSH_CMD(reboot, "Reboot the system");
 * @endcode
 */
#define EMBSH_CMD(cmd, desc) static ::embsh::CmdAutoReg EMBSH_CONCAT(_embsh_reg_, cmd)(#cmd, cmd, desc)

/**
 * @brief RT-Thread MSH compatible registration macro.
 *
 * Maps to EMBSH_CMD for source-level compatibility with RT-Thread finsh.
 * On RT-Thread, replace this header with <finsh.h>.
 */
#define MSH_CMD_EXPORT(cmd, desc) EMBSH_CMD(cmd, desc)

}  // namespace embsh

#endif  // EMBSH_COMMAND_REGISTRY_HPP_
