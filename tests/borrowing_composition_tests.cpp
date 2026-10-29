#include "compiler_test_fixture.h"

namespace {

lang::FrontendResult analyzeBorrowFixture(std::string_view name) {
  const auto path =
      standardLibraryRoot().parent_path() / "tests/fixtures" / name;
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  expect(!source.empty(), "borrow fixture is available: " + std::string(name));
  return lang::Frontend().analyze(path, source, {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

lang::MirInstruction *instructionFor(lang::MirBody &body,
                                     lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.id == id)
        return &instruction;
  return nullptr;
}

const lang::MirInstruction *producerFor(const lang::MirBody &body,
                                        lang::MirValueId id) {
  const auto *value = body.findValue(id);
  if (value)
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == value->definition)
          return &instruction;
  return nullptr;
}

void checkNestedConstructorArguments() {
  const auto frontend = analyzeBorrowFixture("nested_carrier_arguments.gti");
  expect(frontend.canGenerateCode(),
         "nested constructed arguments retain exact reordered formal origins");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "nested constructor publications independently verify");
  bool shared = false;
  bool exclusive = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.constructorTarget ||
            instruction.borrowFields.size() != 2 ||
            instruction.borrowFields.front().resultFields.size() < 3)
          continue;
        (instruction.borrowAccess == lang::AccessMode::Mutable ? exclusive
                                                               : shared) = true;
        const auto reject = [&](const auto &mutate,
                                const std::string &message) {
          auto forged = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          mutate(*instructionFor(instance->body, instruction.id));
          expect(!lang::verifyMirProgram(forged).valid(), message);
        };
        reject(
            [](auto &call) {
              std::swap(call.borrowFields[0].parameter,
                        call.borrowFields[1].parameter);
            },
            "nested constructor cannot forge reordered parameter origins");
        reject(
            [](auto &call) {
              std::swap(call.borrowFields[0].resultFields,
                        call.borrowFields[1].resultFields);
            },
            "nested constructor cannot relabel destination field paths");
        reject(
            [](auto &call) {
              call.borrowFields[0].sourceFields =
                  call.borrowFields[0].resultFields;
            },
            "nested constructor cannot invent an input field path");
        reject(
            [](auto &call) {
              call.borrowFields[0].access =
                  call.borrowFields[0].access == lang::AccessMode::Mutable
                      ? lang::AccessMode::ReadOnly
                      : lang::AccessMode::Mutable;
            },
            "nested constructor cannot change the selected field permission");
      }
  expect(shared && exclusive,
         "nested origin forgeries exercise both access modes");
}

void checkNestedResourceConstruction() {
  const auto frontend =
      analyzeBorrowFixture("source_resource_prefix_cleanup.gti");
  expect(
      frontend.canGenerateCode(),
      "nested resource construction preserves exact parent-loan publication");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "nested resource publication independently verifies");
}

