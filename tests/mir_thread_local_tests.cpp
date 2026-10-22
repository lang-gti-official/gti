#include "gti/frontend.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string_view>

namespace {
bool passed = true;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    passed = false;
  }
}
void reject(const lang::MirProgram &source, std::string_view message,
            const std::function<void(lang::MirProgram &)> &mutate,
            std::string_view diagnostic) {
  auto forged = source;
  mutate(forged);
  const auto result = lang::verifyMirProgram(forged);
  expect(!result.valid(), message);
  expect(std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const auto &error) {
                       return error.message.find(diagnostic) !=
                              std::string::npos;
                     }),
         diagnostic);
}
auto &storage(lang::MirProgram &program) {
  return const_cast<std::vector<lang::MirThreadLocalStorage> &>(
      program.threadLocalStorage());
}
} // namespace

int main() {
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-mir-thread-local/main.gti", R"(
thread_local mut int32_t counter = 0;
int32_t seed() { return 7; }
thread_local mut int32_t dynamic = seed();
int main() {
  thread_local mut int32_t local = 3;
  counter += 1;
  return counter + dynamic + local;
}
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(), "thread-local fixture passes frontend");
  if (!frontend.canGenerateCode())
    return 1;
  const auto lowered =
      lang::MirLowerer().lower(frontend.hir, frontend.failureMetadata);
  for (const auto &error : lowered.errors)
    std::cerr << error.message << '\n';
  expect(lowered.valid(), "thread-local fixture lowers and verifies");
  if (!lowered.valid())
    return 1;
  const auto &program = lowered.program;
  expect(program.threadLocalStorage().size() == 3,
         "namespace and local storage have exact separate inventory");
  expect(program.programInitializationPlan().steps.empty(),
         "thread-local state never enters eager process initialization");
  expect(std::count_if(program.threadLocalStorage().begin(),
                       program.threadLocalStorage().end(),
                       [](const auto &item) {
                         return item.dataInitializer.has_value();
                       }) == 2,
         "literal TLS has independently proved passive data initialization");
  reject(
      program, "duplicate storage identity rejected",
      [](auto &forged) { storage(forged).push_back(storage(forged).front()); },
      "thread-local inventory");
  reject(
      program, "initializer result type substitution rejected",
      [](auto &forged) {
        storage(forged).front().initializer.returnType =
            lang::SemanticType::Bool;
      },
      "thread-local inventory");
  reject(
      program, "cleanup recipe substitution rejected",
      [](auto &forged) {
        storage(forged).front().cleanup.type = lang::SemanticType::Bool;
      },
      "thread-local cleanup");
  reject(
      program, "data constant substitution rejected",
      [](auto &forged) {
        auto &items = storage(forged);
        const auto found =
            std::find_if(items.begin(), items.end(), [](const auto &item) {
              return item.dataInitializer.has_value();
            });
        found->dataInitializer = lang::Literal{std::uint64_t{99}};
      },
      "thread-local data initializer");
  reject(
      program, "missing first-use guard rejected",
      [](auto &forged) {
        for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
          auto *body = lang::findMirBody(forged, address);
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind ==
                  lang::MirInstructionKind::EnsureThreadLocal) {
                const auto id = instruction.id;
                instruction = lang::MirInstruction{.id = id, .tombstone = true};
              }
        }
      },
      "thread-local access is not dominated");
  reject(
      program, "substituted storage place type rejected",
      [](auto &forged) {
        for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
          auto *body = lang::findMirBody(forged, address);
          for (auto &place : body->places)
            if (place.root == lang::MirPlaceRootKind::Symbol &&
                place.projections.empty() &&
                forged.findThreadLocalStorage(place.symbol)) {
              place.type = lang::SemanticType::Bool;
              return;
            }
        }
      },
      "thread-local place diverges");
  reject(
      program, "substituted loan identity key rejected",
      [](auto &forged) {
        for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
          auto *body = lang::findMirBody(forged, address);
          for (auto &place : body->places)
            if (place.root == lang::MirPlaceRootKind::Symbol && place.key &&
                forged.findThreadLocalStorage(place.symbol)) {
              ++place.key->root;
              return;
            }
        }
      },
      "thread-local place diverges");
  reject(
      program, "substituted guard target rejected",
      [](auto &forged) {
        for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
          auto *body = lang::findMirBody(forged, address);
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind ==
                  lang::MirInstructionKind::EnsureThreadLocal) {
                instruction.bodyTarget = lang::MirBodyAddress{
                    .kind = lang::MirBodyKind::Function, .owner = 1};
                return;
              }
        }
      },
      "thread-local guard has no exact");
  reject(
      program, "missing shutdown drain rejected",
      [](auto &forged) {
        auto *body = lang::findMirBody(
            forged, {.kind = lang::MirBodyKind::HostedStartup,
                     .owner = forged.hostedStartupPlan()->entry});
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind ==
                lang::MirInstructionKind::DrainThreadLocals) {
              const auto id = instruction.id;
              instruction = lang::MirInstruction{.id = id, .tombstone = true};
            }
      },
      "hosted-startup");
  return passed ? 0 : 1;
}
