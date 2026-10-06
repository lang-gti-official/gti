#include "build_progress.h"

#include <llvm/Support/ConvertUTF.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace lang::cli {
namespace {

std::string safeText(std::string_view text) {
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < text.size();) {
    const auto *begin =
        reinterpret_cast<const llvm::UTF8 *>(text.data() + index);
    const auto *end =
        reinterpret_cast<const llvm::UTF8 *>(text.data() + text.size());
    const unsigned length = llvm::getUTF8SequenceSize(begin, end);
    const unsigned char byte = text[index];
    const bool c1 =
        length == 2 && byte == 0xc2 && begin[1] >= 0x80 && begin[1] <= 0x9f;
    if (byte < 32 || byte == 127 || length == 0 || c1) {
      result += "\\x";
      result += hex[byte >> 4];
      result += hex[byte & 15];
      ++index;
      if (c1) {
        result += "\\x";
        result += hex[begin[1] >> 4];
        result += hex[begin[1] & 15];
        ++index;
      }
    } else {
      result += text.substr(index, length);
      index += length;
    }
  }
  return result;
}

std::string duration(std::chrono::nanoseconds elapsed) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(2)
       << std::chrono::duration<double>(elapsed).count() << 's';
  return text.str();
}

} // namespace

BuildProgress::BuildProgress(const TerminalStyle &style, ProgressMode mode,
                             std::string name, std::string configuration,
                             int optimization, bool verbose, bool enabled)
    : style_(style), name_(safeText(name)), optimization_(optimization),
      verbose_(verbose), enabled_(enabled && mode != ProgressMode::Never),
      interactive_(mode == ProgressMode::Auto && style.interactive(std::cerr) &&
                   style.columns(std::cerr) >= 32),
      start_(std::chrono::steady_clock::now()) {
  if (enabled_) {
    row("Building", name_ + " (" + safeText(configuration) + ")", Style::Note,
        false);
    try {
      worker_ = std::thread([this] { heartbeat(); });
    } catch (const std::system_error &) {
      // Exhausted thread resources must not turn presentation into a build
      // failure. Durable synchronous phase reporting remains available.
      interactive_ = false;
    }
  }
}

BuildProgress::~BuildProgress() {
  if (!finished_) {
    finish(false);
  }
}

ProgressObserver *BuildProgress::observer() {
  return enabled_ ? this : nullptr;
}

std::string_view BuildProgress::phaseVerb() const {
  switch (phase_) {
  case ProgressPhase::Loading:
    return "Loading";
  case ProgressPhase::Parsing:
    return "Parsing";
  case ProgressPhase::Checking:
    return "Checking";
  case ProgressPhase::HirLowering:
  case ProgressPhase::MirLowering:
    return "Lowering";
  case ProgressPhase::Optimizing:
    return "Optimizing";
  case ProgressPhase::Emitting:
    return "Emitting";
  case ProgressPhase::Cache:
    return "Cache";
  case ProgressPhase::NativeSource:
  case ProgressPhase::NativeLink:
    return "Native";
  case ProgressPhase::Symbols:
    return "Symbols";
  case ProgressPhase::Publishing:
    return "Publishing";
  case ProgressPhase::NativeDependency:
    return "Native";
  case ProgressPhase::Targets:
    return "Building";
  }
  return "Working";
}

std::string BuildProgress::phaseDetail(bool perUnit) const {
  switch (phase_) {
  case ProgressPhase::Loading:
    return total_ ? std::to_string(*total_) + " source units" : "source graph";
  case ProgressPhase::Parsing:
    if (perUnit && !detail_.empty()) {
      return detail_ + (completed_ && total_
                            ? " [" + std::to_string(*completed_) + "/" +
                                  std::to_string(*total_) + "]"
                            : "");
    }
    return total_ ? std::to_string(*total_) + " source units" : "source units";
  case ProgressPhase::Checking:
    return name_;
  case ProgressPhase::HirLowering:
    return "HIR";
  case ProgressPhase::MirLowering:
    return "MIR";
  case ProgressPhase::Optimizing:
    return "program (-O" + std::to_string(optimization_) + ")";
  case ProgressPhase::Emitting:
    return detail_ == "mir"             ? "MIR"
           : detail_ == "native-header" ? "native interface"
                                        : "backend representation";
  case ProgressPhase::Cache:
    return detail_;
  case ProgressPhase::NativeSource:
    return "compiling " + detail_;
  case ProgressPhase::NativeLink:
    return "compiling and linking";
  case ProgressPhase::Symbols:
    return "collecting debug information";
  case ProgressPhase::Publishing:
    return detail_;
  case ProgressPhase::NativeDependency:
    return "preparing dependencies";
  case ProgressPhase::Targets:
    return detail_;
  }
  return {};
}

