#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
using namespace lang;
int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

constexpr std::string_view source = R"(
interface Reader { int read(); };
interface Mirror { int read(); };
interface Scale<T> { T scale(T value); };
interface Label { int label(); };
class Base : public Reader {
public:
  Base() {}
  int read() override { return 2; }
  virtual int marker() { return 1; }
};
class Middle : public Base, public Scale<int> {
public:
  Middle() : Base() {}
  int read() override { return 3; }
  int scale(int value) override { return value; }
};
class Leaf : public Middle, public Label {
public:
  Leaf() : Middle() {}
  int marker() override { return 4; }
  int label() override { return 5; }
};
class Unified : public Reader, public Mirror {
public:
  int read() override { return 6; }
};
class Open : public Base {
public:
  virtual int pending() = 0;
};
class Generic<T> : public Scale<T> {
public:
  T scale(T value) override { return value; }
};
class CheckedBase {
public:
  CheckedBase(int divisor) { mut int value = 12 / divisor; }
  virtual int read() { return 8; }
};
class CheckedDerived : public CheckedBase {
  int value = 7;
public:
  CheckedDerived(int divisor) : CheckedBase(divisor) {}
};
int through(Reader& value) { return value.read(); }
int main() {
  Leaf leaf{};
  Unified unified{};
  Generic<int> generic{};
  CheckedDerived checked{1};
  return through(leaf) + through(unified) + generic.scale(1);
}
)";

template <typename Program>
auto &namedClass(Program &program, const SemanticModel &semantics,
                 std::string_view name) {
  const auto &classes = program.classInstances();
  for (const auto &owner : classes) {
    const auto *declaration = semantics.findClassType(owner.declaration);
    if (declaration && declaration->qualifiedName == name) {
      using Owner =
          std::remove_const_t<std::remove_reference_t<decltype(owner)>>;
      return const_cast<Owner &>(owner);
    }
  }
  throw std::runtime_error("missing dispatch fixture class");
}

void rejectsMir(
    const FrontendResult &frontend,
    const std::function<void(MirProgram &, MirClassInstance &)> &edit,
    std::string_view message) {
  auto program = frontend.mir;
  auto &leaf = namedClass(program, frontend.semantics, "Leaf");
  edit(program, leaf);
  const auto result = verifyMirProgram(program);
  expect(!result.valid() &&
             std::any_of(result.errors.begin(), result.errors.end(),
                         [](const auto &error) {
                           return error.message.find("dispatch") !=
                                  std::string::npos;
                         }),
         message);
}

template <typename Constructor>
decltype(auto) referenceInput(Constructor &constructor,
                              const MirInstruction &base) {
  const auto *value = constructor.body.findValue(base.operands.front().value);
  if (value)
    for (auto &block : constructor.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == value->definition &&
            instruction.kind == MirInstructionKind::CallInput)
          return (instruction);
  throw std::runtime_error("missing prepared base reference input");
}