void checkNestedOwningArguments() {
  const auto frontend =
      analyzeBorrowFixture("nested_owning_constructor_arguments.gti");
  expect(frontend.canGenerateCode(),
         "unrelated nested owning arguments preserve source ancestry");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "nested owning arguments independently verify");
  for (const auto &constructor : frontend.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &call : block.instructions) {
        const auto *result =
            call.loan ? constructor.body.findLoan(*call.loan) : nullptr;
        if (call.kind != lang::MirInstructionKind::Construct || !result ||
            !result->lifecycleBound || result->parent == 0 ||
            call.operands.empty())
          continue;
        const auto *stage =
            producerFor(constructor.body, call.operands.front().value);
        if (!stage ||
            stage->callInputKind != lang::HirCallInputKind::MoveValue ||
            !stage->preparedParameterDrop || stage->operands.size() != 1)
          continue;
        const auto *argument =
            producerFor(constructor.body, stage->operands.front().value);
        const auto *retained = argument && argument->loan
                                   ? constructor.body.findLoan(*argument->loan)
                                   : nullptr;
        if (!argument ||
            argument->kind != lang::MirInstructionKind::Construct ||
            !retained || !retained->lifecycleBound ||
            retained->parent != result->parent)
          continue;
        const auto other =
            std::find_if(constructor.body.loans.begin(),
                         constructor.body.loans.end(), [&](const auto &loan) {
                           return loan.entry &&
                                  loan.kind == lang::MirLoanKind::Stored &&
                                  loan.source != retained->source;
                         });
        if (other == constructor.body.loans.end())
          continue;
        const auto reject = [&](const auto &mutate,
                                const std::string &message) {
          auto forged = frontend.mir;
          auto *instance = const_cast<lang::MirConstructorInstance *>(
              forged.findConstructorInstance(constructor.id));
          mutate(instance->body);
          expect(!lang::verifyMirProgram(forged).valid(), message);
        };
        reject(
            [&](auto &body) {
              body.loans[retained->id - 1].parent = other->id;
            },
            "fresh owning argument cannot substitute another active parent");
        reject(
            [&](auto &body) {
              body.loans[retained->id - 1].source = other->source;
            },
            "fresh owning argument cannot substitute another formal source");
        reject(
            [&](auto &body) {
              body.loans[retained->id - 1].access = lang::AccessMode::Mutable;
            },
            "fresh owning argument cannot upgrade retained access");
        reject(
            [&](auto &body) {
              instructionFor(body, stage->id)->preparedParameterDrop.reset();
            },
            "fresh owning argument requires its exact prepared parameter drop");
        reject(
            [&](auto &body) {
              instructionFor(body, stage->id)->callInputKind =
                  lang::HirCallInputKind::Value;
            },
            "fresh owning argument requires a checked moving parameter stage");
        reject(
            [&](auto &body) {
              std::erase_if(
                  instructionFor(body, call.id)->lifecycle,
                  [&](const auto &event) {
                    return event.kind ==
                               lang::MirLifecycleEventKind::TransferOut &&
                           event.source == *stage->preparedParameterDrop;
                  });
            },
            "fresh owning argument requires the exact callee ownership "
            "handoff");
        bool checkedFailure = false;
        for (const auto &failure : constructor.body.failureRecords) {
          if (failure.producerInstruction != call.id)
            continue;
          const auto *failed =
              constructor.body.findBlock(failure.parameterBlock);
          const auto endpoint = std::find_if(
              failed->instructions.begin(), failed->instructions.end(),
              [&](const auto &instruction) {
                return instruction.kind ==
                           lang::MirInstructionKind::EndBorrow &&
                       instruction.loan == retained->id;
              });
          expect(endpoint != failed->instructions.end(),
                 "failed outer construction retires its consumed child before "
                 "parent cleanup");
          reject(
              [&](auto &body) {
                std::erase_if(
                    body.blocks[failure.parameterBlock - 1].instructions,
                    [&](const auto &instruction) {
                      return instruction.loan == retained->id;
                    });
              },
              "failed construction cannot leave its consumed child active "
              "during parent retirement");
          checkedFailure = true;
        }
        expect(checkedFailure, "fresh owning ancestry tests exercise the "
                               "constructor failure edge");
        return;
      }
  expect(false, "fresh owning ancestry forgeries exercise two independent "
                "formal origins");
}

void checkNestedArgumentRejections() {
  const auto reject = [](const lang::FrontendResult &frontend,
                         std::string_view code, const std::string &message) {
    const bool rejected =
        !frontend.canGenerateCode() &&
        std::any_of(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                    [&](const auto &diagnostic) {
                      return diagnostic.severity ==
                                 lang::DiagnosticSeverity::Error &&
                             diagnostic.code == code;
                    });
    if (!rejected)
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(rejected, message);
    expect(std::none_of(frontend.diagnostics.begin(),
                        frontend.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code == "GTI-B0001";
                        }),
           message + " is a source diagnostic, not an internal compiler error");
  };
  reject(analyzeBorrowFixture("nested_carrier_argument_loan_rejected.gti"),
         "GTI-S2017",
         "nested shared arguments retain the reordered second owner");
  reject(analyzeBorrowFixture("nested_carrier_argument_alias_rejected.gti"),
         "GTI-S2045", "nested exclusive arguments cannot duplicate an owner");
  const auto owning = lang::Frontend().analyze(
      "nested-owning-owner-write.gti", R"(
import std.utility;
struct View {
  int& value;
  View(int& value) : value(value) {}
};
class Lease {
  View view;
public:
  Lease(View view) : view(view) {}
  ~Lease() { int observed = this.view.value; }
};
class Buffer {
  Lease lease;
public:
  Buffer(Lease lease) : lease(std::move(lease)) {}
};
class Owner {
  Buffer buffer;
public:
  Owner(int& value) : buffer(Buffer(Lease(View(value)))) {}
};
int main() {
  mut int resource = 7;
  Owner owner{resource};
  resource = 9;
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  reject(owning, "GTI-S2017",
         "nested owning arguments retain their source through destruction");
}

void checkIndependentDependencies() {
  const auto frontend = lang::Frontend().analyze("composed-borrows.gti", R"(
struct View {
  int& first;
  int& second;
  View(int& a, int& b) : first(a), second(b) {}
  int sum() { return first + second; }
};
View relay(View value) { return value; }
View factory(int& first, int& second) { return View(first, second); }
View same_owner(int& first, int& unused) { return View(first, first); }
int main() {
  int a = 20; int b = 22;
  View view{a, b};
  auto copy = view;
  auto repeated = same_owner(a, b);
  return copy.sum() + repeated.sum() - 82;
}
)");
  expect(frontend.canGenerateCode(),
         "user-defined two-owner carrier should pass complete MIR lowering");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "composed owner dependencies should independently verify");
  const auto semantic = std::find_if(
      frontend.semantics.loans().begin(), frontend.semantics.loans().end(),
      [](const lang::SemanticLoanInfo &loan) {
        return !loan.entry && !loan.additionalDependencies.empty();
      });
  expect(semantic != frontend.semantics.loans().end(),
         "semantic loan must retain the second independent place");
  if (semantic != frontend.semantics.loans().end()) {
    expect(semantic->place.root !=
               semantic->additionalDependencies.front().place.root,
           "independent owners must not collapse into a fabricated root");
    expect(semantic->carriers.size() == 2,
           "copy construction must retain both aliases of the dependency set");
  }
  bool inspected = false;
  bool inspectedPaths = false;
  bool inspectedEffects = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.returnBorrowParameters.empty()) {
      inspectedEffects = true;
      expect(function.effectSummary.argumentsEscape ==
                 std::vector<bool>({true, true}),
             "effect summaries must retain both zero-based parameter escapes");
    }
    for (const auto &loan : function.body.loans) {
      if (loan.entry && !loan.additionalDesignatedFields().empty()) {
        inspectedPaths = true;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1].additionalDependencies.clear();
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "MIR must reject dropping an opaque parameter's second field path");
        malformed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1]
            .additionalDependencies.front()
            .access = lang::AccessMode::Mutable;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "MIR must reject upgrading a shared stored field's permission");
      }
      if (loan.additionalSources().empty()) {
        continue;
      }
      inspected = true;
      auto malformed = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].additionalDependencies.clear();
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject erasing an independent retained owner");
      malformed = frontend.mir;
      instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].additionalDependencies.front().source =
          0;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject an unresolved independent owner place");
      if (!loan.entry && !loan.designatedFields.empty()) {
        malformed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1]
            .additionalDependencies.front()
            .designatedFields.clear();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "MIR must reject losing a local field-to-owner identity");
        malformed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1]
            .additionalDependencies.front()
            .designatedFields = loan.designatedFields;
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "MIR must reject relabeling the second owner as the first field");
      }
      break;
    }
  }
  expect(inspected, "MIR must retain an explicit multi-owner dependency set");
  expect(inspectedPaths, "MIR must retain all opaque parameter field paths");
  expect(inspectedEffects, "factory must publish its complete escape summary");
}

