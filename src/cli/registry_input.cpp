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
  throw driver::RegistryError(
      "GTI-B1725", "Cannot read registry sign-in input securely; "
                   "use a terminal, or --token-stdin for an existing token.");
}
#if !defined(_WIN32)
volatile std::sig_atomic_t interrupted = 0;
void interruptInput(int) { interrupted = 1; }
class HiddenInput {
public:
  explicit HiddenInput(bool secret) : secret(secret) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) != 0) {
      inputError();
    }
    auto hidden = original;
    if (secret) {
      hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    }
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
    if (secret) {
      std::cerr << '\n';
    }
  }

private:
  termios original{};
  bool secret;
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
std::string terminalField(std::string_view prompt, bool secret,
                          bool allowEmpty = false, std::size_t limit = 4096) {
#if defined(_WIN32)
  (void)prompt;
  (void)secret;
  (void)allowEmpty;
  (void)limit;
  inputError();
#else
  HiddenInput input(secret);
  std::cerr << prompt << std::flush;
  std::string result;
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
    if (result.size() >= limit) {
      inputError();
    }
    result += character;
  }
  if (interrupted || (result.empty() && !allowEmpty)) {
    inputError();
  }
  return result;
#endif
}
} // namespace
std::string readRegistryUsername() {
  return terminalField("Username: ", false, false, 256);
}
std::string readRegistrySecret(std::string_view prompt, bool allowEmpty) {
  return terminalField(prompt, true, allowEmpty);
}
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
    token = readRegistrySecret("Access token (input hidden): ");
  }
  driver::validateRegistryCredential({.secret = token});
  return token;
}
} // namespace lang::cli
