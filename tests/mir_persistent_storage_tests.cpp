#include "../src/compiler/mir_storage_effects.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
bool passed = true;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    passed = false;
  }
}

lang::FrontendResult fixture(std::string_view storage, bool cleanup) {
  const std::string declarations =
      std::string(storage) + " mut int32_t first = 1;\n" +
      std::string(storage) + " mut int32_t second = 2;\n";
  const std::string source = cleanup ? R"(
class Worker {
public:
  Worker() = default;
  ~Worker() { second = 9; }
};
int main() {
  int32_t& borrowed = first;
  { Worker task{}; }
  return borrowed;
}
)"
                                     : R"(
void change() { second = 9; }
void relay() { change(); }
int main() {
  int32_t& borrowed = first;
  relay();
  return borrowed;
}
)";
  return lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/main.gti",
                                  declarations + source);
}

lang::SymbolId symbol(const lang::FrontendResult &frontend,
                      std::string_view name) {
  for (const auto &record : frontend.semantics.database().symbols())
    if (record.name == name)
      return record.id;
  return 0;
}

void independentHiddenEffects(std::string_view storage, bool cleanup) {
  const auto frontend = fixture(storage, cleanup);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "a live persistent borrow permits disjoint transitive mutation");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "unmodified disjoint persistent effects independently verify");
  const auto first = symbol(frontend, "first");
  const auto second = symbol(frontend, "second");
  expect(first != 0 && second != 0 && first != second,
         "fixture owns distinct structural storage identities");
  if (!first || !second)
    return;
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    if (address.kind != lang::MirBodyKind::Function &&
        address.kind != lang::MirBodyKind::Destructor)
      continue;
    auto *body = lang::findMirBody(forged, address);
    bool mutated = false;
    for (auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == second) {
        place.symbol = first;
        if (place.key && place.key->root == second)
          place.key->root = first;
        changed = mutated = true;
      }
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == second) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
    if (mutated)
      expect(lang::verifyMirBody(*body, address.owner).valid(),
             "callee mutation is body-local-valid; program proof must catch "
             "alias");
  }
  expect(changed, "mutation reaches executable callee storage places");
  const auto proof = lang::verifyMirProgram(forged);
  expect(!proof.valid(),
         "independent MIR proof rejects a substituted hidden transitive write");
  expect(std::any_of(proof.errors.begin(), proof.errors.end(),
                     [](const auto &error) {
                       return error.message.find(
                                  "ambient source call or cleanup") !=
                              std::string::npos;
                     }),
         "rejection comes from exact ambient access versus live loan proof");
}