void checkMixedDependencies() {
  const auto frontend = lang::Frontend().analyze("mixed-services.gti", R"(
struct Services {
  int& database;
  mut int& renderer;
  int& logger;
  mut int& cache;
  int& clock;
  Services(int& db, mut int& render, int& log, mut int& entries, int& time)
      : database(db), renderer(render), logger(log), cache(entries), clock(time) {}
  int update() mut {
    renderer = database + logger;
    cache = renderer + clock;
    return cache;
  }
};
int main() {
  int database = 10;
  mut int renderer = 0;
  int logger = 20;
  mut int cache = 0;
  int clock = 12;
  mut Services services{database, renderer, logger, cache, clock};
  return services.update() - 42;
}
)");
  expect(frontend.canGenerateCode(),
         "five mixed borrowed fields should retain independent permissions");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "mixed stored dependencies must pass independent MIR verification");
  const auto loan = std::find_if(
      frontend.semantics.loans().begin(), frontend.semantics.loans().end(),
      [](const lang::SemanticLoanInfo &candidate) {
        return !candidate.entry && candidate.additionalDependencies.size() == 4;
      });
  expect(loan != frontend.semantics.loans().end(),
         "all five independent dependencies must survive semantic analysis");
  if (loan != frontend.semantics.loans().end()) {
    expect(loan->access == lang::AccessMode::ReadOnly &&
               loan->additionalDependencies[0].access ==
                   lang::AccessMode::Mutable &&
               loan->additionalDependencies[1].access ==
                   lang::AccessMode::ReadOnly &&
               loan->additionalDependencies[2].access ==
                   lang::AccessMode::Mutable &&
               loan->additionalDependencies[3].access ==
                   lang::AccessMode::ReadOnly,
           "a mixed carrier must not flatten its fields to one access mode");
  }
}

