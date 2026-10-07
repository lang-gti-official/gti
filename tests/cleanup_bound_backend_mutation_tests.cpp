#include "../src/compiler/cpp_mir_body_emitter.h"
#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
int mutations = 0;
void expect(bool condition, std::string_view name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

const std::string prefix = R"gti(
#include <std/utility>
mut int observed = 0;
struct Resource { int value = 7; };
class Guard {
  Resource& resource;
public:
  Guard(Resource& resource) : resource(resource) {}
  ~Guard() { observed = this.resource.value; }
  Resource& read() { return this.resource; }
};
)gti";

lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze(
      root / "cleanup-bound-backend.gti", prefix + source,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
}

lang::MirConstructorInstance *ownedConstructor(lang::MirProgram &program,
                                               std::size_t fields) {
  for (auto &constructor :
       const_cast<std::vector<lang::MirConstructorInstance> &>(
           program.constructorInstances()))
    if (std::count_if(constructor.initializers.begin(),
                      constructor.initializers.end(),
                      [](const auto &initializer) {
                        return initializer.ownedParameter.has_value();
                      }) == static_cast<std::ptrdiff_t>(fields))
      return &constructor;
  return nullptr;
}

lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &item : block.instructions)
      if (item.id == id)
        return &item;
  return nullptr;
}

template <typename Mutation>
void rejected(const lang::MirProgram &source, std::size_t fields,
              Mutation mutate, std::string_view name) {
  auto program = source;
  auto *constructor = ownedConstructor(program, fields);
  expect(constructor != nullptr, "mutation locates owned constructor");
  if (!constructor)
    return;
  const auto bindings =
      lang::cppMirOwnedParameterFieldBindings(program, *constructor);
  expect(bindings && bindings->size() == fields,
         "positive exact backend binding");
  if (!bindings || bindings->size() != fields)
    return;
  expect(mutate(*constructor, *bindings), "mutation changes intended schedule");
  // Exercise the detached backend admission proof itself, independently of
  // earlier MIR diagnostics. The test does not ask emission to repair IR.
  expect(!lang::cppMirOwnedParameterFieldBindings(program, *constructor), name);
  ++mutations;
}

