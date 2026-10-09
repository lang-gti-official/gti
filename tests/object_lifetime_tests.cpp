#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
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
lang::FrontendResult analyze(const std::string &source, bool trusted = false) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "object-lifetime-test.gti";
  std::vector<std::filesystem::path> preludes{root / "stdlib/prelude.gti"};
  if (trusted)
    preludes.push_back(entry);
  return lang::Frontend().analyze(
      entry, source, preludes,
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
}
void diagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
bool motivatingPointee(const lang::FrontendResult &frontend,
                       const lang::MirInstruction &instruction,
                       std::string_view className) {
  if (instruction.parameterTypes.empty() ||
      instruction.parameterTypes.front().kind !=
          lang::SemanticType::RawPointer ||
      instruction.parameterTypes.front().arguments.size() != 1)
    return false;
  const auto &element = instruction.parameterTypes.front().arguments.front();
  if (element == lang::SemanticType::Int32)
    return true;
  const auto *owner = element.kind == lang::SemanticType::Class
                          ? frontend.semantics.findClassType(element.classId)
                          : nullptr;
  return owner != nullptr && owner->qualifiedName == className;
}
template <typename Mutation>
void forged(
    const lang::FrontendResult &frontend, Mutation mutation,
    std::string_view label,
    lang::IntrinsicKind intrinsic = lang::IntrinsicKind::ObjectDestroy) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed && instruction.intrinsic == intrinsic &&
            (intrinsic == lang::IntrinsicKind::ObjectConstruct
                 ? instruction.constructorTarget.has_value()
                 : instruction.elementCleanup.has_value())) {
          mutation(instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), label);
  bool rejected = false;
  try {
    (void)gti_test::emitCpp(frontend, frontend.mir, mir);
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(changed && rejected, "backend rejects forged lifetime operation");
}
} // namespace