void BuildProgress::clearRow() {
  if (transient_) {
    std::cerr << "\r\033[2K";
    transient_ = false;
  }
}

void BuildProgress::row(std::string_view verb, std::string_view detail,
                        Style style, bool transient,
                        std::chrono::nanoseconds elapsed) {
  clearRow();
  const std::string suffix =
      elapsed == std::chrono::nanoseconds{}
          ? std::string{}
          : " (" + duration(elapsed) + (transient ? " elapsed)" : ")");
  std::string clipped;
  if (transient) {
    // Conservative byte budgeting also fits UTF-8 wide glyphs. Never split a
    // code point or fill the last column and accidentally wrap a live row.
    const unsigned width = style_.columns(std::cerr);
    const std::size_t fixed = 12 + suffix.size();
    const std::size_t budget = width > fixed + 1 ? width - fixed - 1 : 0;
    if (detail.size() > budget) {
      std::size_t end = budget > 3 ? budget - 3 : 0;
      while (end > 0 &&
             (static_cast<unsigned char>(detail[end]) & 0xc0) == 0x80)
        --end;
      clipped = std::string(detail.substr(0, end)) + (budget >= 3 ? "..." : "");
      detail = clipped;
    }
  }
  // Padding belongs outside ANSI escapes so colour never changes alignment.
  std::cerr << std::string(verb.size() < 10 ? 10 - verb.size() : 0, ' ')
            << style_.code(std::cerr, style) << verb
            << style_.code(std::cerr, Style::Reset) << "  " << detail;
  std::cerr << suffix;
  if (!transient) {
    std::cerr << '\n';
  }
  transient_ = transient;
  std::cerr.flush();
}

void BuildProgress::onProgress(const ProgressEvent &event) noexcept {
  std::lock_guard lock(mutex_);
  if (stopped_ || !enabled_) {
    return;
  }
  if (event.kind == ProgressKind::Begin) {
    phase_ = event.phase;
    detail_ = safeText(event.detail);
    completed_ = event.completed;
    total_ = event.total;
    phaseStart_ = lastHeartbeat_ = std::chrono::steady_clock::now();
    active_ = true;
    row(phaseVerb(), phaseDetail(false), Style::Note, interactive_);
  } else if (event.kind == ProgressKind::Update) {
    const std::string previous = detail_;
    detail_ = safeText(event.detail);
    if (event.completed)
      completed_ = event.completed;
    if (event.total)
      total_ = event.total;
    if (interactive_ || (verbose_ && !detail_.empty() && detail_ != previous)) {
      row(phaseVerb(), phaseDetail(verbose_), Style::Note, interactive_);
    }
  } else {
    active_ = false;
    if (interactive_ || event.elapsed >= std::chrono::seconds(1) ||
        phase_ == ProgressPhase::Loading || phase_ == ProgressPhase::Cache) {
      row(phaseVerb(), phaseDetail(false), Style::Note, false,
          event.elapsed >= std::chrono::milliseconds(interactive_ ? 100 : 1000)
              ? event.elapsed
              : std::chrono::nanoseconds{});
    }
  }
}

void BuildProgress::heartbeat() {
  std::unique_lock lock(mutex_);
  while (!stopped_) {
    wake_.wait_for(lock, std::chrono::milliseconds(200),
                   [this] { return stopped_; });
    if (stopped_ || !active_)
      continue;
    const auto now = std::chrono::steady_clock::now();
    if (interactive_ || now - lastHeartbeat_ >= std::chrono::seconds(10)) {
      row(phaseVerb(),
          phaseDetail(verbose_ || now - phaseStart_ >= std::chrono::seconds(1)),
          Style::Note, interactive_, now - phaseStart_);
      lastHeartbeat_ = now;
    }
  }
}

void BuildProgress::suspend() {
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    clearRow();
  }
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void BuildProgress::finish(bool success) {
  suspend();
  if (enabled_ && !finished_) {
    row(success ? "Finished" : "Failed", name_,
        success ? Style::Success : Style::Error, false,
        std::chrono::steady_clock::now() - start_);
  }
  finished_ = true;
}

} // namespace lang::cli
