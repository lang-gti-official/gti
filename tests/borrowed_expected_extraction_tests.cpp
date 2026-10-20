#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("borrowed_expected_extraction.gti", source,
                                  {library / "prelude.gti"}, {}, {library});
}

// The prelude's result class, recognized by its declared identity.
bool isExpectedClass(const lang::FrontendResult &result,
                     const lang::SemanticType &type) {
  if (type.kind != lang::SemanticType::Class)
    return false;
  const auto *info = result.semantics.findClassType(type.classId);
  return info && info->qualifiedName == "expected";
}

// A staged receiver names its carrier through its call input: the slot it
// materializes, or the place it reads or moves.
lang::MirPlaceId carrierStorage(const lang::MirBody &body,
                                lang::MirOperand operand) {
  for (unsigned depth = 0; depth != 4 && operand.place == 0; ++depth) {
    const auto *value = body.findValue(operand.value);
    const auto *block = value ? body.findBlock(value->definitionBlock) : nullptr;
    const lang::MirInstruction *producer = nullptr;
    if (block)
      for (const auto &instruction : block->instructions)
        if (instruction.id == value->definition)
          producer = &instruction;
    if (!producer || producer->operands.size() != 1)
      return 0;
    if (producer->kind == lang::MirInstructionKind::CallInput &&
        producer->destination)
      return *producer->destination;
    operand = producer->operands.front();
  }
  return operand.place;
}

const std::string declarations = R"(
class Resource {
  mut int value;
public:
  Resource(int value) : value(value) {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  int read() { return value; }
  void set(int replacement) mut { value = replacement; }
};
expected<Resource&, int> lookup(Resource& owner, bool fail) {
  if (fail) { return unexpected(9); }
  return owner;
}
)";

void reject(std::string_view body, std::string_view reason) {
  const auto result = analyze(declarations + std::string(body));
  check(result.syntaxValid, "negative extraction case has valid source syntax");
  if (result.semanticValid || result.canGenerateCode()) {
    std::cerr << "Accepted invalid case (" << reason << "):\n" << body;
    check(false, "invalid borrow extraction stops in semantics");
  }
  check(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto &diagnostic) {
                      return diagnostic.code.starts_with("GTI-S");
                    }),
        "invalid source receives a semantic diagnostic");
  check(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code.starts_with("GTI-B");
                     }),
        "invalid borrow cannot fall through to backend failure");
}