void constructedFieldPrefix() {
  const auto control = analyze(R"gti(
class ActiveGuard {
  Resource& resource;
public:
  ActiveGuard(Resource& resource) : resource(resource) {}
  ~ActiveGuard() { observed += this.resource.value; }
};
class Reversed {
  ActiveGuard retained;
public:
  Reversed(Resource& first, Resource& second, int divisor)
      : retained(ActiveGuard(second)) { int checked = 1 / divisor; }
};
int main() { Resource first{}; Resource second{}; Reversed value{first, second, 1}; return 0; }
)gti");
  expect(control.canGenerateCode(),
         "constructed retained field control compiles");
  if (!control.canGenerateCode())
    return;
  lang::HirConstructorInstanceId selected = 0;
  for (const auto &constructor : control.mir.constructorInstances())
    if (constructor.parameterTypes.size() == 3)
      selected = constructor.id;
  const auto *constructor = control.mir.findConstructorInstance(selected);
  expect(constructor != nullptr, "constructed retained field target found");
  if (constructor == nullptr)
    return;
  const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
      control.mir, *constructor);
  expect(bindings.size() == 1,
         "constructed retained field has exact native prefix");
  if (bindings.size() != 1 && std::getenv("GTI_TEST_DUMP_MIR") != nullptr)
    std::cerr << lang::MirPrinter().print(constructor->body);
  if (bindings.size() != 1)
    return;
  expect(bindings.front().arguments.size() == 1 &&
             bindings.front().arguments.front().parameter == 1,
         "constructed retained field preserves reordered reference argument");
  expect(!gti_test::emitCppText(control).empty(),
         "constructed retained field emits");
  const auto reject = [&](const auto &mutate, std::string_view name) {
    auto program = control.mir;
    auto &changed = const_cast<lang::MirConstructorInstance &>(
        *program.findConstructorInstance(selected));
    expect(mutate(changed, bindings.front()),
           "constructed prefix mutation applied");
    expect(lang::cppMirConstructedFieldInitializerBindings(program, changed)
               .empty(),
           name);
    ++mutations;
  };
  reject(
      [](auto &target, const auto &binding) {
        auto *initialize =
            instruction(target.body, binding.initializeInstruction);
        initialize->lifecycle.front().target = binding.valueDrop;
        return true;
      },
      "constructed prefix cannot reparent back into its temporary drop");
  reject(
      [](auto &target, const auto &binding) {
        auto *construct =
            instruction(target.body, binding.constructInstruction);
        construct->successResultDrop = binding.fieldDrop;
        return true;
      },
      "constructed prefix cannot activate the field before publication");
  reject(
      [](auto &target, const auto &binding) {
        auto *input =
            instruction(target.body, binding.argumentInstructions.front());
        input->callInputIndex = 1;
        return true;
      },
      "constructed prefix cannot forge selected child argument position");
  reject(
      [](auto &target, const auto &binding) {
        auto *input =
            instruction(target.body, binding.argumentInstructions.front());
        if (!input->ownership)
          return false;
        input->ownership->kind = lang::OwnershipEventKind::Move;
        return true;
      },
      "constructed prefix cannot disguise an ownership transfer as a reference "
      "read");
  reject(
      [](auto &target, const auto &binding) {
        auto *construct =
            instruction(target.body, binding.constructInstruction);
        construct->constructorTarget = target.id;
        return true;
      },
      "constructed prefix cannot substitute an unrelated constructor target");
  reject(
      [](auto &target, const auto &binding) {
        auto *initialize =
            instruction(target.body, binding.initializeInstruction);
        initialize->operands.front().value = 1;
        return true;
      },
      "constructed prefix cannot publish a different SSA value");
  reject(
      [](auto &target, const auto &binding) {
        for (auto &block : target.body.blocks)
          for (auto &item : block.instructions)
            if (item.kind == lang::MirInstructionKind::Borrow &&
                item.operands.size() == 1 &&
                item.operands.front().value == binding.constructedValue) {
              item.operands.front().loan = 0;
              return true;
            }
        return false;
      },
      "constructed prefix cannot lose its exact dependency publication");
  reject(
      [](auto &target, const auto &) {
        auto &entry = target.body.blocks[target.body.entry - 1];
        entry.terminator.successLifecycle.front().target = 0;
        return true;
      },
      "constructed prefix cannot lose success-edge cleanup activation");
  reject(
      [](auto &target, const auto &) {
        auto &entry = target.body.blocks[target.body.entry - 1];
        for (auto &block : target.body.blocks)
          for (const auto &item : block.instructions)
            if (!item.localFailureSites.empty()) {
              entry.instructions.insert(entry.instructions.begin(), item);
              return true;
            }
        return false;
      },
      "constructed prefix cannot hoist before earlier fallible work");
}
} // namespace

