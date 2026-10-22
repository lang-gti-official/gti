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
  const bool expected = std::any_of(
      result.errors.begin(), result.errors.end(), [&](const auto &error) {
        return error.message.find(diagnostic) != std::string::npos;
      });
  if (!expected)
    for (const auto &error : result.errors)
      std::cerr << error.message << '\n';
  expect(expected, diagnostic);
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
      program, "missing unattached-lifetime outcome rejected",
      [](auto &forged) {
        for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
          auto *body = lang::findMirBody(forged, address);
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind ==
                  lang::MirInstructionKind::EnsureThreadLocal)
                for (auto &origin : instruction.definedFailure.localOrigins)
                  std::erase_if(origin.outcomes, [](const auto &outcome) {
                    return outcome.detail ==
                           lang::DefinedFailureDetail::
                               ThreadLocalOutsideAttachedLifetime;
                  });
        }
      },
      "thread-local guard omits initialization-state failure outcomes");
  reject(
      program, "missing hosted attachment rejected",
      [](auto &forged) {
        auto *body = lang::findMirBody(
            forged, {.kind = lang::MirBodyKind::HostedStartup,
                     .owner = forged.hostedStartupPlan()->entry});
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind ==
                lang::MirInstructionKind::BeginThreadLocals) {
              const auto id = instruction.id;
              instruction = lang::MirInstruction{.id = id, .tombstone = true};
            }
      },
      "hosted-startup");
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

  // Matching literal tokens alone do not prove data-only initialization:
  // changing an intermediate precision can round before the final widening.
  const auto precise =
      lang::Frontend().analyze("/tmp/gti-mir-thread-local/precision.gti", R"(
thread_local double exact = 16777217;
int main() { return 0; }
)");
  for (const auto &diagnostic : precise.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(precise.semanticValid && precise.hirValid,
         "precision fixture passes frontend");
  if (precise.semanticValid && precise.hirValid) {
    const auto preciseMir =
        lang::MirLowerer().lower(precise.hir, precise.failureMetadata);
    expect(preciseMir.valid(), "precision fixture lowers and verifies");
    if (preciseMir.valid())
      reject(
          preciseMir.program, "rounding intermediate data conversion rejected",
          [](auto &forged) {
            auto &body = storage(forged).front().initializer;
            lang::MirValueId literalValue = 0;
            for (auto &block : body.blocks)
              for (auto &instruction : block.instructions)
                if (instruction.operation == lang::MirOperation::Literal &&
                    instruction.result) {
                  literalValue = *instruction.result;
                  instruction.info.type = lang::SemanticType::Float;
                  body.values[literalValue - 1].info.type =
                      lang::SemanticType::Float;
                }
            expect(literalValue != 0, "precision mutation has a literal");
            for (auto &block : body.blocks)
              for (auto &instruction : block.instructions)
                for (auto &operand : instruction.operands)
                  if (operand.kind == lang::MirOperandKind::Value &&
                      operand.value == literalValue)
                    operand.type = lang::SemanticType::Float;
            for (auto &block : body.blocks)
              if (block.terminator.value &&
                  block.terminator.value->kind == lang::MirOperandKind::Value &&
                  block.terminator.value->value == literalValue)
                block.terminator.value->type = lang::SemanticType::Float;
          },
          "thread-local data initializer");
  }
  const auto pointer =
      lang::Frontend().analyze("/tmp/gti-mir-thread-local/null-pointer.gti", R"(
thread_local mut int32_t* address = nullptr;
int main() { return address == nullptr ? 0 : 1; }
)");
  for (const auto &diagnostic : pointer.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(pointer.canGenerateCode(),
         "null pointer publication is proved through its typed result");
  const auto localDynamic = lang::Frontend().analyze(
      "/tmp/gti-mir-thread-local/local-dynamic.gti", R"(
int seed() { return 7; }
int& current() { thread_local int value = seed(); return value; }
int main() { int& value = current(); return value - 7; }
)");
  for (const auto &diagnostic : localDynamic.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(localDynamic.canGenerateCode(),
         "dynamic local first-use declaration has an exact failure site");
  if (localDynamic.canGenerateCode())
    reject(
        localDynamic.mir,
        "caller witness cannot hide an unguarded callee return",
        [](auto &forged) {
          for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
            auto *body = lang::findMirBody(forged, address);
            if (body->returnType.kind != lang::SemanticType::Reference)
              continue;
            for (auto &block : body->blocks)
              for (auto &instruction : block.instructions)
                if (instruction.kind ==
                    lang::MirInstructionKind::EnsureThreadLocal) {
                  const auto id = instruction.id;
                  instruction =
                      lang::MirInstruction{.id = id, .tombstone = true};
                }
          }
        },
        "thread-local access is not dominated");
  const auto owningArray =
      lang::Frontend().analyze("/tmp/gti-mir-thread-local/owning-array.gti", R"(
class State {
  int value;
public:
  State(int value) : value(value) {}
  int read() { return this.value; }
  ~State() {}
};
thread_local State states[2] = {State(3), State(4)};
int main() { return states[0].read() + states[1].read() - 7; }
)");
  for (const auto &diagnostic : owningArray.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(owningArray.canGenerateCode(),
         "owning initializer array has exact element and result obligations");
  if (owningArray.canGenerateCode()) {
    reject(
        owningArray.mir, "missing array result publication rejected",
        [](auto &forged) {
          auto &item = storage(forged).front();
          auto &body = item.initializer;
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Lifecycle &&
                  instruction.lifecycle.size() == 1) {
                const auto &event = instruction.lifecycle.front();
                const auto *drop = body.findDropObligation(event.source);
                if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                    drop && drop->dropType.type == item.type) {
                  const auto id = instruction.id;
                  instruction =
                      lang::MirInstruction{.id = id, .tombstone = true};
                }
              }
        },
        "active drop obligation");
    reject(
        owningArray.mir, "duplicate array element publication rejected",
        [](auto &forged) {
          auto &body = storage(forged).front().initializer;
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Compute &&
                  instruction.operation == lang::MirOperation::Aggregate &&
                  instruction.lifecycle.size() >= 3)
                instruction.lifecycle[1].source =
                    instruction.lifecycle[0].source;
        },
        "transfer");
  }
  return passed ? 0 : 1;
}