void testPositive() {
  const auto result = analyze(declarations + R"(
expected<Resource&, int> forwarded(Resource& owner) {
  return ?lookup(owner, false);
}
Resource& required(Resource& owner) {
  return lookup(owner, false).expect("required");
}
class Registry {
  Resource owner;
public:
  Registry(int value) : owner(Resource(value)) {}
  expected<Resource&, int> find() { return this.owner; }
  expected<Resource&, int> forwarded() {
    auto carrier = this.find();
    return ?carrier;
  }
  Resource& required() {
    auto carrier = this.find();
    return std::move(carrier).expect("required receiver owner");
  }
};
Resource global_owner{11};
expected<Resource&, int> find_global() { return global_owner; }
expected<int, int> read_other_origins(Registry& registry) {
  auto receiver_carrier = registry.find();
  Resource& first = ?receiver_carrier;
  auto required_receiver = registry.find();
  Resource& second = std::move(required_receiver).expect("receiver owner");
  Resource& third = ?registry.forwarded();
  Resource& fourth = registry.required();
  auto global_carrier = find_global();
  Resource& fifth = ?global_carrier;
  auto required_global = find_global();
  Resource& sixth = std::move(required_global).expect("global owner");
  Resource& seventh = ?find_global();
  Resource& eighth = find_global().expect("required global owner");
  return first.read() + second.read() + third.read() + fourth.read() +
      fifth.read() + sixth.read() + seventh.read() + eighth.read();
}
class Pinned {
  int identity;
public:
  Pinned(int value) : identity(value) {}
  Pinned(Pinned& other) = delete;
  Pinned(Pinned&& other) = delete;
  int read() { return identity; }
};
expected<Pinned&, int> find_pinned(Pinned& owner) { return owner; }
expected<int, int> read_pinned(Pinned& owner) {
  Pinned& first = ?find_pinned(owner);
  Pinned& second = find_pinned(owner).expect("required pinned owner");
  return first.read() + second.read();
}
expected<int, int> read(Resource& owner) {
  auto source = lookup(owner, false);
  Resource& first = ?source;
  Resource& second = ?source;
  Resource& third = ?forwarded(owner);
  auto consumed = lookup(owner, false);
  Resource& fourth = ?std::move(consumed);
  auto demanded = lookup(owner, false);
  Resource& fifth = std::move(demanded).expect("required");
  Resource& sixth = required(owner);
  return first.read() + second.read() + third.read() + fourth.read() +
      fifth.read() + sixth.read();
}
int main() {
  Registry registry{5};
  auto other_origins = read_other_origins(registry);
  if (!other_origins || other_origins.value() != 64) { return 3; }
  Pinned pinned{3};
  auto pinned_result = read_pinned(pinned);
  if (!pinned_result || pinned_result.value() != 6) { return 2; }
  mut Resource owner{7};
  auto result = read(owner);
  if (!result || result.value() != 42) { return 1; }
  Resource& alias = lookup(owner, false).expect("required");
  int observed = alias.read();
  owner.set(8);
  return observed + owner.read() - 15;
}
)");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  check(result.canGenerateCode(),
        "read-only extraction supports a noncopyable referent");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyMirProgram(result.mir).valid(),
        "borrow extraction has verified MIR");
  unsigned propagationPlans = 0;
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      if (value.kind != lang::HirValueKind::Propagation ||
          !value.propagation ||
          value.info.category != lang::ValueCategory::Place)
        continue;
      ++propagationPlans;
      for (unsigned mutation = 0; mutation != 10; ++mutation) {
        auto malformed = result.hir;
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto *propagation =
            const_cast<lang::HirValue *>(changed->body.findValue(value.id));
        const auto plan = *propagation->propagation;
        auto *test =
            const_cast<lang::HirValue *>(changed->body.findValue(plan.test));
        auto *output =
            const_cast<lang::HirValue *>(changed->body.findValue(plan.output));
        const auto *residual = changed->body.findValue(plan.residual);
        if (mutation == 0)
          propagation->propagation.reset();
        if (mutation == 1)
          propagation->info.category = lang::ValueCategory::Value;
        if (mutation == 2)
          propagation->propagation->consumesOperand = !plan.consumesOperand;
        if (mutation == 3)
          propagation->propagation->output = plan.residual;
        if (mutation == 4)
          output->borrowOrigin = lang::BorrowOriginKind::None;
        if (mutation == 5)
          output->borrowAccess = lang::AccessMode::Mutable;
        if (mutation == 6)
          output->info.type = lang::SemanticType::Bool;
        if (mutation == 7)
          test->info.type = lang::SemanticType::Int32;
        if (mutation == 8)
          output->functionTarget = residual->functionTarget;
        if (mutation == 9)
          propagation->info.type = lang::SemanticType::Bool;
        if (lang::verifyHirProgramPlans(result.semantics, malformed).valid()) {
          std::cerr << "Accepted borrowed propagation HIR mutation " << mutation
                    << " in function " << function.id << '\n';
          check(false,
                "borrowed propagation plans reject lost or forged loan facts");
        }
      }
    }
  }
  check(
      propagationPlans >= 5,
      "mutation coverage includes fresh, named, moved, and forwarded borrows");
  unsigned borrowedExtractions = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        // Extraction is an ordinary member call of the prelude class.
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.loan || !instruction.receiver ||
            !isExpectedClass(result, instruction.receiver->type) ||
            instruction.receiver->type.arguments.size() != 2 ||
            instruction.receiver->type.arguments.front().kind !=
                lang::SemanticType::Reference)
          continue;
        ++borrowedExtractions;
        for (unsigned mutation = 0; mutation != 6; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *changedBlock =
              const_cast<lang::MirBlock *>(changed->body.findBlock(block.id));
          auto extraction = std::find_if(
              changedBlock->instructions.begin(),
              changedBlock->instructions.end(), [&](const auto &candidate) {
                return candidate.id == instruction.id;
              });
          auto *loan = const_cast<lang::MirLoan *>(
              changed->body.findLoan(*extraction->loan));
          if (mutation == 0)
            extraction->loan.reset();
          if (mutation == 1)
            loan->access = lang::AccessMode::Mutable;
          if (mutation == 2) {
            const auto carrier =
                carrierStorage(changed->body, *extraction->receiver);
            check(carrier != 0 && carrier != loan->source,
                  "loan-source mutation targets carrier storage, not its "
                  "referent");
            loan->source = carrier;
          }
          if (mutation == 3)
            extraction->receiver.reset();
          if (mutation == 4)
            extraction->borrowOrigin = lang::BorrowOriginKind::None;
          if (mutation == 5)
            extraction->borrowAccess = lang::AccessMode::Mutable;
          if (lang::verifyMirProgram(malformed).valid()) {
            std::cerr << "Accepted borrowed extraction MIR mutation "
                      << mutation << " in function " << function.id
                      << ", instruction " << instruction.id << '\n';
            check(false,
                  "borrowed extraction rejects forged runtime loan evidence");
          }
        }
      }
    }
  }
  check(borrowedExtractions >= 8,
        "MIR mutations exercise both propagation and expect extraction paths");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      check(!gti_test::emitCppText(result, standard).empty(),
            "borrow extraction uses the production backend in both standards");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      check(false, "borrow extraction fits the verified backend vocabulary");
    }
  }
}