void testPlans(const FrontendResult &frontend) {
  expect(verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "source selections verify in HIR");
  expect(verifyMirProgram(frontend.mir).valid(),
         "concrete hierarchy selections verify in MIR");
  const auto &leaf = namedClass(frontend.mir, frontend.semantics, "Leaf");
  expect(leaf.virtualDispatch.size() == 4,
         "multilevel leaf has each inherited and direct virtual root");
  bool inherited = false, concreteGeneric = false;
  for (const auto &row : leaf.virtualDispatch) {
    const auto *root = frontend.mir.findFunctionInstance(row.root);
    const auto *target = frontend.mir.findFunctionInstance(row.target);
    const auto *declaration =
        frontend.semantics.findFunction(root->declaration);
    if (declaration->declaration->name().lexeme == "read")
      inherited = row.rootPath.size() == 3 && row.targetPath.size() == 1 &&
                  target->owner == leaf.bases.front().instance;
    if (declaration->declaration->name().lexeme == "scale")
      concreteGeneric = root->parameterTypes ==
                            std::vector<SemanticType>{SemanticType::Int32} &&
                        row.rootPath.size() == 2 && row.targetPath.size() == 1;
  }
  expect(inherited, "inherited target retains the exact non-root owner path");
  expect(concreteGeneric, "generic interface root has its concrete signature");
  const auto &unified = namedClass(frontend.mir, frontend.semantics, "Unified");
  expect(unified.virtualDispatch.size() == 2 &&
             unified.virtualDispatch[0].root !=
                 unified.virtualDispatch[1].root &&
             unified.virtualDispatch[0].target ==
                 unified.virtualDispatch[1].target,
         "one semantic override can implement distinct interface roots");
  const auto &open = namedClass(frontend.mir, frontend.semantics, "Open");
  expect(open.abstract && std::any_of(open.virtualDispatch.begin(),
                                      open.virtualDispatch.end(),
                                      [&](const auto &row) {
                                        return frontend.mir
                                            .findFunctionInstance(row.target)
                                            ->pureVirtual;
                                      }),
         "abstract classes intentionally retain pure selections");

  auto hir = frontend.hir;
  namedClass(hir, frontend.semantics, "Leaf").virtualDispatch.front().target =
      0;
  expect(!verifyHirProgramPlans(frontend.semantics, hir).valid(),
         "HIR cannot replace the semantic-selected target");

  rejectsMir(
      frontend, [](auto &, auto &owner) { owner.virtualDispatch.pop_back(); },
      "missing root rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.push_back(owner.virtualDispatch.front());
      },
      "duplicate root rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) { owner.virtualDispatch.front().root = 0; },
      "unresolved root rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) { owner.virtualDispatch.front().target = 0; },
      "unresolved target rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().rootOwner = owner.id;
      },
      "mismatched root owner rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().targetOwner = 0;
      },
      "mismatched target owner rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().rootPath.clear();
      },
      "missing root base path rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().targetPath.push_back(owner.id);
      },
      "forged target base path rejects");
  rejectsMir(
      frontend,
      [](auto &program, auto &owner) {
        const auto target = owner.virtualDispatch.front().target;
        auto &function = const_cast<MirFunctionInstance &>(
            *program.findFunctionInstance(target));
        function.parameterTypes.push_back(SemanticType::Bool);
      },
      "mismatched target signature rejects");
  rejectsMir(
      frontend,
      [](auto &program, auto &owner) {
        const auto target = owner.virtualDispatch.front().target;
        auto &function = const_cast<MirFunctionInstance &>(
            *program.findFunctionInstance(target));
        function.definitionKind = MirDefinitionKind::Declaration;
      },
      "unimplemented concrete target rejects");
  rejectsMir(
      frontend,
      [](auto &program, auto &owner) {
        for (auto &row : owner.virtualDispatch) {
          const auto *root = program.findFunctionInstance(row.root);
          if (root->owner != row.targetOwner && !root->pureVirtual) {
            row.target = row.root;
            row.targetOwner = row.rootOwner;
            row.targetPath = row.rootPath;
            return;
          }
        }
        throw std::runtime_error("fixture needs an overridden concrete root");
      },
      "a signature-compatible ancestor cannot replace the final override");

  auto lowered = gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const auto text = LoweredProgramPrinter().print(lowered);
  expect(text.starts_with("lowered-program-v27\n") &&
             text.find("virtual-dispatch=[{root=") != std::string::npos &&
             text == LoweredProgramPrinter().print(lowered) &&
             MirPrinter().print(frontend.mir).starts_with("mir-v117 "),
         "dispatch selection and paths serialize deterministically");
  auto &sealed = const_cast<LoweredClassInstance &>(*std::find_if(
      lowered.classInstances().begin(), lowered.classInstances().end(),
      [&](const auto &owner) { return owner.id == leaf.id; }));
  sealed.virtualDispatch.clear();
  expect(!verifyLoweredProgram(lowered).empty(),
         "sealed class instance cannot lose its dispatch vector");
}

