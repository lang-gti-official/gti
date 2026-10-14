#include "compiler_test_fixture.h"

// Prefix `?` through the structural propagation protocol: an unrelated user
// result type proves the general rule, and std::optional is its first library
// client. Semantics selects ordinary members, HIR keeps them as ordinary
// calls, and MIR lowers the residual exit as an early return.

namespace {

constexpr std::string_view kLookupProtocol = R"(
struct Missing {
  int code;
  Missing(int code) : code(code) {}
};

class Lookup {
  int found;
  int code;
public:
  Lookup(int value) : found(value), code(0) {}
  implicit Lookup(Missing missing) : found(0), code(missing.code) {}
  bool has_value() { return this.code == 0; }
  int value() && { return this.found; }
  int value() { return this.found; }
  Missing residual() { return Missing(this.code); }
  int code_of() { return this.code; }
};

Lookup find(int key) {
  if (key > 0) { return Lookup(key * 10); }
  return Lookup(Missing(key));
}

Lookup twice(int key) {
  int found = ?find(key);
  return Lookup(found * 2);
}

Lookup sum(int left, int right) {
  return Lookup(?find(left) + ?find(right));
}

Lookup named(int key) {
  Lookup found = find(key);
  int value = ?found;
  return Lookup(value + found.code_of());
}
)";

[[nodiscard]] lang::FrontendResult analyzeUserProgram(const std::string &name,
                                                      const std::string &body) {
  return lang::Frontend().analyze(name, std::string(kLookupProtocol) + body,
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

void reportDiagnostics(const lang::FrontendResult &frontend,
                       std::string_view context) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << context << ": " << diagnostic.code << ": "
              << diagnostic.message << '\n';
  }
}

[[nodiscard]] const lang::HirFunctionInstance *
functionNamed(const lang::FrontendResult &frontend, std::string_view name) {
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr && function.source->name().lexeme == name) {
      return &function;
    }
  }
  return nullptr;
}

[[nodiscard]] const lang::HirFunctionInstance *
callTarget(const lang::FrontendResult &frontend, const lang::HirBody &body,
           lang::HirValueId id) {
  const lang::HirValue *call = body.findValue(id);
  return call == nullptr || !call->functionTarget
             ? nullptr
             : frontend.hir.findFunctionInstance(*call->functionTarget);
}

struct PropagationCensus {
  std::size_t values = 0;
  std::size_t consumed = 0;
  std::size_t copied = 0;
  std::size_t consumingOutputs = 0;
  bool wellFormed = true;
};

[[nodiscard]] PropagationCensus census(const lang::FrontendResult &frontend,
                                       const lang::HirBody &body) {
  PropagationCensus result;
  for (const lang::HirValue &value : body.values) {
    if (value.kind != lang::HirValueKind::Propagation) {
      continue;
    }
    ++result.values;
    const lang::HirPropagationPlan *plan =
        value.propagation ? &*value.propagation : nullptr;
    if (plan == nullptr) {
      result.wellFormed = false;
      continue;
    }
    result.consumed += plan->consumesOperand;
    result.copied += plan->copiesOutput;
    const lang::HirFunctionInstance *test =
        callTarget(frontend, body, plan->test);
    const lang::HirFunctionInstance *output =
        callTarget(frontend, body, plan->output);
    const lang::HirFunctionInstance *residual =
        callTarget(frontend, body, plan->residual);
    const lang::HirValue *construction = body.findValue(plan->residualReturn);
    if (test == nullptr || output == nullptr || residual == nullptr ||
        test->source == nullptr || output->source == nullptr ||
        residual->source == nullptr || construction == nullptr ||
        test->source->name().lexeme != "has_value" ||
        output->source->name().lexeme != "value" ||
        residual->source->name().lexeme != "residual" ||
        construction->kind != lang::HirValueKind::DirectInitializer ||
        !construction->constructorTarget || value.source == nullptr ||
        value.operation != lang::TokenKind::QUESTION ||
        value.fullExpression == 0) {
      result.wellFormed = false;
      continue;
    }
    result.consumingOutputs += output->source->receiverMutability() ==
                               lang::ReceiverMutability::Consuming;
    // Ordinary calls, never the built-in expected intrinsics.
    for (const lang::HirValueId id : value.operands) {
      const lang::HirValue *step = body.findValue(id);
      if (step == nullptr ||
          step->intrinsic == lang::IntrinsicKind::ExpectedHasValue ||
          step->intrinsic == lang::IntrinsicKind::ExpectedValue ||
          step->intrinsic == lang::IntrinsicKind::ExpectedError) {
        result.wellFormed = false;
      }
    }
  }
  return result;
}

