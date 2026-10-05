#include "../src/compiler/mir_verification_queries.h"
#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir.h"
#include "gti/mir_effects.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool valid, const std::string &message) {
  if (!valid) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze(root / "pointer-conversion-test.gti", source,
                                  {root / "stdlib/prelude.gti"}, {},
                                  {root / "stdlib"});
}
void show(const lang::FrontendResult &result) {
  for (const auto &d : result.diagnostics)
    std::cerr << d.code << ": " << d.message << '\n';
}
void negative(const std::string &name, const std::string &source,
              const std::string &fragment) {
  const auto result = analyze(source);
  const bool matched = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.message.find(fragment) != std::string::npos;
      });
  if (result.canGenerateCode() || !matched)
    show(result);
  expect(!result.canGenerateCode() && matched, name);
}
void positiveAndMir() {
  const auto result = analyze(R"(
#include <std/memory>
struct Item { mut int value = 42; };
using Address = void*;
using Pointer = Item*;
using ReadPointer = const Item*;
using IntPointer = int*;
T forward_exact<T, Args...>(Args... args) { return T(args...); }
T default_value<std::default_initializable T>() { return T(); }
unsafe int* member_address(Item* input) {
  Pointer restored = Pointer(Address(input));
  unsafe { return &restored->value; }
}
int main() {
  mut Item item{};
  Address erased = Address(&item);
  Pointer restored = Pointer(erased);
  Pointer forwarded = forward_exact<Pointer>(restored);
  if (forwarded != restored) return 2;
  if (default_value<Pointer>() != nullptr) return 3;
  ReadPointer readonly = ReadPointer(restored);
  const void* shared = std::erase_address(readonly);
  const Item* recovered = std::typed_address<Item>(shared);
  unsafe {
    mut Item& edit = std::assume_mut(restored, item);
    edit.value = 43;
  }
  unsafe {
    int* field = member_address(restored);
    if (*field != 43) return 1;
    return recovered->value - 43;
  }
}
)");
  if (!result.canGenerateCode())
    show(result);
  expect(result.canGenerateCode(),
         "unrelated type, qualification and source helpers compose");
  if (!result.canGenerateCode())
    return;
  unsigned conversions = 0;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Convert ||
            instruction.info.type.kind != lang::SemanticType::RawPointer)
          continue;
        ++conversions;
        expect(lang::mir_detail::rawPointerConversion(instruction),
               "MIR conversion admitted");
        const auto effects = lang::effects(instruction);
        expect(!effects.mayTrap && !effects.readsUnknownMemory &&
                   !effects.writesUnknownMemory && !effects.startsLoan &&
                   !effects.initializesValue && !effects.movesValue,
               "address conversion grants no memory, initialization or loan "
               "effects");
        for (int mutation = 0; mutation < 7; ++mutation) {
          auto forged = instruction;
          switch (mutation) {
          case 0:
            forged.operands.clear();
            break;
          case 1:
            forged.operands.front().type = lang::SemanticType::UInt64;
            break;
          case 2:
            forged.operands.front().type.pointerAccess =
                lang::AccessMode::ReadOnly;
            forged.info.type.pointerAccess = lang::AccessMode::Mutable;
            break;
          case 3:
            forged.operands.front().type.arguments = {
                lang::SemanticType::Int32};
            forged.info.type.arguments = {lang::SemanticType::UInt64};
            break;
          case 4:
            forged.loan = 1;
            break;
          case 5:
            forged.rawMemoryAccess = true;
            break;
          case 6:
            forged.info.access = lang::AccessMode::Mutable;
            break;
          }
          expect(!lang::mir_detail::rawPointerConversion(forged),
                 "independent backend gate rejects forged conversion");
          auto malformed = result.mir;
          auto &body = const_cast<lang::MirBody &>(
              malformed.findFunctionInstance(function.id)->body);
          for (auto &changedBlock : body.blocks)
            for (auto &changed : changedBlock.instructions)
              if (changed.id == instruction.id)
                changed = forged;
          expect(!lang::verifyMirProgram(malformed).errors.empty(),
                 "MIR verifier rejects forged pointer operation");
        }
      }
  expect(conversions >= 4,
         "source aliases and helpers lower to MIR conversions");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
    expect(!gti_test::emitCppText(result, standard).empty(),
           "production MIR emission supports conversion");
}
void negatives() {
  negative("cannot drop const", R"(using P = int*;
int main() { const void* p = nullptr; P q = P(p); return 0; })",
           "cannot remove pointee constness");
  negative("cannot reinterpret unrelated typed pointers",
           R"(using P = uint64_t*;
int main() { mut int value = 0; P q = P(&value); return 0; })",
           "same pointee type");
  negative("cannot convert integer to pointer", R"(using P = int*;
int main() { P q = P(42); return 0; })",
           "typed/void pointers");
  negative("cannot convert pointer to integer", R"(int main() {
int value = 0; uint64_t address = uint64_t(&value); return 0; })",
           "numeric conversions require");
  negative("no implicit erasure", R"(int main() {
mut int value = 0; void* p = &value; return 0; })",
           "Cannot initialize");
  negative("known type survives erasure",
           R"(using E = void*; using P = uint64_t*;
int main() { mut int value = 0; E e = E(&value); P p = P(e); return 0; })",
           "known pointee type");
  negative("ordinary helpers cannot hide known type", R"(
#include <std/memory>
struct Item { int value = 0; };
struct Other { int value = 0; };
int main() { mut Item value{}; void* e = std::erase_address(&value);
Other* p = std::typed_address<Other>(e); return 0; })",
           "known pointee type");
  negative("foreign typed parameter retains known type", R"(
using E = void*; using P = uint64_t*;
void test(int* address) { E e = E(address); P p = P(e); }
int main() { return 0; })",
           "known pointee type");
  negative("aggregate copy retains type-only records", R"(
using E = void*; using P = uint64_t*;
struct Carrier { void* address = nullptr; };
int main() { mut int value = 0; Carrier a{.address = E(&value)};
Carrier b = a; P p = P(b.address); return 0; })",
           "known pointee type");
  negative("conditional retains contradictory possibility", R"(
using E = void*; using P = uint64_t*;
void test(bool choose) { mut int a = 0; mut uint64_t b = 0;
E e = choose ? E(&a) : E(&b); P p = P(e); }
int main() { return 0; })",
           "known pointee type");
  negative("backedge retains contradictory pointee", R"(
using E = void*; using P = int*;
int main() { mut int a = 0; mut uint64_t b = 0; mut E e = E(&a);
for (mut int i = 0; i < 2; i++) { P p = P(e); e = E(&b); }
return 0; })",
           "later loop iteration");
  negative("known expired origin survives round trip",
           R"(using E = const void*; using P = const int*;
int main() { mut E e = nullptr; { int local = 42; e = E(&local); }
P p = P(e); unsafe { return *p; } })",
           "scope has ended");
  negative("round trip grants no access",
           R"(using E = const void*; using P = const int*;
int main() { int value = 42; E e = E(&value); P p = P(e); return *p; })",
           "requires an unsafe block");
  negative("wrong arity diagnosed", R"(using E = void*;
int main() { E e = E(); return 0; })",
           "exactly one value argument");
}
} // namespace
int main() {
  positiveAndMir();
  negatives();
  return failures == 0 ? 0 : 1;
}
