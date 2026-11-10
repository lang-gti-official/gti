// Shared helpers for the driver test executables: the backend-neutral
// driver_pipeline suite (driver_tests.cpp) and the frozen C++-backend driver
// suite (cpp_mir_driver_tests.cpp).
#pragma once

#include "gti/driver/compilation.h"
#include "gti/driver/process.h"
#include "gti/lowered_program.h"
#include "gti/support.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gti_test::driver_support {

inline int failures = 0;

inline void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class RecordingProgress final : public lang::ProgressObserver {
public:
  std::vector<lang::ProgressPhase> phases;
  std::vector<lang::ProgressPhase> active;
  bool balanced = true;
  std::optional<std::size_t> parsed;
  std::optional<std::size_t> total;
  std::optional<std::size_t> loaded;
  void onProgress(const lang::ProgressEvent &event) noexcept override {
    if (event.kind == lang::ProgressKind::Begin) {
      phases.push_back(event.phase);
      active.push_back(event.phase);
    } else if (event.kind == lang::ProgressKind::End) {
      balanced = balanced && !active.empty() && active.back() == event.phase &&
                 event.elapsed >= std::chrono::nanoseconds{};
      if (!active.empty())
        active.pop_back();
    } else if (event.phase == lang::ProgressPhase::Parsing) {
      parsed = event.completed;
      total = event.total;
    } else if (event.phase == lang::ProgressPhase::Loading) {
      loaded = event.total;
    }
  }
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("gti-driver-test-" + std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  [[nodiscard]] const std::filesystem::path &root() const { return path; }

private:
  std::filesystem::path path;
};

class ScopedEnvironment final {
public:
  explicit ScopedEnvironment(std::string name) : variable(std::move(name)) {
    rememberCurrentValue();
    clear();
  }

  ScopedEnvironment(std::string name, std::string value)
      : variable(std::move(name)) {
    rememberCurrentValue();
    set(value);
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

  ~ScopedEnvironment() {
    if (previous) {
      set(*previous);
      return;
    }
    clear();
  }

private:
  void rememberCurrentValue() {
    if (const char *current = std::getenv(variable.c_str())) {
      previous = current;
    }
  }

  void clear() {
#if defined(_WIN32)
    (void)_putenv_s(variable.c_str(), "");
#else
    (void)unsetenv(variable.c_str());
#endif
  }

  void set(const std::string &value) {
#if defined(_WIN32)
    (void)_putenv_s(variable.c_str(), value.c_str());
#else
    (void)setenv(variable.c_str(), value.c_str(), 1);
#endif
  }

  std::string variable;
  std::optional<std::string> previous;
};

class ThrowingBackend final : public lang::Backend {
public:
  explicit ThrowingBackend(bool throwStandardException,
                           bool throwFromName = false)
      : standardException(throwStandardException), throwingName(throwFromName) {
  }

  [[nodiscard]] std::string_view name() const override {
    if (throwingName) {
      throw std::runtime_error("deliberate backend name failure");
    }
    return "throwing-test";
  }

  [[nodiscard]] lang::BackendArtifact generate(lang::LoweredProgram) override {
    invoked = true;
    if (standardException) {
      throw std::runtime_error("deliberate backend failure");
    }
    throw 17;
  }

  [[nodiscard]] bool wasInvoked() const { return invoked; }

private:
  bool standardException = true;
  bool throwingName = false;
  bool invoked = false;
};

class UnsupportedBackend final : public lang::Backend {
public:
  explicit UnsupportedBackend(std::optional<lang::SourceSpan> source = {})
      : source(std::move(source)) {}
  [[nodiscard]] std::string_view name() const override {
    return "unsupported-test";
  }

  [[nodiscard]] lang::BackendArtifact generate(lang::LoweredProgram) override {
    invoked = true;
    if (source)
      throw lang::BackendUnsupportedError("missing bounded test capability",
                                          *source);
    throw lang::BackendUnsupportedError("missing bounded test capability");
  }

  [[nodiscard]] bool wasInvoked() const { return invoked; }

private:
  std::optional<lang::SourceSpan> source;
  bool invoked = false;
};

inline bool writeFile(const std::filesystem::path &path,
                      std::string_view contents) {
  std::ofstream output(path);
  output << contents;
  return static_cast<bool>(output);
}

inline std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

inline std::vector<std::string> activeSanitizerArguments() {
  bool addressSanitizer = false;
  bool undefinedBehaviorSanitizer = false;
#if defined(__SANITIZE_ADDRESS__)
  addressSanitizer = true;
#endif
#if defined(__SANITIZE_UNDEFINED__)
  undefinedBehaviorSanitizer = true;
#endif
#if defined(__clang__)
#if __has_feature(address_sanitizer)
  addressSanitizer = true;
#endif
#if __has_feature(undefined_behavior_sanitizer)
  undefinedBehaviorSanitizer = true;
#endif
#endif
  std::vector<std::string> arguments;
  if (addressSanitizer) {
    arguments.emplace_back("-fsanitize=address");
  }
  if (undefinedBehaviorSanitizer) {
    arguments.emplace_back("-fsanitize=undefined");
  }
  return arguments;
}

// Runs the sanitizer-aware native compiler wrapper when this test executable
// was invoked through one of its wrapper copies; otherwise returns nullopt.
inline std::optional<int> runNativeCompilerWrapper(int argc, char *argv[]) {
  const std::filesystem::path invocation = argc > 0 ? argv[0] : "";
  if (argc > 0 && (invocation.stem() == "native-cxx-wrapper" ||
                   invocation.stem() == "native-framework-cxx-wrapper")) {
    std::vector<std::string> command{"c++"};
    const std::vector<std::string> sanitizerArguments =
        activeSanitizerArguments();
    command.insert(command.end(), sanitizerArguments.begin(),
                   sanitizerArguments.end());
    if (invocation.stem() == "native-framework-cxx-wrapper") {
      command.emplace_back("-F" +
                           (invocation.parent_path() / "Frameworks").string());
    }
    for (int index = 1; index < argc; ++index) {
      command.emplace_back(argv[index]);
    }
    const lang::driver::ProcessResult result = lang::driver::invokeProcess(
        command, {.outputMode = lang::driver::ProcessOutputMode::Inherit,
                  .description = "sanitizer-aware native compiler wrapper"});
    if (result.driverDiagnostic) {
      std::cerr << *result.driverDiagnostic << '\n';
    }
    return result.exitCode;
  }
  return std::nullopt;
}

} // namespace gti_test::driver_support