void testUserProtocolPropagation() {
  const lang::FrontendResult frontend =
      analyzeUserProgram("user-propagation.gti", R"(
int main() {
  Lookup a = twice(2);
  Lookup b = sum(1, -5);
  Lookup c = named(4);
  return std::move(a).value() - 40 + b.code_of() + 5 + std::move(c).value() -
         40;
}
)");
  if (!frontend.canGenerateCode()) {
    reportDiagnostics(frontend, "user propagation");
  }
  expect(
      frontend.canGenerateCode() &&
          lang::verifyMirProgram(frontend.mir).valid() &&
          lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
      "protocol propagation on an unrelated user class should reach "
      "verified HIR plans and MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  PropagationCensus total;
  for (const std::string_view name : {"twice", "sum", "named"}) {
    const lang::HirFunctionInstance *function = functionNamed(frontend, name);
    expect(function != nullptr, "the propagating function should be lowered");
    if (function == nullptr) {
      continue;
    }
    const PropagationCensus counted = census(frontend, function->body);
    total.values += counted.values;
    total.consumed += counted.consumed;
    total.copied += counted.copied;
    total.consumingOutputs += counted.consumingOutputs;
    total.wellFormed = total.wellFormed && counted.wellFormed;
  }
  expect(total.wellFormed && total.values == 4 && total.consumed == 3 &&
             total.consumingOutputs == 3 && total.copied == 0,
         "HIR should keep three consumed temporaries with consuming value() "
         "and one observed named operand, all as ordinary selected calls");

  const lang::HirFunctionInstance *sumHir = functionNamed(frontend, "sum");
  const lang::MirFunctionInstance *sumMir =
      sumHir == nullptr ? nullptr
                        : frontend.mir.findFunctionInstance(sumHir->id);
  std::size_t branches = 0;
  std::size_t returns = 0;
  std::size_t earlyBoundaries = 0;
  std::size_t primaryBoundaries = 0;
  bool builtInExpectedOperation = false;
  if (sumMir != nullptr) {
    for (const lang::MirBlock &block : sumMir->body.blocks) {
      branches += block.terminator.kind == lang::MirTerminatorKind::Branch;
      returns += block.terminator.kind == lang::MirTerminatorKind::Return;
      for (const lang::MirInstruction &instruction : block.instructions) {
        builtInExpectedOperation =
            builtInExpectedOperation ||
            instruction.operation == lang::MirOperation::ExpectedHasValue ||
            instruction.intrinsic == lang::IntrinsicKind::ExpectedValue ||
            instruction.intrinsic == lang::IntrinsicKind::ExpectedError;
        if (instruction.fullExpressionEnd == 0) {
          continue;
        }
        if (instruction.earlyExitFullExpressionEnd) {
          ++earlyBoundaries;
        } else {
          ++primaryBoundaries;
        }
      }
    }
  }
  expect(sumMir != nullptr && branches == 2 && returns == 3 &&
             earlyBoundaries == 2 && primaryBoundaries == 1 &&
             !builtInExpectedOperation,
         "MIR should lower two protocol propagations in one expression to "
         "ordered, mutually exclusive early returns and one normal boundary "
         "without expected operations");

  if (sumMir != nullptr) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    bool changed = false;
    for (lang::MirBlock &block : functions.at(sumHir->id - 1).body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (!changed && instruction.earlyExitFullExpressionEnd) {
          instruction.earlyExitFullExpressionEnd = false;
          changed = true;
        }
      }
    }
    const lang::MirVerificationResult result = lang::verifyMirProgram(forged);
    expect(changed && !result.valid(),
           "the MIR verifier should reject a protocol propagation exit forged "
           "into a second primary full-expression boundary");
  }

  if (sumHir != nullptr) {
    // Retarget the state test to the output member: the HIR plan check must
    // notice that the call no longer names the selected declaration.
    lang::HirProgram forged = frontend.hir;
    auto &functions = const_cast<std::vector<lang::HirFunctionInstance> &>(
        forged.functionInstances());
    lang::HirBody &body = functions.at(sumHir->id - 1).body;
    bool retargeted = false;
    for (lang::HirValue &value : body.values) {
      if (retargeted || value.kind != lang::HirValueKind::Propagation ||
          !value.propagation) {
        continue;
      }
      lang::HirValue *test = nullptr;
      const lang::HirValue *output = nullptr;
      for (lang::HirValue &candidate : body.values) {
        if (candidate.id == value.propagation->test) {
          test = &candidate;
        } else if (candidate.id == value.propagation->output) {
          output = &candidate;
        }
      }
      if (test != nullptr && output != nullptr) {
        test->functionTarget = output->functionTarget;
        retargeted = true;
      }
    }
    const lang::HirProgramPlanVerificationResult result =
        lang::verifyHirProgramPlans(frontend.semantics, forged);
    expect(retargeted && !result.valid() &&
               std::any_of(result.errors.begin(), result.errors.end(),
                           [](const std::string &error) {
                             return error.find("protocol propagation") !=
                                    std::string::npos;
                           }),
           "HIR plan verification should reject a protocol call that no "
           "longer names its selected member");
  }

  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  expect(cpp23.find("has_value") != std::string::npos &&
             cpp23.find("residual") != std::string::npos &&
             cpp20.find("has_value") != std::string::npos &&
             cpp20.find("residual") != std::string::npos,
         "both C++ standards should emit the protocol's ordinary member calls "
         "from verified MIR");
}

