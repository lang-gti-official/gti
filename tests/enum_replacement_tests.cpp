#include "../src/compiler/mir_verification_queries.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &candidate : block.instructions)
      if (candidate.id == id)
        return &candidate;
  return nullptr;
}

void checkReplacementProof() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/enum_replacement_success.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto standardLibrary =
      path.parent_path().parent_path().parent_path() / "stdlib";
  auto target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = true;
  const auto frontend =
      lang::Frontend({.target = target})
          .analyze(path.string(), source, {standardLibrary / "prelude.gti"}, {},
                   {standardLibrary});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "nonassignable payload permits checked whole-enum replacement");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCppText(frontend, lang::CppStandard::Cpp23, target)
              .empty(),
         "checked replacement has complete native representation");
  // A mutex relocates structurally when its payload does: relocation builds a
  // fresh native mutex and transfers the payload, never copying native
  // identity bytes. A locked mutex is borrowed by its lock token, so it cannot
  // be relocated while held. Exercise the exact operation query independently
  // of SSA/body rejection, so an unrelated malformed operand cannot hide it.
  const auto relocation = [](const lang::SemanticType &type) {
    const auto pointer = lang::SemanticType::rawPointerTo(type);
    lang::MirInstruction result;
    result.id = 1;
    result.kind = lang::MirInstructionKind::Call;
    result.unsafeOperation = lang::UnsafeOperationKind::ObjectLifetime;
    result.operands = {
        {.kind = lang::MirOperandKind::Value, .value = 1, .type = pointer},
        {.kind = lang::MirOperandKind::Value, .value = 2, .type = pointer}};
    result.parameterTypes = {pointer, pointer};
    result.intrinsic = lang::IntrinsicKind::ObjectRelocate;
    result.info.type = lang::SemanticType::Void;
    result.info.category = lang::ValueCategory::Value;
    result.info.access = lang::AccessMode::ReadOnly;
    result.info.traits = lang::semanticTraits(lang::SemanticType::Void);
    return result;
  };
  expect(lang::mir_detail::objectRelocate(
             frontend.mir, relocation(lang::SemanticType::Int32)),
         "exact single-object relocation control is accepted");
  std::size_t structural = 0;
  const auto checkStructural = [&](const lang::SemanticType &type) {
    ++structural;
    expect(lang::mirTypeRelocationIsDefinedFailureFree(frontend.mir, type),
           "mutex-bearing owner has failure-free generated movement");
    expect(lang::mirTypeRelocationKind(frontend.mir, type) ==
               lang::RelocationKind::Structural,
           "a mutex and its owners relocate structurally, never bitwise");
    expect(lang::mir_detail::objectRelocate(frontend.mir, relocation(type)),
           "single-object relocation accepts a structurally relocatable "
           "mutex owner");
  };
  for (const auto &instance : frontend.hir.classInstances()) {
    if (!instance.source ||
        (instance.source->name().lexeme != "mutex" &&
         instance.source->name().lexeme != "ReplacementResource"))
      continue;
    const auto *owner = frontend.mir.findClassInstance(instance.id);
    expect(owner != nullptr, "public mutex and wrapper retain MIR identities");
    if (owner)
      checkStructural(owner->type);
  }
  for (const auto &enumeration : frontend.mir.payloadEnums())
    if (enumeration.traits.moveAssignable &&
        lang::mirTypeRelocationKind(frontend.mir, enumeration.type) !=
            lang::RelocationKind::Bitwise)
      checkStructural(enumeration.type);
  expect(structural >= 3,
         "relocation policy covers public mutex, wrapper and replacing enum");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23, target));
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &commit : block.instructions) {
        const auto *destination =
            commit.destination ? function.body.findPlace(*commit.destination)
                               : nullptr;
        if (commit.kind != lang::MirInstructionKind::Assign || !destination ||
            destination->type.kind != lang::SemanticType::Enum ||
            !commit.replacementPair || commit.lifecycle.size() != 1)
          continue;
        ++checked;
        expect(destination->traits.moveAssignable &&
                   !destination->traits.containsBorrowedState,
               "replacement capability is published without weakening borrow "
               "state");
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto *changed = instruction(*body, commit.id);
          auto *cleanup = instruction(*body, commit.replacementPair);
          expect(changed && cleanup,
                 "replacement mutation finds its exact pair");
          if (!changed || !cleanup)
            continue;
          if (mutation == 0)
            cleanup->replacementCleanup.reset();
          else if (mutation == 1)
            changed->replacementPair = 0;
          else if (mutation == 2)
            cleanup->replacementPair = 0;
          else if (mutation == 3)
            changed->lifecycle.clear();
          else if (mutation == 4)
            changed->lifecycle.front().kind =
                lang::MirLifecycleEventKind::Initialize;
          else if (mutation == 5)
            changed->lifecycle.front().source = 0;
          else if (mutation == 6)
            cleanup->replacementCleanup->aggregate.clear();
          else
            changed->operands.front().type = lang::SemanticType::Int32;
          expect(lang::rebuildMirValueUses(*body),
                 "replacement mutations preserve SSA use records");
          const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                             function.id};
          const lang::CppMirBodyEmitter emitter(malformed, map);
          const auto label = "replacement mutation " + std::to_string(mutation);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 label + " rejects malformed MIR");
          expect(!emitter.supportsFailureBodyText(address),
                 label + " rejects failure-form backend admission");
          expect(!emitter.supportsBodyText(address),
                 label + " rejects ordinary backend admission");
        }
      }
  expect(checked == 4, "same/different/empty/nonempty alternatives all retain "
                       "exact replacement proofs");
}

