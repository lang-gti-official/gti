#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lang {

// Observational work telemetry, not compiler policy. Each producer reports
// only its own work; consumers must not infer semantic or artifact validity
// from a phase ending. Event views live only for the synchronous callback.
enum class ProgressPhase {
  Loading,
  Parsing,
  Checking,
  HirLowering,
  MirLowering,
  Optimizing,
  Emitting,
  Cache,
  NativeSource,
  NativeLink,
  Symbols,
  Publishing,
  NativeDependency,
  Targets,
};

enum class ProgressKind { Begin, Update, End };

struct ProgressEvent {
  ProgressPhase phase;
  ProgressKind kind;
  std::string_view detail;
  std::optional<std::size_t> completed;
  std::optional<std::size_t> total;
  std::chrono::nanoseconds elapsed{};
};

class ProgressObserver {
public:
  virtual ~ProgressObserver() = default;
  // Must not throw or mutate the compilation being observed. An observer is
  // caller-owned for the complete invocation and is not retained by results,
  // caches or worker threads. Independent invocations use independent sinks.
  virtual void onProgress(const ProgressEvent &event) noexcept = 0;
};

class ProgressScope final {
public:
  ProgressScope(ProgressObserver *observer, ProgressPhase phase,
                std::string_view detail = {},
                std::optional<std::size_t> total = std::nullopt)
      : observer_(observer), phase_(phase),
        detail_(observer == nullptr ? std::string_view{} : detail),
        total_(total) {
    if (observer_ != nullptr) {
      start_ = std::chrono::steady_clock::now();
      observer_->onProgress({phase_, ProgressKind::Begin, detail_, {}, total_});
    }
  }
  ProgressScope(const ProgressScope &) = delete;
  ProgressScope &operator=(const ProgressScope &) = delete;
  ~ProgressScope() { finish(); }

  void update(std::string_view detail,
              std::optional<std::size_t> completed = std::nullopt,
              std::optional<std::size_t> total = std::nullopt) const {
    if (observer_ != nullptr) {
      observer_->onProgress({phase_, ProgressKind::Update, detail, completed,
                             total ? total : total_, elapsed()});
    }
  }

  void finish() {
    if (observer_ != nullptr) {
      observer_->onProgress(
          {phase_, ProgressKind::End, detail_, {}, total_, elapsed()});
      observer_ = nullptr;
    }
  }

private:
  [[nodiscard]] std::chrono::nanoseconds elapsed() const {
    return std::chrono::steady_clock::now() - start_;
  }
  ProgressObserver *observer_;
  ProgressPhase phase_;
  std::string detail_;
  std::optional<std::size_t> total_;
  std::chrono::steady_clock::time_point start_{};
};

} // namespace lang