void testOptionalPropagation() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "optional-propagation.gti", R"(
#include <std/optional>

std::optional<int> find(int key) {
  if (key > 0) { return std::optional<int>(key * 10); }
  return std::optional<int>();
}

std::optional<bool> positive(int key) {
  int found = ?find(key);
  return std::optional<bool>(found > 15);
}

std::optional<int> forward(std::optional<int> input) {
  return std::optional<int>(?input + 1);
}

std::optional<int> consume(std::optional<int> input) {
  return std::optional<int>(?std::move(input) + 2);
}

std::optional<T> first<std::move_constructible T>(std::optional<T> input)
    requires std::copy_constructible<T> {
  T value = ?input;
  return std::optional<T>(value);
}

int main() {
  auto lambda = [](int key) -> std::optional<int> {
    return std::optional<int>(?find(key) + 5);
  };
  std::optional<bool> a = positive(2);
  std::optional<int> b = forward(find(3));
  std::optional<int> c = consume(find(-4));
  std::optional<int> d = first(find(5));
  std::optional<int> e = lambda(6);
  std::optional<int> empty = std::nullopt_t();
  if (!a.value() || c.has_value() || empty.has_value()) { return 1; }
  return b.value() + d.value() + e.value() - 31 - 50 - 65;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    reportDiagnostics(frontend, "optional propagation");
  }
  expect(
      frontend.canGenerateCode() &&
          lang::verifyMirProgram(frontend.mir).valid() &&
          lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
      "std::optional should propagate through the protocol into "
      "optional<U> results, generic instances and lambdas");
  if (!frontend.canGenerateCode()) {
    return;
  }

  PropagationCensus total;
  const auto add = [&](const lang::HirBody &body) {
    const PropagationCensus counted = census(frontend, body);
    total.values += counted.values;
    total.consumed += counted.consumed;
    total.copied += counted.copied;
    total.consumingOutputs += counted.consumingOutputs;
    total.wellFormed = total.wellFormed && counted.wellFormed;
  };
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    add(function.body);
  }
  for (const lang::HirLambda &lambda : frontend.hir.lambdaInstances()) {
    add(lambda.body);
  }
  expect(total.wellFormed && total.values == 5 && total.consumed == 3 &&
             total.copied == 5 && total.consumingOutputs == 0,
         "optional propagation should copy through the read-only value() for "
         "temporaries, explicit moves and named operands alike");

  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  expect(!cpp23.empty() && !cpp20.empty(),
         "optional propagation should emit C++20 and C++23 from verified MIR");
}

