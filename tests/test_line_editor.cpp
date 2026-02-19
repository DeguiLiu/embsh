/**
 * @file test_line_editor.cpp
 * @brief Unit tests for line editing, history, ESC sequences, and IAC filter.
 */

#include <catch2/catch_test_macros.hpp>

#include "embsh/line_editor.hpp"

#include <cstring>

// ============================================================================
// Helper: create a session with a pipe backend for testing.
// ============================================================================

struct PipePair {
  int read_fd = -1;
  int write_fd = -1;

  PipePair() {
    int fds[2];
    if (::pipe(fds) == 0) {
      read_fd = fds[0];
      write_fd = fds[1];
    }
  }

  ~PipePair() {
    if (read_fd >= 0)
      ::close(read_fd);
    if (write_fd >= 0)
      ::close(write_fd);
  }
};

static void InitTestSession(embsh::Session& s, PipePair& output) {
  s.read_fd = -1;  // Not used directly in ProcessByte tests.
  s.write_fd = output.write_fd;
  s.write_fn = embsh::io::PosixWrite;
  s.read_fn = embsh::io::PosixRead;
  s.telnet_mode = false;
  s.line_pos = 0;
  s.hist_count = 0;
  s.hist_write = 0;
  s.hist_browsing = false;
  s.esc_state = embsh::Session::EscState::kNone;
  s.iac_state = embsh::Session::IacState::kNormal;
  s.active.store(true, std::memory_order_relaxed);
}

// ============================================================================
// ProcessByte tests
// ============================================================================

TEST_CASE("LineEditor: printable characters accumulate in buffer", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 'h', prompt);
  embsh::editor::ProcessByte(s, 'i', prompt);

  CHECK(s.line_pos == 2);
  CHECK(s.line_buf[0] == 'h');
  CHECK(s.line_buf[1] == 'i');
}

TEST_CASE("LineEditor: backspace removes character", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 'a', prompt);
  embsh::editor::ProcessByte(s, 'b', prompt);
  CHECK(s.line_pos == 2);

  embsh::editor::ProcessByte(s, 0x7F, prompt);  // Backspace
  CHECK(s.line_pos == 1);
  CHECK(s.line_buf[0] == 'a');
}

TEST_CASE("LineEditor: backspace on empty line does nothing", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 0x7F, prompt);
  CHECK(s.line_pos == 0);
}

TEST_CASE("LineEditor: Enter returns true for non-empty line", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 'l', prompt);
  embsh::editor::ProcessByte(s, 's', prompt);

  bool ready = embsh::editor::ProcessByte(s, '\r', prompt);
  CHECK(ready == true);
  CHECK(s.line_buf[0] == 'l');
  CHECK(s.line_buf[1] == 's');
}

TEST_CASE("LineEditor: Enter on empty line returns false", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  bool ready = embsh::editor::ProcessByte(s, '\r', prompt);
  CHECK(ready == false);
}

TEST_CASE("LineEditor: Ctrl+C clears line", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 'a', prompt);
  embsh::editor::ProcessByte(s, 'b', prompt);
  CHECK(s.line_pos == 2);

  embsh::editor::ProcessByte(s, 0x03, prompt);  // Ctrl+C
  CHECK(s.line_pos == 0);
}

TEST_CASE("LineEditor: Ctrl+D on empty line deactivates session", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 0x04, prompt);  // Ctrl+D
  CHECK(s.active.load() == false);
}

TEST_CASE("LineEditor: non-printable bytes below 0x20 are ignored", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 0x01, prompt);  // Ctrl+A
  embsh::editor::ProcessByte(s, 0x02, prompt);  // Ctrl+B
  CHECK(s.line_pos == 0);
}

// ============================================================================
// History tests
// ============================================================================

TEST_CASE("LineEditor: PushHistory stores entries", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);

  std::strcpy(s.line_buf, "cmd1");
  s.line_pos = 4;
  embsh::editor::PushHistory(s);
  CHECK(s.hist_count == 1);

  std::strcpy(s.line_buf, "cmd2");
  s.line_pos = 4;
  embsh::editor::PushHistory(s);
  CHECK(s.hist_count == 2);
}

TEST_CASE("LineEditor: PushHistory skips duplicates", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);

  std::strcpy(s.line_buf, "repeat");
  s.line_pos = 6;
  embsh::editor::PushHistory(s);
  CHECK(s.hist_count == 1);

  std::strcpy(s.line_buf, "repeat");
  s.line_pos = 6;
  embsh::editor::PushHistory(s);
  CHECK(s.hist_count == 1);  // Not incremented.
}

TEST_CASE("LineEditor: PushHistory ignores empty input", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);

  s.line_pos = 0;
  embsh::editor::PushHistory(s);
  CHECK(s.hist_count == 0);
}

