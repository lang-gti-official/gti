#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

int main() {
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto forwarded = lang::Frontend().analyze(
      "/tmp/forwarded.gti", R"(
#include <std/string>
class Base {
  std::string name;
public:
  Base(std::string_view text) : name(std::string(text)) {}
  ~Base() {}
};
class Derived : public Base {
public:
  Derived(std::string_view text) : Base(text) {}
  ~Derived() {}
};
int main() { Derived b = Derived("other"); return 0; }
)",
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  if (!forwarded.canGenerateCode() ||
      !lang::verifyMirProgram(forwarded.mir).valid()) {
    std::cerr << "forwarded passive base arguments must retain a valid stage\n";
    for (const auto &diagnostic : forwarded.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return 1;
  }
  bool checkedForward = false;
  for (const auto &constructor : forwarded.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::InitializeBase ||
            stage.operands.size() != 1)
          continue;
        const auto *value = constructor.body.findValue(stage.operands[0].value);
        if (!value || !value->info.traits.containsBorrowedState)
          continue;
        checkedForward = true;
        for (int mutation = 0; mutation < 3; ++mutation) {
          auto forged = forwarded.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              forged.findConstructorInstance(constructor.id));
          auto &instructions = changed->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(
              instructions.begin(), instructions.end(),
              [&](const auto &candidate) { return candidate.id == stage.id; });
          if (mutation == 0) {
            selected->constructorInitializer = 0;
          } else if (mutation == 1) {
            changed->body.loans.clear();
          } else {
            auto end = std::find_if(
                instructions.begin(), instructions.end(),
                [](const auto &candidate) {
                  return candidate.kind == lang::MirInstructionKind::EndBorrow;
                });
            // On a fallible call the endpoint belongs to the successor block.
            if (end == instructions.end()) {
              for (auto &successor : changed->body.blocks) {
                auto found = std::find_if(
                    successor.instructions.begin(),
                    successor.instructions.end(), [](const auto &candidate) {
                      return candidate.kind ==
                             lang::MirInstructionKind::EndBorrow;
                    });
                if (found == successor.instructions.end())
                  continue;
                auto premature = *found;
                successor.instructions.erase(found);
                instructions.insert(selected, premature);
                break;
              }
            } else {
              auto premature = *end;
              instructions.erase(end);
              selected = std::find_if(instructions.begin(), instructions.end(),
                                      [&](const auto &candidate) {
                                        return candidate.id == stage.id;
                                      });
              instructions.insert(selected, premature);
            }
          }
          (void)lang::rebuildMirValueUses(changed->body);
          if (lang::verifyMirProgram(forged).valid()) {
            std::cerr << "base forwarding must reject absent stages/loans and "
                         "loans ended before the base call\n";
            return 1;
          }
        }
      }
    }
  }
  if (!checkedForward) {
    std::cerr << "fixture must exercise a dynamically borrowed base argument\n";
    return 1;
  }
  auto frontend = lang::Frontend().analyze(
      "/tmp/gti-full-expression-loop.gti", R"(
enum class item_error { unavailable };
class Item {
public:
  Item() {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  ~Item() {}
};
expected<Item, item_error> produce(int index) {
  if (index == 2) { return unexpected(item_error::unavailable); }
  return Item();
}
void consume(Item value) {}
class Record {
  int value;
public:
  Record(int value) : value(value) {}
};
expected<Record, item_error> checked_record(int divisor) {
  return Record(12 / divisor);
}
expected<Record, item_error> relay_record(int divisor) {
  return checked_record(divisor);
}
expected<void, item_error> collect() {
  for (mut int outer = 0; outer < 2; outer++) {
    for (mut int inner = 0; inner < 4; inner++) {
      consume(?produce(inner));
    }
  }
  return;
}
int main() { auto result = collect(); return result.has_value() ? 0 : 1; }
)",
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  if (!frontend.mirValid || !lang::verifyMirProgram(frontend.mir).valid()) {
    std::cerr
        << "nested loop argument propagation must have valid boundaries\n";
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return 1;
  }

  auto duplicate = frontend.mir;
  bool mutated = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           duplicate.functionInstances())) {
    auto &body = function.body;
    lang::MirInstructionId maximum = 0;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        maximum = std::max(maximum, instruction.id);
    for (auto &block : body.blocks) {
      for (size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &marker = block.instructions[index];
        if (marker.fullExpressionEnd == 0 || marker.earlyExitFullExpressionEnd)
          continue;
        const auto count = std::count_if(
            body.blocks.begin(), body.blocks.end(), [&](const auto &candidate) {
              return std::any_of(
                  candidate.instructions.begin(), candidate.instructions.end(),
                  [&](const auto &instruction) {
                    return instruction.fullExpressionEnd ==
                               marker.fullExpressionEnd &&
                           instruction.earlyExitFullExpressionEnd;
                  });
            });
        if (count == 0)
          continue;
        auto repeated = marker;
        repeated.id = maximum + 1;
        // Preserve the one-primary invariant and make an alternate boundary
        // reachable in the same iteration as that primary boundary.
        repeated.earlyExitFullExpressionEnd = true;
        block.instructions.insert(block.instructions.begin() + index, repeated);
        (void)lang::rebuildMirValueUses(body);
        mutated = true;
        break;
      }
      if (mutated)
        break;
    }
    if (mutated)
      break;
  }
  if (!mutated) {
    std::cerr
        << "expected a boundary with both normal and propagated-error paths\n";
    return 1;
  }
  const auto verification = lang::verifyMirProgram(duplicate);
  const bool rejectedDuplicate = std::any_of(
      verification.errors.begin(), verification.errors.end(),
      [](const auto &error) {
        return error.message.find("full-expression boundary markers are not on "
                                  "mutually exclusive") != std::string::npos;
      });
  if (verification.valid() || !rejectedDuplicate) {
    std::cerr << "same-iteration duplicate boundaries must still be rejected\n";
    for (const auto &error : verification.errors)
      std::cerr << error.message << '\n';
    return 1;
  }

  // expected<T, E> is the prelude's ordinary source class.
  const auto isExpected = [&](const lang::SemanticType &type) {
    const auto *info = type.kind == lang::SemanticType::Class
                           ? frontend.semantics.findClassType(type.classId)
                           : nullptr;
    return info != nullptr && info->declaration != nullptr &&
           info->declaration->name().lexeme == "expected";
  };
  auto missingInvoke = frontend.mir;
  bool removedInvoke = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           missingInvoke.functionInstances())) {
    const auto *source = frontend.hir.findFunctionInstance(function.id);
    if (source == nullptr ||
        source->sourceUnit != frontend.sourceGraph.entryUnit())
      continue;
    for (auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke ||
          block.instructions.empty())
        continue;
      const auto &call = block.instructions.back();
      if (call.kind != lang::MirInstructionKind::Call ||
          !isExpected(call.info.type) ||
          call.info.type.arguments.front().kind != lang::SemanticType::Class ||
          call.successResultDrop || call.successResultDestination)
        continue;
      const auto target = block.terminator.target;
      block.terminator = {};
      block.terminator.kind = lang::MirTerminatorKind::Goto;
      block.terminator.target = target;
      lang::rebuildMirReachability(function.body);
      (void)lang::rebuildMirValueUses(function.body);
      removedInvoke = true;
      break;
    }
    if (removedInvoke)
      break;
  }
  if (!removedInvoke || lang::verifyMirProgram(missingInvoke).valid()) {
    std::cerr << "cleanup-free expected calls must retain their failure edge\n";
    return 1;
  }

  // An explicit initializer without source arguments still evaluates the
  // base constructor's omitted default arguments. They form one full
  // expression, so a nested owning default argument is a prepared input.
  const auto baseDefaults =
      lang::Frontend().analyze("/tmp/gti-explicit-base-default.gti", R"(
class Token {
  int value;
public:
  Token(int value) : value(value) {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
  ~Token() {}
  int get() { return this.value; }
};
int take(Token token = Token(3)) { return token.get(); }
class Base {
  int value;
public:
  Base(int value = take()) : value(value) {}
};
class Explicit : public Base {
public:
  Explicit() : Base() {}
};
int main() { Explicit first{}; return 0; }
)");
  bool preparedDefault = false;
  for (const auto &constructor : baseDefaults.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        preparedDefault =
            preparedDefault ||
            (instruction.kind == lang::MirInstructionKind::CallInput &&
             instruction.defaultArgument &&
             instruction.callInputKind == lang::HirCallInputKind::MoveValue &&
             instruction.preparedParameterDrop.has_value());
  if (!baseDefaults.mir.valid() ||
      !lang::verifyMirProgram(baseDefaults.mir).valid() || !preparedDefault) {
    std::cerr << "an argumentless explicit base initializer must prepare its "
                 "owning default arguments\n";
    return 1;
  }

  const auto allocationDefaults = lang::Frontend().analyze(
      "/tmp/gti-allocation-default.gti", R"(
#include <std/memory>
struct Spec {
  int count;
  Spec(int value = 2) : count(value) {}
};
class Item {
  int count;
public:
  Item(Spec spec = Spec()) : count(spec.count) {}
};
int main() { auto item = std::make_unique<Item>(); return 0; }
)",
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  if (!allocationDefaults.canGenerateCode() ||
      !lang::verifyMirProgram(allocationDefaults.mir).valid()) {
    for (const auto &diagnostic : allocationDefaults.diagnostics)
      std::cerr << diagnostic.message << '\n';
    std::cerr << "allocation defaults must retain verified construction\n";
    return 1;
  }
  bool plannedDefault = false;
  for (const auto &function : allocationDefaults.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.intrinsic == lang::IntrinsicKind::AllocateUniqueOwner &&
          value.constructorTarget && value.parameterTypes.size() == 1)
        plannedDefault = value.callPlan &&
                         value.callPlan->arguments.size() == 1 &&
                         value.callPlan->arguments.front().defaultArgument &&
                         value.callPlan->arguments.front().kind ==
                             lang::HirCallInputKind::MoveValue;
  bool checkedAllocation = false;
  for (const auto &function : allocationDefaults.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.intrinsic != lang::IntrinsicKind::AllocateUniqueOwner ||
            !call.constructorTarget || call.operands.size() != 1)
          continue;
        const auto *argument = function.body.findValue(call.operands[0].value);
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidateBlock : function.body.blocks)
          for (const auto &instruction : candidateBlock.instructions)
            if (argument && instruction.id == argument->definition)
              stage = &instruction;
        if (!stage || stage->kind != lang::MirInstructionKind::CallInput ||
            stage->callSite != call.callSite || !stage->defaultArgument ||
            !stage->preparedParameterDrop || !stage->destination) {
          std::cerr << "allocation default lacks its exact prepared owner\n";
          return 1;
        }
        checkedAllocation = true;
        for (int mutation = 0; mutation < 3; ++mutation) {
          auto forged = allocationDefaults.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &instruction : changedBlock.instructions)
              if (instruction.id == stage->id) {
                if (mutation == 0)
                  instruction.callSite = 0;
                if (mutation == 1)
                  instruction.callInputIndex = 1;
                if (mutation == 2)
                  instruction.preparedParameterDrop.reset();
              }
          if (lang::verifyMirProgram(forged).valid()) {
            std::cerr << "malformed allocation default stage accepted\n";
            return 1;
          }
        }
      }
    }
  }
  if (!plannedDefault || !checkedAllocation) {
    std::cerr << "allocation defaults must use the ordinary ordered plan\n";
    return 1;
  }
  const auto packDefaults = lang::Frontend().analyze(
      "/tmp/gti-pack-default.gti", R"(
#include <std/memory>
struct Spec {
  int count;
  Spec(int value = 2) : count(value) {}
};
class Item {
  int count;
public:
  Item(int first, int second, Spec spec = Spec())
      : count(first + second + spec.count) {}
};
int main() { auto item = std::make_unique<Item>(3, 4); return 0; }
)",
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  if (!packDefaults.canGenerateCode() ||
      !lang::verifyMirProgram(packDefaults.mir).valid()) {
    for (const auto &diagnostic : packDefaults.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return 1;
  }
  bool checkedSuffix = false;
  for (const auto &function : packDefaults.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      if (value.intrinsic != lang::IntrinsicKind::AllocateUniqueOwner ||
          value.parameterTypes.size() != 3)
        continue;
      checkedSuffix = value.defaultArguments.size() == 1;
      for (int mutation = 0; mutation < 3; ++mutation) {
        auto forged = packDefaults.hir;
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        for (auto &input : changed->body.values)
          if (input.id == value.id) {
            if (mutation == 0)
              input.defaultArguments.clear();
            if (mutation == 1)
              input.defaultArguments[0] = input.operands.front();
            if (mutation == 2)
              input.defaultArguments.push_back(input.defaultArguments[0]);
          }
        if (lang::verifyHirProgramPlans(packDefaults.semantics, forged)
                .valid()) {
          std::cerr << "malformed declaration default suffix accepted\n";
          return 1;
        }
      }
    }
  }
  bool checkedFlattened = false;
  for (const auto &function : packDefaults.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        if (call.intrinsic != lang::IntrinsicKind::AllocateUniqueOwner ||
            call.operands.size() != 3)
          continue;
        checkedFlattened = true;
        for (std::size_t index = 0; index < 3; ++index) {
          const auto *argument =
              function.body.findValue(call.operands[index].value);
          const lang::MirInstruction *stage = nullptr;
          for (const auto &candidateBlock : function.body.blocks)
            for (const auto &input : candidateBlock.instructions)
              if (argument && input.id == argument->definition)
                stage = &input;
          if (!stage || stage->kind != lang::MirInstructionKind::CallInput ||
              stage->callInputIndex != index ||
              stage->defaultArgument != (index == 2)) {
            std::cerr << "pack/default parameter indices or provenance lost\n";
            return 1;
          }
        }
        for (bool conditional : {false, true}) {
          auto forged = packDefaults.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          bool altered = false;
          for (auto &changedBlock : changed->body.blocks)
            for (auto &input : changedBlock.instructions)
              if (input.id == call.id)
                for (auto event = input.lifecycle.begin();
                     event != input.lifecycle.end(); ++event)
                  if (event->kind == lang::MirLifecycleEventKind::TransferOut) {
                    if (conditional)
                      event->conditional = true;
                    else
                      input.lifecycle.erase(event);
                    altered = true;
                    break;
                  }
          if (!altered || lang::verifyMirProgram(forged).valid()) {
            std::cerr << "pack/default prepared transfer lost verification\n";
            return 1;
          }
        }
      }
  if (!checkedSuffix || !checkedFlattened) {
    std::cerr << "pack/default fixture lost its selected allocation\n";
    return 1;
  }
  const auto declarationDefaults =
      lang::Frontend().analyze("/tmp/gti-declaration-default.gti", R"(
class Runner {
public:
  static int Create(int count = 3);
  int Run(int count = 7);
};
int Runner::Create(int renamed) { return renamed; }
int Runner::Run(int renamed) { return renamed; }
int main() { Runner runner{}; return Runner::Create() + runner.Run(); }
)");
  if (!declarationDefaults.canGenerateCode()) {
    for (const auto &diagnostic : declarationDefaults.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return 1;
  }
  std::size_t checkedDeclarations = 0;
  for (const auto &function : declarationDefaults.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      if (value.defaultArguments.empty() || !value.functionTarget)
        continue;
      const auto *target =
          declarationDefaults.hir.findFunctionInstance(*value.functionTarget);
      const auto *api =
          declarationDefaults.semantics.findFunction(target->declaration);
      if (!api || !api->declaration || !api->definition ||
          target->source != api->definition ||
          target->source->parameters().front().defaultArgument ||
          !api->declaration->parameters().front().defaultArgument) {
        std::cerr << "default contract must belong to API, not body syntax\n";
        return 1;
      }
      ++checkedDeclarations;
      const lang::HirFunctionInstance *other = nullptr;
      for (const auto &candidate :
           declarationDefaults.hir.functionInstances()) {
        const auto *otherApi =
            declarationDefaults.semantics.findFunction(candidate.declaration);
        if (candidate.id != target->id && otherApi && otherApi->definition &&
            otherApi->declaration->parameters().size() == 1 &&
            otherApi->declaration->parameters().front().defaultArgument)
          other = &candidate;
      }
      if (!other)
        return 1;
      const auto *otherApi =
          declarationDefaults.semantics.findFunction(other->declaration);
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = declarationDefaults.hir;
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        for (auto &input : changed->body.values) {
          if (mutation == 0 && input.id == value.defaultArguments.front())
            input.source = otherApi->declaration->parameters()
                               .front()
                               .defaultArgument->expression.get();
          if (mutation == 1 && input.id == value.id)
            input.functionTarget = other->id;
          if (mutation == 2 && input.id == value.id)
            input.defaultArguments.push_back(input.defaultArguments.front());
        }
        if (lang::verifyHirProgramPlans(declarationDefaults.semantics, forged)
                .valid()) {
          std::cerr << "foreign or malformed declaration default accepted\n";
          return 1;
        }
      }
    }
  }
  if (checkedDeclarations != 2) {
    std::cerr
        << "static and instance defaults must retain exact declarations\n";
    return 1;
  }
  const auto invalidDefault =
      lang::Frontend().analyze("/tmp/gti-invalid-declaration-default.gti", R"(
class Runner { public: int Run(int count = "bad"); };
int Runner::Run(int renamed) { return renamed; }
int main() { Runner runner{}; return runner.Run(); }
)");
  if (invalidDefault.canGenerateCode()) {
    std::cerr << "a declaration default must retain semantic type checking\n";
    return 1;
  }

  // A constructor initializer's full expression, explicit or from a field
  // declaration default, may materialize a temporary method receiver. Its
  // drop is ordinary cleanup before the initializer's boundary; only the
  // stage's Initialize names the initializer index.
  const auto temporaryReceivers =
      lang::Frontend().analyze("/tmp/gti-initializer-temporary-receiver.gti",
                               R"(
struct Meters {
  int value;
  Meters(int amount) : value(amount) {}
};
struct Feet {
  int value;
  Feet(int amount) : value(amount) {}
  Meters to_meters() { return Meters(this.value / 3); }
};
class Track {
  Meters lap = Feet(30).to_meters();
public:
  Track() {}
  Track(int feet) : lap(Feet(feet).to_meters()) {}
  int total() { return this.lap.value; }
};
struct Split {
  Meters lap = Feet(12).to_meters();
};
int main() {
  Track declared{};
  Track explicit_lap = Track(60);
  Split generated{};
  return declared.total() + explicit_lap.total() + generated.lap.value - 34;
}
)");
  if (!temporaryReceivers.canGenerateCode() ||
      !lang::verifyMirProgram(temporaryReceivers.mir).valid()) {
    std::cerr << "constructor initializers must drop temporary receivers\n";
    for (const auto &diagnostic : temporaryReceivers.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return 1;
  }
  std::size_t checkedReceivers = 0;
  for (const auto &constructor :
       temporaryReceivers.mir.constructorInstances()) {
    const auto &body = constructor.body;
    for (const auto &obligation : body.dropObligations) {
      const auto expression =
          std::find_if(body.fullExpressions.begin(), body.fullExpressions.end(),
                       [&](const auto &candidate) {
                         return obligation.fullExpression != 0 &&
                                candidate.id == obligation.fullExpression;
                       });
      if (expression == body.fullExpressions.end() ||
          expression->constructorInitializer == 0)
        continue;
      for (const auto &block : body.blocks) {
        for (const auto &drop : block.instructions) {
          if (drop.kind != lang::MirInstructionKind::Drop ||
              std::none_of(drop.lifecycle.begin(), drop.lifecycle.end(),
                           [&](const auto &event) {
                             return event.kind ==
                                        lang::MirLifecycleEventKind::Drop &&
                                    event.source == obligation.id &&
                                    !event.failureCleanup;
                           }))
            continue;
          if (drop.constructorInitializer != 0) {
            std::cerr << "a temporary receiver drop must not claim its "
                         "initializer's stage\n";
            return 1;
          }
          ++checkedReceivers;
          auto forged = temporaryReceivers.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              forged.findConstructorInstance(constructor.id));
          for (auto &instruction :
               changed->body.blocks[block.id - 1].instructions)
            if (instruction.id == drop.id)
              instruction.constructorInitializer =
                  expression->constructorInitializer;
          if (lang::verifyMirProgram(forged).valid()) {
            std::cerr << "a drop claiming an initializer stage was accepted\n";
            return 1;
          }
        }
      }
    }
  }
  // The default, explicit and generated constructors each end one receiver.
  if (checkedReceivers != 3) {
    std::cerr << "expected three initializer temporary receiver drops, found "
              << checkedReceivers << '\n';
    return 1;
  }
  return 0;
}