int main() {
  constructedFieldPrefix();
  auto control = analyze(R"gti(
class Box {
  Guard item;
public:
  Box(Guard item, int divisor) : item(std::move(item)) {
    int checked = 7 / divisor;
  }
};
int main() { Resource resource{}; Box box{Guard(resource), 1}; return 0; }
)gti");
  for (const auto &diagnostic : control.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(control.canGenerateCode(), "retained-shell rollback control compiles");
  if (!control.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(control.mir).valid(), "positive MIR verifies");
  const auto cpp = gti_test::emitCppText(control);
  auto *constructor = ownedConstructor(control.mir, 1);
  expect(constructor != nullptr, "positive locates retained-field constructor");
  if (constructor) {
    const auto bindings =
        lang::cppMirOwnedParameterFieldBindings(control.mir, *constructor);
    expect(bindings && bindings->size() == 1,
           "positive locates exact retained-field binding");
    if (bindings && bindings->size() == 1) {
      const auto *initialize = instruction(
          constructor->body, bindings->front().initializeInstruction);
      expect(initialize && initialize->destination,
             "positive locates retained-field place");
      if (initialize && initialize->destination) {
        const auto field =
            "__gti_mir_p_" + std::to_string(*initialize->destination);
        expect(cpp.find("; }(" + field + "); // GTI MIR field rollback") !=
                       std::string::npos &&
                   cpp.find("mir_rollback_field_v1(" + field + ")") ==
                       std::string::npos,
               "positive backend drains exact retained field without default "
               "construction");
      }
    }
  }

  rejected(
      control.mir, 1,
      [](auto &constructor, const auto &bindings) {
        auto &entry = constructor.body.blocks[constructor.body.entry - 1];
        const auto failure = std::find_if(
            entry.instructions.begin(), entry.instructions.end(),
            [](const auto &i) { return !i.localFailureSites.empty(); });
        if (failure == entry.instructions.end())
          return false;
        auto moved = *failure;
        entry.instructions.erase(failure);
        entry.instructions.insert(entry.instructions.begin(), std::move(moved));
        return entry.instructions.front().id !=
               bindings.front().moveInstruction;
      },
      "fallible computation cannot move ahead of native field transfer");

  rejected(
      control.mir, 1,
      [](auto &constructor, const auto &bindings) {
        auto *stage = instruction(constructor.body,
                                  bindings.front().initializeInstruction);
        if (!stage || stage->lifecycle.size() != 1)
          return false;
        stage->lifecycle.front().target = bindings.front().parameterDrop;
        return true;
      },
      "field publication cannot reparent into the formal's drop");

  rejected(
      control.mir, 1,
      [](auto &constructor, const auto &bindings) {
        for (auto &block : constructor.body.blocks)
          for (auto &item : block.instructions)
            if (item.kind == lang::MirInstructionKind::Drop &&
                item.lifecycle.size() == 1 &&
                item.lifecycle.front().failureCleanup &&
                item.lifecycle.front().source == bindings.front().fieldDrop) {
              item.destination = bindings.front().sourcePlace;
              return true;
            }
        return false;
      },
      "rollback field cleanup cannot target the same-typed moved formal");

  rejected(
      control.mir, 1,
      [](auto &constructor, const auto &bindings) {
        for (auto &block : constructor.body.blocks)
          for (auto &item : block.instructions)
            if (item.kind == lang::MirInstructionKind::Drop &&
                item.lifecycle.size() == 1 &&
                item.lifecycle.front().failureCleanup &&
                item.lifecycle.front().source == bindings.front().fieldDrop) {
              item.lifecycle.front().source = bindings.front().valueDrop;
              return true;
            }
        return false;
      },
      "rollback cannot retire the already-transferred temporary");

  auto pair = analyze(R"gti(
class Pair {
  Guard first;
  Guard second;
public:
  Pair(Guard first, Guard second)
      : first(std::move(first)), second(std::move(second)) {}
};
int main() { Resource resource{}; Pair pair{Guard(resource), Guard(resource)}; return 0; }
)gti");
  expect(pair.canGenerateCode(), "two same-typed field control compiles");
  if (pair.canGenerateCode()) {
    expect(!gti_test::emitCppText(pair).empty(), "two-field control emits");
    rejected(
        pair.mir, 2,
        [](auto &constructor, const auto &bindings) {
          auto *first =
              instruction(constructor.body, bindings[0].initializeInstruction);
          const auto *second =
              instruction(constructor.body, bindings[1].initializeInstruction);
          if (!first || !second)
            return false;
          first->destination = second->destination;
          return true;
        },
        "same-typed field identity cannot be exchanged");
    rejected(
        pair.mir, 2,
        [](auto &constructor, const auto &bindings) {
          auto *first =
              instruction(constructor.body, bindings[0].initializeInstruction);
          if (!first || first->lifecycle.size() != 1)
            return false;
          first->lifecycle.front().target = bindings[1].fieldDrop;
          return true;
        },
        "same-typed rollback identity cannot be exchanged");
  }

  auto generic = analyze(R"gti(
class ReadView {
  int& item;
public:
  ReadView(int& item) : item(item) {}
};
class GenericGuard<T> {
  T item;
public:
  GenericGuard(T item) : item(std::move(item)) {}
  ~GenericGuard() { observed = 1; }
};
int main() { int value = 7; GenericGuard<ReadView> guard{ReadView(value)}; return 0; }
)gti");
  expect(generic.canGenerateCode(),
         "concrete nonassignable generic guard control compiles");
  if (generic.canGenerateCode()) {
    expect(!gti_test::emitCppText(generic).empty(),
           "nonassignable generic guard emits");
    const auto found = std::find_if(
        generic.mir.classInstances().begin(),
        generic.mir.classInstances().end(), [](const auto &instance) {
          return !instance.type.arguments.empty() &&
                 instance.lifecycleBoundBorrowCarrier &&
                 !instance.traits.moveAssignable &&
                 !instance.traits.copyAssignable;
        });
    expect(found != generic.mir.classInstances().end(),
           "control locates concrete generic carrier");
    if (found != generic.mir.classInstances().end()) {
      expect(found->moveAssignment == lang::SpecialMemberStatus::Deleted &&
                 found->copyAssignment == lang::SpecialMemberStatus::Deleted,
             "concrete traits delete open-generic generated assignments");
      for (const bool move : {false, true}) {
        auto forged = generic.mir;
        auto &instance = const_cast<lang::MirClassInstance &>(
            *forged.findClassInstance(found->id));
        (move ? instance.moveAssignment : instance.copyAssignment) =
            lang::SpecialMemberStatus::Generated;
        const auto verification = lang::verifyMirProgram(forged);
        expect(std::any_of(verification.errors.begin(),
                           verification.errors.end(),
                           [](const auto &error) {
                             return error.message ==
                                    "generated class special member "
                                    "contradicts its concrete semantic traits";
                           }),
               "forged generated assignment contradicting concrete traits is "
               "rejected");
        ++mutations;
      }
    }
  }

  const auto external = analyze(R"gti(
Resource& external(Guard& guard) { return guard.read(); }
Guard& externalShell(Guard& guard) { return guard; }
Guard acquire(Resource& resource) {
  Guard guard{resource};
  return std::move(guard);
}
Guard forward(Guard guard) { return std::move(guard); }
Guard implicit(Resource& resource) { Guard guard{resource}; return guard; }
class Outer {
  Guard guard;
public:
  Outer(Guard guard) : guard(std::move(guard)) {}
  ~Outer() { observed = 1; }
};
Outer outer(Resource& resource) {
  Guard guard{resource};
  Outer result{std::move(guard)};
  return std::move(result);
}
void nestedParents(mut Resource& resource, Guard& unrelated) {
  Guard guard{resource};
  Outer result{std::move(guard)};
  int keep = unrelated.read().value;
}
int main() {
  Resource resource{};
  auto guard = acquire(resource);
  int read = external(guard).value;
  auto moved = forward(std::move(guard));
  return read - 7;
}
)gti");
  if (!external.canGenerateCode())
    for (const auto &item : external.diagnostics)
      std::cerr << item.code << ": " << item.message << '\n';
  if (std::getenv("GTI_TEST_DUMP_MIR") != nullptr)
    for (const auto &error : lang::verifyMirProgram(external.mir).errors)
      if (error.bodyKind == lang::MirBodyKind::Function)
        if (const auto *function =
                external.mir.findFunctionInstance(error.owner))
          std::cerr << "failed function " << error.owner << '\n'
                    << lang::MirPrinter().print(function->body);
  expect(
      external.canGenerateCode(),
      "external guard reference and complete carrier transfers remain valid");

  if (external.canGenerateCode()) {
    bool checkedParent = false;
    for (const auto &function : external.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        if (!loan.lifecycleBound || loan.kind != lang::MirLoanKind::Stored ||
            loan.entry || loan.escapes || loan.parent == 0)
          continue;
        const bool nestedProducer = std::any_of(
            function.body.blocks.begin(), function.body.blocks.end(),
            [&](const auto &block) {
              return std::any_of(
                  block.instructions.begin(), block.instructions.end(),
                  [&](const auto &item) {
                    return item.loan == loan.id &&
                           item.kind == lang::MirInstructionKind::Construct &&
                           std::any_of(
                               item.parameterTypes.begin(),
                               item.parameterTypes.end(), [](const auto &type) {
                                 return type.kind == lang::SemanticType::Class;
                               });
                  });
            });
        const auto unrelated = std::find_if(
            function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &candidate) {
              return candidate.kind == lang::MirLoanKind::Parameter &&
                     candidate.entry &&
                     candidate.access == lang::AccessMode::ReadOnly &&
                     candidate.id != loan.parent;
            });
        if (!nestedProducer || unrelated == function.body.loans.end())
          continue;
        auto wrongParent = external.mir;
        auto &changed = const_cast<lang::MirFunctionInstance &>(
                            *wrongParent.findFunctionInstance(function.id))
                            .body;
        changed.loans[loan.id - 1].parent = unrelated->id;
        expect(!lang::verifyMirProgram(wrongParent).valid(),
               "nested owned transfer cannot adopt another shared parameter "
               "parent");
        checkedParent = true;
        ++mutations;
      }
    }
    expect(checkedParent,
           "nested control exposes distinct live parameter ancestry");
    for (const auto &function : external.mir.functionInstances())
      for (const auto &loan : function.body.loans)
        if (loan.kind == lang::MirLoanKind::Return && loan.lifecycleBound) {
          auto missing = external.mir;
          auto &changed = const_cast<lang::MirFunctionInstance &>(
              *missing.findFunctionInstance(function.id));
          changed.body.loans[loan.id - 1].lifecycleBound = false;
          expect(!lang::verifyMirProgram(missing).valid(),
                 "returned carrier cannot erase only its own retention marker");
          ++mutations;
        }
    auto forged = external.mir;
    bool moved = false;
    lang::HirFunctionInstanceId changedFunction = 0;
    lang::MirLoanId changedLoan = 0;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             forged.functionInstances())) {
      if (moved)
        break;
      auto &body = function.body;
      for (const auto &loan : body.loans) {
        if (moved || loan.kind != lang::MirLoanKind::Return ||
            !loan.lifecycleBound)
          continue;
        lang::MirBlockId dropBlock = 0;
        lang::MirInstructionId dropId = 0;
        for (const auto &block : body.blocks)
          for (const auto &item : block.instructions)
            if (block.activeFailure != 0 &&
                item.kind == lang::MirInstructionKind::Drop &&
                std::any_of(
                    item.lifecycle.begin(), item.lifecycle.end(),
                    [&](const auto &event) {
                      const auto *drop = body.findDropObligation(event.source);
                      return event.kind == lang::MirLifecycleEventKind::Drop &&
                             event.failureCleanup && drop != nullptr &&
                             drop->kind ==
                                 lang::MirDropObligationKind::ReturnResult &&
                             drop->value == loan.producedBy;
                    })) {
              dropBlock = block.id;
              dropId = item.id;
            }
        if (dropBlock == 0)
          continue;
        std::optional<lang::MirInstruction> end;
        for (auto &block : body.blocks) {
          if (block.activeFailure == 0)
            continue;
          const auto found = std::find_if(
              block.instructions.begin(), block.instructions.end(),
              [&](const auto &item) {
                return item.kind == lang::MirInstructionKind::EndBorrow &&
                       item.loan == loan.id;
              });
          if (found != block.instructions.end()) {
            end = *found;
            block.instructions.erase(found);
            break;
          }
        }
        if (!end)
          continue;
        auto &instructions = body.findBlock(dropBlock)->instructions;
        const auto drop =
            std::find_if(instructions.begin(), instructions.end(),
                         [&](const auto &item) { return item.id == dropId; });
        instructions.insert(drop, std::move(*end));
        moved = true;
        changedFunction = function.id;
        changedLoan = loan.id;
      }
    }
    expect(moved,
           "mutation moves pending-result loan end ahead of exact cleanup");
    if (moved) {
      const auto verification = lang::verifyMirProgram(forged);
      expect(std::any_of(verification.errors.begin(), verification.errors.end(),
                         [](const auto &error) {
                           return error.message ==
                                  "cleanup-bound return dependency ends before "
                                  "its exact pending result drop";
                         }),
             "returned dependency cannot end before failed-return cleanup");
      ++mutations;
      auto &changed = const_cast<lang::MirFunctionInstance &>(
                          *forged.findFunctionInstance(changedFunction))
                          .body;
      changed.loans[changedLoan - 1].lifecycleBound = false;
      expect(!lang::verifyMirProgram(forged).valid(),
             "clearing only return marker cannot authorize premature "
             "pending-result retirement");
      ++mutations;

      // A moved-from binding drop is not destruction of the moved value.
      // Keep the publication/source-end pair adjacent, but place it after that
      // inert source-shell drop: the return marker must remain mandatory even
      // if a predecessor check also sees an earlier same-chain Drop.
      bool reordered = false;
      for (auto &block : changed.blocks) {
        if (reordered)
          break;
        const auto publication = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &item) {
              return item.kind == lang::MirInstructionKind::Borrow &&
                     item.loan == changedLoan && item.operands.size() == 1 &&
                     item.operands.front().kind == lang::MirOperandKind::Loan;
            });
        if (publication == block.instructions.end() ||
            std::next(publication) == block.instructions.end())
          continue;
        const auto *source =
            changed.findLoan(publication->operands.front().loan);
        if (!source ||
            std::next(publication)->kind !=
                lang::MirInstructionKind::EndBorrow ||
            std::next(publication)->loan != source->id)
          continue;
        const auto shellDrop = std::find_if(
            std::next(publication, 2), block.instructions.end(),
            [&](const auto &item) {
              const auto *place = item.destination
                                      ? changed.findPlace(*item.destination)
                                      : nullptr;
              return item.kind == lang::MirInstructionKind::Drop && place &&
                     place->root == lang::MirPlaceRootKind::Binding &&
                     std::find(source->carriers.begin(), source->carriers.end(),
                               place->binding) != source->carriers.end();
            });
        if (shellDrop == block.instructions.end())
          continue;
        const auto dropId = shellDrop->id;
        auto borrow = *publication;
        auto end = *std::next(publication);
        block.instructions.erase(publication, std::next(publication, 2));
        const auto afterDrop = std::next(
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const auto &item) { return item.id == dropId; }));
        const auto index = std::distance(block.instructions.begin(), afterDrop);
        block.instructions.insert(afterDrop, std::move(borrow));
        block.instructions.insert(block.instructions.begin() + index + 1,
                                  std::move(end));
        reordered = true;
      }
      expect(reordered,
             "marker mutation includes an inert moved-from shell drop");
      if (reordered) {
        expect(!lang::verifyMirProgram(forged).valid(),
               "inert shell cleanup cannot hide unmarked premature "
               "returned-loan end");
        ++mutations;
      }
    }
  }

  const std::string passive = R"gti(