void testPropagationDiagnostics() {
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-protocol-propagation.gti", R"(
#include <std/optional>

struct Missing {
  int code;
  Missing(int code) : code(code) {}
};

class NoTest {
public:
  NoTest() {}
  int value() { return 1; }
  Missing residual() { return Missing(1); }
};

class WrongTest {
public:
  WrongTest() {}
  implicit WrongTest(Missing missing) {}
  int has_value() { return 1; }
  int value() { return 1; }
  Missing residual() { return Missing(1); }
};

class ConsumingOnly {
public:
  ConsumingOnly() {}
  implicit ConsumingOnly(Missing missing) {}
  bool has_value() { return true; }
  int value() && { return 1; }
  Missing residual() { return Missing(1); }
};

class NoResidualConstructor {
public:
  NoResidualConstructor() {}
  bool has_value() { return true; }
  int value() { return 1; }
  Missing residual() { return Missing(1); }
};

class BorrowedResidual {
  Missing stored;
public:
  BorrowedResidual() : stored(Missing(1)) {}
  implicit BorrowedResidual(Missing missing) : stored(missing) {}
  bool has_value() { return true; }
  int value() { return 1; }
  Missing& residual() { return this.stored; }
};

class MoveOnly {
public:
  MoveOnly() {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

NoTest no_test() {
  NoTest operand = NoTest();
  int value = ?operand;
  return NoTest();
}

WrongTest wrong_test() {
  WrongTest operand = WrongTest();
  int value = ?operand;
  return WrongTest();
}

ConsumingOnly named_consuming() {
  ConsumingOnly operand = ConsumingOnly();
  int value = ?operand;
  return ConsumingOnly();
}

std::optional<int> wrong_family() {
  ConsumingOnly operand = ConsumingOnly();
  int value = ?std::move(operand);
  return std::optional<int>(value);
}

NoResidualConstructor no_constructor() {
  NoResidualConstructor operand = NoResidualConstructor();
  int value = ?operand;
  return NoResidualConstructor();
}

BorrowedResidual borrowed_residual() {
  BorrowedResidual operand = BorrowedResidual();
  int value = ?operand;
  return BorrowedResidual();
}

std::optional<MoveOnly> make_owner() {
  return std::optional<MoveOnly>(MoveOnly());
}

std::optional<int> move_only_copy() {
  MoveOnly owner = ?make_owner();
  return std::optional<int>(1);
}

int not_a_class() { return ?1; }

int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const bool contract =
      !invalid.canGenerateCode() &&
      countDiagnosticCode(invalid.diagnostics, "GTI-S2086") == 8 &&
      hasDiagnostic(invalid.diagnostics,
                    "requires a member function 'has_value()'") &&
      hasDiagnostic(invalid.diagnostics, "read-only member returning 'bool'") &&
      hasDiagnostic(invalid.diagnostics, "its overloads consume the operand") &&
      hasDiagnosticHint(invalid.diagnostics, "std::move(name)") &&
      hasDiagnostic(invalid.diagnostics,
                    "only available in a function or lambda returning "
                    "'ConsumingOnly'") &&
      hasDiagnostic(invalid.diagnostics,
                    "declares no implicit constructor taking exactly that "
                    "residual") &&
      hasDiagnostic(invalid.diagnostics, "'residual()' to return an owned") &&
      hasDiagnostic(invalid.diagnostics, "cannot copy the 'MoveOnly' output") &&
      hasDiagnostic(invalid.diagnostics, "'expected<T, E>' operand or a class");
  if (!contract) {
    reportDiagnostics(invalid, "invalid protocol propagation");
  }
  expect(contract,
         "invalid protocol operands, receivers, residuals, families and "
         "return constructions should receive focused GTI-S2086 diagnostics");
}

void testPropagationContexts() {
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "protocol-propagation-contexts.gti", R"(
struct Failure {
  int code;
  constexpr Failure(int code) : code(code) {}
};

class Reply {
  int code;
public:
  constexpr Reply(int code) : code(code) {}
  implicit constexpr Reply(Failure failure) : code(failure.code) {}
  constexpr bool has_value() { return this.code == 0; }
  constexpr int value() { return 1; }
  constexpr Failure residual() { return Failure(this.code); }
};

// Another family with the same residual type is still another family.
class Ticket {
  int code;
public:
  Ticket(int code) : code(code) {}
  implicit Ticket(Failure failure) : code(failure.code) {}
  bool has_value() { return this.code == 0; }
  int value() { return 2; }
  Failure residual() { return Failure(this.code); }
};

Reply shared_residual() {
  Ticket ticket = Ticket(0);
  int value = ?ticket;
  return Reply(value);
}

constexpr Reply constant_context() {
  Reply reply = Reply(0);
  int value = ?reply;
  return Reply(value);
}

class Holder {
  int stored;
public:
  Holder(Reply reply) : stored(0) {
    int value = ?reply;
  }
};

int global = ?Reply(0);

int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const bool contract =
      !invalid.canGenerateCode() &&
      countDiagnosticCode(invalid.diagnostics, "GTI-S2086") == 4 &&
      hasDiagnostic(invalid.diagnostics,
                    "returning 'Ticket', but this one returns 'Reply'") &&
      hasDiagnostic(invalid.diagnostics,
                    "not yet available in a constexpr function") &&
      hasDiagnostic(invalid.diagnostics,
                    "returning 'Reply', but this one returns 'void'") &&
      hasDiagnostic(invalid.diagnostics,
                    "only available in a function or lambda returning "
                    "'Reply'.");
  if (!contract) {
    reportDiagnostics(invalid, "protocol propagation contexts");
  }
  expect(contract,
         "protocol propagation should reject another family sharing the "
         "residual type, constexpr functions, constructors and module scope");
}

} // namespace

int main() {
  testUserProtocolPropagation();
  testOptionalPropagation();
  testPropagationDiagnostics();
  testPropagationContexts();
  if (failures != 0) {
    std::cerr << failures << " propagation protocol test(s) failed\n";
    return 1;
  }
  std::cout << "propagation protocol tests passed\n";
  return 0;
}
