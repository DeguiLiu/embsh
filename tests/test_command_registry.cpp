/**
 * @file test_command_registry.cpp
 * @brief Unit tests for CommandRegistry, ShellSplit, and auto-registration.
 */

#include <catch2/catch_test_macros.hpp>

#include "embsh/command_registry.hpp"

#include <cstring>

// ============================================================================
// ShellSplit tests
// ============================================================================

TEST_CASE("ShellSplit: empty input", "[command_registry]") {
  char buf[64] = "";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, 0, argv);
  CHECK(argc == 0);
}

TEST_CASE("ShellSplit: single word", "[command_registry]") {
  char buf[64] = "hello";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, 5, argv);
  REQUIRE(argc == 1);
  CHECK(std::strcmp(argv[0], "hello") == 0);
}

TEST_CASE("ShellSplit: multiple words", "[command_registry]") {
  char buf[64] = "cmd arg1 arg2 arg3";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, static_cast<uint32_t>(std::strlen(buf)), argv);
  REQUIRE(argc == 4);
  CHECK(std::strcmp(argv[0], "cmd") == 0);
  CHECK(std::strcmp(argv[1], "arg1") == 0);
  CHECK(std::strcmp(argv[2], "arg2") == 0);
  CHECK(std::strcmp(argv[3], "arg3") == 0);
}

TEST_CASE("ShellSplit: double-quoted string", "[command_registry]") {
  char buf[64] = "echo \"hello world\"";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, static_cast<uint32_t>(std::strlen(buf)), argv);
  REQUIRE(argc == 2);
  CHECK(std::strcmp(argv[0], "echo") == 0);
  CHECK(std::strcmp(argv[1], "hello world") == 0);
}

TEST_CASE("ShellSplit: single-quoted string", "[command_registry]") {
  char buf[64] = "echo 'hello world'";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, static_cast<uint32_t>(std::strlen(buf)), argv);
  REQUIRE(argc == 2);
  CHECK(std::strcmp(argv[0], "echo") == 0);
  CHECK(std::strcmp(argv[1], "hello world") == 0);
}

TEST_CASE("ShellSplit: leading/trailing whitespace", "[command_registry]") {
  char buf[64] = "  cmd  arg1  ";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, static_cast<uint32_t>(std::strlen(buf)), argv);
  REQUIRE(argc == 2);
  CHECK(std::strcmp(argv[0], "cmd") == 0);
  CHECK(std::strcmp(argv[1], "arg1") == 0);
}

TEST_CASE("ShellSplit: tab separator", "[command_registry]") {
  char buf[64] = "cmd\targ1";
  char* argv[EMBSH_MAX_ARGS] = {};
  int argc = embsh::ShellSplit(buf, static_cast<uint32_t>(std::strlen(buf)), argv);
  REQUIRE(argc == 2);
  CHECK(std::strcmp(argv[0], "cmd") == 0);
  CHECK(std::strcmp(argv[1], "arg1") == 0);
}

// ============================================================================
// CommandRegistry tests
// ============================================================================

static int test_cmd_a(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  return 42;
}

static int test_cmd_b(int /*argc*/, char* /*argv*/[], void* /*ctx*/) {
  return 99;
}

static int test_cmd_ctx(int /*argc*/, char* /*argv*/[], void* ctx) {
  int* val = static_cast<int*>(ctx);
  *val = 123;
  return 0;
}

// NOTE: CommandRegistry is a singleton, so tests share state.
// Tests are ordered to account for this.

TEST_CASE("CommandRegistry: register and find", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  // "help" is already registered by the built-in.
  uint32_t initial_count = reg.Count();

  auto r = reg.Register("test_a", test_cmd_a, "Test command A");
  CHECK(r.has_value());
  CHECK(reg.Count() == initial_count + 1);

  const auto* found = reg.Find("test_a");
  REQUIRE(found != nullptr);
  CHECK(std::strcmp(found->name, "test_a") == 0);
  CHECK(found->fn == test_cmd_a);
}

TEST_CASE("CommandRegistry: duplicate name rejected", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  // Ensure "test_a" is registered (may already be from prior test).
  (void)reg.Register("test_a", test_cmd_a, "Test command A");

  auto r = reg.Register("test_a", test_cmd_b, "Duplicate");
  CHECK_FALSE(r.has_value());
  CHECK(r.error_value() == embsh::ShellError::kDuplicateName);
}

TEST_CASE("CommandRegistry: find non-existent returns nullptr", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  CHECK(reg.Find("nonexistent") == nullptr);
}

TEST_CASE("CommandRegistry: context pointer passed to command", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  int value = 0;
  auto r = reg.Register("test_ctx", test_cmd_ctx, &value, "Context test");
  REQUIRE(r.has_value());

  const auto* cmd = reg.Find("test_ctx");
  REQUIRE(cmd != nullptr);
  cmd->fn(0, nullptr, cmd->ctx);
  CHECK(value == 123);
}

TEST_CASE("CommandRegistry: AutoComplete single match", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  reg.Register("autocomplete_foo", test_cmd_a, "auto foo");

  char buf[64] = {};
  uint32_t matches = reg.AutoComplete("autocomplete_", buf, sizeof(buf));
  CHECK(matches == 1);
  CHECK(std::strcmp(buf, "autocomplete_foo") == 0);
}

TEST_CASE("CommandRegistry: AutoComplete multiple matches", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  reg.Register("multi_alpha", test_cmd_a, "alpha");
  reg.Register("multi_beta", test_cmd_b, "beta");

  char buf[64] = {};
  uint32_t matches = reg.AutoComplete("multi_", buf, sizeof(buf));
  CHECK(matches == 2);
  CHECK(std::strcmp(buf, "multi_") == 0);  // Longest common prefix.
}

TEST_CASE("CommandRegistry: AutoComplete no match", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  char buf[64] = {};
  uint32_t matches = reg.AutoComplete("zzz_no_match_", buf, sizeof(buf));
  CHECK(matches == 0);
  CHECK(buf[0] == '\0');
}

TEST_CASE("CommandRegistry: ForEach visits all commands", "[command_registry]") {
  auto& reg = embsh::CommandRegistry::Instance();
  uint32_t visited = 0;
  reg.ForEach([&visited](const embsh::CmdEntry& /*cmd*/) { ++visited; });
  CHECK(visited == reg.Count());
}
