#include "terminal_style.h"

#include <cstdlib>
#include <iostream>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lang::cli {
namespace {

bool terminalSupportsColor(bool standardError) {
#if defined(_WIN32)
  // Restore console modes on normal exit; redirected handles are not consoles.
  struct Console {
    HANDLE handle;
    DWORD original = 0;
    bool available;
    explicit Console(DWORD id)
        : handle(GetStdHandle(id)),
          available(
              GetConsoleMode(handle, &original) &&
              SetConsoleMode(handle,
                             original | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {}
    ~Console() {
      if (available) {
        SetConsoleMode(handle, original);
      }
    }
  };
  static Console output(STD_OUTPUT_HANDLE);
  static Console error(STD_ERROR_HANDLE);
  return standardError ? error.available : output.available;
#else
  return isatty(standardError ? STDERR_FILENO : STDOUT_FILENO) != 0;
#endif
}

} // namespace

bool TerminalStyle::setMode(std::string_view value) {
  if (value == "auto") {
    mode_ = ColorMode::Auto;
  } else if (value == "always") {
    mode_ = ColorMode::Always;
  } else if (value == "never") {
    mode_ = ColorMode::Never;
  } else {
    return false;
  }
  return true;
}

bool TerminalStyle::enabled(const std::ostream &stream) const {
  if (&stream != &std::cout && &stream != &std::cerr) {
    return false;
  }
  if (mode_ == ColorMode::Never) {
    return false;
  }
  if (mode_ == ColorMode::Auto) {
    const char *noColor = std::getenv("NO_COLOR");
    const char *term = std::getenv("TERM");
    if ((noColor != nullptr && *noColor != '\0') ||
        (term != nullptr && std::string_view(term) == "dumb")) {
      return false;
    }
  }
  const bool terminal = terminalSupportsColor(&stream == &std::cerr);
  return mode_ == ColorMode::Always || terminal;
}

std::string_view TerminalStyle::code(const std::ostream &stream,
                                     Style style) const {
  if (!enabled(stream)) {
    return {};
  }
  switch (style) {
  case Style::Reset:
    return "\033[0m";
  case Style::Error:
    return "\033[1;31m";
  case Style::Warning:
    return "\033[1;33m";
  case Style::Note:
    return "\033[1;36m";
  case Style::Success:
    return "\033[1;32m";
  }
  return {};
}

std::string withoutStyles(std::string_view text) {
  std::string result;
  for (std::size_t index = 0; index < text.size();) {
    if (text.substr(index).starts_with("\033[")) {
      std::size_t end = index + 2;
      while (end < text.size() &&
             ((text[end] >= '0' && text[end] <= '9') || text[end] == ';')) {
        ++end;
      }
      if (end < text.size() && text[end] == 'm') {
        index = end + 1;
        continue;
      }
    }
    result += text[index++];
  }
  return result;
}

} // namespace lang::cli