void checkMixedAccessRules() {
  const std::string declarations = R"(
struct Mixed {
  int& read;
  mut int& write;
  mut int& other;
  Mixed(int& a, mut int& b, mut int& c) : read(a), write(b), other(c) {}
  int update() mut { write = read; other = write + 1; return other; }
};
struct MutableFirst {
  mut int& write;
  int& read;
  mut int& other;
  MutableFirst(mut int& a, int& b, mut int& c) : write(a), read(b), other(c) {}
  int update() mut { write = read; other = write + 1; return other; }
};
struct State { mut int a = 1; mut int b = 2; mut int c = 3; State() = default; };
int main() {
  mut int a = 1; mut int b = 2; mut int c = 3;
)";
  const auto check = [&](std::string_view body, bool valid,
                         std::string_view description) {
    const auto frontend = lang::Frontend().analyze(
        "mixed-access.gti", declarations + std::string(body) + "\n}");
    const bool semanticError =
        std::any_of(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                    [](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code.starts_with("GTI-S");
                    });
    expect(valid ? frontend.canGenerateCode() : semanticError,
           std::string(description));
    if ((valid && !frontend.canGenerateCode()) || (!valid && !semanticError))
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << description << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
  };
  check("mut Mixed v{a, b, c}; int read = a; return v.update() + read;", true,
        "shared owner stays readable beside exclusive dependencies");
  check("int result = Mixed(a, b, c).update(); b = 7; c = 8; return result;",
        true,
        "an immediate temporary method call retains all mixed dependencies");
  check("mut Mixed v{a, b, c}; int read = c; return v.update() + read;", false,
        "later exclusive dependency prevents direct owner reads");
  check("mut Mixed v{a, b, c}; c = 9; return v.update();", false,
        "later exclusive dependency prevents competing owner writes");
  check("mut Mixed v{a, a, c}; return v.update();", false,
        "overlapping shared and exclusive constructor inputs are rejected");
  check("mut Mixed v{a, b, b}; return v.update();", false,
        "overlapping exclusive constructor inputs are rejected");
  check("mut Mixed v{a, b, c}; auto copied = v; return copied.update();", false,
        "a shared first field cannot make an exclusive carrier copyable");
  check("mut Mixed v{a, b, c}; int result = v.update(); b = 10; c = 20; return "
        "result;",
        true, "all independent owners reactivate after the carrier's last use");
  check("mut MutableFirst v{b, a, c}; int read = a; return v.update() + read;",
        true, "mutable first field does not make a shared sibling exclusive");
  check("mut State state{}; mut Mixed v{state.a, state.b, state.c}; return "
        "v.update();",
        true, "disjoint fields of one owner can supply mixed dependencies");
  check("mut Mixed v{a, b, c}; if (a > 0) { [[discard]] v.update(); } "
        "else { [[discard]] v.update(); } b = 7; c = 8; return b + c;",
        true, "branch-joined last uses reactivate all exclusive owners");
  check("for (mut int i = 0; i < 2; i++) { mut Mixed v{a, b, c}; "
        "[[discard]] v.update(); } b = 7; c = 8; return b + c;",
        true, "loop-local mixed loans end before the next iteration");
  check("mut Mixed v{a, b, c}; if (a > 0) { c = 7; } "
        "return v.update();",
        false, "a later carrier use retains every dependency across branches");
  check("for (mut int i = 0; i < 2; i++) { mut Mixed v{a, b, c}; "
        "c = 7; [[discard]] v.update(); } return 0;",
        false,
        "loop-local borrowed owners cannot be mutated before the final use");
  check("mut Mixed v{a, b, c}; [[discard]] v.update(); "
        "for (mut int i = 0; i < 2; i++) { mut Mixed next{a, b, c}; "
        "[[discard]] next.update(); } return b + c;",
        true, "entering a later loop does not revive ended outer dependencies");
  check("mut Mixed v{a, b, c}; "
        "for (mut int i = 0; i < 2; i++) { c = 7; [[discard]] v.update(); } "
        "return 0;",
        false, "a dependency used inside a loop survives its backedge");
  check("mut Mixed v{a, b, c}; [[discard]] v.update(); "
        "for (mut int i = 0; i < 2; i++) { c = 7; } return v.update();",
        false, "a dependency used after the loop cannot end before loop entry");
}

void checkFieldDependencyIdentity() {
  const auto shared = lang::Frontend().analyze("repeated-shared-fields.gti", R"(
struct Repeated {
  int& first;
  int& second;
  int& third;
  Repeated(int& value) : first(value), second(value), third(value) {}
  int sum() { return first + second + third; }
};
int main() { int value = 14; Repeated view{value}; return view.sum() - 42; }
)");
  expect(
      shared.canGenerateCode(),
      "one shared constructor parameter may initialize distinct stored fields");
  if (!shared.canGenerateCode())
    for (const auto &diagnostic : shared.diagnostics)
      std::cerr << "repeated shared: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  const auto loan = std::find_if(
      shared.semantics.loans().begin(), shared.semantics.loans().end(),
      [](const auto &candidate) {
        return !candidate.entry && candidate.additionalDependencies.size() == 2;
      });
  expect(loan != shared.semantics.loans().end(),
         "repeated shared origins must retain all three field identities");
  if (loan != shared.semantics.loans().end()) {
    expect(loan->designatedFields.size() == 1 &&
               loan->additionalDependencies[0].designatedFields.size() == 1 &&
               loan->additionalDependencies[1].designatedFields.size() == 1 &&
               loan->designatedFields !=
                   loan->additionalDependencies[0].designatedFields &&
               loan->additionalDependencies[0].designatedFields !=
                   loan->additionalDependencies[1].designatedFields &&
               loan->place == loan->additionalDependencies[0].place &&
               loan->place == loan->additionalDependencies[1].place,
           "field identity must not be deduplicated by owner identity");
  }

  const auto exclusive = lang::Frontend().analyze("duplicate-exclusive.gti", R"(
struct Duplicate {
  mut int& first;
  mut int& second;
  Duplicate(mut int& value) : first(value), second(value) {}
};
int main() { mut int value = 0; Duplicate view{value}; return 0; }
)");
  expect(std::any_of(exclusive.diagnostics.begin(), exclusive.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2045" &&
                              diagnostic.message.find(
                                  "duplicate an exclusive") !=
                                  std::string::npos;
                     }),
         "duplicate exclusive fields must be rejected at the source contract");
  expect(std::none_of(exclusive.diagnostics.begin(),
                      exclusive.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "invalid exclusive field composition must never reach MIR as an ICE");
}

