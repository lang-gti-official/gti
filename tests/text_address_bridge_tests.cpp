#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view label) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << label << '\n';
  }
}
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "text-address-bridge-test.gti";
  return lang::Frontend().analyze(
      entry, source, {root / "stdlib/prelude.gti"},
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
}
template <typename Mutation>
void forged(const lang::FrontendResult &frontend, lang::IntrinsicKind kind,
            Mutation mutation, std::string_view label) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed && instruction.intrinsic == kind) {
          mutation(function.body, instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), label);
  bool rejected = false;
  try {
    (void)gti_test::emitCpp(frontend, frontend.mir, mir);
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(changed && rejected, "backend refuses forged raw text assertion");
}
} // namespace

int main() {
  const auto frontend = analyze(R"(
#include <std/memory>
class ByteOwner {
  char bytes[2] = {'x', '\0'};
public:
  ByteOwner() {}
  const char* address() { return &this.bytes[0]; }
  std::string_view counted() {
    unsafe { return std::assume_string_view(this.address(), uint64_t(1), this); }
  }
  c_string terminated() {
    unsafe { return std::assume_c_string(this.address(), this); }
  }
};
int main() {
  ByteOwner owner{};
  auto counted = owner.counted();
  c_string terminated = owner.terminated();
  return counted.size() == uint64_t(1) ? 0 : 1;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &function : frontend.mir.functionInstances()) {
      if (std::any_of(
              function.body.blocks.begin(), function.body.blocks.end(),
              [](const auto &block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [](const auto &instruction) {
                      return instruction.intrinsic ==
                                 lang::IntrinsicKind::TextViewFromAddress ||
                             instruction.intrinsic ==
                                 lang::IntrinsicKind::CStringFromAddress;
                    });
              }))
        std::cerr << lang::MirPrinter().print(function.body);
    }
  }
  expect(frontend.canGenerateCode(),
         "independent byte owner produces verified text carriers");
  if (!frontend.canGenerateCode())
    return 1;

  for (const auto kind : {lang::IntrinsicKind::TextViewFromAddress,
                          lang::IntrinsicKind::CStringFromAddress}) {
    forged(
        frontend, kind,
        [](auto &, auto &instruction) { instruction.loan.reset(); },
        "raw text assertion cannot discard the checked loan");
    forged(
        frontend, kind,
        [](auto &, auto &instruction) { instruction.borrowArgument = 0; },
        "address is not a substitute for the nominated checked owner");
    forged(
        frontend, kind,
        [](auto &, auto &instruction) {
          instruction.unsafeOperation = lang::UnsafeOperationKind::None;
        },
        "raw text assertion preserves unsafe authority");
    forged(
        frontend, kind,
        [](auto &body, auto &instruction) {
          for (auto &block : body.blocks)
            for (auto &stage : block.instructions)
              if (stage.result == instruction.operands.front().value &&
                  stage.kind == lang::MirInstructionKind::CallInput)
                stage.callInputIndex = 99;
        },
        "raw text assertion requires exact ordered input stages");
    forged(
        frontend, kind,
        [](auto &body, auto &instruction) {
          for (auto &loan : body.loans)
            if (loan.id == instruction.loan)
              loan.access = lang::AccessMode::Mutable;
        },
        "raw text carrier cannot forge exclusive witness access");
    forged(
        frontend, kind,
        [](auto &body, auto &instruction) {
          for (auto &loan : body.loans)
            if (loan.id == instruction.loan)
              loan.source = 1;
        },
        "raw text carrier preserves the nominated owner source identity");
    forged(
        frontend, kind,
        [](auto &, auto &instruction) {
          instruction.parameterTypes[0] = lang::SemanticType::rawPointerTo(
              lang::SemanticType::UInt8, lang::AccessMode::ReadOnly);
        },
        "raw text carrier does not reinterpret an unverified pointer type");
    forged(
        frontend, kind,
        [](auto &, auto &instruction) { instruction.rawMemoryAccess = true; },
        "raw text construction cannot forge an additional memory operation");
    forged(
        frontend, kind,
        [](auto &, auto &instruction) {
          instruction.info.traits.containsBorrowedState = false;
        },
        "raw text result retains mandatory owner-dependent traits");
  }
  forged(
      frontend, lang::IntrinsicKind::TextViewFromAddress,
      [](auto &, auto &instruction) {
        instruction.parameterTypes[1] = lang::SemanticType::Int32;
      },
      "counted text requires the exact unsigned count type");
  return failures == 0 ? 0 : 1;
}
