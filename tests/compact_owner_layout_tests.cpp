#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_printer.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/compact_owner_layout.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "compact owner fixture analyzes");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::MirClassInstance *guard = nullptr;
  const lang::MirClassInstance *unconditional = nullptr;
  for (const auto &owner : frontend.hir.classInstances()) {
    if (!owner.source)
      continue;
    const auto &name = owner.source->name().lexeme;
    const auto *mir = frontend.mir.findClassInstance(owner.id);
    if (name == "Guard") {
      guard = mir;
      expect(owner.requiresActiveDropState && owner.inactiveDropPointerField &&
                 mir->inactiveDropPointerField ==
                     owner.inactiveDropPointerField,
             "logical lifetime state retains its exact pointer witness");
    } else if (name == "Unconditional" || name == "BeforeGuard" ||
               name == "ElseEffect" || name == "ScalarField") {
      expect(!owner.inactiveDropPointerField && !mir->inactiveDropPointerField,
             "effects on null and unsupported representation keep their byte");
      if (name == "Unconditional")
        unconditional = mir;
    }
  }
  expect(guard && unconditional, "positive and negative owners instantiated");
  if (!guard || !unconditional)
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(), "null witness verifies");
  expect(
      lang::MirPrinter().print(frontend.mir).find("inactive-drop-pointer=") !=
          std::string::npos,
      "witness is part of the deterministic MIR contract");
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto malformed = frontend.mir;
    auto *owner = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(guard->id));
    if (mutation == 0)
      owner->inactiveDropPointerField = 999999;
    if (mutation == 1)
      owner->inactiveDropPointerField = owner->declaredFields.back().symbol;
    if (mutation == 2)
      owner->polymorphic = true;
    if (mutation == 3) {
      auto *other = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(unconditional->id));
      other->inactiveDropPointerField = other->declaredFields.front().symbol;
    }
    if (mutation == 4) {
      auto *body = const_cast<lang::MirDestructorInstance *>(
          malformed.findDestructorInstance(*owner->destructor));
      bool changed = false;
      for (auto &block : body->body.blocks)
        for (auto &row : block.instructions)
          if (row.operation == lang::MirOperation::NotEqual) {
            row.operation = lang::MirOperation::Equal;
            changed = true;
          }
      expect(changed, "destructor predicate mutation found its comparison");
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "forged null-state witnesses fail closed");
  }
  return failures == 0 ? 0 : 1;
}