void checkNestedMixedTransfer() {
  const auto differingPermissions = lang::Frontend().analyze(
      "nested-field-permissions.gti", R"(
import std.utility;
struct Exclusive {
  mut int& value;
  Exclusive(mut int& source) : value(source) {}
};
struct Combined {
  int& input;
  mut Exclusive output;
  Combined(int& source, Exclusive target)
      : input(source), output(std::move(target)) {}
  int run() mut { output.value = input; return output.value; }
};
int main() {
  int a = 42; mut int b = 0;
  Exclusive item{b};
  mut Combined group{a, std::move(item)};
  return group.run() - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(differingPermissions.canGenerateCode(),
         "nested publication uses the nested field's permission, not the "
         "outer carrier's first dependency");
  if (differingPermissions.canGenerateCode()) {
    const auto cpp = gti_test::emitCppText(differingPermissions);
    expect(!cpp.empty(),
           "backend resolves nested receiver reference projections");
    std::size_t constructorBorrows = 0;
    for (const auto &function : differingPermissions.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &stage : block.instructions) {
          if (stage.kind != lang::MirInstructionKind::CallInput ||
              !stage.result ||
              stage.callInputRole != lang::MirCallInputRole::Argument ||
              stage.callInputIndex != 0 || stage.operands.size() != 1 ||
              (stage.operands[0].kind != lang::MirOperandKind::BorrowRead &&
               stage.operands[0].kind != lang::MirOperandKind::BorrowWrite))
            continue;
          for (const auto &use : function.body.usesOf(*stage.result)) {
            const auto *consumerBlock = function.body.findBlock(use.block);
            if (!consumerBlock)
              continue;
            for (const auto &consumer : consumerBlock->instructions) {
              if (consumer.id != use.instruction ||
                  consumer.kind != lang::MirInstructionKind::Construct ||
                  consumer.operands.size() != 2)
                continue;
              ++constructorBorrows;
              expect(cpp.find(".construct(__gti_mir_v_" +
                              std::to_string(*stage.result) + ",") ==
                         std::string::npos,
                     "constructor publishes exact borrowed referent, not an "
                     "erased SSA stage");
            }
          }
        }
    expect(constructorBorrows == 1, "mixed constructor regression checks its "
                                    "borrowed argument publication");
    bool inspected = false;
    for (const auto &function : differingPermissions.mir.functionInstances()) {
      for (const auto &place : function.body.places) {
        if (place.root != lang::MirPlaceRootKind::This ||
            place.projections.size() != 3 ||
            place.projections[0].kind != lang::MirProjectionKind::Field ||
            place.projections[1].kind != lang::MirProjectionKind::Field ||
            place.projections[2].kind != lang::MirProjectionKind::Dereference)
          continue;
        inspected = true;
        auto changed = differingPermissions.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.places[place.id - 1].projections.pop_back();
        bool rejected = false;
        try {
          static_cast<void>(
              gti_test::emitCpp(differingPermissions, changed, changed));
        } catch (const std::logic_error &) {
          rejected = true;
        }
        expect(rejected,
               "backend cannot erase a nested reference dereference step");
      }
    }
    expect(inspected, "nested receiver regression exercises its exact path");
  }
  const std::string declarations = R"(
import std.utility;
struct Mixed {
  int& read;
  mut int& write;
  Mixed(int& a, mut int& b) : read(a), write(b) {}
  int update() mut { write = read; return write; }
};
struct Envelope<T> {
  mut T payload;
  Envelope(T value) : payload(std::move(value)) {}
};
int main() {
  int a = 42; mut int b = 0;
  Mixed view{a, b};
  mut Envelope<Mixed> outer{std::move(view)};
)";
  const auto analyze = [&](std::string_view body) {
    return lang::Frontend().analyze(
        "nested-mixed-transfer.gti", declarations + std::string(body) + "\n}",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  auto valid = analyze("return outer.payload.update() - 42;");
  expect(valid.canGenerateCode(),
         "a generic nominal wrapper can retain every moved mixed dependency");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << "nested mixed: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  for (const auto &constructor : valid.mir.constructorInstances()) {
    for (const auto &loan : constructor.body.loans) {
      if (!loan.escapes || loan.additionalDependencies.empty())
        continue;
      auto changed = valid.mir;
      auto *instance = const_cast<lang::MirConstructorInstance *>(
          changed.findConstructorInstance(constructor.id));
      instance->body.loans[loan.id - 1].additionalDependencies.clear();
      expect(!lang::verifyMirProgram(changed).valid(),
             "nested field publication cannot discard a secondary dependency");
    }
  }
  const auto invalid = analyze("b = 9; return outer.payload.update();");
  expect(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2017";
                     }),
         "a moved nested carrier still excludes access to its later owner");
}

void checkProjectedMixedReborrows() {
  const std::string declarations = R"(
struct Mixed {
  int& read;
  mut int& write;
  mut int& other;
  Mixed(int& a, mut int& b, mut int& c) : read(a), write(b), other(c) {}
};
int main() {
  mut int a = 42; mut int b = 0; mut int c = 3;
  mut Mixed view{a, b, c};
)";
  const auto analyze = [&](std::string_view body) {
    return lang::Frontend().analyze("projected-mixed-reborrow.gti",
                                    declarations + std::string(body) + "\n}");
  };
  for (const auto body :
       {"mut int& selected = view.write; selected = 42; return b - 42;",
        "int& selected = view.read; return selected - 42;",
        "int& first = view.read; int& second = view.read; "
        "return first + second + view.read - 126;",
        "mut int& selected = view.write; view.other = 3; "
        "selected = view.read; return b + c - 45;",
        "mut int& first = view.write; mut int& second = view.other; "
        "first = 42; second = 3; return b + c - 45;"}) {
    const auto valid = analyze(body);
    expect(valid.canGenerateCode(),
           "a projected dependency carries its own access and owner");
    if (!valid.canGenerateCode())
      for (const auto &diagnostic : valid.diagnostics)
        std::cerr << "projected mixed: " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    if (valid.canGenerateCode())
      for (const auto &function : valid.mir.functionInstances())
        for (const auto &loan : function.body.loans) {
          if (loan.parent == 0 || loan.access != lang::AccessMode::Mutable)
            continue;
          auto changed = valid.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &parent = instance->body.loans[loan.parent - 1];
          for (auto &dependency : parent.additionalDependencies)
            dependency.access = lang::AccessMode::ReadOnly;
          expect(!lang::verifyMirProgram(changed).valid(),
                 "a mutable child cannot obtain authority from shared "
                 "dependencies");
        }
  }
  for (const auto body :
       {"mut int& selected = view.write; b = 9; return selected;",
        "mut int& selected = view.write; view.write = 9; return selected;",
        "mut int& first = view.write; mut int& second = view.write; "
        "first = 1; return second;",
        "mut int& selected = view.read; selected = 1; return selected;"}) {
    const auto invalid = analyze(body);
    expect(
        !invalid.canGenerateCode() &&
            std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code == "GTI-S2017";
                        }),
        "a projected borrow rejects conflicting access or permission upgrades");
  }
}

