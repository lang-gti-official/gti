#pragma once

#include "gti/progress.h"
#include "terminal_style.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace lang::cli {

enum class ProgressMode { Auto, Plain, Never };
enum class ProgressOperation { Build, Check };

// One invocation's stderr renderer. Compiler/driver observers remain entirely
// synchronous; only this presentation layer owns an elapsed-time heartbeat.
class BuildProgress final : public ProgressObserver {
public:
  BuildProgress(const TerminalStyle &style, ProgressMode mode, std::string name,
                std::string configuration, int optimization, bool verbose,
                bool enabled = true,
                ProgressOperation operation = ProgressOperation::Build);
  ~BuildProgress();
  void onProgress(const ProgressEvent &event) noexcept override;
  [[nodiscard]] ProgressObserver *observer();
  // Join the heartbeat and clear any transient row before diagnostics or
  // program streams are rendered. finish() reports the actual final outcome.
  void suspend();
  void finish(bool success);

private:
  void heartbeat();
  void row(std::string_view verb, std::string_view detail, Style style,
           bool transient, std::chrono::nanoseconds elapsed = {});
  [[nodiscard]] std::string phaseDetail(bool perUnit) const;
  [[nodiscard]] std::string_view phaseVerb() const;
  [[nodiscard]] std::string_view operationVerb() const;
  [[nodiscard]] bool phaseShown() const;
  void clearRow();

  const TerminalStyle &style_;
  std::string name_;
  const ProgressOperation operation_;
  int optimization_;
  bool verbose_;
  bool enabled_;
  bool interactive_;
  bool finished_ = false;
  bool stopped_ = false;
  bool active_ = false;
  bool transient_ = false;
  ProgressPhase phase_ = ProgressPhase::Loading;
  std::string detail_;
  std::optional<std::size_t> completed_;
  std::optional<std::size_t> total_;
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point phaseStart_;
  std::chrono::steady_clock::time_point lastHeartbeat_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
};

} // namespace lang::cli
