#include "util/terminal.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>

#include "util/text.hpp"
#else
#include <termios.h>
#include <unistd.h>

#include <tuple>
#endif

namespace streamextract {

namespace {

std::atomic<bool> g_interrupted{false};

void on_interrupt(int) { g_interrupted = true; }

}  // namespace

void install_interrupt_handler() {
  std::signal(SIGINT, on_interrupt);
  std::signal(SIGTERM, on_interrupt);
}

bool interrupt_requested() { return g_interrupted; }

#ifdef _WIN32

bool stdin_is_terminal() { return _isatty(_fileno(stdin)) != 0; }
bool stdout_is_terminal() { return _isatty(_fileno(stdout)) != 0; }

std::optional<std::string> prompt_hidden(const std::string& prompt) {
  std::fputs(prompt.c_str(), stderr);
  std::fflush(stderr);

  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
    return std::nullopt;
  }
  SetConsoleMode(input, (mode | ENABLE_LINE_INPUT) & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));

  std::wstring line;
  bool ok = false;
  wchar_t buffer[256];
  DWORD read = 0;
  while (ReadConsoleW(input, buffer, 256, &read, nullptr) && read > 0) {
    line.append(buffer, read);
    const auto eol = line.find_first_of(L"\r\n");
    if (eol != std::wstring::npos) {
      line.resize(eol);
      ok = true;
      break;
    }
  }
  SetConsoleMode(input, mode);
  std::fputs("\n", stderr);
  if (!ok) {
    return std::nullopt;
  }
  return to_utf8(line);
}

void setup_console() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
}

#else

bool stdin_is_terminal() { return isatty(STDIN_FILENO) != 0; }
bool stdout_is_terminal() { return isatty(STDOUT_FILENO) != 0; }

namespace {

termios g_saved_termios{};

// Ctrl-C while echo is off: restore the terminal before dying.
void restore_terminal_and_die(int sig) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
  std::ignore = write(STDERR_FILENO, "\n", 1);
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

}  // namespace

std::optional<std::string> prompt_hidden(const std::string& prompt) {
  std::fputs(prompt.c_str(), stderr);
  std::fflush(stderr);

  const bool is_tty = tcgetattr(STDIN_FILENO, &g_saved_termios) == 0;
  struct sigaction old_int{};
  struct sigaction old_term{};
  if (is_tty) {
    struct sigaction restore{};
    restore.sa_handler = restore_terminal_and_die;
    sigemptyset(&restore.sa_mask);
    sigaction(SIGINT, &restore, &old_int);
    sigaction(SIGTERM, &restore, &old_term);
    termios new_attr = g_saved_termios;
    new_attr.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    new_attr.c_lflag |= ECHONL;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_attr);
  }
  std::string line;
  const bool ok = static_cast<bool>(std::getline(std::cin, line));
  if (is_tty) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
  }
  if (!ok) {
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

void setup_console() {}

#endif

}  // namespace streamextract