void checkMixedReturnOrigins() {
  const auto frontend = lang::Frontend().analyze("mixed-return-origins.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  int& read() { return this.first; }
  mut int& write() mut { return this.second; }
};
Services factory(mut int& last, int& first, mut int& middle) {
  return Services(first, middle, last);
}

int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  mut auto services = factory(c, a, b);
  mut int& target = services.write();
  target = 40;
  int first = services.read();
  return first + b + c - 82;
}
)");
  expect(frontend.canGenerateCode(), "mixed factories and accessors preserve "
                                     "their exact returned field origins");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "mixed returns: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  bool forgedReturn = false;
  bool erasedConstruction = false;
  if (frontend.canGenerateCode())
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (!instruction.constructorTarget ||
              instruction.borrowFields.empty())
            continue;
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : instance->body.blocks)
            for (auto &changedInstruction : changedBlock.instructions)
              if (changedInstruction.id == instruction.id)
                changedInstruction.borrowFields.clear();
          erasedConstruction = true;
          expect(
              !lang::verifyMirProgram(malformed).valid(),
              "constructor publication cannot omit its borrowed field mapping");
        }
      if (function.owner || function.returnBorrowFields.size() != 3)
        continue;
      forgedReturn = true;
      auto changed = frontend.mir;
      auto *factory = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id));
      factory->returnBorrowFields[1].parameter =
          factory->returnBorrowFields[2].parameter;
      for (const auto &caller : changed.functionInstances()) {
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(caller.id));
        for (auto &block : instance->body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.functionTarget == function.id)
              instruction.borrowFields = factory->returnBorrowFields;
      }
      expect(!lang::verifyMirProgram(changed).valid(),
             "matching caller/callee metadata cannot forge the callee's "
             "returned field owner");
      bool forgedNonCall = false;
      auto malformed = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      for (auto &block : instance->body.blocks)
        for (auto &instruction : block.instructions) {
          if (forgedNonCall || instruction.tombstone ||
              instruction.kind == lang::MirInstructionKind::Call ||
              instruction.kind == lang::MirInstructionKind::Construct)
            continue;
          instruction.borrowFields = function.returnBorrowFields;
          forgedNonCall = true;
        }
      expect(forgedNonCall && !lang::verifyMirProgram(malformed).valid(),
             "non-call instructions cannot smuggle returned-field contracts");
    }
  expect(forgedReturn, "return-contract forgery exercised a mapped factory");
  expect(erasedConstruction,
         "constructor-contract mutation exercised a publication");
}

