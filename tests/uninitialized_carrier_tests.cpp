#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}
// Results are ordinary prelude classes: recognize them by their declared
// identity, never by a compiler-owned type kind.
bool isPreludeClass(const lang::FrontendResult &result,
                    const lang::SemanticType &type, std::string_view name) {
  if (type.kind != lang::SemanticType::Class)
    return false;
  const auto *info = result.semantics.findClassType(type.classId);
  return info && info->qualifiedName == name;
}
lang::FrontendResult analyze(const std::string &source,
                             const std::string &trustedMemorySuffix = {}) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "uninitialized-carrier-test.gti";
  std::unordered_map<std::string, std::string> overrides{
      {std::filesystem::weakly_canonical(entry).string(), source}};
  if (!trustedMemorySuffix.empty()) {
    const auto memory = root / "stdlib/std/memory.gti";
    std::ifstream input(memory);
    expect(input.good(), "trusted memory fixture exists");
    std::ostringstream contents;
    contents << input.rdbuf() << trustedMemorySuffix;
    overrides.emplace(std::filesystem::weakly_canonical(memory).string(),
                      contents.str());
  }
  return lang::Frontend().analyze(entry, source, {root / "stdlib/prelude.gti"},
                                  overrides, {root / "stdlib"});
}
} // namespace

int main() {
  const std::string prefix = R"(
import std.memory;
import std.string;
struct Plain { int value = 8;  public: Plain() = default; };
class Stationary {
public:
  Stationary(int value) {}
  Stationary(Stationary& other) = delete;
  Stationary(Stationary&& other) = delete;
public:
  Stationary() = default;
};
class Holder<std::layout_queryable T> {
  mut std::uninitialized<T> bytes{};
public:
  Holder() {}
};
)";
  const auto valid = analyze(prefix + R"(
int main() {
  Holder<Stationary> holder{};
  std::uninitialized<Stationary> vacant{};
  std::uninitialized<Plain> first{};
  auto second = first;
  auto third = std::move(second);
  return 0;
}
)");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(valid.canGenerateCode(),
         "carrier admits empty immovable and bitwise copy/move");
  if (valid.canGenerateCode()) {
    try {
      for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
        (void)gti_test::emitCppText(valid, standard);
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false, "carrier copy/move has complete MIR emission");
    }
    for (int mutation = 0; mutation < 5; ++mutation) {
      auto mir = valid.mir;
      bool changed = false;
      for (auto &owner : const_cast<std::vector<lang::MirClassInstance> &>(
               mir.classInstances())) {
        if (owner.compilerCapability !=
            lang::CompilerCapabilityTypeKind::UninitializedStorage)
          continue;
        switch (mutation) {
        case 0:
          owner.traits.movable = !owner.traits.movable;
          break;
        case 1:
          owner.traits.copyable = !owner.traits.copyable;
          break;
        case 2:
          owner.requiresActiveCleanup = true;
          break;
        case 3:
          owner.traits.transferCapable = !owner.traits.transferCapable;
          break;
        case 4:
          owner.traits.shareCapable = !owner.traits.shareCapable;
          break;
        }
        changed = true;
        break;
      }
      expect(changed && !lang::verifyMirProgram(mir).valid(),
             "forged carrier lifetime/capability facts rejected");
    }
  }
  const std::string policies = R"(
