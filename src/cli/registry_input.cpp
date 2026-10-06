#include "registry_input.h"
#include "gti/driver/registry.h"

#include <array>
#include <cerrno>
#include <iostream>
#if !defined(_WIN32)
#include <csignal>
#include <termios.h>
#include <unistd.h>
#endif

namespace lang::cli {
namespace {
[[noreturn]] void inputError() {
  throw driver::RegistryError("GTI-B1725",
                              "Cannot read a registry token securely; "
                              "use a terminal or explicit --token-stdin.");
}
#if !defined(_WIN32)
volatile std::sig_atomic_t interrupted = 0;
void interruptInput(int) { interrupted = 1; }
class HiddenInput {
public:
  HiddenInput() {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) != 0) {
      inputError();
    }
    auto hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    interrupted = 0;
    struct sigaction action{};
    action.sa_handler = interruptInput;
    sigemptyset(&action.sa_mask);
    for (std::size_t index = 0; index < signals.size(); ++index) {
      if (sigaction(signals[index], &action, &previous[index]) != 0) {
        restoreSignals();
        inputError();
      }
      ++installed;
    }
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
      restoreSignals();
      inputError();
    }
  }
  ~HiddenInput() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    restoreSignals();
    std::cerr << '\n';
  }

private:
  termios original{};
  // Suspension also cancels entry instead of leaving terminal echo disabled.
  static constexpr std::array signals{SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                      SIGTSTP};
  std::array<struct sigaction, signals.size()> previous{};
  std::size_t installed = 0;
  void restoreSignals() {
    for (std::size_t index = 0; index < installed; ++index) {
      sigaction(signals[index], &previous[index], nullptr);
    }
  }
};
#endif
} // namespace
std::string readRegistryToken(bool tokenStdin) {
  std::string token;
  if (tokenStdin) {
    char character;
    while (std::cin.get(character)) {
      if (token.size() >= 4098) {
        inputError();
      }
      token += character;
    }
    if (!std::cin.eof()) {
      inputError();
    }
    if (token.ends_with('\n')) {
      token.pop_back();
      if (token.ends_with('\r')) {
        token.pop_back();
      }
    }
  } else {
#if defined(_WIN32)
    inputError();
#else
    HiddenInput hidden;
    std::cerr << "Access token (input hidden): " << std::flush;
    while (!interrupted) {
      char character;
      const auto count = read(STDIN_FILENO, &character, 1);
      if (count < 0 && errno == EINTR && !interrupted) {
        continue;
      }
      if (count != 1) {
        inputError();
      }
      if (character == '\n') {
        break;
      }
      if (token.size() >= 4096) {
        inputError();
      }
      token += character;
    }
    if (interrupted) {
      inputError();
    }
#endif
  }
  driver::validateRegistryCredential({.secret = token});
  return token;
}
} // namespace lang::cli