void testConstructorState(const FrontendResult &frontend) {
  bool entry = false, anchored = false, fallible = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner->polymorphic)
      continue;
    expect(
        constructor.dispatchState &&
            constructor.dispatchState->activeClass == owner->id,
        "represented polymorphic constructors retain their exact active class");
    if (!constructor.dispatchState)
      continue;
    const auto anchor = constructor.dispatchState->afterBaseInitialization;
    entry |= anchor == 0;
    anchored |= anchor != 0;
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == anchor)
          fallible |= block.terminator.kind == MirTerminatorKind::Invoke &&
                      block.terminator.invokeInstruction == anchor;
  }
  expect(
      entry && anchored && fallible,
      "constructor plans exercise entry, base and fallible-success activation");

  const auto optimized =
      OptimizationPipeline().run({.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = OptimizationLevel::O3,
                                  .target = TargetInfo::host()});
  expect(optimized.valid(), "O3 preserves verified constructor state plans");
  for (const auto &constructor : frontend.mir.constructorInstances())
    expect(
        optimized.mir.findConstructorInstance(constructor.id)->dispatchState ==
            constructor.dispatchState,
        "optimizer preserves exact body-local activation identities");

  const auto rejects = [&](const auto &edit, std::string_view message) {
    auto program = frontend.mir;
    auto &constructor = const_cast<MirConstructorInstance &>(*std::find_if(
        program.constructorInstances().begin(),
        program.constructorInstances().end(), [](const auto &row) {
          return row.dispatchState &&
                 row.dispatchState->afterBaseInitialization != 0;
        }));
    edit(program, constructor);
    const auto result = verifyMirProgram(program);
    expect(!result.valid() &&
               std::any_of(result.errors.begin(), result.errors.end(),
                           [](const auto &error) {
                             return error.message.find("dispatch state") !=
                                    std::string::npos;
                           }),
           message);
  };
  rejects([](auto &,
             auto &constructor) { constructor.dispatchState->activeClass = 0; },
          "forged active class rejects");
  rejects(
      [](auto &, auto &constructor) {
        constructor.dispatchState->afterBaseInitialization = 0;
      },
      "entry activation cannot bypass a real state-bearing base");
  rejects(
      [](auto &, auto &constructor) {
        constructor.dispatchState->afterBaseInitialization = 999999;
      },
      "stale activation anchor rejects");
  rejects(
      [](auto &, auto &constructor) {
        const auto anchor = constructor.dispatchState->afterBaseInitialization;
        for (auto &block : constructor.body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.id == anchor)
              instruction = makeMirInstructionTombstone(instruction);
      },
      "activation cannot survive removal of its base initialization");
  rejects(
      [](auto &program, auto &constructor) {
        for (const auto &row : program.constructorInstances()) {
          if (!row.dispatchState)
            continue;
          const auto anchor = row.dispatchState->afterBaseInitialization;
          for (const auto &block : row.body.blocks)
            if (anchor && block.terminator.kind == MirTerminatorKind::Invoke &&
                block.terminator.invokeInstruction == anchor) {
              constructor = row;
              for (auto &changed : constructor.body.blocks)
                if (changed.id == block.id)
                  std::swap(changed.terminator.target,
                            changed.terminator.elseTarget);
              return;
            }
        }
        throw std::runtime_error(
            "fixture needs a fallible base initialization");
      },
      "activation must follow the real success edge and dominate normal "
      "completion");
}