struct RawRecord { int* address = nullptr;  public: RawRecord() = default; };
[[no_transfer, no_share]] struct AffineRecord { int value = 0;  public: AffineRecord() = default; };
[[no_share]] struct ExclusiveRecord { int value = 0;  public: ExclusiveRecord() = default; };
)";
  const auto aggregates = analyze(prefix + policies + R"(
int main() {
  std::uninitialized<expected<Plain, int>> plain{};
  std::uninitialized<expected<void, Plain>> error{};
  std::uninitialized<expected<RawRecord, int>> raw{};
  std::uninitialized<expected<Plain, RawRecord>> raw_error{};
  std::uninitialized<expected<AffineRecord, int>> affine{};
  std::uninitialized<expected<ExclusiveRecord, int>> exclusive{};
  return 0;
}
)");
  if (!aggregates.canGenerateCode())
    for (const auto &diagnostic : aggregates.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(aggregates.canGenerateCode(),
         "aggregate byte carriers retain exact nominal concurrency facts");
  if (aggregates.canGenerateCode()) {
    std::size_t expected = 0;
    bool raw = false, affine = false, exclusive = false;
    for (const auto &owner : aggregates.mir.classInstances()) {
      if (owner.compilerCapability !=
              lang::CompilerCapabilityTypeKind::UninitializedStorage ||
          owner.type.arguments.size() != 1)
        continue;
      const auto &payload = owner.type.arguments.front();
      if (!isPreludeClass(aggregates, payload, "expected"))
        continue;
      ++expected;
      bool transfer = true, share = true;
      for (auto component : payload.arguments) {
        if (component.kind == lang::SemanticType::Void ||
            component.kind == lang::SemanticType::Int32)
          continue;
        const auto nominal = std::find_if(
            aggregates.mir.classInstances().begin(),
            aggregates.mir.classInstances().end(),
            [&](const auto &record) { return record.type == component; });
        expect(nominal != aggregates.mir.classInstances().end(),
               "aggregate components have exact published record facts");
        if (nominal == aggregates.mir.classInstances().end())
          continue;
        const auto *info =
            aggregates.semantics.findClassType(nominal->declaration);
        expect(info && info->declaration,
               "record control has its source declaration identity");
        if (info && info->declaration) {
          const auto &name = info->declaration->name().lexeme;
          const bool mayTransfer = name == "Plain" || name == "ExclusiveRecord";
          const bool mayShare = name == "Plain";
          expect(nominal->traits.transferCapable == mayTransfer &&
                     nominal->traits.shareCapable == mayShare,
                 "unrelated nominal controls retain their source policies");
          raw |= name == "RawRecord";
          affine |= name == "AffineRecord";
          exclusive |= name == "ExclusiveRecord";
        }
        transfer &= nominal->traits.transferCapable;
        share &= nominal->traits.shareCapable;
      }
      expect(owner.traits.transferCapable == transfer &&
                 owner.traits.shareCapable == share,
             "Expected carriers fold concrete success/error capabilities");
      for (const bool forgeTransfer : {true, false}) {
        auto forged = aggregates.mir;
        auto *changed = const_cast<lang::MirClassInstance *>(
            forged.findClassInstance(owner.id));
        if (forgeTransfer)
          changed->traits.transferCapable = !transfer;
        else
          changed->traits.shareCapable = !share;
        const auto verification = lang::verifyMirProgram(forged);
        expect(!verification.valid() &&
                   std::any_of(
                       verification.errors.begin(), verification.errors.end(),
                       [&](const auto &error) {
                         return error.bodyKind ==
                                    lang::MirBodyKind::FieldInitializers &&
                                error.owner == owner.id &&
                                error.message ==
                                    "uninitialized storage lost its exact "
                                    "layout-only lifetime contract";
                       }),
               "MIR rejects both granted and erased aggregate capabilities");
      }
    }
    expect(expected == 6 && raw && affine && exclusive,
           "controls cover nominal success/error, void success, raw/affine and "
           "exclusive-only policies");
  }
  // Unexpected and builtin owning handles are ordinary transported values,
  // but do not currently participate in the runtime layout-query surface.
  // Capture each in a concrete lambda and forward it by value through a pack:
  // the verifier recomputes the lambda's complete traits from its actual
  // capture type, including the aggregate's concrete concurrency facts.
  const auto transported = analyze(prefix + policies + R"(
int main() {
  std::error_state(unexpected(Plain()));
  std::error_state(unexpected(RawRecord()));
  std::error_state(unexpected(AffineRecord()));
  std::error_state(unexpected(ExclusiveRecord()));
  std::owner_states<Plain>();
  std::owner_states<RawRecord>();
  std::owner_states<AffineRecord>();
  std::owner_states<ExclusiveRecord>();
  return 0;
}
)",
                                   R"(
// The helper's actual trusted source identity permits naming the builtin
// owning handle. Neither the public wrapper's name nor its generic arguments
// supply compiler meaning in this test.
namespace std {
void receive_states<States...>(States... states) {}
void forward_states<States...>(States... states) { receive_states(states...); }
export void error_state<Error>(Error error) {
  forward_states([value = std::move(error)]() -> void {});
}
export void owner_states<T>() {
  gti_internal::unique_owner<T> owner = nullptr;
  gti_internal::unique_owner<T> result_owner = nullptr;
  expected<gti_internal::unique_owner<T>, int> result = std::move(result_owner);
  forward_states([owned = std::move(owner)]() -> void {},
                 [owned = std::move(result)]() -> void {});
}
}
)");
  if (!transported.canGenerateCode())
    for (const auto &diagnostic : transported.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(transported.canGenerateCode(),
         "ordinary by-value forwarding verifies Unexpected and builtin owners");
  if (transported.canGenerateCode()) {
    std::size_t unexpected = 0, owned = 0, nested = 0;
    bool raw = false, affine = false, exclusive = false;
    unsigned errorPolicies = 0, ownerPolicies = 0, nestedPolicies = 0;
    for (const auto &function : transported.mir.functionInstances()) {
      for (const auto &place : function.body.places) {
        if (place.type.kind != lang::SemanticType::Lambda ||
            place.projections.size() != 1 ||
            place.projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        const auto lambda =
            std::find_if(transported.mir.lambdaInstances().begin(),
                         transported.mir.lambdaInstances().end(),
                         [&](const auto &candidate) {
                           return candidate.type == place.type;
                         });
        if (lambda == transported.mir.lambdaInstances().end() ||
            lambda->captureTypes.size() != 1)
          continue;
        auto component = lambda->captureTypes.front();
        const bool directOwner =
            component.kind == lang::SemanticType::UniqueOwner;
        const bool nestedOwner =
            isPreludeClass(transported, component, "expected") &&
            component.arguments.size() == 2 &&
            component.arguments.front().kind == lang::SemanticType::UniqueOwner;
        const bool error =
            isPreludeClass(transported, component, "std::unexpected_value");
        if (!directOwner && !nestedOwner && !error)
          continue;
        unexpected += error;
        owned += directOwner;
        nested += nestedOwner;
        if (nestedOwner)
          component = lang::SemanticType(component.arguments.front());
        expect(component.arguments.size() == 1,
               "transported aggregate retains its one exact component");
        if (component.arguments.size() != 1)
          continue;
        component = lang::SemanticType(component.arguments.front());
        const auto nominal = std::find_if(
            transported.mir.classInstances().begin(),
            transported.mir.classInstances().end(),
            [&](const auto &record) { return record.type == component; });
        expect(nominal != transported.mir.classInstances().end(),
               "transported state retains exact unrelated record traits");
        if (nominal == transported.mir.classInstances().end())
          continue;
        const auto *info =
            transported.semantics.findClassType(nominal->declaration);
        expect(info && info->declaration,
               "transported record retains source identity");
        if (!info || !info->declaration)
          continue;
        const auto &name = info->declaration->name().lexeme;
        const bool transfer = name == "Plain" || name == "ExclusiveRecord";
        const bool share = name == "Plain";
        raw |= name == "RawRecord";
        affine |= name == "AffineRecord";
        exclusive |= name == "ExclusiveRecord";
        auto &policyCoverage = error         ? errorPolicies
                               : directOwner ? ownerPolicies
                                             : nestedPolicies;
        policyCoverage |= name == "Plain"             ? 1u
                          : name == "RawRecord"       ? 2u
                          : name == "AffineRecord"    ? 4u
                          : name == "ExclusiveRecord" ? 8u
                                                      : 0u;
        expect(place.traits.transferCapable == transfer &&
                   place.traits.shareCapable == share &&
                   nominal->traits.transferCapable == transfer &&
                   nominal->traits.shareCapable == share,
               "forwarding retains exact aggregate/pointee concurrency facts");
        if (directOwner || nestedOwner)
          expect(
              !place.traits.copyable && place.traits.movable &&
                  place.traits.drop == lang::DropKind::Lexical &&
                  !place.traits.containsBorrowedState,
              "pointee capabilities do not overwrite owning transport traits");
        for (const bool forgeTransfer : {true, false}) {
          auto forged = transported.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          auto &traits = changed->body.places[place.id - 1].traits;
          if (forgeTransfer)
            traits.transferCapable = !transfer;
          else
            traits.shareCapable = !share;
          const auto verification = lang::verifyMirProgram(forged);
          expect(!verification.valid() &&
                     std::any_of(
                         verification.errors.begin(), verification.errors.end(),
                         [&](const auto &error) {
                           return error.bodyKind ==
                                      lang::MirBodyKind::Function &&
                                  error.owner == function.id &&
                                  error.message ==
                                      "concrete pack element traits do not "
                                      "match its resolved type";
                         }),
                 "MIR rejects forged transport capability grants and erasures");
        }
      }
    }
    expect(unexpected >= 4 && owned >= 4 && nested >= 4 && raw && affine &&
               exclusive && errorPolicies == 15 && ownerPolicies == 15 &&
               nestedPolicies == 15,
           "pack controls exercise inferred errors and direct/nested builtin "
           "owners");
  }
  for (const auto *body :
       {"int main() { Holder<Stationary> a{}; auto b = std::move(a); "
        "return 0; }",
        "int main() { Holder<std::string> a{}; auto b = std::move(a); "
        "return 0; }",
        "int main() { std::uninitialized<Stationary> a{}; auto b = "
        "std::move(a); return 0; }",
        "int main() { std::uninitialized<Stationary> a{}; auto b = a; return "
        "0; }",
        "int main() { std::uninitialized<std::string> a{}; auto b = "
        "std::move(a); return 0; }",
        "int main() { std::uninitialized<int> a{}; int* p = a.data(); return "
        "0; }",
        "int main() { mut std::uninitialized<int> a{}; "
        "std::construct_at(a.data(), 1); return 0; }",
        "int main() { std::uninitialized<void> a{}; return 0; }",
        "int main() { std::uninitialized<expected<Plain&, int>> a{}; return 0; "
        "}"}) {
    expect(!analyze(prefix + body).canGenerateCode(),
           "invalid carrier movement, access or construction rejected");
  }
  const auto relocation = analyze(prefix + R"(
int main() {
  Plain* destination = nullptr;
  Plain* source = nullptr;
  unsafe { std::relocate_at(destination, source); }
  return 0;
}
)");
  // Static-only: the two null addresses deliberately do not assert runtime
  // validity. Separate executable fixtures supply actual storage.
  expect(relocation.canGenerateCode(),
         "typed raw relocation forms verified MIR");
  if (relocation.canGenerateCode()) {
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto mir = relocation.mir;
      bool changed = false;
      for (auto &function :
           const_cast<std::vector<lang::MirFunctionInstance> &>(
               mir.functionInstances()))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed ||
                instruction.intrinsic != lang::IntrinsicKind::ObjectRelocate)
              continue;
            const auto effects = lang::effects(instruction);
            expect(effects.readsUnknownMemory && effects.writesUnknownMemory &&
                       effects.movesValue && effects.initializesValue &&
                       !effects.mayTrap && !effects.allocates &&
                       !effects.invokesUserCode,
                   "raw relocation retains its no-failure ownership barrier");
            switch (mutation) {
            case 0:
              instruction.unsafeOperation = lang::UnsafeOperationKind::None;
              break;
            case 1:
              instruction.parameterTypes[1].pointerAccess =
                  lang::AccessMode::ReadOnly;
              break;
            case 2:
              instruction.definedFailure.propagation =
                  lang::FailurePropagationKind::Destructor;
              break;
            case 3:
              instruction.objectAllocation = instruction.id;
              break;
            }
            changed = true;
          }
      expect(changed && !lang::verifyMirProgram(mir).valid(),
             "forged relocation rejected");
      bool rejected = false;
      try {
        (void)gti_test::emitCpp(relocation, relocation.mir, mir);
      } catch (const std::exception &) {
        rejected = true;
      }
      expect(rejected, "backend rejects forged raw relocation");
    }
  }
  for (const auto *body :
       {"int main() { Stationary* a = nullptr; Stationary* b = nullptr; unsafe "
        "{ std::relocate_at(a, b); } return 0; }",
        "int main() { int* a = nullptr; const int* b = nullptr; unsafe { "
        "std::relocate_at(a, b); } return 0; }",
        "int main() { int* a = nullptr; int* b = nullptr; std::relocate_at(a, "
        "b); return 0; }"})
    expect(!analyze(prefix + body).canGenerateCode(),
           "invalid raw relocation rejected");

  const auto range = analyze(prefix + R"(
void transfer(Plain* destination, Plain* source, std::size_t count) {
  unsafe { std::relocate_range_at(destination, source, count); }
}
int main() { transfer(nullptr, nullptr, std::size_t(0)); return 0; }
)");
  expect(range.canGenerateCode(),
         "bitwise range relocation forms verified MIR");
  if (range.canGenerateCode()) {
    const auto emitted = gti_test::emitCppText(range);
    expect(emitted.find(
               "::gti_internal::backend::mir_object_relocate_range_v1(") !=
               std::string::npos,
           "production MIR emitter consumes bulk relocation");
    for (int mutation = 0; mutation != 7; ++mutation) {
      auto mir = range.mir;
      bool changed = false;
      for (auto &function :
           const_cast<std::vector<lang::MirFunctionInstance> &>(
               mir.functionInstances()))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed || instruction.intrinsic !=
                               lang::IntrinsicKind::ObjectRelocateRange)
              continue;
            const auto effects = lang::effects(instruction);
            expect(effects.readsUnknownMemory && effects.writesUnknownMemory &&
                       effects.movesValue && effects.initializesValue &&
                       !effects.mayTrap && !effects.allocates &&
                       !effects.invokesUserCode && !effects.removableWhenUnused,
                   "bulk relocation retains its exact lifetime/memory barrier");
            switch (mutation) {
            case 0:
              instruction.unsafeOperation = lang::UnsafeOperationKind::None;
              break;
            case 1:
              instruction.parameterTypes[1].pointerAccess =
                  lang::AccessMode::ReadOnly;
              break;
            case 2:
              instruction.parameterTypes[2] = lang::SemanticType::Int64;
              break;
            case 3:
              instruction.operands.pop_back();
              break;
            case 4:
              instruction.definedFailure.propagation =
                  lang::FailurePropagationKind::Destructor;
              break;
            case 5:
              instruction.intrinsic = lang::IntrinsicKind::ObjectRelocate;
              break;
            case 6:
              instruction.operands[2].kind = lang::MirOperandKind::BorrowRead;
              break;
            }
            changed = true;
          }
      expect(changed && !lang::verifyMirProgram(mir).valid(),
             "forged range relocation rejected by MIR");
      bool rejected = false;
      try {
        (void)gti_test::emitCpp(range, range.mir, mir);
      } catch (const std::exception &) {
        rejected = true;
      }
      expect(rejected, "backend rejects forged range relocation");
    }
  } else {
    for (const auto &diagnostic : range.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  for (const auto *body :
       {"void transfer(std::string* a, std::string* b) { unsafe { "
        "std::relocate_range_at(a, b, std::size_t(1)); } }",
        "void transfer(Stationary* a, Stationary* b) { unsafe { "
        "std::relocate_range_at(a, b, std::size_t(1)); } }",
        "void transfer(int* a, const int* b) { unsafe { "
        "std::relocate_range_at(a, b, std::size_t(1)); } }",
        "void transfer(int* a, int* b) { std::relocate_range_at(a, b, "
        "std::size_t(1)); }",
        "void transfer(int* a, int* b) { unsafe { std::relocate_range_at(a, b, "
        "-1); } }"}) {
    expect(
        !analyze(prefix + body + "int main() { return 0; }").canGenerateCode(),
        "range relocation rejects non-bitwise, read-only, safe and "
        "signed-count uses");
  }
  return failures == 0 ? 0 : 1;
}