void nonLexicalAndFieldBoundaries() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/endpoints.gti", R"(
class Counters {
public:
  mut int32_t left = 1;
  mut int32_t right = 2;
};
thread_local mut Counters state{};
void change_right() { state.right = 3; }
void change_left() { state.left = 4; }
int main() {
  int32_t& borrowed = state.left;
  change_right();
  int32_t copied = borrowed;
  change_left();
  return copied;
}
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "field-disjoint calls and a later write after last use are accepted");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "MIR hidden effects preserve field projections and EndBorrow");
}
void readySlotThroughNestedCalls() {
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/ready.gti", R"(
thread_local mut int32_t first = 1;
int32_t initialize() { first = 7; return 2; }
thread_local int32_t second = initialize();
int32_t read_second() { return second; }
int32_t relay() { return read_second(); }
int main() {
  int32_t& ready = second;
  int32_t& protected_first = first;
  int32_t copied = relay();
  return ready + protected_first + copied;
}
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "an existing exact TLS loan proves initialization completed through "
         "nested helpers");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "MIR readiness retains the conditional initializer edge, not a "
           "blanket call exemption");
}
void stagedInputBoundaries(std::string_view storage, bool branch) {
  const std::string source = std::string(storage) + R"( mut int32_t first = 1;
)" + std::string(storage) + R"( mut int32_t second = 2;
int32_t change() { second = 9; return 3; }
int32_t read(int32_t& input, int32_t copied) { return input + copied; }
int main() { return read(first, )" +
                             (branch ? "true ? change() : 0" : "change()") +
                             R"(); }
)";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/staged.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "later input may mutate storage disjoint from an earlier binding");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "independent staged-input proof accepts disjoint later arguments");
  auto forged = frontend.mir;
  const auto first = symbol(frontend, "first");
  const auto second = symbol(frontend, "second");
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    auto *body = lang::findMirBody(forged, address);
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    for (auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == second) {
        place.symbol = first;
        if (place.key && place.key->root == second)
          place.key->root = first;
        changed = true;
      }
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == second) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
  }
  expect(changed, "staged-input fixture has an independently mutable callee");
  const auto proof = lang::verifyMirProgram(forged);
  expect(!proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find("persistent call input") !=
                                  std::string::npos;
                         }),
         "independent proof rejects hidden invalidation after input binding");
}
void persistentTransferIsIndependentlyRejected() {
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/move.gti", R"(
thread_local mut int32_t value = 1;
int main() { return value; }
)");
  expect(frontend.canGenerateCode(), "persistent observation fixture is valid");
  if (!frontend.canGenerateCode())
    return;
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    auto *body = lang::findMirBody(forged, address);
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Load ||
            instruction.operands.size() != 1)
          continue;
        auto &operand = instruction.operands.front();
        const auto *place = body->findPlace(operand.place);
        if (operand.kind == lang::MirOperandKind::Copy && place &&
            place->root == lang::MirPlaceRootKind::Symbol &&
            place->symbol == symbol(frontend, "value")) {
          instruction.kind = lang::MirInstructionKind::Move;
          operand.kind = lang::MirOperandKind::Move;
          changed = true;
        }
      }
  }
  expect(changed, "malformed fixture changes an actual persistent load");
  expect(!lang::verifyMirProgram(forged).valid(),
         "program verification rejects the malformed persistent move");
  const auto proof = lang::verifyMirPersistentStorageEffects(forged);
  expect(!proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "destructively transferred") !=
                                  std::string::npos;
                         }),
         "independent storage proof rejects an unrepresented destructive "
         "transfer");
}
void stagedBorrowMayPublishItsOriginalLifetime() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/staged-return.gti", R"(
thread_local mut int32_t value = 1;
int32_t& retain(int32_t& input, int32_t copied) { return input; }
int main() { int32_t& result = retain(value, 0); return result; }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "an ordered persistent input may publish its actual storage lifetime");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "input protection does not invent a shorter-lived result parent");
}
void nativeBoundaryIsNotAnUnknownSourceBody() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/native-input.gti", R"(
extern "C" { int32_t number(); }
thread_local mut int32_t value = 1;
void write(mut int32_t& target, int32_t copied) { target = copied; }
int main() { write(value, number()); return value; }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "declared native inputs retain their foreign-boundary obligations");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "a scalar native input is not an arbitrary hidden GTI storage write");
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto &instance : forged.functionInstances()) {
    auto &function = const_cast<lang::MirFunctionInstance &>(instance);
    if (function.linkage == lang::LanguageLinkage::C &&
        function.definitionKind != lang::MirDefinitionKind::Source) {
      function.linkage = lang::LanguageLinkage::Gti;
      function.externalSymbol.clear();
      changed = true;
    }
  }
  expect(changed, "fixture owns a declared native target");
  const auto proof = lang::verifyMirPersistentStorageEffects(forged);
  expect(!proof.valid(),
         "removing the native boundary cannot authorize an unknown GTI effect");
}
void exactRestorationRemainsAvailable() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/restore.gti", R"(
#include <std/utility>
thread_local mut int value = 1;
int main() { int previous = std::exchange(value, 2); return previous + value; }
)",
      {library / "prelude.gti"}, {}, {library});
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "persistent storage may use the existing exact restoration operation");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "restoration proof is not confused with an unrepresented raw Move");
}
} // namespace

int main() {
  for (const bool cleanup : {false, true}) {
    independentHiddenEffects("thread_local", cleanup);
    independentHiddenEffects("", cleanup);
  }
  nonLexicalAndFieldBoundaries();
  readySlotThroughNestedCalls();
  for (const auto storage : {"thread_local", ""})
    for (const bool branch : {false, true})
      stagedInputBoundaries(storage, branch);
  persistentTransferIsIndependentlyRejected();
  stagedBorrowMayPublishItsOriginalLifetime();
  nativeBoundaryIsNotAnUnknownSourceBody();
  exactRestorationRemainsAvailable();
  return passed ? 0 : 1;
}
