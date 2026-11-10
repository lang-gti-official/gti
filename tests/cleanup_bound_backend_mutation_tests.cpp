#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "llvm_backend_test_support.h"

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
import std.utility;
mut int observed = 0;
struct Resource { int value = 7;  public: Resource() = default; };
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

// The shipped backend accepts the verified program; reports why it did not.
bool emitsNative(const lang::FrontendResult &frontend) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  if (!emitted)
    std::cerr << "native emission: " << error << '\n';
  return emitted;
}

// One constructed field child of a constructor, located from MIR alone: the
// Invoke-terminated Construct of the child, its result value, the published
// field, and the child's first staged argument.
struct ChildConstruction {
  lang::MirInstructionId constructInstruction = 0;
  lang::MirValueId constructedValue = 0;
  lang::SymbolId field = 0;
  lang::MirInstructionId firstArgument = 0;
};

std::vector<ChildConstruction>
childConstructions(const lang::MirConstructorInstance &constructor) {
  std::vector<ChildConstruction> children;
  for (const auto &block : constructor.body.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
      continue;
    for (const auto &item : block.instructions) {
      if (item.id != block.terminator.invokeInstruction ||
          item.kind != lang::MirInstructionKind::Construct || !item.result ||
          !item.constructorTarget)
        continue;
      ChildConstruction child{.constructInstruction = item.id,
                              .constructedValue = *item.result};
      for (const auto &stageBlock : constructor.body.blocks)
        for (const auto &stage : stageBlock.instructions)
          if (child.firstArgument == 0 &&
              stage.kind == lang::MirInstructionKind::CallInput &&
              stage.callSite == item.callSite && stage.callInputIndex == 0)
            child.firstArgument = stage.id;
      children.push_back(child);
    }
  }
  for (std::size_t index = 0;
       index < children.size() && index < constructor.initializers.size();
       ++index)
    children[index].field = constructor.initializers[index].field;
  return children;
}

lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &item : block.instructions)
      if (item.id == id)
        return &item;
  return nullptr;
}