void checkMixedReturnComposition() {
  const auto frontend = lang::Frontend().analyze(
      "mixed-return-composition.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  int& read() { return this.first; }
  mut int& write() mut { return this.second; }
};
Services relay(Services value) { return std::move(value); }
int inspect(Services& value) { return value.read(); }
int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  Services original{a, b, c};
  mut Services moved = relay(std::move(original));
  mut int& target = moved.write();
  target = 40;
  int first = inspect(moved);
  return first + b + c - 82;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "mixed borrowed aggregates compose through value relay and reference "
         "parameters");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "mixed composition: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
}

void checkGenericMixedReturnComposition() {
  const auto frontend = lang::Frontend().analyze("generic-mixed-return.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
};
T relay<T>(T value) { return std::move(value); }
struct Wrapper<T> {
  T& first;
  mut T& second;
  mut T& third;
  Wrapper(T& a, mut T& b, mut T& c) : first(a), second(b), third(c) {}
  T& read() { return this.first; }
  mut T& write() mut { return this.second; }
};
int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  Services original{a, b, c};
  mut Services moved = relay<Services>(std::move(original));
  moved.second = moved.first;
  mut Wrapper<int> wrapped{a, b, c};
  int source = wrapped.read();
  mut int& destination = wrapped.write();
  destination = source;
  return b + c - 42;
}
)",
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "concrete generic relay retains each "
                                     "borrowed field's origin and permission");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "generic mixed return: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
}

void checkCompactInputComposition() {
  const auto unsupported =
      lang::Frontend().analyze("single-input-reshape.gti", R"(
struct Reading {
  int& value;
  Reading(int& source) : value(source) {}
};
struct Readings {
  Reading first;
  Reading second;
  Readings(Reading left, Reading right) : first(left), second(right) {}
};
Readings repeat(Reading input) { return Readings(input, input); }
)");
  expect(!unsupported.canGenerateCode() &&
             std::any_of(unsupported.diagnostics.begin(),
                         unsupported.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2085";
                         }),
         "single-input by-value return remains bounded to its exact carrier "
         "dependency");
  const auto frontend =
      lang::Frontend().analyze("compact-input-composition.gti", R"(
struct Reading {
  int& value;
  Reading(int& source) : value(source) {}
};
struct Readings {
  Reading first;
  Reading second;
  Readings(Reading left, Reading right) : first(left), second(right) {}
};
Readings combine(Reading left, Reading right) { return Readings(left, right); }
Readings reverse(Reading left, Reading right) { return Readings(right, left); }
int main() {
  int a = 17; int b = 25;
  Reading left{a}; Reading right{b};
  auto combined = combine(left, right);
  auto reversed = reverse(left, right);
  auto repeated = combine(left, left);
  return combined.first.value + reversed.first.value + repeated.second.value - 59;
}
)");
  expect(frontend.canGenerateCode(),
         "composing compact inputs preserves each exact external identity");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "compact composition: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    return;
  }
  bool inspected = false;
  bool mutatedIndependentSources = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.constructorTarget || !instruction.loan ||
            instruction.borrowFields.size() != 2)
          continue;
        const auto *result = function.body.findLoan(*instruction.loan);
        if (result == nullptr || result->additionalDependencies.size() != 1)
          continue;
        const auto entry =
            std::find_if(function.body.loans.begin(), function.body.loans.end(),
                         [&](const auto &loan) {
                           return loan.entry &&
                                  loan.kind == lang::MirLoanKind::Stored &&
                                  loan.source == result->source;
                         });
        if (entry == function.body.loans.end())
          continue;
        inspected = true;
        expect(
            function.body.findPlace(result->source)->projections.empty(),
            "compact input retains its opaque entry rather than shell fields");
        if (result->source == result->additionalDependencies.front().source)
          continue;
        mutatedIndependentSources = true;
        auto changed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.loans[result->id - 1].source =
            result->additionalDependencies.front().source;
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot substitute the other compact input");
        changed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.loans[result->id - 1]
            .additionalDependencies.front()
            .source = result->source;
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot collapse its second independent input");
        changed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        for (auto &changedBlock : instance->body.blocks)
          for (auto &changedInstruction : changedBlock.instructions)
            if (changedInstruction.id == instruction.id)
              std::swap(changedInstruction.borrowFields[0].parameter,
                        changedInstruction.borrowFields[1].parameter);
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot forge reordered constructor input "
               "mappings");
        changed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        for (auto &changedBlock : instance->body.blocks)
          for (auto &changedInstruction : changedBlock.instructions)
            if (changedInstruction.id == instruction.id)
              std::swap(changedInstruction.borrowFields[0].resultFields,
                        changedInstruction.borrowFields[1].resultFields);
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot relabel destination field identities");
      }
  expect(inspected,
         "compact input composition exercised an entry-backed result");
  expect(mutatedIndependentSources,
         "compact input mutations exercised distinct external owners");
}