TEST_CASE("LineEditor: arrow up navigates history", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);

  // Push two history entries.
  std::strcpy(s.line_buf, "first");
  s.line_pos = 5;
  embsh::editor::PushHistory(s);

  std::strcpy(s.line_buf, "second");
  s.line_pos = 6;
  embsh::editor::PushHistory(s);

  // Clear current line.
  s.line_pos = 0;
  s.line_buf[0] = '\0';

  // Arrow up: should show "second" (most recent).
  embsh::editor::HistoryUp(s);
  CHECK(std::strcmp(s.line_buf, "second") == 0);

  // Arrow up again: should show "first".
  embsh::editor::HistoryUp(s);
  CHECK(std::strcmp(s.line_buf, "first") == 0);
}

TEST_CASE("LineEditor: arrow down returns to empty line", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);

  std::strcpy(s.line_buf, "cmd");
  s.line_pos = 3;
  embsh::editor::PushHistory(s);

  s.line_pos = 0;
  s.line_buf[0] = '\0';

  // Navigate up then down.
  embsh::editor::HistoryUp(s);
  CHECK(std::strcmp(s.line_buf, "cmd") == 0);

  embsh::editor::HistoryDown(s);
  CHECK(s.line_pos == 0);
}

// ============================================================================
// ESC sequence tests
// ============================================================================

TEST_CASE("LineEditor: ESC sequence triggers history up", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  // Add history.
  std::strcpy(s.line_buf, "history_cmd");
  s.line_pos = 11;
  embsh::editor::PushHistory(s);
  s.line_pos = 0;
  s.line_buf[0] = '\0';

  // ESC [ A = Up arrow.
  embsh::editor::ProcessByte(s, 0x1B, prompt);  // ESC
  embsh::editor::ProcessByte(s, '[', prompt);
  embsh::editor::ProcessByte(s, 'A', prompt);  // Up

  CHECK(std::strcmp(s.line_buf, "history_cmd") == 0);
}

TEST_CASE("LineEditor: unknown ESC sequence is ignored", "[line_editor]") {
  PipePair out;
  embsh::Session s;
  InitTestSession(s, out);
  const char* prompt = "> ";

  embsh::editor::ProcessByte(s, 'x', prompt);
  embsh::editor::ProcessByte(s, 0x1B, prompt);  // ESC
  embsh::editor::ProcessByte(s, 'O', prompt);   // Not '[', reset

  CHECK(s.line_pos == 1);
  CHECK(s.line_buf[0] == 'x');
}

// ============================================================================
// IAC filter tests
// ============================================================================

TEST_CASE("LineEditor: IAC filter passes normal bytes", "[line_editor]") {
  embsh::Session s;
  s.iac_state = embsh::Session::IacState::kNormal;

  CHECK(embsh::editor::FilterIac(s, 'A') == 'A');
  CHECK(embsh::editor::FilterIac(s, 'z') == 'z');
}

TEST_CASE("LineEditor: IAC filter consumes IAC WILL", "[line_editor]") {
  embsh::Session s;
  s.iac_state = embsh::Session::IacState::kNormal;

  CHECK(embsh::editor::FilterIac(s, 0xFF) == '\0');  // IAC
  CHECK(embsh::editor::FilterIac(s, 0xFB) == '\0');  // WILL
  CHECK(embsh::editor::FilterIac(s, 0x01) == '\0');  // Option: ECHO
  // Back to normal.
  CHECK(embsh::editor::FilterIac(s, 'x') == 'x');
}

TEST_CASE("LineEditor: IAC filter consumes subnegotiation", "[line_editor]") {
  embsh::Session s;
  s.iac_state = embsh::Session::IacState::kNormal;

  CHECK(embsh::editor::FilterIac(s, 0xFF) == '\0');  // IAC
  CHECK(embsh::editor::FilterIac(s, 0xFA) == '\0');  // SB
  CHECK(embsh::editor::FilterIac(s, 0x1F) == '\0');  // Data
  CHECK(embsh::editor::FilterIac(s, 0x00) == '\0');  // Data
  CHECK(embsh::editor::FilterIac(s, 0xFF) == '\0');  // IAC (in sub)
  CHECK(embsh::editor::FilterIac(s, 0xF0) == '\0');  // SE (via IAC handler reset)
  // Should be back to normal now.
  CHECK(embsh::editor::FilterIac(s, 'y') == 'y');
}

TEST_CASE("LineEditor: IAC IAC passes literal 0xFF", "[line_editor]") {
  embsh::Session s;
  s.iac_state = embsh::Session::IacState::kNormal;

  CHECK(embsh::editor::FilterIac(s, 0xFF) == '\0');                     // IAC
  CHECK(embsh::editor::FilterIac(s, 0xFF) == static_cast<char>(0xFF));  // Literal 0xFF
}