void testExistingBaseBoundaries() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  for (const auto &[file, name] :
       {std::pair{"llvm_dispatch_owned_base_input.gti", "OwnerLeaf"},
        std::pair{"llvm_dispatch_reference_base_input.gti", "ReferenceLeaf"},
        std::pair{"llvm_dispatch_reference_base_input.gti", "ReadViaMutable"},
        std::pair{"llvm_dispatch_reference_base_input.gti",
                  "LiteralReferenceLeaf"},
        std::pair{"llvm_dispatch_reference_base_input.gti",
                  "EffectfulReferenceLeaf"},
        std::pair{"llvm_dispatch_reference_base_input.gti", "MutableLeaf"}}) {
    const auto frontend = Frontend().analyze(
        root / "tests/fixtures" / file, std::nullopt,
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    expect(frontend.canGenerateCode(),
           "existing owning and transient-reference base inputs stay valid");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    const auto &owner = namedClass(frontend.mir, frontend.semantics, name);
    bool found = false;
    for (const auto &constructor : frontend.mir.constructorInstances())
      if (constructor.owner == owner.id) {
        found = true;
        if (std::string_view(name) == "OwnerLeaf" ||
            std::string_view(name) == "EffectfulReferenceLeaf") {
          expect(!constructor.dispatchState,
                 "unstaged owning/effectful inputs do not acquire a false "
                 "entry anchor");
          for (const auto &block : constructor.body.blocks)
            for (const auto &instruction : block.instructions)
              expect(instruction.kind != MirInstructionKind::CallInput ||
                         instruction.constructorInitializer == 0,
                     "unsupported later inputs leave no partly prepared base "
                     "input");
        } else {
          expect(constructor.dispatchState &&
                     constructor.dispatchState->afterBaseInitialization != 0,
                 "forwarded references retain the real base success anchor");
          for (const auto &block : constructor.body.blocks)
            for (const auto &instruction : block.instructions)
              if (instruction.kind == MirInstructionKind::InitializeBase) {
                expect(instruction.operands.front().type.kind ==
                               SemanticType::Reference &&
                           referenceInput(constructor, instruction)
                                   .operands.front()
                                   .kind ==
                               (std::string_view(name) == "ReferenceLeaf" ||
                                        std::string_view(name) ==
                                            "LiteralReferenceLeaf"
                                    ? MirOperandKind::BorrowRead
                                    : MirOperandKind::Loan) &&
                           instruction.parameterTypes.front() ==
                               instruction.operands.front().type,
                       "a base reference carries its address and selected "
                       "formal type");
                const auto argument =
                    constructor.initializers.front().arguments.front();
                bool loaded = false;
                for (const auto &prefix : constructor.body.blocks)
                  for (const auto &operation : prefix.instructions)
                    loaded |= operation.kind == MirInstructionKind::Load &&
                              operation.hirValue == argument;
                expect(!loaded, "passing a base reference does not load the "
                                "referent value");
                expect(instruction.definedFailure.propagation ==
                               FailurePropagationKind::Constructor &&
                           block.terminator.kind == MirTerminatorKind::Invoke &&
                           block.terminator.invokeInstruction ==
                               instruction.id &&
                           instruction.lifecycle.empty() &&
                           std::any_of(
                               block.terminator.successLifecycle.begin(),
                               block.terminator.successLifecycle.end(),
                               [&](const auto &event) {
                                 return event.kind ==
                                            MirLifecycleEventKind::Initialize &&
                                        event.target ==
                                            instruction.successResultDrop;
                               }),
                       "fallible reference-base initialization arms rollback "
                       "only on the existing success edge");
              }
        }
      }
    expect(found && verifyMirProgram(frontend.mir).valid(),
           "missing optional native state plan does not reject valid core MIR");
    const auto optimized =
        OptimizationPipeline().run({.hir = frontend.hir,
                                    .mir = frontend.mir,
                                    .level = OptimizationLevel::O3,
                                    .target = TargetInfo::host()});
    expect(optimized.valid(), "O3 preserves forwarded base argument contracts");
    const auto sealedForwarding = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, frontend.mir,
        optimized.mir, {}, TargetInfo::host());
    expect(sealedForwarding.valid(),
           "source-sealed reference forwarding preserves the exact formal");
    if (std::string_view(name) == "MutableLeaf") {
      for (const int mutation : {0, 1, 2}) {
        auto changed = frontend.mir;
        auto &constructor = const_cast<MirConstructorInstance &>(*std::find_if(
            changed.constructorInstances().begin(),
            changed.constructorInstances().end(),
            [&](const auto &row) { return row.owner == owner.id; }));
        for (auto &block : constructor.body.blocks)
          for (auto &stage : block.instructions)
            if (stage.kind == MirInstructionKind::InitializeBase) {
              auto &address =
                  referenceInput(constructor, stage).operands.front();
              auto *loan = const_cast<MirLoan *>(
                  constructor.body.findLoan(address.loan));
              if (mutation == 0)
                loan->source = address.place;
              else if (mutation == 1)
                loan->access = AccessMode::ReadOnly;
              else {
                address.kind = MirOperandKind::BorrowWrite;
                address.loan = 0;
              }
            }
        expect(!verifyMirProgram(changed).valid(),
               "a mutable base argument retains the exact entry loan source "
               "and permission, never an anonymous replacement");
      }
    }
    if (std::string_view(name) == "ReferenceLeaf") {
      const auto mutate = [&](const auto &edit) {
        auto changed = frontend.mir;
        auto &constructor = const_cast<MirConstructorInstance &>(*std::find_if(
            changed.constructorInstances().begin(),
            changed.constructorInstances().end(),
            [&](const auto &row) { return row.owner == owner.id; }));
        for (auto &block : constructor.body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == MirInstructionKind::InitializeBase) {
              edit(constructor, instruction);
              return changed;
            }
        throw std::runtime_error("missing forwarded-reference base stage");
      };
      for (const auto &changed :
           {mutate([](auto &, auto &stage) {
              stage.operands.front().type = SemanticType::Int32;
            }),
            mutate([](auto &constructor, auto &stage) {
              referenceInput(constructor, stage).operands.front().kind =
                  MirOperandKind::BorrowWrite;
            }),
            mutate([](auto &constructor, auto &stage) {
              auto *place = const_cast<MirPlace *>(constructor.body.findPlace(
                  referenceInput(constructor, stage).operands.front().place));
              place->projections.push_back(
                  {.kind = MirProjectionKind::Dereference});
            }),
            mutate([](auto &constructor, auto &stage) {
              referenceInput(constructor, stage).callInputIndex = 1;
            }),
            mutate([](auto &constructor, auto &stage) {
              referenceInput(constructor, stage).constructorInitializer = 0;
            }),
            mutate([](auto &constructor, auto &stage) {
              referenceInput(constructor, stage).callSite = 1;
            }),
            mutate([](auto &constructor, auto &stage) {
              referenceInput(constructor, stage).hirValue =
                  constructor.initializers.front().arguments.back();
            }),
            mutate([](auto &constructor, auto &stage) {
              const auto input = referenceInput(constructor, stage).id;
              for (auto &block : constructor.body.blocks) {
                const auto found = std::find_if(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const auto &instruction) {
                      return instruction.id == input;
                    });
                if (found != block.instructions.end() &&
                    std::next(found) != block.instructions.end()) {
                  std::iter_swap(found, std::next(found));
                  return;
                }
              }
            })})
        expect(!verifyMirProgram(changed).valid(),
               "wrong reference type/access/extra projection rejects");
      const auto wrongFormal = mutate([](auto &constructor, auto &stage) {
        auto *place = const_cast<MirPlace *>(constructor.body.findPlace(
            referenceInput(constructor, stage).operands.front().place));
        const auto formal = std::find_if(
            constructor.body.places.begin(), constructor.body.places.end(),
            [&](const auto &candidate) {
              return candidate.root == MirPlaceRootKind::Binding &&
                     candidate.binding == constructor.parameterBindings[1] &&
                     candidate.projections.empty();
            });
        if (formal == constructor.body.places.end())
          throw std::runtime_error("missing second reference formal");
        place->binding = formal->binding;
        place->symbol = formal->symbol;
        if (place->key)
          place->key->root = formal->symbol;
      });
      const auto sealed = LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir, wrongFormal,
          wrongFormal, {}, TargetInfo::host());
      expect(!sealed.valid() &&
                 std::any_of(sealed.issues.begin(), sealed.issues.end(),
                             [](const auto &issue) {
                               return issue.detail.find("exact HIR formal") !=
                                      std::string::npos;
                             }),
             "a same-typed different reference formal cannot replace the "
             "source argument");
    }
  }
  const auto rejected = Frontend().analyze(
      root / "tests/fixtures/llvm_dispatch_borrowed_base.gti");
  expect(!rejected.canGenerateCode() &&
             std::any_of(rejected.diagnostics.begin(),
                         rejected.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2045";
                         }),
         "inherited borrowed state keeps its existing source rejection");
}
} // namespace

int main() {
  const auto frontend =
      Frontend().analyze("llvm-dispatch-plan.gti", std::string(source));
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  testPlans(frontend);
  testConstructorState(frontend);
  testExistingBaseBoundaries();
  if (failures)
    return 1;
  std::cout << "LLVM dispatch prerequisite plans and mutations passed\n";
}
