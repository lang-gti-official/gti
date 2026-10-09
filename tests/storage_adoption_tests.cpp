#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool okay, const char *message) {
  if (!okay) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
lang::FrontendResult analyze(const std::string &source, bool trusted = true) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "storage-adoption-test.gti";
  auto preludes =
      std::vector<std::filesystem::path>{root / "stdlib/prelude.gti"};
  if (trusted)
    preludes.push_back(entry);
  return lang::Frontend().analyze(
      entry, source, preludes,
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
}
void diagnostics(const lang::FrontendResult &result) {
  for (const auto &item : result.diagnostics)
    std::cerr << item.code << ": " << item.message << '\n';
}
template <typename Mutation>
void forged(const lang::FrontendResult &frontend, Mutation mutation,
            const char *message) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed &&
            instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy &&
            instruction.elementCleanup) {
          mutation(instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), message);
  bool rejected = false;
  try {
    (void)gti_test::emitCpp(frontend, frontend.mir, mir);
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(changed && rejected,
         "detached backend rejects malformed lifetime authority");
}
} // namespace

int main() {
  const std::string prefix = "#include <std/memory>\n";
  // Sparse region admission is now ordinary library policy. The underlying
  // operation that starts/ends a payload lifetime retains general unsafe
  // authority and independently verified cleanup, not adoption-specific MIR.
  // This static fixture is not executed: its destructor deliberately fails.
  const std::string source = prefix + R"(
#include <std/panic>
class Payload {
  int value;
public:
  Payload(int value) : value(value) {}
  Payload(Payload& other) = delete;
  Payload(Payload&& other) = delete;
  ~Payload() { std::panic("payload cleanup"); }
};
int main() {
  mut std::uninitialized<Payload> slot{};
  unsafe {
    [[discard]] std::construct_at(slot.data(), 42);
    std::destroy_at(slot.data());
  }
  return 0;
})";
  const auto frontend = analyze(source, false);
  if (!frontend.canGenerateCode()) {
    diagnostics(frontend);
    std::cerr << lang::MirPrinter().print(frontend.mir);
  }
  expect(frontend.canGenerateCode(),
         "application-defined payload uses public manual lifetime operations");
  if (frontend.canGenerateCode()) {
    std::size_t count = 0;
    for (const auto &function : frontend.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy &&
              instruction.elementCleanup) {
            ++count;
            const auto effects = lang::effects(instruction);
            expect(!effects.allocates && effects.dropsValue &&
                       effects.writesUnknownMemory &&
                       !effects.removableWhenUnused &&
                       instruction.unsafeOperation ==
                           lang::UnsafeOperationKind::ObjectLifetime &&
                       instruction.definedFailure.propagation ==
                           lang::FailurePropagationKind::Destructor,
                   "manual lifetime cleanup retains unsafe authority and "
                   "original destructor failure without allocation policy");
          }
    expect(count == 1, "exactly one payload destruction operation");
    for (auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto cpp = gti_test::emitCppText(frontend, lang::CppStandard::Cpp20,
                                             lang::TargetInfo::host(), level);
      expect(
          cpp.find("mir_object_destroy_v1(") != std::string::npos &&
              cpp.find("mir_storage_") == std::string::npos &&
              cpp.find("backend::storage<") == std::string::npos,
          "verified general lifetime operation replaces native sparse helpers");
    }
    forged(
        frontend,
        [](auto &i) { i.unsafeOperation = lang::UnsafeOperationKind::None; },
        "missing unsafe lifetime marker rejected");
    forged(
        frontend, [](auto &i) { i.operands.pop_back(); },
        "missing object address rejected");
    forged(
        frontend,
        [](auto &i) {
          i.operands.front().type.pointerAccess = lang::AccessMode::ReadOnly;
          i.parameterTypes.front() = i.operands.front().type;
        },
        "readonly object address rejected");
    forged(
        frontend, [](auto &i) { i.info.type = lang::SemanticType::Int32; },
        "destruction cannot invent a status result");
    forged(
        frontend, [](auto &i) { i.borrowArgument = 1; },
        "unrelated borrow metadata rejected");
    forged(
        frontend, [](auto &i) { i.neverReturns = true; },
        "terminal-call authority cannot be attached to object destruction");
    forged(
        frontend, [](auto &i) { i.objectAllocation = i.id; },
        "allocator handoff cannot be attached to raw object destruction");
    forged(
        frontend, [](auto &i) { i.info.category = lang::ValueCategory::Place; },
        "object destruction cannot publish a borrow");
    forged(
        frontend, [](auto &i) { i.elementCleanup.reset(); },
        "missing exact payload cleanup rejected");
    forged(
        frontend, [](auto &i) { i.definedFailure = {}; },
        "original destructor failure cannot be erased");
  }
  auto withoutUnsafe = source;
  withoutUnsafe.replace(withoutUnsafe.find("unsafe {"), 8, "{");
  const auto missing = analyze(withoutUnsafe, false);
  expect(!missing.canGenerateCode() &&
             std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2055"; }),
         "manual payload lifetime changes require an explicit unsafe block");

  // Even trusted source cannot regain compiler authority from retired names.
  const auto ordinary = analyze(R"(
namespace gti_internal {
  class storage<T> {
  public:
    storage() {}
  };
  int allocate_storage() { return 1; }
  int storage_try_adopt(storage<int>& value) { return 2; }
}
int main() {
  gti_internal::storage<int> value{};
  return gti_internal::allocate_storage() +
         gti_internal::storage_try_adopt(value) - 3;
}
)");
  if (!ordinary.canGenerateCode())
    diagnostics(ordinary);
  expect(ordinary.canGenerateCode(),
         "retired private storage spellings are ordinary trusted declarations");
  if (ordinary.canGenerateCode()) {
    std::size_t ordinaryClasses = 0;
    for (const auto &instance : ordinary.mir.classInstances()) {
      const auto *declaration =
          ordinary.semantics.findClassType(instance.declaration);
      if (declaration == nullptr ||
          declaration->qualifiedName != "gti_internal::storage")
        continue;
      ++ordinaryClasses;
      expect(instance.compilerCapability ==
                     lang::CompilerCapabilityTypeKind::None &&
                 instance.type.kind == lang::SemanticType::Class,
             "retired storage spelling cannot mint a capability type");
    }
    expect(ordinaryClasses == 1,
           "exact retired type declaration is instantiated once");
    std::vector<lang::HirFunctionInstanceId> ordinaryFunctions;
    for (const auto &function : ordinary.mir.functionInstances()) {
      const auto *declaration =
          ordinary.semantics.findFunction(function.declaration);
      if (declaration != nullptr &&
          (declaration->qualifiedName == "gti_internal::allocate_storage" ||
           declaration->qualifiedName == "gti_internal::storage_try_adopt"))
        ordinaryFunctions.push_back(function.id);
    }
    expect(ordinaryFunctions.size() == 2,
           "both retired call declarations have ordinary bodies");
    std::size_t ordinaryCalls = 0;
    for (const auto &function : ordinary.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (!instruction.functionTarget ||
              std::find(ordinaryFunctions.begin(), ordinaryFunctions.end(),
                        *instruction.functionTarget) == ordinaryFunctions.end())
            continue;
          ++ordinaryCalls;
          expect(instruction.intrinsic == lang::IntrinsicKind::None,
                 "retired allocation/adoption names cannot mint intrinsics");
        }
    expect(ordinaryCalls == 2, "both retired call spellings resolve to their "
                               "ordinary declaration identities");
    expect(!gti_test::emitCppText(ordinary).empty(),
           "ordinary declarations with retired names still emit");
  }
  const std::string resourceSource = R"(