class View {
  Resource& resource;
public:
  View(Resource& resource) : resource(resource) {}
  Resource& read() { return this.resource; }
};
class GuardRef {
  Guard& guard;
public:
  GuardRef(Guard& guard) : guard(guard) {}
  ~GuardRef() { observed = 1; }
};
)gti";
  const std::vector<std::string> escapingCases{
      "Resource& escape(Resource& resource) { Guard guard{resource}; return "
      "guard.read(); }",
      "Resource& escape(Resource& resource) { Guard guard{resource}; Resource& "
      "child = guard.read(); return child; }",
      "Resource& escape(Resource& resource) { Guard guard{resource}; View "
      "view{guard.read()}; return view.read(); }",
      "View escape(Resource& resource) { Guard guard{resource}; View "
      "view{guard.read()}; return view; }",
      "Resource& escape(Guard guard) { return guard.read(); }",
      "Resource& escape(Guard guard) { View view{guard.read()}; return "
      "view.read(); }",
      "GuardRef escape(Resource& resource) { Guard inner{resource}; GuardRef "
      "outer{inner}; return std::move(outer); }"};
  for (const auto &body : escapingCases) {
    const std::string escaping =
        passive + body + "\nint main() { return 0; }\n";
    const auto invalid = analyze(escaping);
    const auto diagnostic =
        std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                     [](const auto &item) {
                       return item.code == "GTI-S2017" &&
                              item.message.find("cleanup-bound carrier") !=
                                  std::string::npos;
                     });
    expect(!invalid.canGenerateCode() &&
               diagnostic != invalid.diagnostics.end(),
           "local or owned-parameter cleanup-bound child cannot escape");
    if (diagnostic != invalid.diagnostics.end()) {
      expect(diagnostic->phase == lang::DiagnosticPhase::Semantics &&
                 diagnostic->severity == lang::DiagnosticSeverity::Error &&
                 diagnostic->message.find("mutex") == std::string::npos &&
                 !diagnostic->related.empty() && !diagnostic->hints.empty() &&
                 diagnostic->fixes.empty(),
             "general cleanup diagnostic has actionable non-lock wording");
    }
  }
  std::cout << "cleanup-bound backend: " << mutations
            << " rejected mutations\n";
  return failures ? 1 : 0;
}