int main() {
  // This is a static/MIR fixture, not an executable program: raw validity is
  // the unsafe caller's obligation, independently of pointer type checking.
  const auto frontend = analyze(R"(
#include <std/memory>
#include <std/panic>
class Sensor {
public:
  Sensor(Sensor& other) = delete;
  Sensor(Sensor&& other) = delete;
  ~Sensor() { std::panic("sensor cleanup failed"); }
};
int main() {
  int* scalar = nullptr;
  Sensor* sensor = nullptr;
  unsafe {
    std::destroy_at(scalar);
    std::destroy_at(sensor);
  }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    diagnostics(frontend);
    std::cerr << lang::MirPrinter().print(frontend.mir);
  }
  expect(frontend.canGenerateCode(), "raw destruction forms verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  std::size_t count = 0, cleanups = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy &&
            motivatingPointee(frontend, instruction, "Sensor")) {
          ++count;
          const auto effects = lang::effects(instruction);
          expect(effects.writesUnknownMemory && effects.dropsValue &&
                     !effects.removableWhenUnused && !effects.allocates,
                 "raw destruction retains memory/cleanup effects without "
                 "allocation");
          expect(
              instruction.unsafeOperation ==
                      lang::UnsafeOperationKind::ObjectLifetime &&
                  instruction.localFailureSites.empty(),
              "raw validity has unsafe authority, not invented runtime checks");
          if (instruction.elementCleanup) {
            ++cleanups;
            expect(instruction.definedFailure.propagation ==
                       lang::FailurePropagationKind::Destructor,
                   "user cleanup retains original failure propagation");
          }
        }
  expect(count == 2 && cleanups == 1,
         "scalar and unrelated immovable cleanup paths");
  for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
    for (auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto cpp = gti_test::emitCppText(frontend, standard,
                                             lang::TargetInfo::host(), level);
      expect(cpp.find("= ::gti_internal::backend::mir_object_destroy_v1(") !=
                 std::string::npos,
             "verified cleanup survives optimization and uses failure-aware "
             "lowering");
    }
  forged(
      frontend,
      [](auto &i) { i.unsafeOperation = lang::UnsafeOperationKind::None; },
      "missing unsafe lifetime authority rejected");
  forged(
      frontend, [](auto &i) { i.elementCleanup.reset(); },
      "missing exact cleanup rejected");
  forged(
      frontend, [](auto &i) { i.definedFailure = {}; },
      "erased destructor failure rejected");
  forged(
      frontend,
      [](auto &i) {
        i.operands.front().type.pointerAccess = lang::AccessMode::ReadOnly;
        i.parameterTypes.front() = i.operands.front().type;
      },
      "readonly destruction rejected");
  forged(
      frontend, [](auto &i) { i.borrowArgument = 1; },
      "unrelated borrowed authority rejected");
  forged(
      frontend, [](auto &i) { i.objectAllocation = i.id; },
      "allocation authority rejected");
  forged(
      frontend, [](auto &i) { i.neverReturns = true; },
      "unrelated terminal authority rejected");
  forged(
      frontend, [](auto &i) { i.info.category = lang::ValueCategory::Place; },
      "destruction cannot publish a borrowed result");
  const std::string unsafeSource =
      "#include <std/memory>\nint main() { int* p = nullptr; "
      "std::destroy_at(p); return 0; }";
  const auto missingUnsafe = analyze(unsafeSource);
  const auto close =
      unsafeSource.find(")", unsafeSource.find("std::destroy_at"));
  expect(!missingUnsafe.canGenerateCode() &&
             std::any_of(missingUnsafe.diagnostics.begin(),
                         missingUnsafe.diagnostics.end(),
                         [&](const auto &d) {
                           return d.code == "GTI-S2055" &&
                                  d.phase == lang::DiagnosticPhase::Semantics &&
                                  d.primary.source.ends_with(
                                      "object-lifetime-test.gti") &&
                                  d.primary.start == close &&
                                  d.primary.end == close + 1 &&
                                  d.message.find("unsafe") !=
                                      std::string::npos &&
                                  d.fixes.empty();
                         }),
         "public destruction requires explicit unsafe permission, with no "
         "unsafe fix-it");
  for (const std::string pointer : {"const int*", "void*"}) {
    const auto invalid =
        analyze("#include <std/memory>\nint main() { " + pointer +
                    " p = nullptr; unsafe { gti_internal::object_destroy(p); } "
                    "return 0; }",
                true);
    expect(!invalid.canGenerateCode() &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &d) {
                             return d.code == "GTI-S2019" &&
                                    d.message.find("writable pointer") !=
                                        std::string::npos;
                           }),
           "private destruction rejects readonly and incomplete pointees");
  }
  const auto construction = analyze(R"(
#include <std/memory>
class Stationary {
  int value;
public:
  Stationary(int& divisor) : value(7 / divisor) {}
  Stationary(Stationary& other) = delete;
  Stationary(Stationary&& other) = delete;
};
int main() {
  Stationary* object = nullptr;
  int* scalar = nullptr;
  int divisor = 2;
  unsafe {
    std::construct_at(object, divisor);
    std::construct_at(scalar, 9);
    std::destroy_at(object);
    std::destroy_at(scalar);
  }
  return 0;
}
)");
  if (!construction.canGenerateCode())
    diagnostics(construction);
  expect(construction.canGenerateCode(),
         "raw construction admits immovable final-address values, borrowed "
         "constructor inputs, and scalars");
  if (construction.canGenerateCode()) {
    std::size_t constructions = 0;
    for (const auto &function : construction.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.intrinsic == lang::IntrinsicKind::ObjectConstruct &&
              motivatingPointee(construction, instruction, "Stationary")) {
            ++constructions;
            const auto effect = lang::effects(instruction);
            expect(effect.writesUnknownMemory && effect.initializesValue &&
                       !effect.removableWhenUnused && !effect.allocates,
                   "construction initializes caller bytes, not new allocation");
            expect(instruction.localFailureSites.empty() &&
                       instruction.unsafeOperation ==
                           lang::UnsafeOperationKind::ObjectLifetime,
                   "raw construction does not invent storage occupancy proof");
          }
    expect(constructions == 2, "concrete raw construction census");
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      for (auto level :
           {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
        try {
          const auto cpp = gti_test::emitCppText(
              construction, standard, lang::TargetInfo::host(), level);
          expect(cpp.find("__gti_mir_construct_success_") != std::string::npos,
                 "constructor failure uses verified status edge");
        } catch (const std::exception &error) {
          std::cerr << error.what() << '\n';
          expect(false, "raw construction emits through verified MIR");
        }
      }
    }
    for (int mutation = 0; mutation != 5; ++mutation)
      forged(
          construction,
          [=](auto &i) {
            switch (mutation) {
            case 0:
              i.unsafeOperation = lang::UnsafeOperationKind::None;
              break;
            case 1:
              i.constructorTarget.reset();
              break;
            case 2:
              i.definedFailure = {};
              break;
            case 3:
              i.objectAllocation = i.id;
              break;
            case 4:
              i.borrowArgument = 1;
              break;
            }
          },
          "forged construction authority rejected",
          lang::IntrinsicKind::ObjectConstruct);

    const auto lowered = gti_test::lowerProgram(construction, construction.mir,
                                                construction.mir);
    const lang::CppMirBodyEmissionMap map{lang::buildCppMirBodyEmissionMapRows(
        lowered, lang::CppStandard::Cpp23)};
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto mir = construction.mir;
      bool changed = false;
      lang::MirBodyAddress address;
      for (auto &function :
           const_cast<std::vector<lang::MirFunctionInstance> &>(
               mir.functionInstances())) {
        for (auto &block : function.body.blocks) {
          for (auto &call : block.instructions) {
            if (changed ||
                call.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
                call.operands.size() != 2 ||
                call.parameterTypes.back().kind !=
                    lang::SemanticType::Reference)
              continue;
            const auto *value =
                function.body.findValue(call.operands.back().value);
            if (value == nullptr)
              continue;
            for (auto &inputBlock : function.body.blocks) {
              for (auto &input : inputBlock.instructions) {
                if (input.id != value->definition)
                  continue;
                if (mutation == 0)
                  ++input.callSite;
                else if (mutation == 1)
                  input.callInputIndex = 0;
                else
                  input.callInputKind = lang::HirCallInputKind::Value;
                changed = true;
                address = {.kind = lang::MirBodyKind::Function,
                           .owner = function.id};
              }
            }
          }
        }
      }
      expect(changed && !lang::verifyMirProgram(mir).valid(),
             "raw constructor reference input retains exact staged call "
             "authority");
      expect(changed &&
                 !lang::CppMirBodyEmitter(mir, map).supportsFailureBodyText(
                     address),
             "detached backend rejects forged constructor reference staging");
    }
  }
  const auto transfers = analyze(R"(
#include <std/memory>
#include <std/string>
struct Record { int number = 4; };
int main() {
  Record record{};
  Record* copy = nullptr;
  std::string text{"owned"};
  std::string* moved = nullptr;
  unsafe {
    std::construct_at(copy, record);
    std::construct_at(moved, std::move(text));
    std::destroy_at(copy);
    std::destroy_at(moved);
  }
  return 0;
}
)");
  if (!transfers.canGenerateCode())
    diagnostics(transfers);
  expect(transfers.canGenerateCode(),
         "raw construction preserves generated copy and owning movement");
  if (transfers.canGenerateCode()) {
    try {
      (void)gti_test::emitCppText(transfers);
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false,
             "generated transfer construction has complete MIR lowering");
    }
  }
  return failures == 0 ? 0 : 1;
}
