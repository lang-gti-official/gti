#include "build_progress.h"
#include "terminal_style.h"

#include <iostream>
#include <string_view>

// Hold a real renderer in a hidden phase until the PTY test observes a live
// heartbeat. This avoids relying on a particular program's compilation speed.
int main(int argc, char **argv) {
  if (argc != 3) {
    return 64;
  }
  const std::string_view operation = argv[1];
  const std::string_view phase = argv[2];
  if ((operation != "check" && operation != "build") ||
      (phase != "hir" && phase != "mir" && phase != "native")) {
    return 64;
  }
  lang::cli::TerminalStyle style;
  if (!style.setMode("never")) {
    return 1;
  }
  lang::cli::BuildProgress progress(style, lang::cli::ProgressMode::Auto,
                                    "label-probe", "test", 0, false, true,
                                    operation == "check"
                                        ? lang::cli::ProgressOperation::Check
                                        : lang::cli::ProgressOperation::Build);
  lang::ProgressScope lowering(progress.observer(),
                               phase == "hir" ? lang::ProgressPhase::HirLowering
                               : phase == "mir"
                                   ? lang::ProgressPhase::MirLowering
                                   : lang::ProgressPhase::NativeDependency);
  if (std::cin.get() == std::char_traits<char>::eof()) {
    return 2;
  }
  if (phase == "native") {
    progress.writeMessage("native script log");
    progress.writeMessage("Reused native dependency alpha@0.1.0\n");
    progress.writeMessage("Built native dependency beta@0.1.0\n");
    if (std::cin.get() == std::char_traits<char>::eof()) {
      return 2;
    }
  }
  lowering.finish();
  progress.finish(true);
  return 0;
}