void checkGetterWithOwnedDependencies() {
  const auto fixture = standardLibraryRoot().parent_path() /
                       "tests/fixtures/shared_getter_worker_loans.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  lang::FrontendOptions options;
  options.target.executionProfile = lang::ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  const auto analyze = [&](const std::string &text) {
    return lang::Frontend(options).analyze(
        fixture, text, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  const auto frontend = analyze(source);
  expect(frontend.canGenerateCode(),
         "getter authority composes with independent owned local dependencies");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t tested = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || loan.parent == 0 ||
          loan.entry || loan.escapes || loan.additionalDependencies.size() != 1)
        continue;
      const auto *additional =
          function.body.findPlace(loan.additionalDependencies.front().source);
      if (!additional || additional->root != lang::MirPlaceRootKind::Binding ||
          loan.additionalDependencies.front().access !=
              lang::AccessMode::Mutable)
        continue;
      ++tested;
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        auto &changed = body->loans[loan.id - 1];
        if (mutation == 0)
          changed.additionalDependencies.front().source = changed.source;
        else if (mutation == 1)
          changed.additionalDependencies.front().access =
              lang::AccessMode::ReadOnly;
        else if (mutation == 2)
          changed.additionalDependencies.clear();
        else if (mutation == 3)
          changed.parent = 0;
        else if (mutation == 4)
          body->loans[changed.parent - 1].access = lang::AccessMode::Mutable;
        else
          for (auto &place : body->places)
            if (place.id == additional->id)
              place.projections.push_back(
                  {.kind = lang::MirProjectionKind::Dereference});
        expect(!lang::verifyMirProgram(malformed).valid(),
               "independent input cannot forge source, access, dependency, "
               "parent or pointer authority: " +
                   std::to_string(mutation));
      }
    }
  }
  expect(tested >= 3,
         "named, temporary and projected owned inputs are checked");
  auto overlapping = source;
  const std::string distinct = "Task(owner.get(), second)";
  const auto other = overlapping.find(distinct);
  expect(other != std::string::npos,
         "parallel output mutation finds its input");
  if (other != std::string::npos)
    overlapping.replace(other, distinct.size(), "Task(owner.get(), first)");
  const auto alias = analyze(overlapping);
  expect(!alias.semanticValid &&
             std::none_of(alias.diagnostics.begin(), alias.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code == "GTI-B0001";
                          }),
         "shared getter does not permit overlapping mutable worker outputs");
  auto ownerWrite = source;
  const auto run = ownerWrite.find("    named();");
  expect(run != std::string::npos, "owner write mutation finds retained use");
  if (run != std::string::npos)
    ownerWrite.insert(run, "    sequential = 9;\n");
  const auto conflict = analyze(ownerWrite);
  expect(!conflict.semanticValid && std::none_of(conflict.diagnostics.begin(),
                                                 conflict.diagnostics.end(),
                                                 [](const auto &diagnostic) {
                                                   return diagnostic.code ==
                                                          "GTI-B0001";
                                                 }),
         "independent local remains protected until the retained carrier ends");
}

void checkMixedOperatorOrigins() {
  const auto analyze = [](std::string_view body) {
    return lang::Frontend().analyze("mixed-operator.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  mut int& operator*() mut { return this.second; }
};
int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  mut Services value{a, b, c};
)" + std::string(body) + "\n}");
  };
  const auto valid =
      analyze("mut int& selected = *value; selected = 40; return b + c - 42;");
  expect(
      valid.canGenerateCode(),
      "operator return preserves the selected field, not the first dependency");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << "mixed operator: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  const auto invalid =
      analyze("mut int& selected = *value; b = 9; return selected;");
  expect(!invalid.canGenerateCode() &&
             std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2017";
                         }),
         "operator-derived borrow keeps its exact owner protected");
}

constexpr NamedTest kTests[] = {
    {"nested_constructor_arguments", checkNestedConstructorArguments},
    {"nested_resource_construction", checkNestedResourceConstruction},
    {"nested_owning_arguments", checkNestedOwningArguments},
    {"nested_argument_rejections", checkNestedArgumentRejections},
    {"independent_dependencies", checkIndependentDependencies},
    {"mixed_dependencies", checkMixedDependencies},
    {"mixed_access_rules", checkMixedAccessRules},
    {"field_dependency_identity", checkFieldDependencyIdentity},
    {"nested_mixed_transfer", checkNestedMixedTransfer},
    {"projected_mixed_reborrows", checkProjectedMixedReborrows},
    {"mixed_return_origins", checkMixedReturnOrigins},
    {"mixed_return_composition", checkMixedReturnComposition},
    {"generic_mixed_return_composition", checkGenericMixedReturnComposition},
    {"compact_input_composition", checkCompactInputComposition},
    {"mixed_operator_origins", checkMixedOperatorOrigins},
    {"getter_owned_dependencies", checkGetterWithOwnedDependencies},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_borrowing_composition_tests");
  return runNamedTests(argc, argv, kTests, "borrowing composition");
}