#include <std/resource>
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_storage<int, std::process_resource> slots{resource};
  auto allocated = slots.try_allocate(std::size_t(1));
  if (!allocated) { return 1; }
  slots.emplace(std::size_t(0), 42);
  return slots.get(std::size_t(0)) - 42;
}
int main() {
  std::process_resource resource{};
  unsafe { return use(std::bind_resource(resource)); }
}
)";
  const auto resource = analyze(resourceSource, false);
  if (!resource.canGenerateCode())
    diagnostics(resource);
  expect(resource.canGenerateCode(),
         "one unsafe resource binding enables safe source-defined storage");
  if (resource.canGenerateCode()) {
    const auto cpp = gti_test::emitCppText(resource);
    expect(!cpp.empty() && cpp.find("backend::storage<") == std::string::npos &&
               cpp.find("mir_storage_") == std::string::npos &&
               cpp.find("storage_try_adopt(") == std::string::npos,
           "resource regions use source policy and general object lifetimes");
  }
  auto uncheckedBinding = resourceSource;
  uncheckedBinding.replace(uncheckedBinding.find("unsafe {"), 8, "{");
  const auto unsafeBinding = analyze(uncheckedBinding, false);
  expect(!unsafeBinding.canGenerateCode() &&
             std::any_of(unsafeBinding.diagnostics.begin(),
                         unsafeBinding.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2055"; }),
         "source-defined admission cannot bypass the unsafe resource contract");

  const auto immovable = analyze(prefix + R"(
class Anchor {
  int value;
public:
  Anchor(int value) : value(value) {}
  Anchor(Anchor& other) = delete;
  Anchor(Anchor&& other) = delete;
  int read() { return this.value; }
};
int main() {
  std::allocator allocator{};
  auto attempt = allocator.try_storage<Anchor>(std::size_t(1));
  if (!attempt) { return 1; }
  mut auto slots = std::move(attempt).value();
  slots.emplace(std::size_t(0), 42);
  auto moved = std::move(slots);
  return moved.get(std::size_t(0)).read() - 42;
})",
                                 false);
  if (!immovable.canGenerateCode())
    diagnostics(immovable);
  expect(immovable.canGenerateCode(),
         "owner moves without moving fixed-address elements");
  if (immovable.canGenerateCode()) {
    const auto cpp = gti_test::emitCppText(immovable);
    expect(
        !cpp.empty() && cpp.find("backend::storage<") == std::string::npos &&
            cpp.find("mir_storage_") == std::string::npos &&
            cpp.find("storage_try_adopt(") == std::string::npos,
        "source-defined immovable storage emits without native sparse helpers");
  }
  const auto relocation = analyze(prefix + R"(
class Anchor {
public:
  Anchor() {}
  Anchor(Anchor& other) = delete;
  Anchor(Anchor&& other) = delete;
};
int main() {
  std::allocator allocator{};
  mut auto first_result = allocator.try_storage<Anchor>(std::size_t(1));
  mut auto second_result = allocator.try_storage<Anchor>(std::size_t(1));
  if (!first_result || !second_result) { return 1; }
  mut auto first = std::move(first_result).value();
  mut auto second = std::move(second_result).value();
  first.emplace(std::size_t(0));
  first.relocate_to(second, std::size_t(1));
  return 0;
})",
                                  false);
  expect(!relocation.canGenerateCode() &&
             std::any_of(relocation.diagnostics.begin(),
                         relocation.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2029"; }),
         "moving an immovable element remains rejected by the relocation "
         "contract");
  const auto borrowed = analyze(prefix + R"(
struct View {
  int& value;
  View(int& source) : value(source) {}
};
int main() {
  std::allocator allocator{};
  [[discard]] allocator.try_storage<View>(std::size_t(1));
  return 0;
})",
                                false);
  expect(!borrowed.canGenerateCode(),
         "fixed-address storage does not admit untracked borrowed elements");
  return failures ? 1 : 0;
}