void testNegative() {
  for (const std::string extraction :
       {"?lookup(owner, false)", "lookup(owner, false).expect(\"required\")"}) {
    const std::string prefix =
        "expected<int, int> run() { mut Resource owner{7}; "
        "Resource& alias = " +
        extraction + "; ";
    reject(prefix + "owner.set(8); return alias.read(); }",
           "owner mutation while borrowed");
    reject(prefix + "Resource moved = std::move(owner); return alias.read(); }",
           "owner move while borrowed");
    reject(prefix + "alias.set(8); return alias.read(); }",
           "borrow remains read-only");
    reject("expected<Resource&, int> run() { Resource owner{7}; return " +
               extraction + "; }",
           "local owner escape");
    reject("expected<int, int> run() { mut Resource owner{7}; mut Resource& "
           "alias = " +
               extraction + "; return alias.read(); }",
           "borrow cannot become mutable");
  }
  for (const std::string extraction :
       {"?find_global()", "find_global().expect(\"required\")"}) {
    reject("mut Resource global_owner{7}; "
           "expected<Resource&, int> find_global() { return global_owner; } "
           "expected<int, int> run() { Resource& alias = " +
               extraction + "; global_owner.set(8); return alias.read(); }",
           "global owner mutation still conflicts with extraction");
  }
  reject(R"(
expected<int, int> run() {
  Resource& alias = ?lookup(Resource(7), false);
  return alias.read();
})",
         "temporary owner propagation");
  reject(R"(
int run() {
  Resource& alias = lookup(Resource(7), false).expect("required");
  return alias.read();
})",
         "temporary owner expect");
  reject(R"(
int run() { Resource owner{7}; auto source = lookup(owner, false);
  Resource& alias = source.expect("required"); return alias.read(); }
)",
         "expect requires explicit named consumption");
  reject(R"(
int run() { Resource owner{7}; auto source = lookup(owner, false);
  Resource& alias = std::move(source).expect("required");
  return alias.read() + source.value().read(); }
)",
         "consumed carrier is unavailable");
  reject(
      R"(
struct Error {
  Error() {}
  Error(Error& other) = delete;
  Error(Error&& other) = default;
};
expected<Resource&, Error> make(Resource& owner) { return owner; }
expected<int, Error> run(Resource& owner) {
  auto source = make(owner); Resource& alias = ?source; return alias.read();
})",
      "named propagation requires a copyable error, not a copyable referent");
  reject(R"(
expected<int, bool> run(Resource& owner) {
  Resource& alias = ?lookup(owner, false); return alias.read();
})",
         "propagation error types stay exact");
  // The result is an ordinary class, so a borrowed one may be a parameter.
  check(analyze(declarations + "void run(expected<Resource&, int> source) {}")
            .canGenerateCode(),
        "borrowed result parameters are ordinary class values");
  reject(R"(
int run() {
  auto callback = [](Resource& owner) -> expected<Resource&, int> {
    return ?lookup(owner, false);
  };
  return 0;
})",
         "borrowed expected lambda return remains unsupported");
}
} // namespace

int main() {
  testPositive();
  testNegative();
  return failures == 0 ? 0 : 1;
}
