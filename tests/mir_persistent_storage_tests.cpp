#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
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

lang::FrontendResult fixture(std::string_view storage) {
  return lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/main.gti",
                                  std::string(storage) +
                                      R"( mut int32_t first = 1;
)" + std::string(storage) + R"( mut int32_t second = 2;
void change() { second = 9; }
void relay() { change(); }
int main() {
  int32_t& borrowed = first;
  relay();
  return borrowed;
}
)");
}

lang::SymbolId symbol(const lang::FrontendResult &frontend,
                      std::string_view name) {
  for (const auto &record : frontend.semantics.database().symbols())
    if (record.name == name)
      return record.id;
  return 0;
}

void independentHiddenEffects(std::string_view storage) {
  const auto frontend = fixture(storage);
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
    if (address.kind != lang::MirBodyKind::Function)
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
} // namespace

int main() {
  independentHiddenEffects("thread_local");
  independentHiddenEffects("");
  nonLexicalAndFieldBoundaries();
  return passed ? 0 : 1;
}