void checkBorrowBoundary() {
  const std::string prefix = R"gti(
import std.string;
enum BorrowedEvent { empty, borrowed(int& owner), owned(std::string value) };
)gti";
  const auto valid =
      analyzeWithStandardLibrary("borrowed-enum-control.gti", prefix + R"gti(
int main() { int owner = 3; auto value = BorrowedEvent::borrowed(owner); return 0; }
)gti");
  expect(valid.canGenerateCode(),
         "borrowed enum control remains valid without replacement");
  const auto rejected = analyzeWithStandardLibrary(
      "borrowed-enum-replacement.gti", prefix + R"gti(
int main() {
  int owner = 3;
  mut auto value = BorrowedEvent::borrowed(owner);
  value = BorrowedEvent::owned(std::string("replacement"));
  return 0;
}
)gti");
  expect(!rejected.semanticValid && std::none_of(rejected.diagnostics.begin(),
                                                 rejected.diagnostics.end(),
                                                 [](const auto &diagnostic) {
                                                   return diagnostic.code ==
                                                          "GTI-B0001";
                                                 }),
         "replacement does not grant new borrowed-enum assignment permissions");
  if (!valid.canGenerateCode())
    return;
  for (const auto &enumeration : valid.mir.payloadEnums()) {
    if (!enumeration.traits.containsBorrowedState)
      continue;
    auto malformed = valid.mir;
    auto *changed = const_cast<lang::MirPayloadEnum *>(
        malformed.findPayloadEnum(enumeration.type));
    changed->traits.moveAssignable = true;
    // Forge all copied body claims together: rejection must come from the
    // concrete enum capability rule, not merely a mismatched local cache.
    for (const auto address : lang::enumerateMirBodyAddresses(malformed)) {
      auto *body = lang::findMirBody(malformed, address);
      for (auto &place : body->places)
        if (place.type == enumeration.type)
          place.traits.moveAssignable = true;
      for (auto &value : body->values)
        if (value.info.type == enumeration.type)
          value.info.traits.moveAssignable = true;
      for (auto &block : body->blocks)
        for (auto &value : block.instructions)
          if (value.info.type == enumeration.type)
            value.info.traits.moveAssignable = true;
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "mutually forged borrowed-enum replacement capability is rejected "
           "independently");
  }
}

void checkImmovableBoundary() {
  const auto result =
      analyzeWithStandardLibrary("immovable-enum-replacement.gti", R"gti(
class Immovable {
public:
  Immovable() {}
  Immovable(Immovable& other) = delete;
  Immovable(Immovable&& other) = delete;
  ~Immovable() {}
};
enum ImmovableEvent { empty, held(Immovable value) };
void replace(mut ImmovableEvent& destination) {
  destination = ImmovableEvent::empty;
}
int main() { return 0; }
)gti");
  expect(!result.semanticValid &&
             std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code == "GTI-B0001";
                          }),
         "whole-enum replacement cannot manufacture payload move construction");
}
} // namespace

int main() {
  checkReplacementProof();
  checkBorrowBoundary();
  checkImmovableBoundary();
  return failures ? 1 : 0;
}
