/**
 * @file test_telnet_server.cpp
 * @brief Unit tests for TelnetServer: start/stop, connection, authentication.
 */

#include <catch2/catch_test_macros.hpp>

#include "embsh/telnet_server.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// ============================================================================
// Helper: connect to localhost TCP port.
// ============================================================================

static int TcpConnect(uint16_t port, int timeout_ms = 1000) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) == 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::close(fd);
  return -1;
}

static std::string TcpRecv(int fd, int timeout_ms = 500) {
  std::string result;
  char buf[512];
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 50);
    if (pr > 0) {
      ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        result += buf;
      } else {
        break;
      }
    }
  }
  return result;
}

static void TcpSend(int fd, const char* str) {
  (void)::send(fd, str, std::strlen(str), MSG_NOSIGNAL);
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("TelnetServer: start and stop", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23230;
  cfg.banner = nullptr;
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());
  CHECK(server.IsRunning());

  server.Stop();
  CHECK_FALSE(server.IsRunning());
}

TEST_CASE("TelnetServer: start is idempotent", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23231;
  cfg.banner = nullptr;
  embsh::TelnetServer server(cfg);

  auto r1 = server.Start();
  REQUIRE(r1.has_value());

  auto r2 = server.Start();
  CHECK_FALSE(r2.has_value());
  CHECK(r2.error_value() == embsh::ShellError::kAlreadyRunning);

  server.Stop();
}

TEST_CASE("TelnetServer: stop when not running is safe", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23232;
  embsh::TelnetServer server(cfg);
  server.Stop();  // Should not crash.
  CHECK_FALSE(server.IsRunning());
}

TEST_CASE("TelnetServer: client can connect and receive banner", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23233;
  cfg.banner = "\r\nWelcome!\r\n";
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());

  int client = TcpConnect(cfg.port);
  REQUIRE(client >= 0);

  std::string data = TcpRecv(client, 500);
  // Should contain the banner and prompt (may also contain IAC sequences).
  CHECK(data.find("Welcome!") != std::string::npos);

  ::close(client);
  server.Stop();
}

TEST_CASE("TelnetServer: command execution via telnet", "[telnet_server]") {
  // Register a test command.
  static bool cmd_executed = false;
  cmd_executed = false;
  auto test_fn = [](int /*argc*/, char* /*argv*/[], void* /*ctx*/) -> int {
    cmd_executed = true;
    return 0;
  };
  embsh::CommandRegistry::Instance().Register("telnet_test_cmd", test_fn,
                                              "test cmd");

  embsh::ServerConfig cfg;
  cfg.port = 23234;
  cfg.banner = nullptr;
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());

  int client = TcpConnect(cfg.port);
  REQUIRE(client >= 0);

  // Wait for prompt.
  (void)TcpRecv(client, 300);

  // Send command.
  TcpSend(client, "telnet_test_cmd\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  CHECK(cmd_executed == true);

  ::close(client);
  server.Stop();
}

TEST_CASE("TelnetServer: exit command closes session", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23235;
  cfg.banner = nullptr;
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());

  int client = TcpConnect(cfg.port);
  REQUIRE(client >= 0);

  // Wait for prompt.
  (void)TcpRecv(client, 300);

  // Send exit command.
  TcpSend(client, "exit\r\n");
  std::string response = TcpRecv(client, 300);
  CHECK(response.find("Bye") != std::string::npos);

  ::close(client);
  server.Stop();
}

TEST_CASE("TelnetServer: authentication required", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23236;
  cfg.banner = nullptr;
  cfg.username = "admin";
  cfg.password = "secret";
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());

  int client = TcpConnect(cfg.port);
  REQUIRE(client >= 0);

  // Should receive "Username: " prompt.
  std::string data = TcpRecv(client, 500);
  CHECK(data.find("Username:") != std::string::npos);

  // Send correct credentials.
  TcpSend(client, "admin\r\n");
  data = TcpRecv(client, 300);
  CHECK(data.find("Password:") != std::string::npos);

  TcpSend(client, "secret\r\n");
  data = TcpRecv(client, 300);
  CHECK(data.find("Login successful") != std::string::npos);

  ::close(client);
  server.Stop();
}

TEST_CASE("TelnetServer: authentication failure", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23237;
  cfg.banner = nullptr;
  cfg.username = "admin";
  cfg.password = "secret";
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());

  int client = TcpConnect(cfg.port);
  REQUIRE(client >= 0);

  // Wait for Username prompt.
  (void)TcpRecv(client, 500);

  // Send wrong credentials 3 times.
  for (int i = 0; i < 3; ++i) {
    TcpSend(client, "wrong\r\n");
    (void)TcpRecv(client, 300);
    TcpSend(client, "wrong\r\n");
    (void)TcpRecv(client, 300);
  }

  // Connection should be closed after max attempts.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ::close(client);
  server.Stop();
}

TEST_CASE("TelnetServer: reject connection when full", "[telnet_server]") {
  embsh::ServerConfig cfg;
  cfg.port = 23238;
  cfg.max_sessions = 1;
  cfg.banner = nullptr;
  embsh::TelnetServer server(cfg);

  auto r = server.Start();
  REQUIRE(r.has_value());

  // First connection should succeed.
  int client1 = TcpConnect(cfg.port);
  REQUIRE(client1 >= 0);
  (void)TcpRecv(client1, 300);

  // Second connection should be rejected.
  int client2 = TcpConnect(cfg.port);
  if (client2 >= 0) {
    std::string data = TcpRecv(client2, 300);
    CHECK(data.find("Too many") != std::string::npos);
    ::close(client2);
  }

  ::close(client1);
  server.Stop();
}
