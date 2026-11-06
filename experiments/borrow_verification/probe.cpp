// Standalone laboratory probe. Intentionally not a CMake/CTest target.
#include "compiler_test_fixture.h"
#include "gti/mir_editor.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

namespace {
llvm::json::Array errors(const lang::MirVerificationResult &result) {
  llvm::json::Array out;
  for (const auto &error : result.errors)
    out.push_back(error.message);
  return out;
}

llvm::json::Array mutate(const lang::FrontendResult &frontend,
                         const std::filesystem::path &dumpDirectory = {}) {
  llvm::json::Array reports;
  // Each edit is isolated, and the unmodified artifact must verify first.
  // Keep all verifier messages: rejection by shape alone is not proof that an
  // ownership invariant was checked. The runner distinguishes these outcomes.
  for (const std::string kind :
       {"temporary_escape", "failure_end_order", "missing_endpoint",
        "wrong_origin", "permission_upgrade", "missing_initialization"}) {
    bool changed = false;
    auto program = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    for (auto &function : functions) {
      const auto *info = frontend.semantics.findFunction(function.declaration);
      if (!info || info->sourceUnit != frontend.sourceGraph.entryUnit())
        continue;
      auto &body = function.body;
      // Only source functions with a small local loan profile, never stdlib
      // bodies incidentally included by imports.
      if (body.loans.empty() || body.loans.size() > 16)
        continue;
      for (auto &block : body.blocks) {
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
          auto &ins = block.instructions[i];
          if (kind == "temporary_escape" && ins.loan &&
              ins.kind == lang::MirInstructionKind::CallInput &&
              ins.callInputKind ==
                  lang::HirCallInputKind::ReadTemporaryBorrow) {
            body.loans[*ins.loan - 1].escapes = true;
            changed = true;
          } else if (kind == "failure_end_order" &&
                     block.terminator.kind == lang::MirTerminatorKind::Invoke) {
            auto &next =
                body.blocks[block.terminator.elseTarget - 1].instructions;
            if (next.size() >= 2 &&
                next[0].kind == lang::MirInstructionKind::EndBorrow &&
                next[1].kind == lang::MirInstructionKind::EndBorrow) {
              std::swap(next[0], next[1]);
              changed = true;
            }
          } else if (kind == "missing_endpoint" &&
                     ins.kind == lang::MirInstructionKind::EndBorrow) {
            block.instructions.erase(block.instructions.begin() + i);
            changed = true;
          } else if (kind == "wrong_origin" && ins.loan &&
                     ins.kind == lang::MirInstructionKind::Borrow &&
                     body.places.size() > 1) {
            auto &loan = body.loans[*ins.loan - 1];
            loan.source = loan.source == 1 ? 2 : 1;
            changed = true;
          } else if (kind == "permission_upgrade" && ins.loan &&
                     ins.kind == lang::MirInstructionKind::Borrow &&
                     body.loans[*ins.loan - 1].access ==
                         lang::AccessMode::ReadOnly) {
            body.loans[*ins.loan - 1].access = lang::AccessMode::Mutable;
            changed = true;
          } else if (kind == "missing_initialization" &&
                     ins.kind == lang::MirInstructionKind::Initialize) {
            block.instructions.erase(block.instructions.begin() + i);
            changed = true;
          }
          if (changed)
            break;
        }
        if (changed)
          break;
      }
      if (changed)
        break;
    }
    if (!changed) {
      reports.push_back(
          llvm::json::Object{{"mutation", kind}, {"status", "inapplicable"}});
      continue;
    }
    for (auto &function : functions)
      (void)lang::rebuildMirValueUses(function.body);
    const auto verification = lang::verifyMirProgram(program);
    if (!dumpDirectory.empty()) {
      std::ofstream dump(dumpDirectory / (kind + ".mir"));
      lang::MirPrinter().print(program, dump);
    }
    reports.push_back(llvm::json::Object{
        {"mutation", kind},
        {"status", verification.valid() ? "survived" : "rejected"},
        {"errors", errors(verification)}});
  }
  return reports;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc > 5) {
    std::cerr << "usage: borrow_probe <source.gti> <stdlib-dir> [--mutate "
                 "[--dump-mutants]]\n";
    return 2;
  }
  std::ifstream stream(argv[1]);
  if (!stream)
    return 2;
  const std::string source((std::istreambuf_iterator<char>(stream)), {});
  const std::filesystem::path root(argv[2]);
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  const auto semantic = lang::Frontend(options).analyze(
      argv[1], source, {root / "prelude.gti"}, {}, {root});
  const auto full = lang::Frontend().analyze(
      argv[1], source, {root / "prelude.gti"}, {}, {root});
  llvm::json::Array diagnostics;
  for (const auto &d : full.diagnostics)
    diagnostics.push_back(
        llvm::json::Object{{"code", d.code},
                           {"message", d.message},
                           {"line", d.primary.line},
                           {"start", static_cast<int64_t>(d.primary.start)},
                           {"end", static_cast<int64_t>(d.primary.end)},
                           {"source", d.primary.source},
                           {"severity", static_cast<int>(d.severity)}});
  llvm::json::Object report{{"schema", 1},
                            {"syntax_valid", full.syntaxValid},
                            {"semantic_valid", semantic.semanticValid},
                            {"full_semantic_valid", full.semanticValid},
                            {"accepted", full.canGenerateCode()},
                            {"diagnostics", std::move(diagnostics)}};
  if (argc >= 4 && std::string(argv[3]) == "--mutate" &&
      full.canGenerateCode()) {
    const auto valid = lang::verifyMirProgram(full.mir);
    report["baseline_verified"] = valid.valid();
    if (valid.valid())
      report["mutations"] =
          mutate(full, argc == 5 && std::string(argv[4]) == "--dump-mutants"
                           ? std::filesystem::path(argv[1]).parent_path()
                           : std::filesystem::path{});
  }
  llvm::outs() << llvm::json::Value(std::move(report)) << '\n';
}