void constructedFieldSequence() {
  const std::string source = R"gti(
class Handle {
  Resource& resource;
public:
  Handle(Resource& resource) : resource(resource) {}
  Resource& read() { return this.resource; }
};
class SequenceGuard {
  Handle handle;
public:
  SequenceGuard(Handle handle) : handle(handle) {}
  ~SequenceGuard() { observed += this.handle.read().value; }
};
class SequenceOwner {
  SequenceGuard first;
  SequenceGuard second;
  SequenceGuard third;
public:
  SequenceOwner(Handle handle, int divisor)
      : first(SequenceGuard(handle)), second(SequenceGuard(handle)),
        third(SequenceGuard(handle)) { int checked = 1 / divisor; }
};
int main() { Resource resource{}; SequenceOwner owner{Handle(resource), 1}; return 0; }
)gti";
  const auto control = analyze(source);
  expect(control.canGenerateCode(), "borrowed-field sequence control compiles");
  if (!control.canGenerateCode()) {
    for (const auto &diagnostic : control.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(control.mir).valid(),
         "borrowed-field sequence control verifies");
  lang::HirConstructorInstanceId selected = 0;
  for (const auto &constructor : control.mir.constructorInstances())
    if (constructor.parameterTypes.size() == 2 &&
        constructor.initializers.size() == 3)
      selected = constructor.id;
  const auto *constructor = control.mir.findConstructorInstance(selected);
  expect(constructor != nullptr, "borrowed-field sequence target found");
  if (!constructor)
    return;
  const auto bindings = childConstructions(*constructor);
  expect(bindings.size() == 3 && std::all_of(bindings.begin(), bindings.end(),
                                             [](const auto &child) {
                                               return child.field != 0 &&
                                                      child.firstArgument != 0;
                                             }),
         "three independent borrowed children have exact staged "
         "constructions");
  if (bindings.size() != 3) {
    if (std::getenv("GTI_TEST_DUMP_MIR") != nullptr)
      std::cerr << lang::MirPrinter().print(constructor->body);
    return;
  }
  expect(emitsNative(control), "independent borrowed-field sequence emits");

  const auto childFailure = [](auto &target,
                               const auto &binding) -> lang::MirBlock * {
    for (auto &block : target.body.blocks)
      if (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
          block.terminator.invokeInstruction == binding.constructInstruction)
        return target.body.findBlock(block.terminator.elseTarget);
    return nullptr;
  };
  const auto rollbackDrops = [&](auto &target, const auto &binding) {
    std::vector<lang::MirInstruction *> drops;
    auto *block = childFailure(target, binding);
    // The positive control has an acyclic success chain of cleanup Invokes.
    for (std::size_t count = 0; block && count < target.body.blocks.size();
         ++count) {
      for (auto &item : block->instructions) {
        const auto *drop =
            item.lifecycle.size() == 1
                ? target.body.findDropObligation(item.lifecycle.front().source)
                : nullptr;
        if (item.kind == lang::MirInstructionKind::Drop && drop &&
            drop->kind == lang::MirDropObligationKind::ConstructionRollback)
          drops.push_back(&item);
      }
      if (block->terminator.kind != lang::MirTerminatorKind::Invoke)
        break;
      block = target.body.findBlock(block->terminator.target);
    }
    return drops;
  };
  const auto reject = [&](const auto &mutate, std::string_view name) {
    auto program = control.mir;
    auto &changed = const_cast<lang::MirConstructorInstance &>(
        *program.findConstructorInstance(selected));
    const bool applied = mutate(changed);
    expect(applied, "borrowed-field sequence mutation applied");
    if (!applied)
      return;
    // Deliberately malformed MIR must fail normal verification rather than
    // relying on emission to repair the schedule. (The C++ backend's
    // detached binding proof is frozen in
    // cpp_mir_cleanup_bound_backend_mutation.)
    expect(!lang::verifyMirProgram(program).valid(), name);
    ++mutations;
  };
  reject(
      [&](auto &target) {
        auto drops = rollbackDrops(target, bindings[2]);
        if (drops.size() != 2)
          return false;
        std::swap(drops[0]->destination, drops[1]->destination);
        std::swap(drops[0]->lifecycle, drops[1]->lifecycle);
        return true;
      },
      "borrowed-field rollback cannot reverse prior field obligations");
  reject(
      [&](auto &target) {
        auto drops = rollbackDrops(target, bindings[2]);
        if (drops.size() != 2)
          return false;
        drops[1]->destination = drops[0]->destination;
        drops[1]->lifecycle = drops[0]->lifecycle;
        return true;
      },
      "borrowed-field rollback cannot drop one prior field twice");
  reject(
      [&](auto &target) {
        auto *failure = childFailure(target, bindings[1]);
        const auto *construct =
            instruction(target.body, bindings[1].constructInstruction);
        if (!failure || !construct || !construct->loan)
          return false;
        const auto end = std::find_if(
            failure->instructions.begin(), failure->instructions.end(),
            [&](const auto &item) {
              return item.kind == lang::MirInstructionKind::EndBorrow &&
                     item.loan == construct->loan;
            });
        if (end == failure->instructions.end())
          return false;
        failure->instructions.erase(end);
        return true;
      },
      "borrowed-field failure cannot lose the failed child's dependency end");
  reject(
      [&](auto &target) {
        auto *block = childFailure(target, bindings[1]);
        const auto *value = target.body.findValue(bindings[0].constructedValue);
        const auto published =
            std::find_if(target.body.loans.begin(), target.body.loans.end(),
                         [&](const auto &loan) {
                           return value &&
                                  loan.storedField == bindings[0].field &&
                                  loan.producedBy == value->sourceValue;
                         });
        if (published == target.body.loans.end())
          return false;
        for (std::size_t count = 0; block && count < target.body.blocks.size();
             ++count) {
          const auto end = std::find_if(
              block->instructions.begin(), block->instructions.end(),
              [&](const auto &item) {
                return item.kind == lang::MirInstructionKind::EndBorrow &&
                       item.loan == published->id;
              });
          if (end != block->instructions.end()) {
            block->instructions.erase(end);
            return true;
          }
          if (block->terminator.kind != lang::MirTerminatorKind::Invoke)
            break;
          block = target.body.findBlock(block->terminator.target);
        }
        return false;
      },
      "borrowed-field failure cannot retain a cleaned prior field dependency");
  reject(
      [&](auto &target) {
        auto *block = childFailure(target, bindings[1]);
        if (!block || block->terminator.kind != lang::MirTerminatorKind::Invoke)
          return false;
        auto *secondary = target.body.findBlock(block->terminator.elseTarget);
        if (!secondary)
          return false;
        secondary->terminator.failureRecord = block->activeFailure;
        return true;
      },
      "borrowed-field double failure cannot report the primary as secondary");
  reject(
      [&](auto &target) {
        auto *block = childFailure(target, bindings[1]);
        if (!block || block->terminator.kind != lang::MirTerminatorKind::Invoke)
          return false;
        auto *secondary = target.body.findBlock(block->terminator.elseTarget);
        if (!secondary || block->instructions.empty())
          return false;
        secondary->instructions.push_back(block->instructions.back());
        return true;
      },
      "borrowed-field double-failure exit cannot execute another drop");
  reject(
      [&](auto &target) {
        for (auto &block : target.body.blocks) {
          const auto stage =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &item) {
                             return item.id == bindings[1].firstArgument;
                           });
          if (stage != block.instructions.end()) {
            const auto extra = *stage;
            block.instructions.insert(stage, extra);
            return true;
          }
        }
        return false;
      },
      "borrowed-field sequence cannot hide executable work before staging");

  // A valid source program, not corrupted metadata: actual checked work in a
  // child must not be mistaken for the passive bookkeeping of this slice.
  auto fallibleSource = source;
  const std::string nonfailing =
      "SequenceGuard(Handle handle) : handle(handle) {}";
  const auto position = fallibleSource.find(nonfailing);
  expect(position != std::string::npos, "fallible child source location found");
  if (position != std::string::npos) {
    fallibleSource.replace(position, nonfailing.size(),
                           "SequenceGuard(Handle handle) : handle(handle) { "
                           "int checked = 1 / observed; }");
    const auto fallible = analyze(fallibleSource);
    expect(fallible.canGenerateCode() &&
               lang::verifyMirProgram(fallible.mir).valid(),
           "genuinely fallible borrowed child remains valid source/MIR");
    bool checked = false;
    for (const auto &target : fallible.mir.constructorInstances())
      if (target.parameterTypes.size() == 2 && target.initializers.size() == 3)
        checked = true;
    expect(checked && emitsNative(fallible),
           "genuinely fallible sequence target found");
  }
}
} // namespace

int main() {
  constructedFieldSequence();
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
  expect(emitsNative(control), "retained-shell rollback control emits");
  // The C++ backend's owned-parameter field binding proof and its rollback
  // text are frozen in cpp_mir_cleanup_bound_backend_mutation.

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
    expect(emitsNative(pair), "two-field control emits");
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
    expect(emitsNative(generic), "nonassignable generic guard emits");
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
  // The moved-from guard retires without its destructor; the unmoved witness
  // still runs cleanup that can fail while the returned result is pending.
  Guard guard{resource};
  Guard witness{resource};
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
