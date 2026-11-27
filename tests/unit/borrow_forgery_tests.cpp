// Borrow, loan and ownership facts in MIR and HIR that the verifiers must
// re-prove, which have no source-level form: valid programs are compiled,
// then a loan, origin, dependency, receiver, capture, swap or construction
// fact is forged and the verifier must reject the result. (Restored from the
// deleted compiler_ownership_tests.cpp, borrowing_callbacks_mir_tests.cpp,
// borrowing_composition_tests.cpp and complete_borrow_summary_tests.cpp, the
// last as three of its main's blocks, minimally adapted: whole test
// functions, so their source-level assertions run again beside the
// forgeries; their GTI sources are also tests/ui/ownership and tests/ui/borrow
// cases. Each namespace keeps its origin's helpers; the shared fixture
// helpers of compiler_test_fixture.h are at file scope.)
#include "../../src/compiler/mir_verification_queries.h"
#include "../llvm_backend_test_support.h"
#include "../lowered_program_test_support.h"
#include "../private_stdlib_fixture.h"
#include "gti/ast_printer.h"
#include "gti/backend.h"
#include "gti/binary_float.h"
#include "gti/executable_path.h"
#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/standard_library.h"
#include "gti/support.h"
#include "unit_test.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

int failures = 0;

[[maybe_unused]] void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] bool containsMangledFunction(std::string_view text,
                                              std::string_view prefix,
                                              std::string_view sourceName,
                                              std::string_view suffix) {
  constexpr std::string_view legacyMarker = "__gti_fn_";
  constexpr std::string_view concreteMarker = "__gti_inst_";
  std::size_t search = 0;
  while (true) {
    const std::size_t legacy = text.find(legacyMarker, search);
    const std::size_t concrete = text.find(concreteMarker, search);
    const bool concreteName =
        concrete != std::string_view::npos &&
        (legacy == std::string_view::npos || concrete < legacy);
    const std::size_t function = concreteName ? concrete : legacy;
    if (function == std::string_view::npos) {
      return false;
    }
    const std::string_view marker =
        concreteName ? concreteMarker : legacyMarker;
    search = function + marker.size();
    if (function < prefix.size() ||
        text.substr(function - prefix.size(), prefix.size()) != prefix) {
      continue;
    }
    std::size_t tail = search;
    if (concreteName) {
      while (tail < text.size() && tail - search < 64 &&
             ((text[tail] >= '0' && text[tail] <= '9') ||
              (text[tail] >= 'a' && text[tail] <= 'f'))) {
        ++tail;
      }
      if (tail - search != 64) {
        continue;
      }
    } else {
      while (tail < text.size() && text[tail] >= '0' && text[tail] <= '9') {
        ++tail;
      }
      if (tail == search) {
        continue;
      }
    }
    if (tail >= text.size() || text[tail] != '_') {
      continue;
    }
    ++tail;
    if (text.substr(tail, sourceName.size()) != sourceName) {
      continue;
    }
    tail += sourceName.size();
    if (text.substr(tail, suffix.size()) == suffix) {
      return true;
    }
  }
}

[[maybe_unused]] std::unordered_set<std::string>
concreteBackendSymbols(std::string_view text, std::string_view marker,
                       std::string_view sourceName) {
  std::unordered_set<std::string> result;
  std::size_t search = 0;
  while ((search = text.find(marker, search)) != std::string_view::npos) {
    const std::size_t digest = search + marker.size();
    std::size_t tail = digest;
    while (tail < text.size() && tail - digest < 64 &&
           ((text[tail] >= '0' && text[tail] <= '9') ||
            (text[tail] >= 'a' && text[tail] <= 'f'))) {
      ++tail;
    }
    if (tail - digest == 64 && tail < text.size() && text[tail] == '_' &&
        text.substr(tail + 1, sourceName.size()) == sourceName) {
      const std::size_t end = tail + 1 + sourceName.size();
      result.emplace(text.substr(search, end - search));
    }
    search = digest;
  }
  return result;
}

// Analyzes a backend-neutral emission fixture and requires that it completes
// verified MIR lowering. Assert on the returned MIR/HIR/semantics, or on
// gti_test::llvmEmits for native emission.
[[maybe_unused]] lang::FrontendResult
analyzeEmissionFixture(std::string_view source,
                       lang::TargetInfo target = lang::TargetInfo::host()) {
  lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("compiler-emission-fixture.gti", std::string(source));
  expect(frontend.canGenerateCode(),
         "a semantically valid emission fixture should complete MIR "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected emission-fixture diagnostic: "
                << diagnostic.message << '\n';
    }
  } else {
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "an emission fixture should produce verified MIR");
  }
  return frontend;
}

[[maybe_unused]] bool hasDiagnostic(const lang::SemanticVisitor &semantic,
                                    const std::string &text) {
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
              const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

[[maybe_unused]] bool
hasDiagnosticAt(const std::vector<lang::Diagnostic> &diagnostics,
                std::string_view code, std::string_view source,
                std::size_t start, std::string_view reason) {
  const bool matched =
      std::any_of(diagnostics.begin(), diagnostics.end(),
                  [&](const lang::Diagnostic &diagnostic) {
                    const std::string_view actualSource =
                        diagnostic.primary.source;
                    const std::string actualFile =
                        std::filesystem::path(diagnostic.primary.source)
                            .filename()
                            .string();
                    return diagnostic.code == code &&
                           (actualSource == source || actualFile == source) &&
                           diagnostic.primary.start == start &&
                           diagnostic.message.find(reason) != std::string::npos;
                  });
  if (!matched) {
    std::cerr << "Expected diagnostic " << code << " at " << source << ':'
              << start << " containing '" << reason << "'.\n";
    for (const lang::Diagnostic &diagnostic : diagnostics) {
      std::cerr << "  found " << diagnostic.code << " at "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  return matched;
}

[[maybe_unused]] bool
hasMirVerificationError(const lang::MirVerificationResult &result,
                        std::string_view reason) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(reason) != std::string::npos;
                     });
}

[[maybe_unused]] bool
hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const lang::SemanticVisitor &semantic,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticContaining(const std::vector<lang::Diagnostic> &diagnostics,
                          const std::string &text) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &item) {
                      return item.message.find(text) != std::string::npos;
                    }));
}

[[maybe_unused]] std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "stdlib/prelude.gti";
}

[[maybe_unused]] std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedPreludeFixture(const std::string &name, const std::string &source,
                             lang::FrontendOptions options = {}) {
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / ("gti-trusted-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {standardLibraryPrelude(), entry},
               {{entryKey, source}}, {standardLibraryRoot()});
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedMemoryFixture(const std::string &name, const std::string &source,
                            lang::FrontendOptions options = {}) {
  const auto entry = standardLibraryRoot() / "__trusted_tests__" /
                     std::filesystem::path(name).filename();
  const std::string body =
      "import std.memory; import \"../std/detail/memory_capabilities.gti\";\n" +
      source;
  return lang::Frontend(std::move(options))
      .analyze(entry, body, {standardLibraryPrelude(), entry},
               {{std::filesystem::weakly_canonical(entry).string(), body}},
               {standardLibraryRoot()});
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedStandalonePreludeFixture(const std::string &name,
                                       const std::string &source,
                                       lang::FrontendOptions options = {}) {
  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      ("gti-trusted-standalone-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {entry}, {{entryKey, source}});
}

[[maybe_unused]] const lang::FunctionDecl *
findTopLevelFunction(const lang::Program &program, const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::ClassDecl *
findTopLevelClass(const lang::Program &program, const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::ClassDecl *
findClassRecursive(const lang::StmtList &statements, const std::string &name) {
  for (const lang::StmtPtr &statement : statements) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(statement.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
    const auto *namespaceDecl =
        dynamic_cast<const lang::NamespaceDecl *>(statement.get());
    if (namespaceDecl != nullptr) {
      if (const lang::ClassDecl *nested =
              findClassRecursive(namespaceDecl->declarations(), name)) {
        return nested;
      }
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::VariableDecl *
findDirectLocal(const lang::FunctionDecl *function, const std::string &name) {
  if (function == nullptr || !function->body()) {
    return nullptr;
  }
  for (const lang::StmtPtr &statement : function->body()->statements()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(statement.get());
    if (variable != nullptr && variable->name().lexeme == name) {
      return variable;
    }
  }
  return nullptr;
}

[[maybe_unused]] std::string nonNullReferenceSource() {
  return R"(
struct Counter {
public:
  Counter() = default;
  mut int value = 0;
  void bump() mut { this.value += 1; }
};

int read(Counter& counter) { return counter.value; }
void increment(mut Counter& counter) { counter.bump(); }

int main() {
  mut Counter counter = Counter();
  mut Counter& writable = counter;
  increment(writable);
  Counter& read_only = counter;

  mut int value = 1;
  mut int& alias = value;
  alias += 2;
  if (read(read_only) == 1 and value == 3) {
    return 0;
  }
  return 1;
}
)";
}

struct NamedTest {
  std::string_view name;
  void (*run)();
};

} // namespace

namespace ownership {
namespace {

// A source local's MIR facts: the function body that declares it, its HIR
// binding identity, and the MIR loan it retains (the most derived loan whose
// carriers name that binding).
struct MirRetained {
  const lang::MirFunctionInstance *function = nullptr;
  lang::HirBindingId binding = 0;
  const lang::MirLoan *loan = nullptr;
};

MirRetained mirRetained(const lang::FrontendResult &frontend,
                        const lang::VariableDecl *variable) {
  MirRetained result;
  if (variable == nullptr)
    return result;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    for (const lang::HirBinding &binding : instance.body.bindings) {
      if (binding.variable != variable)
        continue;
      result.binding = binding.id;
      result.function = frontend.mir.findFunctionInstance(instance.id);
      if (result.function == nullptr)
        return result;
      for (const lang::MirLoan &loan : result.function->body.loans)
        if (std::find(loan.carriers.begin(), loan.carriers.end(), binding.id) !=
            loan.carriers.end())
          result.loan = &loan;
      return result;
    }
  }
  return result;
}

const lang::MirPlace *mirLoanSource(const MirRetained &retained) {
  return retained.function == nullptr || retained.loan == nullptr
             ? nullptr
             : retained.function->body.findPlace(retained.loan->source);
}

const lang::MirLoan *mirLoanParent(const MirRetained &retained) {
  return retained.function == nullptr || retained.loan == nullptr
             ? nullptr
             : retained.function->body.findLoan(retained.loan->parent);
}

// The prelude's result class, identified by its declaration rather than by a
// compiler-known type kind.
bool isExpectedClass(const lang::FrontendResult &frontend,
                     const lang::SemanticType &type) {
  if (type.kind != lang::SemanticType::Class)
    return false;
  const lang::ClassTypeInfo *info =
      frontend.semantics.findClassType(type.classId);
  return info != nullptr && info->qualifiedName == "expected";
}

// The fixture's own bodies; prelude instances also appear in HIR and MIR.
bool entryFunction(const lang::FrontendResult &frontend,
                   lang::HirFunctionInstanceId id) {
  const lang::HirFunctionInstance *function =
      frontend.hir.findFunctionInstance(id);
  return function != nullptr &&
         function->sourceUnit == frontend.sourceGraph.entryUnit();
}

// The instruction defining a MIR value, such as a staged call input.
const lang::MirInstruction *definingInstruction(const lang::MirBody &body,
                                                lang::MirValueId value) {
  const lang::MirValue *defined = body.findValue(value);
  if (defined == nullptr)
    return nullptr;
  for (const lang::MirBlock &block : body.blocks)
    for (const lang::MirInstruction &instruction : block.instructions)
      if (instruction.id == defined->definition)
        return &instruction;
  return nullptr;
}

bool checkedTemporaryObserver(const lang::FrontendResult &frontend,
                              std::string_view name, bool storedOwner) {
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            !call.functionTarget || call.operands.size() != 1 ||
            call.parameterTypes.size() != 1)
          continue;
        const auto *target =
            frontend.hir.findFunctionInstance(*call.functionTarget);
        const auto *input =
            definingInstruction(function.body, call.operands.front().value);
        const auto *loan = input && input->loan
                               ? function.body.findLoan(*input->loan)
                               : nullptr;
        const auto &parameter = call.parameterTypes.front();
        if (target && target->source && target->source->name().lexeme == name &&
            parameter.kind == lang::SemanticType::Reference &&
            parameter.referenceAccess == lang::AccessMode::ReadOnly && input &&
            input->callSite == call.callSite && input->info.type == parameter &&
            input->callInputRole == lang::MirCallInputRole::Argument &&
            input->callInputKind ==
                lang::HirCallInputKind::ReadTemporaryBorrow &&
            loan && loan->kind == lang::MirLoanKind::CallArgument &&
            loan->access == lang::AccessMode::ReadOnly && !loan->escapes &&
            bool(loan->storedDependency) == storedOwner &&
            call.borrowOrigin == lang::BorrowOriginKind::None)
          return true;
      }
  return false;
}

// The shipped native backend accepts the verified program: MIR verifies and
// the LLVM backend produces an object from the production lowering.
bool nativeBackendEmits(
    const lang::FrontendResult &frontend,
    lang::OptimizationLevel level = lang::OptimizationLevel::O0) {
  if (!frontend.canGenerateCode() ||
      !lang::verifyMirProgram(frontend.mir).valid())
    return false;
  std::string error;
  if (gti_test::llvmEmits(frontend, level, lang::TargetInfo::host(), &error))
    return true;
  std::cerr << "Native emission failed: " << error << '\n';
  return false;
}

// Every MIR function instance of the entry unit whose source is `name`.
std::vector<const lang::MirFunctionInstance *>
mirFunctionsNamed(const lang::FrontendResult &frontend, std::string_view name) {
  std::vector<const lang::MirFunctionInstance *> result;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    if (source != nullptr && source->source != nullptr &&
        entryFunction(frontend, function.id) &&
        source->source->name().lexeme == name)
      result.push_back(&function);
  }
  return result;
}

const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend, std::string_view name) {
  const auto functions = mirFunctionsNamed(frontend, name);
  return functions.empty() ? nullptr : functions.front();
}

// A body MIR lowered from source and that verifies independently: the
// function goes through the verified-MIR path rather than a declaration.
bool hasVerifiedSourceBody(const lang::MirFunctionInstance *function) {
  return function != nullptr &&
         function->definitionKind == lang::MirDefinitionKind::Source &&
         !function->body.blocks.empty() &&
         lang::verifyMirBody(function->body).valid();
}

// Some MIR call selects a member whose operator identity is `op`; when
// `mayRaise` is given the selected target's failure summary must match it.
bool mirCallsOperator(const lang::FrontendResult &frontend,
                      lang::OverloadedOperator op,
                      std::optional<bool> mayRaise = std::nullopt) {
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirFunctionInstance *target =
            instruction.kind == lang::MirInstructionKind::Call &&
                    instruction.functionTarget
                ? frontend.mir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (target != nullptr && target->overloadedOperator == op &&
            (!mayRaise || target->mayRaiseDefinedFailure == *mayRaise))
          return true;
      }
  return false;
}

lang::FrontendResult analyzeWithMemory(std::string_view name,
                                       std::string source) {
  source.insert(0, "import std.memory;\n");
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() /
                                      std::string(name),
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void testIndexedPlaceMovesAndInitialization() {
  const lang::PlaceDomain domain{.snapshot = 1, .body = 1, .revision = 0};
  const lang::PlaceKey array{.domain = domain, .root = 1};
  lang::PlaceKey firstElement = array;
  firstElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::ConstantIndex, .index = 0});
  lang::PlaceKey secondElement = array;
  secondElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::ConstantIndex, .index = 1});
  const lang::PlaceKey otherRoot{.domain = domain, .root = 2};
  lang::PlaceKey leftField = array;
  leftField.projections.push_back(
      {.kind = lang::PlaceProjectionKind::Field, .field = 3});
  lang::PlaceKey rightField = array;
  rightField.projections.push_back(
      {.kind = lang::PlaceProjectionKind::Field, .field = 4});
  lang::PlaceKey dynamicElement = array;
  dynamicElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::DynamicIndex, .selection = 1});
  lang::PlaceKey otherDynamicElement = array;
  otherDynamicElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::DynamicIndex, .selection = 2});
  lang::PlaceKey otherDomain = firstElement;
  otherDomain.domain.body = 2;
  expect(
      lang::placeRelation(firstElement, firstElement).relation ==
              lang::PlaceRelation::Equal &&
          lang::placeRelation(array, firstElement).relation ==
              lang::PlaceRelation::LeftStrictPrefix &&
          lang::placeRelation(firstElement, array).relation ==
              lang::PlaceRelation::RightStrictPrefix &&
          lang::placeRelation(firstElement, secondElement).relation ==
              lang::PlaceRelation::Disjoint &&
          lang::placeRelation(array, otherRoot).relation ==
              lang::PlaceRelation::Disjoint &&
          lang::placeRelation(leftField, rightField).relation ==
              lang::PlaceRelation::Disjoint &&
          lang::placeRelation(dynamicElement, firstElement).relation ==
              lang::PlaceRelation::MayAlias &&
          lang::placeRelation(dynamicElement, dynamicElement).relation ==
              lang::PlaceRelation::Equal &&
          lang::placeRelation(dynamicElement, otherDynamicElement).relation ==
              lang::PlaceRelation::MayAlias &&
          !lang::placeRelation(firstElement, otherDomain).compatibleDomain,
      "the shared place relation should distinguish equality, prefixes, "
      "constant-index disjointness, dynamic may-alias, and domains");

  const lang::FrontendResult firstSnapshot =
      lang::Frontend().analyze("place-snapshot.gti", R"(
int main() { int value = 1; return value - 1; }
)");
  const lang::FrontendResult secondSnapshot =
      lang::Frontend().analyze("place-snapshot.gti", R"(
int main() { int value = 1; return value - 1; }
)");
  expect(firstSnapshot.canGenerateCode() && secondSnapshot.canGenerateCode() &&
             firstSnapshot.hir.module().placeDomain.snapshot != 0 &&
             firstSnapshot.hir.module().placeDomain.snapshot !=
                 secondSnapshot.hir.module().placeDomain.snapshot &&
             lang::MirPrinter().print(firstSnapshot.mir) ==
                 lang::MirPrinter().print(secondSnapshot.mir),
         "separate frontend snapshots should reject stale place identity "
         "while deterministic MIR printing normalizes the process-local "
         "generation");

  const lang::FrontendResult valid =
      analyzeWithMemory("indexed-place-moves.gti", R"(
struct Record {
  int value;
public:
  Record(int initial) : value(initial) {}
};

struct Holder {
  mut Record slots[2] = {Record(3), Record(4)};

  int rotate() mut {
    Record first = std::move(this.slots[0]);
    Record second = std::move(this.slots[1]);
    this.slots[0] = std::move(second);
    this.slots[1] = std::move(first);
    return this.slots[0].value + this.slots[1].value;
  }
public:
  Holder() = default;
};

int branch_restore(bool select) {
  mut Record values[1] = {Record(5)};
  if (select) {
    Record consumed = std::move(values[0]);
    values[0] = std::move(consumed);
  } else {
    Record consumed = std::move(values[0]);
    values[0] = std::move(consumed);
  }
  return values[0].value;
}

int loop_restore() {
  mut Record values[1] = {Record(6)};
  for (mut int index = 0; index < 2; index++) {
    Record consumed = std::move(values[0]);
    values[0] = std::move(consumed);
  }
  return values[0].value;
}

int partial_drop() {
  mut std::unique_ptr<Record> owners[2] = {
      std::make_unique<Record>(7), std::make_unique<Record>(8)};
  auto first = std::move(owners[0]);
  return first->value;
}

int main() {
  mut Record values[2] = {Record(1), Record(2)};
  Record first = std::move(values[1 - 1]);
  Record second = std::move(values[1]);
  values[0] = std::move(first);
  values[1] = std::move(second);
  Record moved[2] = std::move(values);

  mut Holder holder = Holder();
  return moved[0].value + moved[1].value + holder.rotate() +
         branch_restore(true) + loop_restore() + partial_drop() - 26;
}
)");
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected indexed-place diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "constant fixed-array elements should support disjoint moves, exact "
         "restoration, branch joins, loop backedges, field arrays, and "
         "partially moved drop boundaries");

  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const lang::HirFunctionInstance *hirMain = nullptr;
  const lang::MirFunctionInstance *mirMain = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == main) {
      hirMain = &instance;
      break;
    }
  }
  for (const lang::MirFunctionInstance &instance :
       valid.mir.functionInstances()) {
    if (hirMain != nullptr && instance.id == hirMain->id) {
      mirMain = &instance;
      break;
    }
  }

  const lang::HirValue *indexedMove = nullptr;
  const lang::HirValue *indexedRestore = nullptr;
  if (hirMain != nullptr) {
    for (const lang::HirValue &value : hirMain->body.values) {
      if (!value.ownership || value.ownership->place.projections.empty()) {
        continue;
      }
      const lang::PlaceProjection &projection =
          value.ownership->place.projections.back();
      if (projection.kind != lang::PlaceProjectionKind::ConstantIndex) {
        continue;
      }
      if (value.ownership->kind == lang::OwnershipEventKind::Move &&
          indexedMove == nullptr) {
        indexedMove = &value;
      } else if (value.ownership->kind ==
                     lang::OwnershipEventKind::Reinitialize &&
                 indexedRestore == nullptr) {
        indexedRestore = &value;
      }
    }
  }
  const lang::OwnershipEvent *semanticIndexedMove =
      indexedMove == nullptr || indexedMove->source == nullptr
          ? nullptr
          : valid.semantics.findOwnershipEvent(*indexedMove->source);
  expect(hirMain != nullptr && hirMain->body.placeDomain.body != 0 &&
             indexedMove != nullptr && indexedMove->place &&
             indexedMove->place == indexedMove->ownership->place &&
             semanticIndexedMove != nullptr &&
             semanticIndexedMove->place.domain.snapshot ==
                 valid.semantics.placeSnapshot() &&
             indexedMove->ownership->place.domain ==
                 hirMain->body.placeDomain &&
             indexedRestore != nullptr,
         "HIR should retain one body-qualified constant-index place and its "
         "move/reinitialization events");

  const lang::MirInstruction *mirIndexedMove = nullptr;
  const lang::MirInstruction *mirIndexedRestore = nullptr;
  if (mirMain != nullptr) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (!instruction.ownership ||
            instruction.ownership->place.projections.empty()) {
          continue;
        }
        if (instruction.ownership->kind == lang::OwnershipEventKind::Move &&
            mirIndexedMove == nullptr) {
          mirIndexedMove = &instruction;
        } else if (instruction.ownership->kind ==
                       lang::OwnershipEventKind::Reinitialize &&
                   mirIndexedRestore == nullptr) {
          mirIndexedRestore = &instruction;
        }
      }
    }
  }
  const lang::MirPlace *mirMovePlace =
      mirMain == nullptr || mirIndexedMove == nullptr ||
              mirIndexedMove->operands.empty()
          ? nullptr
          : mirMain->body.findPlace(mirIndexedMove->operands.front().place);
  expect(mirMain != nullptr && mirIndexedMove != nullptr &&
             mirIndexedRestore != nullptr && mirMovePlace != nullptr &&
             mirMovePlace->key == mirIndexedMove->ownership->place &&
             !mirMovePlace->projections.empty() &&
             mirMovePlace->projections.back().constantIndex == 0 &&
             lang::verifyMirBody(mirMain->body).valid(),
         "MIR should preserve the semantic constant index and validate its "
         "ownership-state transfer");
  const std::string indexedMirDump =
      mirMain == nullptr ? std::string{}
                         : lang::MirPrinter().print(mirMain->body);
  expect(indexedMirDump.starts_with("mir-body-v93\n") &&
             indexedMirDump.find(" domain=1:") != std::string::npos &&
             indexedMirDump.find(";constant=0;selection=0") !=
                 std::string::npos &&
             indexedMirDump.find(" ownership=event(") != std::string::npos,
         "the deterministic MIR snapshot should expose place domains, "
         "constant indices, and ownership events");

  if (mirMain != nullptr && mirIndexedMove != nullptr &&
      mirIndexedRestore != nullptr) {
    lang::MirBody mismatchedPlace = mirMain->body;
    bool changedMove = false;
    for (lang::MirBlock &block : mismatchedPlace.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == mirIndexedMove->id && instruction.ownership) {
          instruction.ownership->place.projections.back().index = 99;
          changedMove = true;
        }
      }
    }
    expect(changedMove && !lang::verifyMirBody(mismatchedPlace).valid(),
           "the MIR verifier should reject a forged ownership-event place");

    lang::MirBody missingRestore = mirMain->body;
    bool changedRestore = false;
    for (lang::MirBlock &block : missingRestore.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == mirIndexedRestore->id && instruction.ownership) {
          instruction.ownership->reachable = false;
          changedRestore = true;
        }
      }
    }
    expect(changedRestore && !lang::verifyMirBody(missingRestore).valid(),
           "the MIR fixed point should reject a forged missing element "
           "restoration before whole-owner movement");

    const lang::SymbolId arrayRoot = mirIndexedMove->ownership->place.root;
    lang::MirBody missingInitialization = mirMain->body;
    bool changedInitialization = false;
    for (lang::MirBlock &block : missingInitialization.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.destination) {
          continue;
        }
        const lang::MirPlace *destination =
            missingInitialization.findPlace(*instruction.destination);
        if (destination != nullptr && destination->key &&
            destination->key->root == arrayRoot &&
            destination->key->projections.empty()) {
          instruction.kind = lang::MirInstructionKind::Modify;
          instruction.operation = lang::MirOperation::AddAssign;
          changedInitialization = true;
          break;
        }
      }
      if (changedInitialization) {
        break;
      }
    }
    expect(changedInitialization &&
               !lang::verifyMirBody(missingInitialization).valid(),
           "the MIR fixed point should reject access before a local place is "
           "definitely initialized");

    lang::MirBody doubleInitialization = mirMain->body;
    bool changedDrop = false;
    for (lang::MirBlock &block : doubleInitialization.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            !instruction.destination) {
          continue;
        }
        const lang::MirPlace *destination =
            doubleInitialization.findPlace(*instruction.destination);
        if (destination != nullptr && destination->key &&
            destination->key->root == arrayRoot &&
            destination->key->projections.empty()) {
          instruction.kind = lang::MirInstructionKind::Initialize;
          changedDrop = true;
          break;
        }
      }
      if (changedDrop) {
        break;
      }
    }
    expect(changedDrop && !lang::verifyMirBody(doubleInitialization).valid(),
           "the MIR fixed point should reject initialization while a local "
           "place lifetime is still active");
  }

  const lang::FrontendResult partialOwner = lang::Frontend().analyze(
      "partial-index-owner.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int main() {
  mut Record values[2] = {Record(1), Record(2)};
  Record consumed = std::move(values[0]);
  Record moved[2] = std::move(values);
  return consumed.value + moved[1].value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!partialOwner.canGenerateCode() &&
             hasDiagnostic(partialOwner.diagnostics,
                           "one of its owned places remains moved"),
         "whole-array movement should be rejected while one constant element "
         "is moved");

  const std::string dynamicMoveSource = R"(
int main() {
  mut int values[2] = {1, 2};
  mut int index = 0;
  int consumed = std::move(values[index]);
  return consumed;
}
int after() { return 0; }
)";
  const lang::FrontendResult dynamicMove = lang::Frontend().analyze(
      "dynamic-index-move.gti", dynamicMoveSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  const auto dynamicMoveDiagnostic = std::find_if(
      dynamicMove.diagnostics.begin(), dynamicMove.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2018" &&
               diagnostic.message.find(
                   "requires an in-range constant fixed-array "
                   "index") != std::string::npos;
      });
  const std::size_t dynamicBracket = dynamicMoveSource.find("[index]");
  expect(!dynamicMove.canGenerateCode() &&
             dynamicMoveDiagnostic != dynamicMove.diagnostics.end() &&
             countDiagnosticCode(dynamicMove.diagnostics, "GTI-S2018") == 1 &&
             dynamicMoveDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             std::filesystem::path(dynamicMoveDiagnostic->primary.source)
                     .filename() == "dynamic-index-move.gti" &&
             dynamicMoveDiagnostic->primary.start == dynamicBracket &&
             dynamicMoveDiagnostic->primary.end == dynamicBracket + 1 &&
             dynamicMoveDiagnostic->fixes.empty() &&
             findTopLevelFunction(dynamicMove.program, "after") != nullptr,
         "dynamic indexes should diagnose their exact bracket while "
         "preserving later declarations and disabling code generation");

  const lang::FrontendResult branchMove = lang::Frontend().analyze(
      "branch-index-move.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int read(bool select) {
  mut Record values[1] = {Record(1)};
  if (select) {
    Record consumed = std::move(values[0]);
  }
  return values[0].value;
}
int main() { return read(false) - 1; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!branchMove.canGenerateCode() &&
             hasDiagnostic(branchMove.diagnostics,
                           "may have been moved on another control-flow path"),
         "branch joins should retain maybe-moved element state");

  const lang::FrontendResult loopMove = lang::Frontend().analyze(
      "loop-index-move.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int main() {
  mut Record values[1] = {Record(1)};
  mut int index = 0;
  while (index < 2) {
    Record consumed = std::move(values[0]);
    index++;
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!loopMove.canGenerateCode() &&
             hasDiagnostic(loopMove.diagnostics,
                           "not available at a reachable loop backedge"),
         "loop backedges should require an exact moved element to be "
         "restored");

  const lang::FrontendResult dynamicRead = lang::Frontend().analyze(
      "dynamic-index-after-move.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int read(mut int index) {
  mut Record values[2] = {Record(1), Record(2)};
  Record consumed = std::move(values[0]);
  return values[index].value + consumed.value;
}
int main() { return read(1) - 3; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!dynamicRead.canGenerateCode() &&
             hasDiagnostic(dynamicRead.diagnostics, "has already been moved"),
         "a dynamic index should may-alias an unavailable constant element");
}

void testExclusiveReborrowLoanGraph() {
  const std::string exclusiveSource = R"(
struct Pair {
public:
  mut int left = 1;
  mut int right = 2;
public:
  Pair() = default;
};

void change(mut int& value) { value += 1; }
int observe(int& value) { return value; }
void combine(int& observed, mut int& changed) { changed += observed; }

int endpoint_chain() {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  child += 1;
  return value;
}

int switch_reactivation(int selected) {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  switch (child) {
  case 0:
    parent += 1;
    break;
  default:
    parent += 2;
    break;
  }
  return value;
}

int switch_unmatched_reactivation(int selected) {
  mut int value = selected;
  mut int& parent = value;
  mut int& child = parent;
  switch (child) {
  case 0:
    parent += 1;
    break;
  }
  parent += 1;
  return value;
}

class CheckedOwner {
  mut int value;

public:
  CheckedOwner(int initial) : value(initial) {}
  mut int& operator*() mut { return this.value; }
};

int checked_dereference_reactivation() {
  mut CheckedOwner owner = CheckedOwner(1);
  mut int& parent = *owner;
  mut int& child = parent;
  child += 1;
  parent += 1;
  return *owner - 3;
}

int main() {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  mut int& grandchild = child;
  grandchild += 1;

  mut int& sequential = parent;
  sequential += 1;
  parent += 1;

  mut Pair pair = Pair();
  mut int& left = pair.left;
  mut int& right = pair.right;
  left += 1;
  right += 1;
  combine(left, right);

  mut Pair second_pair = Pair();
  mut Pair& pair_parent = second_pair;
  mut int& projected_child = pair_parent.left;
  pair_parent.right += 1;
  projected_child += 1;

  int& shared = value;
  int& shared_alias = shared;
  return observe(shared_alias) + pair.left + pair.right - 10 +
         checked_dereference_reactivation();
}
)";
  const lang::FrontendResult exclusive =
      lang::Frontend().analyze("exclusive-reborrow.gti", exclusiveSource);
  expect(exclusive.syntaxValid,
         "exclusive reborrow coverage should remain valid GTI syntax");
  const bool valid = exclusive.semanticValid;
  if (!valid) {
    for (const lang::Diagnostic &diagnostic : exclusive.diagnostics) {
      std::cerr << "Unexpected exclusive-reborrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "nested and sequential exclusive reborrows, shared aliases, and "
         "disjoint field loans should validate");

  const lang::FunctionDecl *mainFunction =
      findTopLevelFunction(exclusive.program, "main");
  const auto local = [&](std::size_t index) -> const lang::VariableDecl * {
    return mainFunction == nullptr
               ? nullptr
               : dynamic_cast<const lang::VariableDecl *>(
                     mainFunction->body()->statements().at(index).get());
  };
  const lang::VariableDecl *parent = local(1);
  const lang::VariableDecl *child = local(2);
  const lang::VariableDecl *grandchild = local(3);
  const lang::VariableDecl *left = local(9);
  const lang::VariableDecl *right = local(10);
  const lang::VariableDecl *pairParent = local(15);
  const lang::VariableDecl *projectedChild = local(16);
  const lang::VariableDecl *shared = local(19);
  const lang::VariableDecl *sharedAlias = local(20);
  const MirRetained parentLoan = mirRetained(exclusive, parent);
  const MirRetained childLoan = mirRetained(exclusive, child);
  const MirRetained grandchildLoan = mirRetained(exclusive, grandchild);
  const MirRetained leftLoan = mirRetained(exclusive, left);
  const MirRetained rightLoan = mirRetained(exclusive, right);
  const MirRetained pairParentLoan = mirRetained(exclusive, pairParent);
  const MirRetained projectedChildLoan = mirRetained(exclusive, projectedChild);
  const MirRetained sharedLoan = mirRetained(exclusive, shared);
  const MirRetained sharedAliasLoan = mirRetained(exclusive, sharedAlias);
  // A reborrow's source is its parent loan's place, with no projection.
  const auto reborrowsWhole = [](const MirRetained &reborrow,
                                 const MirRetained &from) {
    const lang::MirPlace *source = mirLoanSource(reborrow);
    return reborrow.loan != nullptr && from.loan != nullptr &&
           reborrow.loan != from.loan &&
           reborrow.loan->parent == from.loan->id && source != nullptr &&
           source->root == lang::MirPlaceRootKind::Loan &&
           source->loan == from.loan->id && source->projections.empty() &&
           reborrow.loan->access == lang::AccessMode::Mutable;
  };
  const lang::MirPlace *parentSource = mirLoanSource(parentLoan);
  expect(parentLoan.loan != nullptr &&
             parentLoan.loan->access == lang::AccessMode::Mutable &&
             parentSource != nullptr &&
             parentSource->root == lang::MirPlaceRootKind::Binding &&
             reborrowsWhole(childLoan, parentLoan) &&
             reborrowsWhole(grandchildLoan, childLoan) &&
             !childLoan.loan->carriers.empty() &&
             !grandchildLoan.loan->carriers.empty(),
         "mutable reborrows should form distinct parent-linked MIR loans over "
         "one stable place, each with its body-local carrier");
  const lang::MirPlace *leftSource = mirLoanSource(leftLoan);
  const lang::MirPlace *rightSource = mirLoanSource(rightLoan);
  expect(leftSource != nullptr && rightSource != nullptr &&
             leftSource->root == lang::MirPlaceRootKind::Binding &&
             leftSource->binding == rightSource->binding &&
             leftSource->projections.size() == 1 &&
             rightSource->projections.size() == 1 &&
             leftSource->projections.front().kind ==
                 lang::MirProjectionKind::Field &&
             rightSource->projections.front().kind ==
                 lang::MirProjectionKind::Field &&
             leftSource->projections.front().field !=
                 rightSource->projections.front().field,
         "named sibling fields should retain disjoint stable projections");
  const lang::MirPlace *projectedSource = mirLoanSource(projectedChildLoan);
  expect(pairParentLoan.loan != nullptr && projectedChildLoan.loan != nullptr &&
             projectedChildLoan.loan->parent == pairParentLoan.loan->id &&
             projectedSource != nullptr &&
             projectedSource->root == lang::MirPlaceRootKind::Loan &&
             projectedSource->loan == pairParentLoan.loan->id &&
             projectedSource->projections.size() == 1 &&
             projectedSource->projections.front().kind ==
                 lang::MirProjectionKind::Field,
         "a projected child should suspend only its overlapping parent place, "
         "not a known-disjoint sibling field");

  const lang::FunctionDecl *checkedDereferenceFunction = findTopLevelFunction(
      exclusive.program, "checked_dereference_reactivation");
  const auto checkedLocal =
      [&](std::size_t index) -> const lang::VariableDecl * {
    return checkedDereferenceFunction == nullptr
               ? nullptr
               : dynamic_cast<const lang::VariableDecl *>(
                     checkedDereferenceFunction->body()
                         ->statements()
                         .at(index)
                         .get());
  };
  const MirRetained checkedOwner = mirRetained(exclusive, checkedLocal(0));
  const MirRetained checkedParentLoan = mirRetained(exclusive, checkedLocal(1));
  const MirRetained checkedChildLoan = mirRetained(exclusive, checkedLocal(2));
  const lang::MirPlace *checkedParentSource = mirLoanSource(checkedParentLoan);
  expect(checkedParentLoan.loan != nullptr &&
             checkedParentLoan.loan->kind == lang::MirLoanKind::CallResult &&
             checkedParentLoan.loan->access == lang::AccessMode::Mutable &&
             checkedParentSource != nullptr &&
             checkedParentSource->root == lang::MirPlaceRootKind::Binding &&
             checkedParentSource->binding == checkedOwner.binding &&
             checkedParentSource->projections.empty() &&
             reborrowsWhole(checkedChildLoan, checkedParentLoan),
         "a checked owner dereference should retain its exact owner place "
         "through exclusive reborrow analysis");
  expect(sharedLoan.loan != nullptr &&
             sharedLoan.loan == sharedAliasLoan.loan &&
             sharedLoan.loan->access == lang::AccessMode::ReadOnly &&
             sharedLoan.loan->carriers.size() == 2,
         "shared read-only aliases should continue sharing one MIR loan");

  const lang::FrontendResult transientResults =
      lang::Frontend().analyze("transient-call-results.gti", R"(
class Box {
  mut int value;

public:
  Box(int initial) : value(initial) {}
  int& read() { return this.value; }
  mut int& write() mut { return this.value; }
};

class Boxes {
public:
  mut Box left = Box(1);
  mut Box right = Box(2);
public:
  Boxes() = default;
};

int sum(int& left, int& right) { return left + right; }

int disjoint_writes(mut Boxes& parent) {
  return sum(parent.left.write(), parent.right.write());
}

int main() {
  mut Boxes parent = Boxes();
  int shared_reads = sum(parent.left.read(), parent.left.read());
  int disjoint = disjoint_writes(parent);
  return shared_reads + disjoint - 5;
}
)");
  expect(transientResults.canGenerateCode(),
         "read-only call results may overlap and mutable call results may "
         "remain live across one outer call when their places are disjoint");

  const lang::FrontendResult retainedProjected =
      lang::Frontend().analyze("retained-projected-results.gti", R"(
class Cell {
public:
  mut int value = 1;
  mut int& write() mut { return this.value; }
public:
  Cell() = default;
};
class Pair {
public:
  mut Cell left = Cell();
  mut int right = 2;
public:
  Pair() = default;
};
int& relay(int& source) { return source; }
int receiver_projection(mut Pair& parent) {
  mut int& child = parent.left.write();
  parent.right += 1;
  child += 1;
  return child + parent.right;
}
int helper_projection(mut Pair& parent) {
  int& child = relay(parent.right);
  int observed = child;
  parent.left.value += 1;
  return observed + parent.left.value;
}
int main() {
  mut Pair pair = Pair();
  return receiver_projection(pair) + helper_projection(pair) - 10;
}
)");
  expect(retainedProjected.canGenerateCode(),
         "retained receiver- and argument-origin results should preserve "
         "their exact caller-visible projected places");
  for (const std::string name : {"receiver_projection", "helper_projection"}) {
    const lang::FunctionDecl *function =
        findTopLevelFunction(retainedProjected.program, name);
    const auto *child = function == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  function->body()->statements().front().get());
    // The retained call result keeps the caller-visible place below the
    // reference parameter: one dereference and one exact field.
    const MirRetained retained = mirRetained(retainedProjected, child);
    const lang::MirPlace *source = mirLoanSource(retained);
    const lang::MirLoan *entry = mirLoanParent(retained);
    expect(retained.loan != nullptr &&
               retained.loan->kind == lang::MirLoanKind::CallResult &&
               entry != nullptr && entry->entry && source != nullptr &&
               source->root == lang::MirPlaceRootKind::Binding &&
               source->projections.size() == 2 &&
               source->projections[0].kind ==
                   lang::MirProjectionKind::Dereference &&
               source->projections[1].kind == lang::MirProjectionKind::Field,
           "MIR retained loans should preserve one exact field projection "
           "under their entry parent in " +
               name);
  }

  const lang::FrontendResult storedMutableParent =
      lang::Frontend().analyze("stored-mutable-parent.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int direct_parent(mut Box& parent) {
  View retained = View(parent.read());
  return retained.get();
}
int child_parent(mut Box& parent) {
  int& child = parent.read();
  View retained = View(child);
  return retained.get();
}
int main() { return 0; }
)");
  expect(storedMutableParent.canGenerateCode(),
         "single-path shared carriers retain a checked child of mutable "
         "parents and their read-only reborrows");
  bool checkedSharedAccessor = false;
  for (const auto &function : storedMutableParent.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *parent = function.body.findLoan(loan.parent);
      if (loan.kind != lang::MirLoanKind::Stored || parent == nullptr ||
          parent->kind != lang::MirLoanKind::CallResult ||
          parent->access != lang::AccessMode::ReadOnly || parent->parent == 0)
        continue;
      auto missingParent = storedMutableParent.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          missingParent.findFunctionInstance(function.id));
      changed->body.loans[loan.id - 1].parent = 0;
      expect(!lang::verifyMirProgram(missingParent).valid(),
             "a shared stored accessor cannot lose its suspended-owner chain");
      checkedSharedAccessor = true;
    }
  }
  expect(checkedSharedAccessor,
         "shared stored accessor exercises an exact parent chain");
  for (const std::string mutation :
       {"parent.value = 99;", "mut Box& alias = parent; alias.value = 99;"}) {
    const auto invalid =
        lang::Frontend().analyze("invalid-shared-stored-parent.gti",
                                 R"(
class Box { public: mut int value = 2; int& read() { return this.value; } };
class View { int& value; public: View(int& source) : value(source) {}
  int get() { return this.value; } };
int bad(mut Box& parent) { View retained = View(parent.read());
)" + mutation + " return retained.get(); }\nint main() { return 0; }\n");
    expect(!invalid.semanticValid, "shared stored accessor forbids mutable "
                                   "owner/reference invalidation: " +
                                       mutation);
  }

  const lang::FrontendResult ordinaryStoredDependency =
      lang::Frontend().analyze("ordinary-stored-dependency.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  void bump() mut { this.value += 1; }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int main() {
  mut Box owner = Box();
  mut int observed = 0;
  {
    View retained = View(owner.read());
    observed = retained.get();
  }
  owner.bump();
  return observed + owner.value - 5;
}
)");
  expect(ordinaryStoredDependency.canGenerateCode(),
         "ordinary read-only owner dependencies should remain valid for a "
         "lexically bounded stored carrier");

  const lang::FrontendResult mutableTransientConflict =
      lang::Frontend().analyze("mutable-transient-call-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  mut int& write() mut { return this.value; }
public:
  Box() = default;
};
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Box parent = Box();
  return sum(parent.write(), parent.write());
}
)");
  expect(
      !mutableTransientConflict.canGenerateCode() &&
          countDiagnosticCode(mutableTransientConflict.diagnostics,
                              "GTI-S2017") == 1 &&
          hasDiagnostic(mutableTransientConflict.diagnostics,
                        "Call creates overlapping reference accesses") &&
          !hasDiagnosticCode(mutableTransientConflict.diagnostics, "GTI-B0001"),
      "overlapping mutable reference call results should produce one "
      "outer-call exclusivity diagnostic");

  const lang::FrontendResult mutatingSharedTransientConflict =
      lang::Frontend().analyze("mutating-shared-call-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& write() mut { return this.value; }
public:
  Box() = default;
};
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Box parent = Box();
  return sum(parent.write(), parent.write());
}
)");
  expect(!mutatingSharedTransientConflict.canGenerateCode() &&
             countDiagnosticCode(mutatingSharedTransientConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(mutatingSharedTransientConflict.diagnostics,
                           "Call creates overlapping reference accesses") &&
             !hasDiagnosticCode(mutatingSharedTransientConflict.diagnostics,
                                "GTI-B0001"),
         "a mutable producer should retain exclusive evaluation access even "
         "when its returned reference is read-only");

  const std::string resultThenMutationSource = R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
int combine(int& borrowed, int changed) { return borrowed + changed; }
int main() {
  mut Box parent = Box();
  return combine(parent.read(), parent.bump());
}
)";
  const lang::FrontendResult resultThenMutationConflict =
      lang::Frontend().analyze("result-then-mutation-conflict.gti",
                               resultThenMutationSource);
  expect(!resultThenMutationConflict.canGenerateCode() &&
             countDiagnosticCode(resultThenMutationConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(resultThenMutationConflict.diagnostics,
                           "call operand mutates storage borrowed") &&
             !hasDiagnosticCode(resultThenMutationConflict.diagnostics,
                                "GTI-B0001"),
         "a later mutable receiver evaluation should not invalidate an "
         "earlier retained call result");
  const auto conflict =
      std::find_if(resultThenMutationConflict.diagnostics.begin(),
                   resultThenMutationConflict.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2017";
                   });
  expect(conflict != resultThenMutationConflict.diagnostics.end() &&
             conflict->primary.start ==
                 resultThenMutationSource.rfind("bump()") &&
             conflict->primary.end == conflict->primary.start + 4 &&
             conflict->related.size() == 1 &&
             conflict->related.front().span.start ==
                 resultThenMutationSource.rfind("read()") &&
             !conflict->hints.empty() && conflict->fixes.empty(),
         "the conflict should identify the later mutation and earlier borrow "
         "without offering an unsafe automatic reorder");

  const lang::FrontendResult resultThenMutableArgumentConflict =
      lang::Frontend().analyze("result-then-mut-argument-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  void bump() mut { this.value += 1; }
public:
  Box() = default;
};
int mutate(mut Box& box) {
  box.bump();
  return 0;
}
int combine(int& borrowed, int changed) { return borrowed + changed; }
int main() {
  mut Box parent = Box();
  return combine(parent.read(), mutate(parent));
}
)");
  expect(!resultThenMutableArgumentConflict.canGenerateCode() &&
             countDiagnosticCode(resultThenMutableArgumentConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(resultThenMutableArgumentConflict.diagnostics,
                           "later call operand mutates storage borrowed") &&
             !hasDiagnosticCode(resultThenMutableArgumentConflict.diagnostics,
                                "GTI-B0001"),
         "a later mutable-reference helper evaluation should not invalidate "
         "an earlier retained call result");

  const lang::FrontendResult mutationThenResult =
      lang::Frontend().analyze("mutation-then-result.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
int combine(int changed, int& borrowed) { return changed + borrowed; }
int main() {
  mut Box parent = Box();
  return combine(parent.bump(), parent.read());
}
)");
  expect(mutationThenResult.canGenerateCode() &&
             lang::verifyMirProgram(mutationThenResult.mir).valid(),
         "a completed earlier mutation may precede an overlapping borrow "
         "through the ordinary verified ordered-call schedule");
  bool provedBorrowOrder = false;
  bool reorderedInputRejected = false;
  lang::MirProgram reordered = mutationThenResult.mir;
  auto &orderedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      reordered.functionInstances());
  for (auto &function : orderedFunctions) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    auto &body = function.body;
    const auto dominance = lang::computeMirDominance(body);
    struct Position {
      lang::MirBlockId block = 0;
      std::size_t index = 0;
    };
    Position mutation, producer, invocation, firstInput, secondInput;
    lang::HirValueId producerValue = 0;
    for (const auto &block : body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &instruction = block.instructions[index];
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.functionTarget) {
          continue;
        }
        const auto *target =
            reordered.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr) {
          continue;
        }
        if (target->receiverMutability == lang::ReceiverMutability::Mutable) {
          mutation = {block.id, index};
        } else if (instruction.borrowOrigin ==
                   lang::BorrowOriginKind::Receiver) {
          producer = {block.id, index};
          producerValue = instruction.hirValue;
        } else if (instruction.parameterTypes.size() == 2) {
          invocation = {block.id, index};
          for (const auto &inputBlock : body.blocks) {
            for (std::size_t inputIndex = 0;
                 inputIndex < inputBlock.instructions.size(); ++inputIndex) {
              const auto &input = inputBlock.instructions[inputIndex];
              if (input.kind != lang::MirInstructionKind::CallInput ||
                  input.callSite != instruction.callSite) {
                continue;
              }
              (input.callInputIndex == 0 ? firstInput : secondInput) = {
                  inputBlock.id, inputIndex};
            }
          }
        }
      }
    }
    const auto precedes = [&](Position left, Position right) {
      return dominance && left.block != 0 && right.block != 0 &&
             (left.block == right.block
                  ? left.index < right.index
                  : dominance->dominates(left.block, right.block));
    };
    const bool hasExactLoan = std::any_of(
        body.loans.begin(), body.loans.end(), [&](const lang::MirLoan &loan) {
          return loan.producedBy == producerValue && producerValue != 0 &&
                 loan.source != 0 && loan.access == lang::AccessMode::ReadOnly;
        });
    provedBorrowOrder = precedes(mutation, firstInput) &&
                        precedes(firstInput, producer) &&
                        precedes(producer, secondInput) &&
                        precedes(secondInput, invocation) && hasExactLoan;
    if (!provedBorrowOrder) {
      continue;
    }
    // Preserve identities, types, uses and the first input's dominating value;
    // only reverse the outer call's input schedule across the borrow producer.
    auto &firstBlock = *body.findBlock(firstInput.block);
    auto movedInput = firstBlock.instructions[firstInput.index];
    firstBlock.instructions.erase(firstBlock.instructions.begin() +
                                  firstInput.index);
    if (firstInput.block == secondInput.block &&
        firstInput.index < secondInput.index) {
      --secondInput.index;
    }
    auto &secondBlock = *body.findBlock(secondInput.block);
    secondBlock.instructions.insert(
        secondBlock.instructions.begin() + secondInput.index + 1, movedInput);
    for (auto &value : body.values) {
      if (value.definition == movedInput.id) {
        value.definitionBlock = secondBlock.id;
      }
    }
    reorderedInputRejected = lang::rebuildMirValueUses(body) &&
                             !lang::verifyMirProgram(reordered).valid();
  }
  expect(provedBorrowOrder && reorderedInputRejected,
         "MIR must create the lookup loan only after mutation completes, and "
         "reject forged ordered-input reordering");

  const auto borrowedCarrierInput =
      lang::Frontend().analyze("ordered-borrowed-carrier-input.gti", R"(
class Box {
  mut int value = 1;
public:
  int bump() mut { this.value += 1; return this.value; }
  int& read() { return this.value; }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
View lookup(Box& box) { return View(box.read()); }
int inspect(int changed, View view) { return changed + view.get(); }
int main() {
  mut Box box{};
  return inspect(box.bump(), lookup(box));
}
)");
  expect(borrowedCarrierInput.canGenerateCode() &&
             lang::verifyMirProgram(borrowedCarrierInput.mir).valid(),
         "ordinary calls should share the exact borrowed-carrier input "
         "contract with constructors");
  const auto rejectsCarrierMutation = [&](bool eraseProducerLoan) {
    lang::MirProgram invalid = borrowedCarrierInput.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        invalid.functionInstances());
    for (auto &function : functions) {
      if (function.entryKind == lang::ProgramEntryKind::None) {
        continue;
      }
      auto &body = function.body;
      for (auto &block : body.blocks) {
        for (auto &stage : block.instructions) {
          if (stage.kind != lang::MirInstructionKind::CallInput ||
              stage.callInputKind !=
                  lang::HirCallInputKind::MoveBorrowedTemporary ||
              stage.operands.size() != 1 || !stage.preparedParameterDrop) {
            continue;
          }
          const auto *source = body.findValue(stage.operands.front().value);
          if (source == nullptr) {
            return false;
          }
          if (!eraseProducerLoan) {
            stage.lifecycle.clear();
            return !lang::verifyMirProgram(invalid).valid();
          }
          for (auto &producerBlock : body.blocks) {
            for (auto &producer : producerBlock.instructions) {
              if (producer.id == source->definition && producer.loan) {
                producer.loan.reset();
                return !lang::verifyMirProgram(invalid).valid();
              }
            }
          }
        }
      }
    }
    return false;
  };
  expect(rejectsCarrierMutation(true) && rejectsCarrierMutation(false),
         "ordinary borrowed-carrier inputs must fail closed when the exact "
         "producer loan or unique ownership handoff is erased");

  const lang::FrontendResult nestedTransientConflict =
      lang::Frontend().analyze("nested-transient-call-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  int& expose() mut { return this.value; }
public:
  Box() = default;
};
int& observe(int& value) { return value; }
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Box parent = Box();
  return sum(parent.read(), observe(parent.expose()));
}
)");
  expect(
      !nestedTransientConflict.canGenerateCode() &&
          countDiagnosticCode(nestedTransientConflict.diagnostics,
                              "GTI-S2017") == 1 &&
          hasDiagnostic(nestedTransientConflict.diagnostics,
                        "Call creates overlapping reference accesses") &&
          !hasDiagnosticCode(nestedTransientConflict.diagnostics, "GTI-B0001"),
      "nested borrow-origin wrappers should preserve a mutable producer's "
      "full-expression access");

  const lang::FrontendResult constructorTransientConflict =
      lang::Frontend().analyze("constructor-transient-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& expose() mut { return this.value; }
public:
  Box() = default;
};
class Inspector {
public:
  Inspector(int& first, int& second) {}
public:
  Inspector() = default;
};
int main() {
  mut Box parent = Box();
  Inspector inspector = Inspector(parent.expose(), parent.expose());
  return 0;
}
)");
  expect(!constructorTransientConflict.canGenerateCode() &&
             countDiagnosticCode(constructorTransientConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(constructorTransientConflict.diagnostics,
                           "Call creates overlapping reference accesses") &&
             !hasDiagnosticCode(constructorTransientConflict.diagnostics,
                                "GTI-B0001"),
         "constructor arguments should use recursive transient-production "
         "access when validating aliases");

  const lang::FrontendResult conservativeReceiverConflict =
      lang::Frontend().analyze("conservative-receiver-result.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  mut int& write() mut { return this.value; }
public:
  Box() = default;
};
class Boxes {
public:
  mut Box left = Box();
  mut Box right = Box();
  int& read_left() { return this.left.read(); }
public:
  Boxes() = default;
};
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Boxes parent = Boxes();
  return sum(parent.read_left(), parent.right.write());
}
)");
  expect(!conservativeReceiverConflict.canGenerateCode() &&
             countDiagnosticCode(conservativeReceiverConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(conservativeReceiverConflict.diagnostics,
                           "Call creates overlapping reference accesses") &&
             !hasDiagnosticCode(conservativeReceiverConflict.diagnostics,
                                "GTI-B0001"),
         "a receiver-derived result should conservatively protect its whole "
         "selected receiver rather than infer a callee-internal field");

  const lang::FrontendResult nestedMutationSurfaces =
      lang::Frontend().analyze("nested-call-mutation-surfaces.gti", R"(
class Box {
public:
  mut int value = 2;

  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
int combine(int& observed, int changed) { return observed + changed; }
int binary_case(mut Box& parent) {
  return combine(parent.read(), 1 + parent.bump());
}
int conditional_case(mut Box& parent, bool choose) {
  return combine(parent.read(), choose ? parent.bump() : 0);
}
int assignment_case(mut Box& parent) {
  return combine(parent.read(), parent.value += 1);
}
int postfix_case(mut Box& parent) {
  return combine(parent.read(), parent.value++);
}
int main() { return 0; }
)");
  expect(
      !nestedMutationSurfaces.canGenerateCode() &&
          countDiagnosticCode(nestedMutationSurfaces.diagnostics,
                              "GTI-S2017") == 4 &&
          !hasDiagnosticCode(nestedMutationSurfaces.diagnostics, "GTI-B0001"),
      "nested binary, conditional, assignment, and postfix mutations "
      "should be visible to full-call borrow validation");

  const lang::FrontendResult movedCallOperand = lang::Frontend().analyze(
      "moved-call-operand.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
public:
  Box() = default;
};
int consume(int& observed, Box moved) { return observed + moved.value; }
int run(mut Box parent) {
  return consume(parent.read(), std::move(parent));
}
int main() { return run(Box()) - 4; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!movedCallOperand.canGenerateCode() &&
             countDiagnosticCode(movedCallOperand.diagnostics, "GTI-S2017") ==
                 1 &&
             hasDiagnostic(movedCallOperand.diagnostics,
                           "call operand mutates storage borrowed") &&
             !hasDiagnosticCode(movedCallOperand.diagnostics, "GTI-B0001"),
         "std::move should count as call-operand invalidation while an "
         "overlapping result remains borrowed");

  const lang::FrontendResult storedCarrierConflicts =
      lang::Frontend().analyze("stored-carrier-call-conflicts.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int forward(View view, int changed) { return view.get() + changed; }
int reverse(int changed, View view) { return changed + view.get(); }
int forward_case(mut Box& parent) {
  return forward(View(parent.read()), parent.bump());
}
int reverse_case(mut Box& parent) {
  return reverse(parent.bump(), View(parent.read()));
}
int main() { return 0; }
)");
  expect(
      !storedCarrierConflicts.canGenerateCode() &&
          countDiagnosticCode(storedCarrierConflicts.diagnostics,
                              "GTI-S2017") == 1 &&
          !hasDiagnosticCode(storedCarrierConflicts.diagnostics, "GTI-B0001"),
      "a borrowed carrier blocks later mutation, not mutation completed before "
      "its construction");

  const lang::FrontendResult disjointStoredCarrier =
      lang::Frontend().analyze("disjoint-stored-carrier-call.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
class Pair {
public:
  mut Box left = Box();
  mut Box right = Box();
public:
  Pair() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int consume(View view, int changed) { return view.get() + changed; }
int main() {
  mut Pair parent = Pair();
  return consume(View(parent.left.read()), parent.right.bump()) - 5;
}
)");
  expect(disjointStoredCarrier.canGenerateCode(),
         "borrowed-state carrier arguments should remain compatible with a "
         "mutation of a disjoint sibling place");

  const lang::FrontendResult transientReceiverConflict =
      lang::Frontend().analyze("transient-receiver-call-conflict.gti", R"(
class Inner {
public:
  mut int value = 1;
  int inspect(int changed) { return this.value + changed; }
public:
  Inner() = default;
};
class Outer {
public:
  mut Inner inner = Inner();
  mut int count = 1;
  Inner& get() { return this.inner; }
  int bump() mut {
    this.count += 1;
    return this.count;
  }
public:
  Outer() = default;
};
int run(mut Outer& parent) {
  return parent.get().inspect(parent.bump());
}
int main() { return 0; }
)");
  expect(!transientReceiverConflict.canGenerateCode() &&
             countDiagnosticCode(transientReceiverConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(transientReceiverConflict.diagnostics,
                           "call operand mutates storage borrowed") &&
             !hasDiagnosticCode(transientReceiverConflict.diagnostics,
                                "GTI-B0001"),
         "a transient borrowed receiver should conflict with overlapping "
         "argument mutation");

  const lang::FrontendResult disjointTransientReceiver =
      lang::Frontend().analyze("disjoint-transient-receiver.gti", R"(
class Inner {
public:
  mut int value = 1;
  int inspect(int changed) { return this.value + changed; }
public:
  Inner() = default;
};
class Side {
public:
  mut Inner inner = Inner();
  mut int count = 1;
  Inner& get() { return this.inner; }
  int bump() mut {
    this.count += 1;
    return this.count;
  }
public:
  Side() = default;
};
class Pair {
public:
  mut Side left = Side();
  mut Side right = Side();
public:
  Pair() = default;
};
int run(mut Pair& parent) {
  return parent.left.get().inspect(parent.right.bump());
}
int main() {
  mut Pair pair = Pair();
  return run(pair) - 3;
}
)");
  expect(disjointTransientReceiver.canGenerateCode(),
         "a transient borrowed receiver should remain compatible with a "
         "mutation of a disjoint sibling place");

  const lang::FrontendResult sameArgumentCommaConflict =
      lang::Frontend().analyze("same-argument-comma-conflict.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  int& expose() mut { return this.value; }
public:
  Box() = default;
};
int inspect(int& value) { return value; }
int run(mut Box& parent) {
  return inspect((parent.read(), parent.expose()));
}
int main() { return 0; }
)");
  expect(!sameArgumentCommaConflict.canGenerateCode() &&
             countDiagnosticCode(sameArgumentCommaConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(sameArgumentCommaConflict.diagnostics,
                           "comma expression creates overlapping transient") &&
             !hasDiagnosticCode(sameArgumentCommaConflict.diagnostics,
                                "GTI-B0001"),
         "comma operands should not create overlapping full-expression "
         "borrows within one outer argument");

  const auto rejects = [](std::string source, std::string_view expected) {
    const lang::FrontendResult invalid = lang::Frontend().analyze(
        "invalid-exclusive-reborrow.gti", std::move(source));
    return invalid.syntaxValid && !invalid.canGenerateCode() &&
           hasDiagnosticCode(invalid.diagnostics, "GTI-S2017") &&
           !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001") &&
           hasDiagnostic(invalid.diagnostics, std::string(expected));
  };
  expect(rejects(R"(
int inspect(int& value) { return value; }
void change(mut int& value) { value += 1; }
int main() {
  mut int value = 0;
  int& shared = value;
  mut int& exclusive = value;
  change(exclusive);
  return inspect(shared);
}
)",
                 "Cannot create a mutable borrow"),
         "a direct mutable loan should conflict with a later-live shared "
         "loan of the same ordinary local place");
  expect(rejects(R"(
void collide(int& observed, mut int& changed) { changed += observed; }
int main() {
  mut int value = 1;
  collide(value, value);
  return value;
}
)",
                 "Call creates overlapping reference accesses"),
         "one call should not create overlapping shared and mutable access to "
         "the same place");
  expect(rejects(R"(
class Box {
  mut int value = 0;

public:
  mut int& get() mut { return this.value; }
public:
  Box() = default;
};
void both(int& observed, mut int& changed) { changed += observed; }
int main() {
  mut Box box = Box();
  both(box.get(), box.get());
  return 0;
}
)",
                 "Call creates overlapping reference accesses"),
         "receiver-tied reference call results should retain their stable "
         "place at an outer call boundary");
  expect(rejects(R"(
class Cell {
  mut int value = 0;

  void combine(mut int& other) mut { other += 1; }

public:
  void invalid() mut { combine(this.value); }
public:
  Cell() = default;
};
int main() { return 0; }
)",
                 "Call creates overlapping reference accesses"),
         "an implicit mutable receiver should conflict with an overlapping "
         "mutable reference argument");
  expect(rejects(R"(
int main() {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  parent += 1;
  child += 1;
  return value;
}
)",
                 "child reborrow"),
         "a mutable parent should remain suspended until its child endpoint");
  expect(rejects(R"(
int main() {
  mut int value = 0;
  mut int& exclusive = value;
  int snapshot = value;
  exclusive += 1;
  return snapshot;
}
)",
                 "Cannot read storage"),
         "direct owner reads should conflict with a later-live mutable loan");
  expect(rejects(R"(
int main() {
  mut int value = 0;
  int& shared = value;
  mut int& upgraded = shared;
  upgraded += 1;
  return value;
}
)",
                 "cannot be upgraded"),
         "read-only loans should not be upgradeable to mutable children");
  expect(rejects(R"(
void invalid(mut int& parent) {
  mut int& child = parent;
  parent += 1;
  child += 1;
}
int main() { return 0; }
)",
                 "child reborrow"),
         "mutable reference parameters should enter with a parent loan that "
         "can be suspended by a local child");
  const auto accepts = [&](std::string source) {
    const auto result = lang::Frontend().analyze("returned-local-reborrow.gti",
                                                 std::move(source));
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << "Unexpected returned-reborrow diagnostic: "
                  << diagnostic.message << '\n';
    return result.canGenerateCode() &&
           lang::verifyMirProgram(result.mir).valid();
  };
  expect(
      accepts(R"(
class Cell {
  mut int value = 0;

public:
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return child;
  }
public:
  Cell() = default;
};
int main() { return 0; }
)"),
      "local mutable child loans should relay caller-owned receiver storage");
  expect(accepts(R"(
class GroupedCell {
  mut int value = 0;

public:
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return (child);
  }
public:
  GroupedCell() = default;
};

class CommaCell {
  mut int value = 0;

public:
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return (0, child);
  }
public:
  CommaCell() = default;
};
int main() { return 0; }
)"),
         "grouping and comma wrappers should preserve a returned child loan's "
         "caller-owned origin");
  expect(accepts(R"(
class Cell {
  mut int value = 0;

public:
  int& expose() mut {
    mut int& parent = this.value;
    int& child = parent;
    return child;
  }
public:
  Cell() = default;
};
int main() { return 0; }
)"),
         "a read-only child of a mutable local parent should return its "
         "receiver-owned referent without upgrading access");
  expect(accepts(R"(
class View {
  int& value;

public:
  View(int& source) : value(source) {}
  int& get() { return this.value; }
};

class Cell {
  mut int value = 0;

public:
  View expose() mut {
    mut int& parent = this.value;
    int& child = parent;
    return View(child);
  }
public:
  Cell() = default;
};
int main() { return 0; }
)"),
         "a child loan may return in a carrier with the same proven owner");
  const auto returnedChains =
      lang::Frontend().analyze("returned-reborrow-chains.gti", R"(
class Pair {
public:
  mut int left = 1;
  mut int right = 2;
  Pair() = default;
};
mut T& relay<T>(mut T& value) {
  mut T& parent = value;
  mut T& child = parent;
  mut T& grandchild = child;
  return grandchild;
}
int& read_relay(int& value) {
  int& parent = value;
  int& child = parent;
  return child;
}
mut int& projected(mut Pair& pair) {
  mut Pair& parent = pair;
  mut int& child = parent.left;
  return child;
}
mut int& call_relay(mut Pair& pair) {
  mut int& parent = projected(pair);
  mut int& child = parent;
  return child;
}
int main() {
  mut Pair pair = Pair();
  mut int& slot = relay(call_relay(pair));
  slot += 3;
  int& observed = read_relay(pair.left);
  return observed - 4;
}
)");
  expect(returnedChains.canGenerateCode() &&
             lang::verifyMirProgram(returnedChains.mir).valid(),
         "generic, projected, and accessor-result reborrow chains should "
         "preserve an incoming reference's owner through verified MIR");
  bool checkedPublication = false;
  for (const auto &function : returnedChains.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *published = instruction.loan
                                    ? function.body.findLoan(*instruction.loan)
                                    : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Borrow ||
            !published || published->kind != lang::MirLoanKind::Return ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan)
          continue;
        const auto *child =
            function.body.findLoan(instruction.operands.front().loan);
        const auto *parent =
            child ? function.body.findLoan(child->parent) : nullptr;
        if (!child || !parent || parent->kind != lang::MirLoanKind::CallResult)
          continue;
        checkedPublication = true;
        auto missingParent = returnedChains.mir;
        auto *body = &const_cast<lang::MirFunctionInstance *>(
                          missingParent.findFunctionInstance(function.id))
                          ->body;
        body->loans[child->id - 1].parent = 0;
        expect(hasMirVerificationError(
                   lang::verifyMirProgram(missingParent),
                   "borrow instruction does not match the loan's parent"),
               "a returned reborrow cannot lose its accessor-result parent");
        auto missingRetirement = returnedChains.mir;
        auto *retiredBody =
            &const_cast<lang::MirFunctionInstance *>(
                 missingRetirement.findFunctionInstance(function.id))
                 ->body;
        for (auto &retiredBlock : retiredBody->blocks)
          std::erase_if(retiredBlock.instructions, [&](const auto &candidate) {
            return candidate.kind == lang::MirInstructionKind::EndBorrow &&
                   candidate.loan == child->id;
          });
        expect(hasMirVerificationError(
                   lang::verifyMirProgram(missingRetirement),
                   "borrow instruction does not match the loan's parent"),
               "return publication must retire the exact body-local child");
      }
    }
  }
  expect(
      checkedPublication,
      "accessor-chain return should publish a fresh capability from its child");
  expect(rejects(R"(
class Cell {
public:
  Cell() = default;
  mut int& invalid() mut {
    mut int local = 1;
    mut int& parent = local;
    mut int& child = parent;
    return child;
  }
};
int main() { return 0; }
)",
                 "must derive from this method's receiver"),
         "a reborrow chain ending in a local object must not escape");
  expect(rejects(R"(
class Cell {
public:
  Cell() = default;
  mut int& invalid(mut int& other) mut {
    mut int& parent = other;
    mut int& child = parent;
    return child;
  }
};
int main() { return 0; }
)",
                 "must derive from this method's receiver"),
         "caller-owned ancestry must not replace the declared return owner");
  expect(rejects(R"(
class Cell {
public:
  mut int value = 1;
  Cell() = default;
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return child;
  }
};
int main() {
  mut Cell cell = Cell();
  mut int& result = cell.expose();
  cell.value += 1;
  return result;
}
)",
                 // MIR rejects the compound assignment's read first.
                 "Cannot read storage while an overlapping mutable borrow"),
         "a returned reborrow must retain the caller's mutable exclusion");
  expect(rejects(R"(
class View {
public:
  int& borrowed;
  mut int owned = 1;
  View(int& source) : borrowed(source) {}
  mut int& invalid() mut {
    mut View local{this.borrowed};
    mut int& parent = local.owned;
    mut int& child = parent;
    return child;
  }
};
int main() { return 0; }
)",
                 // MIR rejects the escape of the local object's own field.
                 "does not outlive the function"),
         "a local carrier's external dependency cannot extend its owned field");
  const lang::FrontendResult disjointIndexes =
      lang::Frontend().analyze("disjoint-index-borrows.gti", R"(
int main() {
  mut int values[2] = {};
  mut int& first = values[0];
  mut int& second = values[1];
  second += 1;
  first += 1;
  return 0;
}
)");
  expect(disjointIndexes.canGenerateCode(),
         "different constant fixed-array elements should be disjoint safe "
         "places");
  expect(rejects(R"(
int main() {
  mut int values[2] = {};
  mut int index = 1;
  mut int& first = values[0];
  mut int& dynamic = values[index];
  dynamic += 1;
  first += 1;
  return 0;
}
)",
                 "Cannot create a mutable borrow"),
         "a dynamic fixed-array index should remain may-alias with every "
         "constant element");
}

void testGlobalBorrowReturns() {
  const std::string source = R"(
class LayerStack {
public:
  void Update() mut {}
public:
  LayerStack() = default;
};

class Application {
public:
  static mut Application& Get() {
    unsafe {
      return *Application::app;
    }
  }

  mut LayerStack& GetLayerStack() mut { return this.layers; }

private:
  mut LayerStack layers = LayerStack();
  static mut Application* app = nullptr;
public:
  Application() = default;
};

mut int global_value = 1;
mut int& global_value_ref() { return global_value; }

void update_application() {
  Application::Get().GetLayerStack().Update();
  Application::Get().GetLayerStack().Update();
}

void update_application_in_scopes() {
  {
    mut Application& first = Application::Get();
    first.GetLayerStack().Update();
  }
  mut Application& second = Application::Get();
  second.GetLayerStack().Update();
}

int main() {
  mut int& value = global_value_ref();
  value += 1;
  return value - 2;
}
)";
  const lang::FrontendResult valid =
      lang::Frontend().analyze("global-borrow-returns.gti", source);
  const lang::MirVerificationResult validMirVerification =
      lang::verifyMirProgram(valid.mir);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected global-borrow-return diagnostic: "
                << diagnostic.primary.start << ": " << diagnostic.message
                << '\n';
    }
  }
  if (!validMirVerification.valid()) {
    for (const lang::MirVerificationError &error :
         validMirVerification.errors) {
      std::cerr << "Unexpected global-borrow MIR error: " << error.owner << ':'
                << error.block << ':' << error.instruction << ' '
                << error.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mirValid && valid.mir.valid() &&
             validMirVerification.valid() &&
             !hasDiagnosticCode(valid.diagnostics, "GTI-S2055"),
         "an unsafe global/static dereference should be contained by its "
         "accessor while safe callers receive a checked mutable borrow");

  const lang::ClassDecl *application =
      findTopLevelClass(valid.program, "Application");
  const lang::FunctionDecl *get = nullptr;
  if (application != nullptr) {
    for (const lang::StmtPtr &member : application->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == "Get") {
        get = function;
        break;
      }
    }
  }
  const lang::FunctionInfo *getInfo =
      get == nullptr ? nullptr : valid.semantics.findFunction(*get);
  const bool exactSemanticOrigin =
      getInfo != nullptr &&
      getInfo->returnBorrowOrigin == lang::BorrowOriginKind::Global &&
      getInfo->returnBorrowAccess == lang::AccessMode::Mutable &&
      getInfo->returnBorrowPlace && getInfo->returnBorrowPlace->root != 0 &&
      getInfo->returnBorrowPlace->projections.size() == 1 &&
      getInfo->returnBorrowPlace->projections.front().kind ==
          lang::PlaceProjectionKind::Dereference;
  expect(exactSemanticOrigin,
         "semantic function metadata should retain the exact dereferenced "
         "static-field origin");
  const std::optional<lang::BorrowOriginPlace> getPlace =
      getInfo == nullptr ? std::nullopt : getInfo->returnBorrowPlace;

  const lang::HirFunctionInstance *getHir = nullptr;
  if (getInfo != nullptr) {
    for (const lang::HirFunctionInstance &instance :
         valid.hir.functionInstances()) {
      if (instance.declaration == getInfo->id) {
        getHir = &instance;
        break;
      }
    }
  }
  const lang::MirFunctionInstance *getMir =
      getHir == nullptr ? nullptr : valid.mir.findFunctionInstance(getHir->id);
  if (!validMirVerification.valid() && getMir != nullptr) {
    std::cerr << lang::MirPrinter().print(getMir->body) << '\n';
  }
  expect(getHir != nullptr && getMir != nullptr &&
             getHir->returnBorrowOrigin == lang::BorrowOriginKind::Global &&
             getHir->returnBorrowPlace == getInfo->returnBorrowPlace &&
             getMir->returnBorrowOrigin == lang::BorrowOriginKind::Global &&
             getMir->returnBorrowPlace == getInfo->returnBorrowPlace,
         "HIR and MIR function contracts should preserve the exact global "
         "borrow origin");

  if (getHir != nullptr && getMir != nullptr && getMir->returnBorrowPlace) {
    lang::MirProgram forged = valid.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    lang::MirFunctionInstance &forgedGet = functions.at(getHir->id - 1);
    ++forgedGet.returnBorrowPlace->root;
    const lang::MirVerificationResult forgedResult =
        lang::verifyMirProgram(forged);
    expect(!forgedResult.valid() &&
               std::any_of(forgedResult.errors.begin(),
                           forgedResult.errors.end(),
                           [](const lang::MirVerificationError &error) {
                             return error.message.find(
                                        "summarized static storage place") !=
                                    std::string::npos;
                           }),
           "the MIR verifier should reject a forged global return origin");
  }

  std::size_t globalCalls = 0;
  std::size_t endedGlobalCalls = 0;
  for (const lang::MirFunctionInstance &instance :
       valid.mir.functionInstances()) {
    std::vector<lang::MirLoanId> callLoans;
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.borrowOrigin == lang::BorrowOriginKind::Global &&
            instruction.borrowPlace == getPlace && instruction.loan) {
          ++globalCalls;
          callLoans.push_back(*instruction.loan);
        }
      }
    }
    for (const lang::MirLoanId loan : callLoans) {
      const bool ended = std::any_of(
          instance.body.blocks.begin(), instance.body.blocks.end(),
          [&](const lang::MirBlock &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const lang::MirInstruction &instruction) {
                  return instruction.kind ==
                             lang::MirInstructionKind::EndBorrow &&
                         instruction.loan == loan;
                });
          });
      endedGlobalCalls += ended ? 1 : 0;
    }
  }
  expect(globalCalls >= 2 && endedGlobalCalls == globalCalls,
         "unretained global call-result loans should end at their enclosing "
         "full-expression boundary");

  const std::string mirDump = lang::MirPrinter().print(valid.mir);
  expect(mirDump.starts_with("mir-v121 ") &&
             mirDump.find("return-borrow-place=origin(root=") !=
                 std::string::npos &&
             mirDump.find("borrow-place=origin(root=") != std::string::npos,
         "deterministic MIR should serialize global function and call origin "
         "places");

  // Reading the static pointer cannot fail, so the accessor is a safe,
  // failure-free static member whose every return publishes its loan.
  const bool returnsLoan =
      getMir != nullptr &&
      std::all_of(getMir->body.blocks.begin(), getMir->body.blocks.end(),
                  [](const lang::MirBlock &block) {
                    return block.terminator.kind !=
                               lang::MirTerminatorKind::Return ||
                           block.terminator.returnLoan.has_value();
                  });
  const lang::MirFunctionInstance *updateMir =
      mirFunctionNamed(valid, "update_application");
  expect(getMir != nullptr && getMir->staticMember && !getMir->requiresUnsafe &&
             !getMir->mayRaiseDefinedFailure && returnsLoan &&
             hasVerifiedSourceBody(getMir) && updateMir != nullptr &&
             !updateMir->requiresUnsafe && nativeBackendEmits(valid),
         "the native backend should emit the static mutable-reference "
         "accessor without exposing unsafe syntax at its call sites");

  const lang::FrontendResult overlapping =
      lang::Frontend().analyze("overlapping-global-borrow-returns.gti", R"(
class Application {
public:
  static mut Application& Get() {
    unsafe { return *Application::app; }
  }
private:
  static mut Application* app = nullptr;
public:
  Application() = default;
};
int main() {
  mut Application& first = Application::Get();
  mut Application& second = Application::Get();
  return 0;
}
)");
  // Under MIR authority the semantic persistent-storage check, which also
  // names the protected borrow's origin, rejects the second call first.
  expect(!overlapping.canGenerateCode() &&
             ((countDiagnosticCode(overlapping.diagnostics, "GTI-S2017") == 1 &&
               hasDiagnostic(overlapping.diagnostics,
                             "Cannot create a mutable borrow while an "
                             "overlapping borrow") &&
               hasRelatedDiagnostic(overlapping.diagnostics,
                                    "Retained borrow originates here")) ||
              (hasDiagnostic(overlapping.diagnostics,
                             "while a conflicting borrow is live") &&
               hasRelatedDiagnostic(overlapping.diagnostics,
                                    "protected borrow originates here"))),
         "two retained mutable borrows from one global accessor should "
         "conflict even when the first carrier is otherwise unused");

  const lang::FrontendResult missingUnsafe =
      lang::Frontend().analyze("unsafe-global-borrow-return.gti", R"(
class Application {
public:
  static mut Application& Get() { return *Application::app; }
private:
  static mut Application* app = nullptr;
public:
  Application() = default;
};
int main() { return 0; }
)");
  expect(!missingUnsafe.canGenerateCode() &&
             countDiagnosticCode(missingUnsafe.diagnostics, "GTI-S2055") == 1 &&
             !hasDiagnostic(missingUnsafe.diagnostics,
                            "A free or static borrowed return requires"),
         "the accessor body should still require unsafe for its raw "
         "dereference while retaining a valid global borrow summary");

  const lang::FrontendResult mixedOrigins =
      lang::Frontend().analyze("mixed-global-borrow-returns.gti", R"(
class Values {
public:
  static mut int left = 1;
  static mut int right = 2;
  static mut int& pick(bool choose) {
    if (choose) { return Values::left; }
    return Values::right;
  }
public:
  Values() = default;
};
int main() { return 0; }
)");
  expect(!mixedOrigins.canGenerateCode() &&
             hasDiagnosticCode(mixedOrigins.diagnostics, "GTI-S2017") &&
             hasDiagnostic(mixedOrigins.diagnostics,
                           "requires either one exact global/static storage "
                           "origin"),
         "a global borrow accessor should reject differing return roots");

  const lang::FrontendResult noForwardLookup =
      lang::Frontend().analyze("forward-global-borrow-return.gti", R"(
mut int& value_ref() { return value; }
mut int value = 1;
int main() { return 0; }
)");
  expect(
      !noForwardLookup.canGenerateCode() &&
          hasDiagnosticCode(noForwardLookup.diagnostics, "GTI-S2001") &&
          !hasDiagnosticCode(noForwardLookup.diagnostics, "GTI-S2017") &&
          hasDiagnostic(noForwardLookup.diagnostics, "Undefined name 'value'"),
      "global borrow summary registration must not make namespace storage "
      "forward-declarable or cascade an ownership diagnostic in ordinary "
      "source lookup");

  lang::FrontendOptions concurrentOptions;
  concurrentOptions.target = lang::TargetInfo::host();
  concurrentOptions.target.executionProfile =
      lang::ExecutionProfile::Concurrent;
  const lang::FrontendResult concurrent =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-global-borrow-return.gti", R"(
class Application {
public:
  static mut Application& Get() {
    unsafe { return *Application::app; }
  }
private:
  static mut Application* app = nullptr;
public:
  Application() = default;
};
int main() { return 0; }
)");
  expect(!concurrent.canGenerateCode() &&
             hasDiagnosticCode(concurrent.diagnostics, "GTI-S2060") &&
             hasDiagnostic(concurrent.diagnostics,
                           "Concurrent execution profile requires static "
                           "field 'app'"),
         "the concurrent profile should reject unsynchronized mutable "
         "global/static accessor storage");
}

void testUniqueOwnerSwap() {
  const lang::FrontendResult frontend =
      analyzeWithMemory("unique-owner-swap.gti", R"(
struct Value {
  int32_t number;
  Value(int32_t initial) : number(initial) {}
  int32_t read() { return this.number; }
};

int main() {
  mut std::unique_ptr<Value> first = std::make_unique<Value>(1);
  mut std::unique_ptr<Value> second = std::make_unique<Value>(2);
  first.swap(second);
  return first->read() == 2 and second->read() == 1 ? 0 : 1;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected unique-owner swap diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "unique_ptr::swap should preserve two matching mutable owners");

  lang::HirFunctionInstanceId functionId = 0;
  lang::MirBlockId blockId = 0;
  lang::MirInstructionId instructionId = 0;
  lang::MirPlaceId secondPlace = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::UniqueOwnerSwap ||
            instruction.operands.size() != 2) {
          continue;
        }
        functionId = function.id;
        blockId = block.id;
        instructionId = instruction.id;
        secondPlace = instruction.operands[1].place;
      }
    }
  }
  expect(functionId != 0 && blockId != 0 && instructionId != 0 &&
             secondPlace != 0,
         "MIR should retain the exact two-place unique-owner swap");
  expect(lang::effects(lang::IntrinsicKind::UniqueOwnerSwap).readsPlace &&
             lang::effects(lang::IntrinsicKind::UniqueOwnerSwap).writesPlace &&
             lang::effects(lang::IntrinsicKind::UniqueOwnerSwap).movesValue,
         "unique-owner swap effects should retain its non-copying mutation");

  for (int mutation = 0; mutation < 3; ++mutation) {
    auto malformed = frontend.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(functionId));
    lang::MirBlock *block =
        function == nullptr ? nullptr : function->body.findBlock(blockId);
    auto instruction =
        block == nullptr ? std::vector<lang::MirInstruction>::iterator{}
                         : std::find_if(block->instructions.begin(),
                                        block->instructions.end(),
                                        [&](const auto &candidate) {
                                          return candidate.id == instructionId;
                                        });
    expect(block != nullptr && instruction != block->instructions.end(),
           "unique-owner swap mutation should find its MIR instruction");
    if (block == nullptr || instruction == block->instructions.end()) {
      continue;
    }
    if (mutation == 0) {
      instruction->parameterTypes[1].referenceAccess =
          lang::AccessMode::ReadOnly;
    } else if (mutation == 1) {
      instruction->result = 1;
    } else {
      lang::MirPlace *place =
          const_cast<lang::MirPlace *>(function->body.findPlace(secondPlace));
      expect(place != nullptr,
             "unique-owner swap mutation should find its second place");
      if (place != nullptr) {
        place->access = lang::AccessMode::ReadOnly;
      }
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "MIR should reject a read-only, value-producing, or immutable "
           "unique-owner swap");
  }

  bool swapWithoutReplacementCleanup = false;
  if (const lang::MirFunctionInstance *function =
          frontend.mir.findFunctionInstance(functionId)) {
    for (const lang::MirBlock &block : function->body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        if (instruction.id == instructionId)
          swapWithoutReplacementCleanup = !instruction.replacementCleanup &&
                                          instruction.replacementPair == 0 &&
                                          !instruction.elementCleanup;
  }
  expect(swapWithoutReplacementCleanup && nativeBackendEmits(frontend),
         "the backend should lower unique-owner swap as one exchange without "
         "replacement cleanup");

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-unique-owner-swap.gti", R"(
struct Left { public: Left() = default; };
struct Right { public: Right() = default; };
int main() {
  mut std::unique_ptr<Left> left = std::make_unique<Left>();
  mut std::unique_ptr<Right> right = std::make_unique<Right>();
  left.swap(right);
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "unique_ptr::swap should reject owners with different pointee "
         "types before MIR");
}

void testRestrictedMemberOperators() {
  const std::string source = R"(
struct Payload {
  mut int value = 0;

  void increment() mut { this.value += 1; }
public:
  Payload() = default;
};

class Handle {
  mut Payload payload = Payload();
  mut int values[2] = {1, 2};

public:
  Payload& operator->() { return this.payload; }
  mut Payload& operator->() mut { return this.payload; }
  int& operator*() { return this.values[0]; }
  mut int& operator*() mut { return this.values[0]; }
  int& operator[](uint64_t index) { return this.values[index]; }
  mut int& operator[](uint64_t index) mut { return this.values[index]; }
  bool operator==(nullptr_t other) { return false; }
  bool operator!=(nullptr_t other) { return true; }
  operator bool() { return true; }
public:
  Handle() = default;
};

class Number {
  mut int value = 0;

public:
  Number(int initial) : value(initial) {}

  Number operator+() { return Number(this.value); }
  Number operator-() { return Number(0 - this.value); }
  Number operator+(int other) { return Number(this.value + other); }
  Number operator-(int other) { return Number(this.value - other); }
  Number operator*(int other) { return Number(this.value * other); }
  Number operator/(int other) { return Number(this.value / other); }
  Number operator%(int other) { return Number(this.value % other); }
  Number operator~() { return Number(~this.value); }
  Number operator&(int other) { return Number(this.value & other); }
  Number operator|(int other) { return Number(this.value | other); }
  Number operator^(int other) { return Number(this.value ^ other); }
  Number operator<<(int amount) { return Number(this.value << amount); }
  Number operator>>(int amount) { return Number(this.value >> amount); }

  void operator+=(int other) mut { this.value += other; }
  void operator-=(int other) mut { this.value -= other; }
  void operator*=(int other) mut { this.value *= other; }
  void operator/=(int other) mut { this.value /= other; }
  void operator%=(int other) mut { this.value %= other; }
  void operator&=(int other) mut { this.value &= other; }
  void operator|=(int other) mut { this.value |= other; }
  void operator^=(int other) mut { this.value ^= other; }
  void operator<<=(int amount) mut { this.value <<= amount; }
  void operator>>=(int amount) mut { this.value >>= amount; }
  void operator--() mut { this.value--; }

  int get() { return this.value; }
public:
  Number() = default;
};

struct NumberHolder {
  mut Number value = Number(1);
public:
  NumberHolder() = default;
};

class NumberIndex {
  mut Number value = Number(2);

public:
  mut Number& operator[](int index) mut { return this.value; }
public:
  NumberIndex() = default;
};

class NumberOwner {
  mut Number value = Number(3);

public:
  mut Number& operator*() mut { return this.value; }
public:
  NumberOwner() = default;
};

class NumberArrow {
  mut NumberHolder holder = NumberHolder();

public:
  mut NumberHolder& operator->() mut { return this.holder; }
public:
  NumberArrow() = default;
};

int main() {
  mut Handle handle = Handle();
  handle->increment();
  *handle = 7;
  handle[uint64_t(1)] += 3;
  mut Number number = Number(9);
  Number unaryPositive = +number;
  Number unaryNegative = -number;
  mut Number sum = number + 2;
  Number difference = number - 2;
  Number product = number * 2;
  Number quotient = number / 3;
  Number remainder = number % 4;
  Number conjunction = number & 7;
  Number disjunction = number | 2;
  Number exclusive = number ^ 3;
  Number shiftedLeft = number << 1;
  Number shiftedRight = number >> 1;
  sum += 3;
  --sum;
  Number inverted = ~sum;
  mut Number mutations = Number(60);
  mutations -= 1;
  mutations *= 2;
  mutations /= 3;
  mutations %= 17;
  mutations &= 15;
  mutations |= 16;
  mutations ^= 3;
  mutations <<= 1;
  mutations >>= 1;
  mut NumberHolder holder = NumberHolder();
  holder.value += 2;
  mut NumberIndex indexed = NumberIndex();
  indexed[0] += 2;
  mut NumberOwner owner = NumberOwner();
  *owner += 2;
  mut NumberArrow arrow = NumberArrow();
  arrow->value += 2;
  if (handle and handle != nullptr) {
    return *handle + handle[uint64_t(1)] - 12;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("member-operators.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected operator diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the restricted pointer-like member operators should validate");

  const auto *handle = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(1).get());
  std::size_t operatorCount = 0;
  bool foundMutableReferenceReturn = false;
  if (handle != nullptr) {
    for (const lang::StmtPtr &member : handle->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function == nullptr || !function->operatorName()) {
        continue;
      }
      ++operatorCount;
      foundMutableReferenceReturn =
          foundMutableReferenceReturn ||
          (function->returnMutability() == lang::Mutability::Mutable &&
           function->returnType().reference.has_value());
    }
  }
  expect(operatorCount == 9 && foundMutableReferenceReturn,
         "the AST should retain operator identity and mutable reference "
         "returns");

  const lang::FunctionDecl *operatorMain =
      findTopLevelFunction(frontend.program, "main");
  const lang::IfStmt *conditional = nullptr;
  if (operatorMain != nullptr) {
    for (const lang::StmtPtr &statement : operatorMain->body()->statements()) {
      if (const auto *candidate =
              dynamic_cast<const lang::IfStmt *>(statement.get())) {
        conditional = candidate;
        break;
      }
    }
  }
  const auto *logical =
      conditional == nullptr
          ? nullptr
          : dynamic_cast<const lang::Logical *>(conditional->condition().get());
  const lang::ResolvedOperatorInfo *contextual =
      logical == nullptr
          ? nullptr
          : frontend.semantics.findContextualConversion(*logical->left());
  const lang::FunctionInfo *contextualTarget =
      contextual == nullptr
          ? nullptr
          : frontend.semantics.findFunction(contextual->function);
  expect(contextual != nullptr && contextualTarget != nullptr &&
             contextual->kind == lang::OverloadedOperator::ContextualBool &&
             contextual->borrowOrigin == contextualTarget->returnBorrowOrigin &&
             contextual->borrowArgument ==
                 contextualTarget->returnBorrowParameter &&
             contextual->borrowAccess == contextualTarget->returnBorrowAccess,
         "contextual conversion resolution should retain the selected "
         "declaration's owner-dependency summary");

  const auto *number = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(2).get());
  std::size_t arithmeticOperatorCount = 0;
  if (number != nullptr) {
    for (const lang::StmtPtr &member : number->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      arithmeticOperatorCount +=
          function != nullptr && function->operatorName() ? 1 : 0;
    }
  }
  expect(arithmeticOperatorCount == 24,
         "the AST should retain every restricted arithmetic, bitwise, "
         "compound-assignment, and prefix-decrement operator identity");

  // The field-returning arrow cannot fail and publishes its loan directly;
  // the checked-index dereference keeps its failure edge.
  expect(
      mirCallsOperator(frontend, lang::OverloadedOperator::Arrow, false) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::Dereference,
                           true) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::Subscript) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::NotEqual) &&
          mirCallsOperator(frontend,
                           lang::OverloadedOperator::ContextualBool) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::Plus) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::AddAssign) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::PreDecrement) &&
          nativeBackendEmits(frontend),
      "MIR should retain the selected operator member calls");

  const std::string formatted = lang::Formatter().format(
      "class Handle{public:mut int& operator*()mut{return this.value;}"
      "bool operator<(Handle& other){return false;}"
      "Handle operator+(int other){return Handle();}"
      "void operator+=(int other)mut{}void operator--()mut{}"
      "operator bool(){return true;} public: Handle() = default; };");
  expect(formatted.find("mut int& operator*() mut {") != std::string::npos &&
             formatted.find("bool operator<(Handle& other) {") !=
                 std::string::npos &&
             formatted.find("Handle operator+(int other) {") !=
                 std::string::npos &&
             formatted.find("void operator+=(int other) mut {") !=
                 std::string::npos &&
             formatted.find("void operator--() mut {") != std::string::npos &&
             formatted.find("operator bool() {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator declarations should format with stable C++-style layout");

  const auto valueDereference =
      lang::Frontend().analyze("independent-value-dereference.gti", R"(
class ValueHandle {
  int value = 7;
public:
  int operator*() { return this.value; }
public:
  ValueHandle() = default;
};
int main() { ValueHandle handle{}; return *handle - 7; }
)");
  expect(valueDereference.canGenerateCode(),
         "read-only dereference may yield an independent copyable value");

  const std::pair<std::string_view, std::string_view> invalidContracts[] = {
      {"int operator*() mut { return this.value; }",
       "operator* must return a checked reference"},
      {"int operator->() { return this.value; }",
       "operator-> must return a checked reference"},
      {"int& operator[](uint64_t first, uint64_t second) { return this.value; "
       "}",
       "operator[] expects 1 parameter"},
      {"bool operator==(nullptr_t other) mut { return true; }",
       "must use a read-only receiver"},
      {"int operator<(InvalidOperators& other) { return 0; }",
       "operator< must return bool"},
      {"int operator+(int other) mut { return this.value + other; }",
       "operator+ must use a read-only or consuming receiver"},
      {"int& operator-(int other) { return this.value; }",
       "operator- must return an owned value"},
      {"int operator+=(int other) mut { return this.value + other; }",
       "operator+= must return void"},
      {"void operator+=(int other) {}",
       "operator+= must use a mutable receiver"},
      {"void operator--() {}", "operator-- must use a mutable receiver"},
      {"int operator~(int other) { return other; }",
       "operator~ expects 0 parameters"},
      {"operator bool() mut { return true; }", "must use a read-only receiver"},
  };
  for (const auto &[declaration, message] : invalidContracts) {
    const auto invalid = lang::Frontend().analyze(
        "invalid-operator-contract.gti",
        "class InvalidOperators { mut int value = 0; public: " +
            std::string(declaration) + " }; int main() { return 0; }");
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2022") &&
               hasDiagnostic(invalid.diagnostics, std::string(message)),
           "each invalid operator retains its own contract diagnostic: " +
               std::string(declaration));
  }

  const lang::FrontendResult invalidUses =
      lang::Frontend().analyze("invalid-operator-uses.gti", R"(
class ReadOnlyHandle {
  int value = 0;
public:
  int& operator*() { return this.value; }
  bool operator==(int other) { return true; }
public:
  ReadOnlyHandle() = default;
};
int main() {
  mut ReadOnlyHandle handle = ReadOnlyHandle();
  *handle = 1;
  if (handle == nullptr) { return 1; }
  if (handle) { return 2; }
  return 0;
}
)");
  expect(!invalidUses.canGenerateCode() &&
             hasDiagnostic(invalidUses.diagnostics,
                           "Dereference assignment requires mutable access") &&
             hasDiagnostic(invalidUses.diagnostics,
                           "No exact overload of operator==") &&
             hasDiagnostic(invalidUses.diagnostics,
                           "does not define operator bool"),
         "operator use should require mutable access, exact operands, and an "
         "explicit contextual conversion");

  const lang::FrontendResult exactArithmetic =
      lang::Frontend().analyze("exact-arithmetic-operator.gti", R"(
class ExactArithmetic {
public:
  ExactArithmetic operator+(int64_t other) { return ExactArithmetic(); }
public:
  ExactArithmetic() = default;
};
int main() {
  ExactArithmetic value = ExactArithmetic();
  ExactArithmetic result = value + int32_t(1);
  return 0;
}
)");
  expect(!exactArithmetic.canGenerateCode() &&
             hasDiagnostic(exactArithmetic.diagnostics,
                           "No exact overload of operator+ for argument "
                           "types (int32_t)") &&
             hasRelatedDiagnostic(exactArithmetic.diagnostics,
                                  "Candidate: operator+") &&
             hasDiagnosticHint(exactArithmetic.diagnostics,
                               "do not perform implicit conversions"),
         "arithmetic operator resolution should reject implicit operand "
         "conversions and list the exact candidates");

  const lang::FrontendResult noReversedOperands =
      lang::Frontend().analyze("no-reversed-arithmetic-operator.gti", R"(
class RightHandOnly {
public:
  RightHandOnly operator+(int other) { return RightHandOnly(); }
public:
  RightHandOnly() = default;
};
int main() {
  RightHandOnly value = RightHandOnly();
  RightHandOnly result = 1 + value;
  return 0;
}
)");
  expect(!noReversedOperands.canGenerateCode() &&
             hasDiagnostic(noReversedOperands.diagnostics,
                           "requires numeric operands"),
         "operator resolution should not reverse operands or search a "
         "right-hand class for a member operator");

  const lang::FrontendResult noOperatorSynthesis =
      lang::Frontend().analyze("no-operator-synthesis.gti", R"(
class PlusOnly {
public:
  PlusOnly operator+(int other) { return PlusOnly(); }
public:
  PlusOnly() = default;
};
int main() {
  mut PlusOnly value = PlusOnly();
  value += 1;
  return 0;
}
)");
  expect(!noOperatorSynthesis.canGenerateCode() &&
             hasDiagnostic(noOperatorSynthesis.diagnostics,
                           "does not define operator+=") &&
             hasDiagnosticHint(noOperatorSynthesis.diagnostics,
                               "does not synthesize compound operators"),
         "operator+= should require its own declaration instead of being "
         "synthesized from operator+");

  lang::Lexer lexer;
  lang::Parser unsupportedParser(lexer.scan(R"(
class Unsupported {
public:
  bool operator!() { return false; }
};
int main() { return 0; }
)"));
  const lang::Program recovered = unsupportedParser.parse();
  expect(unsupportedParser.hadError() &&
             hasDiagnostic(unsupportedParser.errors(),
                           "operator spelling is not supported for "
                           "overloading") &&
             !recovered.declarations().empty(),
         "unsupported operators should receive a focused parser diagnostic "
         "and recover to later declarations");

  lang::Parser freeOperatorParser(lexer.scan(R"(
int operator*(int value) { return value; }
int main() { return 0; }
)"));
  freeOperatorParser.parse();
  expect(freeOperatorParser.errors().size() == 1 &&
             hasDiagnostic(freeOperatorParser.errors(),
                           "only be declared as class or struct members"),
         "free operator overload declarations should be rejected by the "
         "source grammar");

  const lang::FrontendResult invalidAccess =
      lang::Frontend().analyze("invalid-operator-access.gti", R"(
class PrivateTruth {
  operator bool() { return true; }
public:
  PrivateTruth() = default;
};
class MutableOnly {
  mut int value = 0;
public:
  mut int& operator*() mut { return this.value; }
public:
  MutableOnly() = default;
};
int main() {
  PrivateTruth hidden = PrivateTruth();
  MutableOnly fixed = MutableOnly();
  if (hidden) { return *fixed; }
  return 0;
}
)");
  expect(!invalidAccess.canGenerateCode() &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "operator bool of 'PrivateTruth' is private") &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "operator* requires a mutable receiver"),
         "operator access and receiver mutability should be enforced before "
         "lowering");
}

void testConsumingMethodsAndValueOperators() {
  const std::string source = R"(
class Builder {
public:
  mut int value = 0;
  Builder(int n) : value(n) {}
  Builder(Builder& other) = delete;
  Builder(Builder&& other) = default;
  int finish() { return this.value; }
  int finish() mut { return this.value + 10; }
  int finish() && { return this.value + 20; }
  Builder append(int n) && { this.value += n; return std::move(this); }
  Builder operator/(int n) { return Builder(this.value + n); }
  Builder operator/(int n) && { this.value += n; return std::move(this); }
public:
  Builder() = default;
};
Builder make() { return Builder(1); }
int main() {
  Builder base = Builder(1);
  mut Builder writable = Builder(2);
  int read = base.finish();
  int changed = writable.finish();
  int fresh = make().append(2).finish();
  int chain = (base / 3 / 4).finish();
  int moved = std::move(writable).append(3).finish();
  int conditional = (read == 1 ? make() : Builder(4)).finish();
  return read + changed + fresh + chain + moved + conditional + base.finish() - 111;
}
)";
  const auto frontend = lang::Frontend().analyze(
      "consuming-methods.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << "Consuming methods: " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(), "ordinary methods and value operators "
                                     "should select consuming owned receivers");
  const auto formatted = lang::Formatter().format(source);
  expect(formatted.find("append(int n) &&") != std::string::npos &&
             formatted.find("operator/(int n) &&") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "consuming methods and operators must format without losing their "
         "qualifiers");
  if (frontend.canGenerateCode()) {
    const lang::LanguageQueries queries;
    const auto hover = queries.hover(frontend, frontend.sourceGraph.entryUnit(),
                                     source.find("append(int") + 2);
    expect(hover && hover->signature.find("&&") != std::string::npos,
           "method hover must preserve consuming receiver qualification");
    for (int mutation = 0; mutation < 4; ++mutation) {
      lang::MirProgram forged = frontend.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      bool changed = false;
      for (auto &function : functions) {
        if (changed)
          break;
        for (auto &block : function.body.blocks) {
          if (changed)
            break;
          for (auto &instruction : block.instructions) {
            if (mutation == 3 &&
                instruction.kind == lang::MirInstructionKind::Initialize &&
                instruction.destination) {
              const auto *place =
                  function.body.findPlace(*instruction.destination);
              if (place && place->root == lang::MirPlaceRootKind::Temporary &&
                  place->type.kind == lang::SemanticType::Class) {
                instruction = lang::makeMirInstructionTombstone(instruction);
                changed = true;
              }
            }
            if (mutation == 0 && instruction.callableInvocation ==
                                     lang::CallableInvocationCapability::Once) {
              instruction.callableInvocation.reset();
              changed = true;
            } else if (mutation == 1 &&
                       instruction.callInputKind ==
                           lang::HirCallInputKind::MutableTemporaryBorrow) {
              instruction.callInputKind =
                  lang::HirCallInputKind::ReadTemporaryBorrow;
              instruction.operands.front().kind =
                  lang::MirOperandKind::BorrowRead;
              changed = true;
            } else if (mutation == 2 &&
                       instruction.kind == lang::MirInstructionKind::Move &&
                       instruction.operands.size() == 1) {
              const auto *place =
                  function.body.findPlace(instruction.operands.front().place);
              if (place && place->root == lang::MirPlaceRootKind::This &&
                  place->projections.empty()) {
                function.receiverMutability =
                    lang::ReceiverMutability::ReadOnly;
                changed = true;
              }
            }
            if (changed)
              break;
          }
        }
      }
      expect(
          changed && !lang::verifyMirProgram(forged).valid(),
          "MIR must independently reject forged consuming receiver authority");
    }
  }

  const std::vector<std::pair<std::string, std::string>> invalid = {
      {"class C { public: void take() && {}  public: C() = default; }; int "
       "main() { C c{}; c.take(); "
       "return 0; }",
       "requires an owned temporary"},
      {"class C { public: int x = 1; void take() && {}  public: C() = default; "
       "}; int main() { C c{}; "
       "std::move(c).take(); return c.x; }",
       "already been moved"},
      {"class C { public: C take() { return std::move(this); }  public: C() = "
       "default; }; int main() { "
       "return 0; }",
       "std::move(this) requires a consuming receiver"},
      {"class C { public: ~C() {} C take() && { return this; }  public: C() = "
       "default; }; int main() { "
       "return 0; }",
       "Returning this value would copy move-only type"},
      {"class C { public: int x = 1; int take() && { auto owned = "
       "std::move(this); return this.x; }  public: C() = default; }; int "
       "main() { return 0; }",
       "already been moved"},
      {"class C { public: C take() && { auto owned = std::move(this); return "
       "std::move(this); }  public: C() = default; }; int main() { return 0; }",
       "already been moved"},
      {"class C { public: mut int x = 1; C take() && { int old = "
       "std::move(this.x); return std::move(this); }  public: C() = default; "
       "}; int main() { return 0; "
       "}",
       "already been moved"},
      {"class C { public: int x = 1; int& take() && { return this.x; }  "
       "public: C() = default; }; int "
       "main() { return 0; }",
       "consuming method cannot return"},
      {"class C { public: bool operator==(C& other) && { return true; }  "
       "public: C() = default; }; int "
       "main() { return 0; }",
       "value-producing operators"},
      {"class C { public: void operator+=(int n) && {}  public: C() = default; "
       "}; int main() { return "
       "0; }",
       "value-producing operators"},
      {"class C { public: int x = 1; void take() && {}  public: C() = default; "
       "}; int main() { C c{}; "
       "int& alias = c.x; std::move(c).take(); return alias; }",
       "borrow"},
      {"class C { public: int x = 1; C take() && { int& alias = this.x; auto c "
       "= std::move(this); int observed = alias; return std::move(c); }  "
       "public: C() = default; }; int "
       "main() { return 0; }",
       "borrow"},
      {"class C { public: int x = 1; int read() { return this.x; } int take() "
       "&& { auto c = std::move(this); return read(); }  public: C() = "
       "default; }; int main() { return "
       "0; }",
       "already been moved"},
      {"class C { public: int x = 1; int take(bool condition) && { if "
       "(condition) { auto c = std::move(this); } return this.x; }  public: "
       "C() = default; }; int "
       "main() { return 0; }",
       "may have been moved"},
      {"class C { public: mut int x = 1; void take() && { auto c = "
       "std::move(this); this.x = 2; }  public: C() = default; }; int main() { "
       "return 0; }",
       "already been moved"},
      {"class C { public: int x = 1; void take() && { while (true) { auto c = "
       "std::move(this); } }  public: C() = default; }; int main() { return 0; "
       "}",
       "not available at a reachable loop backedge"},
      {"class C { public: void take() && {}  public: C() = default; }; void "
       "run(C& borrowed) { "
       "borrowed.take(); } int main() { return 0; }",
       "requires an owned temporary"},
      {"interface I { int& take() &&; }; int main() { return 0; }",
       "consuming method cannot return"},
      {"class C<T> { public: T take(T value) && { return value; }  public: C() "
       "= default; }; int "
       "main() { C<std::string_view> c{}; auto value = "
       "std::move(c).take(\"\"); return 0; }",
       "consuming method cannot return"},
  };
  for (const auto &[text, fragment] : invalid) {
    const auto result = lang::Frontend().analyze(
        "invalid-consuming-method.gti", text, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    if (!hasDiagnostic(result.diagnostics, fragment)) {
      std::cerr << "Missing diagnostic " << fragment << " for " << text << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.message << '\n';
    }
    expect(!result.canGenerateCode() &&
               hasDiagnostic(result.diagnostics, fragment) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "invalid consuming method use must fail in the frontend");
  }
  const auto generic = lang::Frontend().analyze(
      "generic-consuming-method.gti", R"(
class Box<T> {
public:
  mut T value;
  Box(T value) : value(std::move(value)) {}
  T take() && { return std::move(this.value); }
  Box<T> identity<U>(U ignored) && { return std::move(this); }
};
int main() { return Box<int>(4).identity<int>(0).take() - 4; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!generic.canGenerateCode())
    for (const auto &diagnostic : generic.diagnostics)
      std::cerr << "Generic consuming: " << diagnostic.message << '\n';
  expect(generic.canGenerateCode(),
         "generic methods and generic owner instances must preserve consuming "
         "receivers");
}

void testLambdas() {
  const std::string source = R"(
int main() {
  int offset = 3;
  auto add = [offset](int value) -> int { return offset + value; };
  auto copied = add;
  auto compose = [copied](int value) -> int {
    return copied(value) + 1;
  };
  int direct = [](int value) -> int { return value * 2; }(4);
  auto inferred = direct;
  return compose(inferred) - 12;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("lambdas.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected lambda diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "explicit value-capture lambdas should pass the frontend");
  expect(frontend.semantics.lambdaCount() == 3 &&
             frontend.hir.lambdaInstances().size() == 3,
         "semantic analysis and HIR should retain each typed closure");

  bool foundCapturedCall = false;
  for (const lang::HirLambda &lambda : frontend.hir.lambdaInstances()) {
    expect(lambda.source != nullptr &&
               lambda.returnType == lang::SemanticType::Int32 &&
               lambda.parameterTypes.size() == 1 &&
               lambda.parameterTypes.front() == lang::SemanticType::Int32,
           "HIR lambdas should retain concrete signatures and source identity");
    for (const lang::HirValue &value : lambda.body.values) {
      foundCapturedCall =
          foundCapturedCall || (value.kind == lang::HirValueKind::Call &&
                                value.lambdaTarget.has_value());
    }
  }
  expect(foundCapturedCall,
         "calls through captured lambda values should resolve in typed HIR");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "MIR should verify concrete closure capture signatures without "
         "reconstructing closure-environment initialization");

  lang::MirProgram wrongLambdaCaptureSize = frontend.mir;
  auto &wrongSizeLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      wrongLambdaCaptureSize.lambdaInstances());
  if (!wrongSizeLambdas.empty()) {
    wrongSizeLambdas.front().captureRequiresActiveCleanup.push_back(false);
  }
  const lang::MirVerificationResult wrongLambdaCaptureSizeResult =
      lang::verifyMirProgram(wrongLambdaCaptureSize);
  expect(!wrongLambdaCaptureSizeResult.valid() &&
             std::any_of(wrongLambdaCaptureSizeResult.errors.begin(),
                         wrongLambdaCaptureSizeResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("wrong size") !=
                                  std::string::npos;
                         }),
         "MIR verification should reject lambda capture cleanup vectors with "
         "a forged size");

  lang::MirProgram wrongLambdaDeclaration = frontend.mir;
  auto &wrongDeclarationLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          wrongLambdaDeclaration.lambdaInstances());
  if (!wrongDeclarationLambdas.empty()) {
    wrongDeclarationLambdas.front().declaration = 0;
  }
  const lang::MirVerificationResult wrongLambdaDeclarationResult =
      lang::verifyMirProgram(wrongLambdaDeclaration);
  expect(!wrongLambdaDeclarationResult.valid(),
         "MIR verification should reject a forged zero lambda declaration "
         "identity");

  lang::MirProgram wrongLambdaType = frontend.mir;
  auto &wrongTypeLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      wrongLambdaType.lambdaInstances());
  if (!wrongTypeLambdas.empty()) {
    wrongTypeLambdas.front().type = lang::SemanticType::Int32;
  }
  const lang::MirVerificationResult wrongLambdaTypeResult =
      lang::verifyMirProgram(wrongLambdaType);
  expect(!wrongTypeLambdas.empty() && !wrongLambdaTypeResult.valid() &&
             std::any_of(wrongLambdaTypeResult.errors.begin(),
                         wrongLambdaTypeResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("lambda instance type") !=
                                  std::string::npos;
                         }),
         "MIR verification should reject lambda-instance type metadata that "
         "does not reproduce its exact declaration and physical shape");

  lang::MirProgram wrongLambdaCaptureFlag = frontend.mir;
  auto &wrongFlagLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      wrongLambdaCaptureFlag.lambdaInstances());
  if (!wrongFlagLambdas.empty() &&
      !wrongFlagLambdas.front().captureRequiresActiveCleanup.empty()) {
    wrongFlagLambdas.front().captureRequiresActiveCleanup.front() =
        !wrongFlagLambdas.front().captureRequiresActiveCleanup.front();
  }
  const lang::MirVerificationResult wrongLambdaCaptureFlagResult =
      lang::verifyMirProgram(wrongLambdaCaptureFlag);
  expect(!wrongLambdaCaptureFlagResult.valid(),
         "MIR verification should reject a forged lambda capture cleanup "
         "fact");

  lang::MirProgram wrongLambdaDropInstance = frontend.mir;
  auto &wrongLambdaFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          wrongLambdaDropInstance.functionInstances());
  bool changedLambdaDropInstance = false;
  for (lang::MirFunctionInstance &function : wrongLambdaFunctions) {
    for (lang::MirDropObligation &obligation : function.body.dropObligations) {
      if (obligation.dropType.type.kind == lang::SemanticType::Lambda) {
        obligation.dropType.lambdaInstance.reset();
        changedLambdaDropInstance = true;
        break;
      }
    }
    if (changedLambdaDropInstance) {
      break;
    }
  }
  const lang::MirVerificationResult wrongLambdaDropInstanceResult =
      lang::verifyMirProgram(wrongLambdaDropInstance);
  expect(!changedLambdaDropInstance || !wrongLambdaDropInstanceResult.valid(),
         "MIR verification should bind lambda cleanup to its exact concrete "
         "HIR instance");

  // Closures copy their immutable value captures, including a captured
  // closure, and the immediately invoked lambda is called directly.
  bool copiedScalarCapture = false;
  bool copiedClosureCapture = false;
  bool directLambdaCall = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure)
          for (std::size_t index = 0;
               index < instruction.closureCaptureTypes.size() &&
               index < instruction.closureCaptureModes.size();
               ++index) {
            if (instruction.closureCaptureModes[index] !=
                lang::LambdaCaptureMode::Copy)
              continue;
            copiedScalarCapture |= instruction.closureCaptureTypes[index] ==
                                   lang::SemanticType::Int32;
            copiedClosureCapture |=
                instruction.closureCaptureTypes[index].kind ==
                lang::SemanticType::Lambda;
          }
        directLambdaCall |=
            instruction.kind == lang::MirInstructionKind::Call &&
            instruction.lambdaTarget.has_value() &&
            entryFunction(frontend, function.id);
      }
  const bool verifiedLambdaBodies =
      frontend.mir.lambdaInstances().size() == 3 &&
      std::all_of(frontend.mir.lambdaInstances().begin(),
                  frontend.mir.lambdaInstances().end(),
                  [](const lang::MirLambdaInstance &lambda) {
                    return !lambda.body.blocks.empty() &&
                           lang::verifyMirBody(lambda.body).valid();
                  });
  expect(copiedScalarCapture && copiedClosureCapture && directLambdaCall &&
             verifiedLambdaBodies &&
             nativeBackendEmits(frontend, lang::OptimizationLevel::O1),
         "verified MIR should materialize immutable closure copies, nested "
         "value captures, and direct invocation");

  const lang::FrontendResult generic =
      lang::Frontend().analyze("generic-lambdas.gti", R"(
T add_with<T>(T offset, T value) {
  auto add = [offset](T input) -> T { return input; };
  return add(value);
}

int main() {
  int signed_result = add_with(1, 2);
  uint64_t unsigned_result = add_with(uint64_t(1), uint64_t(2));
  return signed_result - int(unsigned_result);
}
)");
  expect(generic.canGenerateCode() && generic.hir.lambdaInstances().size() == 2,
         "generic function instances should receive distinct concrete "
         "closure bodies");
  bool foundSigned = false;
  bool foundUnsigned = false;
  for (const lang::HirLambda &lambda : generic.hir.lambdaInstances()) {
    foundSigned = foundSigned ||
                  (lambda.returnType == lang::SemanticType::Int32 &&
                   lambda.parameterTypes.front() == lang::SemanticType::Int32);
    foundUnsigned =
        foundUnsigned ||
        (lambda.returnType == lang::SemanticType::UInt64 &&
         lambda.parameterTypes.front() == lang::SemanticType::UInt64);
  }
  expect(foundSigned && foundUnsigned,
         "HIR should substitute lambda signatures per generic instance");
  expect(lang::verifyMirProgram(generic.mir).valid(),
         "generic closure producers should retain their exact concrete "
         "capture signature in verified MIR");

  lang::MirProgram wrongConcreteClosure = generic.mir;
  bool changedConcreteClosure = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           wrongConcreteClosure.functionInstances())) {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Compute ||
            instruction.operation != lang::MirOperation::Closure ||
            !instruction.lambdaTarget) {
          continue;
        }
        const auto alternate = std::find_if(
            wrongConcreteClosure.lambdaInstances().begin(),
            wrongConcreteClosure.lambdaInstances().end(),
            [&](const lang::MirLambdaInstance &lambda) {
              return lambda.id != *instruction.lambdaTarget &&
                     lambda.captureTypes != instruction.closureCaptureTypes;
            });
        if (alternate != wrongConcreteClosure.lambdaInstances().end()) {
          instruction.lambdaTarget = alternate->id;
          changedConcreteClosure = true;
          break;
        }
      }
      if (changedConcreteClosure) {
        break;
      }
    }
    if (changedConcreteClosure) {
      break;
    }
  }
  expect(changedConcreteClosure &&
             !lang::verifyMirProgram(wrongConcreteClosure).valid(),
         "MIR verification should reject a closure producer retargeted to a "
         "generic lambda with an incompatible capture projection");

  const lang::FrontendResult nestedGeneric = lang::Frontend().analyze(
      "nested-generic-lambdas.gti", R"(
struct CopyableCapture {
  int value = 7;
  CopyableCapture() {}
  CopyableCapture(CopyableCapture& other) = default;
};

int nest<T>(T value) {
  auto outer = [value]() -> int { return 1; };
  auto inner = [outer]() -> int { return outer(); };
  return inner();
}

int main() {
  return nest(1) + nest(CopyableCapture()) - 2;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  bool foundNestedClosureCapture = false;
  for (const lang::MirFunctionInstance &function :
       nestedGeneric.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        foundNestedClosureCapture =
            foundNestedClosureCapture ||
            (instruction.operation == lang::MirOperation::Closure &&
             std::any_of(instruction.closureCaptureTypes.begin(),
                         instruction.closureCaptureTypes.end(),
                         [](const lang::SemanticType &capture) {
                           return capture.kind == lang::SemanticType::Lambda;
                         }));
      }
    }
  }
  expect(nestedGeneric.canGenerateCode() &&
             nestedGeneric.hir.lambdaInstances().size() == 4 &&
             foundNestedClosureCapture &&
             lang::verifyMirProgram(nestedGeneric.mir).valid(),
         "nested generic closure producers should retain and verify the "
         "capture signature of each concrete lambda instance");

  const lang::FrontendResult nestedValueCapture =
      lang::Frontend().analyze("nested-value-capture.gti", R"(
int main() {
  mut int32_t source = 7;
  auto outer = [source]() -> int32_t {
    return [source]() -> int32_t { return source; }();
  };
  return int(outer()) - 7;
}
)");
  expect(nestedValueCapture.canGenerateCode() &&
             nestedValueCapture.hir.lambdaInstances().size() == 2 &&
             lang::verifyMirProgram(nestedValueCapture.mir).valid(),
         "a nested lambda should be able to copy an immutable value capture "
         "from its enclosing lambda environment");

  const lang::FrontendResult expectedLambda = lang::Frontend().analyze(
      "expected-lambda.gti", R"(
int main() {
  auto calculate = [](bool fail) -> expected<int, std::string_view> {
    if (fail) { return unexpected("failed"); }
    return 1;
  };
  expected<int, std::string_view> result = calculate(false);
  return result.value_or(0) - 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  // The result in the lambda signature is an ordinary class instance that
  // the program defines, not a built-in helper.
  const bool expectedLambdaResult = std::any_of(
      expectedLambda.mir.lambdaInstances().begin(),
      expectedLambda.mir.lambdaInstances().end(),
      [&](const lang::MirLambdaInstance &lambda) {
        return isExpectedClass(expectedLambda, lambda.returnType) &&
               std::any_of(expectedLambda.mir.classInstances().begin(),
                           expectedLambda.mir.classInstances().end(),
                           [&](const lang::MirClassInstance &instance) {
                             return instance.type == lambda.returnType;
                           });
      });
  expect(expectedLambda.canGenerateCode() && expectedLambdaResult &&
             nativeBackendEmits(expectedLambda),
         "backend support should account for types nested in lambda "
         "signatures");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-lambdas.gti", R"(
T invoke<T>(T value) { return value; }
auto global_value = 1;

class ThisCapture {
  int value = 1;

public:
  int read() {
    auto invalid = []() -> int { return this.value; };
    return invalid();
  }
public:
  ThisCapture() = default;
};

class BorrowView {
  int& value;

public:
  BorrowView(int& source) : value(source) {}
};

int main() {
  mut int local = 1;
  int& alias = local;
  auto missing_initializer;
  auto inferred_reference = alias;
  auto missing = [](int value) -> int { return local + value; };
  auto duplicate = [local, local]() -> int { return local; };
  auto referenced = [alias]() -> int { return alias; };
  mut auto mutable_lambda = [local](int value) -> int {
    local += value;
    return local;
  };
  auto exact = [](int value) -> int { return value; };
  auto borrowed_return = [](int& source) -> BorrowView {
    return BorrowView(source);
  };
  int wrong_type = exact(true);
  int escaped = invoke(missing);
  return escaped;
}
)");
  expect(!invalid.canGenerateCode(),
         "implicit, duplicate, reference, mutable, and escaping lambdas "
         "should fail");
  expect(
      hasDiagnostic(invalid.diagnostics, "not captured") &&
          hasDiagnostic(invalid.diagnostics, "listed more than once") &&
          hasDiagnostic(invalid.diagnostics,
                        "use an explicit '&' or 'mut &' capture") &&
          hasDiagnostic(invalid.diagnostics,
                        "captured snapshots cannot be made mutable") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot assign to immutable lambda capture") &&
          hasDiagnostic(invalid.diagnostics,
                        "requires an explicit std::move ownership transfer") &&
          hasDiagnostic(invalid.diagnostics,
                        "Lambda return types cannot carry borrowed state") &&
          hasDiagnostic(invalid.diagnostics,
                        "The receiver is not captured; use '[&this]' or '[mut "
                        "&this]'.") &&
          hasDiagnostic(invalid.diagnostics,
                        "Lambda argument 1 has type 'bool'") &&
          hasDiagnostic(invalid.diagnostics, "limited to local bindings") &&
          hasDiagnostic(invalid.diagnostics, "requires an initializer"),
      "lambda diagnostics should explain each restricted lifetime or "
      "capture operation");
  expect(hasRelatedDiagnostic(invalid.diagnostics, "Local binding declared") &&
             hasDiagnosticHint(invalid.diagnostics, "capture list"),
         "missing capture diagnostics should point back to the declaration");

  const lang::FrontendResult moveCapture =
      analyzeWithMemory("move-capture.gti", R"(
int main() {
  std::unique_ptr<int> owner = std::make_unique<int>(1);
  auto invalid = [owner]() -> int { return *owner; };
  return invalid();
}
)");
  expect(!moveCapture.canGenerateCode() &&
             hasDiagnostic(moveCapture.diagnostics, "is not copyable"),
         "move-only values should not enter closures without explicit "
         "ownership-transfer capture syntax");

  const lang::FrontendResult ownedMoveCapture = lang::Frontend().analyze(
      "owned-move-capture.gti", R"(
import std.memory;

class Owner {
  int value = 5;

public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  ~Owner() {}

  int read() { return this.value; }
};

int main() {
  int offset = 2;
  Owner source{};
  auto operation = [offset, owned = std::move(source)]() -> int {
    return offset + owned.read();
  };
  auto relocated = std::move(operation);
  return relocated() - 7;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!ownedMoveCapture.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : ownedMoveCapture.diagnostics) {
      std::cerr << "Unexpected owned-move-capture diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  const lang::FunctionDecl *moveMain =
      findTopLevelFunction(ownedMoveCapture.program, "main");
  const auto *operationDecl =
      moveMain == nullptr || moveMain->body()->statements().size() < 3
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                moveMain->body()->statements()[2].get());
  const auto *moveLambda = operationDecl == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::Lambda *>(
                                     operationDecl->initializer().get());
  const lang::HirLambda *moveHirLambda =
      ownedMoveCapture.hir.lambdaInstances().empty()
          ? nullptr
          : &ownedMoveCapture.hir.lambdaInstances().front();
  const lang::HirFunctionInstance *moveMainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       ownedMoveCapture.hir.functionInstances()) {
    if (instance.source == moveMain) {
      moveMainInstance = &instance;
      break;
    }
  }
  const lang::HirValue *closureValue = nullptr;
  if (moveMainInstance != nullptr) {
    const auto closure = std::find_if(
        moveMainInstance->body.values.begin(),
        moveMainInstance->body.values.end(), [](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Lambda;
        });
    if (closure != moveMainInstance->body.values.end()) {
      closureValue = &*closure;
    }
  }
  const lang::HirValue *moveCaptureOperand =
      closureValue == nullptr || closureValue->operands.size() != 2
          ? nullptr
          : moveMainInstance->body.findValue(closureValue->operands[1]);
  expect(ownedMoveCapture.canGenerateCode() && moveLambda != nullptr &&
             moveLambda->captures().size() == 2 &&
             !moveLambda->captures()[0].explicitInitializer() &&
             moveLambda->captures()[1].explicitInitializer() &&
             moveHirLambda != nullptr && moveHirLambda->captures.size() == 2 &&
             moveHirLambda->captures[0].mode == lang::LambdaCaptureMode::Copy &&
             moveHirLambda->captures[1].mode == lang::LambdaCaptureMode::Move &&
             moveHirLambda->captures[0].bindingSymbol != 0 &&
             moveHirLambda->captures[1].bindingSymbol != 0 &&
             closureValue != nullptr && closureValue->operands.size() == 2 &&
             moveCaptureOperand != nullptr && moveCaptureOperand->ownership &&
             moveCaptureOperand->ownership->kind ==
                 lang::OwnershipEventKind::Move,
         "AST, semantics, and HIR should retain ordered copy and owned-move "
         "closure-environment initialization");

  const lang::MirFunctionInstance *moveMirMain =
      moveMainInstance == nullptr
          ? nullptr
          : ownedMoveCapture.mir.findFunctionInstance(moveMainInstance->id);
  const lang::MirInstruction *closureInstruction = nullptr;
  if (moveMirMain != nullptr) {
    for (const lang::MirBlock &block : moveMirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure) {
          closureInstruction = &instruction;
          break;
        }
      }
      if (closureInstruction != nullptr) {
        break;
      }
    }
  }
  const lang::MirLambdaInstance *moveMirLambda =
      closureInstruction == nullptr || !closureInstruction->lambdaTarget
          ? nullptr
          : ownedMoveCapture.mir.findLambda(*closureInstruction->lambdaTarget);
  const bool exactCapturePlaces =
      moveMirLambda != nullptr &&
      std::all_of(
          moveMirLambda->body.places.begin(), moveMirLambda->body.places.end(),
          [&](const lang::MirPlace &place) {
            return place.capture == 0 ||
                   (place.capture <= moveMirLambda->captureSymbols.size() &&
                    place.symbol ==
                        moveMirLambda->captureSymbols[place.capture - 1] &&
                    place.type ==
                        moveMirLambda->captureTypes[place.capture - 1]);
          });
  expect(
      closureInstruction != nullptr && moveMirLambda != nullptr &&
          closureInstruction->operands.size() == 2 &&
          closureInstruction->closureCaptureModes ==
              std::vector<lang::LambdaCaptureMode>{
                  lang::LambdaCaptureMode::Copy,
                  lang::LambdaCaptureMode::Move} &&
          closureInstruction->operands[0].kind == lang::MirOperandKind::Copy &&
          closureInstruction->operands[1].kind == lang::MirOperandKind::Value &&
          moveMirLambda->captureModes ==
              closureInstruction->closureCaptureModes &&
          moveMirLambda->captureSymbols.size() == 2 && exactCapturePlaces &&
          lang::verifyMirProgram(ownedMoveCapture.mir).valid(),
      "MIR should prove each closure field's exact copy/move initializer, "
      "symbol identity, projection, and cleanup-bearing lambda type");

  lang::MirProgram forgedCaptureMode = ownedMoveCapture.mir;
  bool changedCaptureMode = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           forgedCaptureMode.functionInstances())) {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure &&
            !instruction.closureCaptureModes.empty()) {
          instruction.closureCaptureModes.front() =
              lang::LambdaCaptureMode::Move;
          changedCaptureMode = true;
          break;
        }
      }
      if (changedCaptureMode) {
        break;
      }
    }
    if (changedCaptureMode) {
      break;
    }
  }
  expect(changedCaptureMode &&
             !lang::verifyMirProgram(forgedCaptureMode).valid(),
         "MIR verification should reject a forged closure capture mode");

  lang::MirProgram invalidCaptureMode = ownedMoveCapture.mir;
  auto &invalidModeLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      invalidCaptureMode.lambdaInstances());
  bool changedInvalidCaptureMode = false;
  if (!invalidModeLambdas.empty() &&
      !invalidModeLambdas.front().captureModes.empty()) {
    const auto invalidMode = static_cast<lang::LambdaCaptureMode>(255);
    invalidModeLambdas.front().captureModes.front() = invalidMode;
    for (lang::MirFunctionInstance &function :
         const_cast<std::vector<lang::MirFunctionInstance> &>(
             invalidCaptureMode.functionInstances())) {
      for (lang::MirBlock &block : function.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.operation == lang::MirOperation::Closure &&
              !instruction.closureCaptureModes.empty()) {
            instruction.closureCaptureModes.front() = invalidMode;
            changedInvalidCaptureMode = true;
          }
        }
      }
    }
  }
  expect(changedInvalidCaptureMode &&
             !lang::verifyMirProgram(invalidCaptureMode).valid(),
         "MIR verification should reject an invalid capture mode even when "
         "closure construction and instance metadata agree");

  lang::MirProgram missingCaptureMove = ownedMoveCapture.mir;
  bool changedCaptureMove = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           missingCaptureMove.functionInstances())) {
    std::optional<lang::MirValueId> movedCaptureValue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure &&
            instruction.operands.size() == 2 &&
            instruction.operands[1].kind == lang::MirOperandKind::Value) {
          movedCaptureValue = instruction.operands[1].value;
        }
      }
    }
    if (!movedCaptureValue) {
      continue;
    }
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.result == movedCaptureValue &&
            instruction.kind == lang::MirInstructionKind::Move) {
          instruction.ownership.reset();
          changedCaptureMove = true;
          break;
        }
      }
      if (changedCaptureMove) {
        break;
      }
    }
    if (changedCaptureMove) {
      break;
    }
  }
  expect(changedCaptureMove &&
             !lang::verifyMirProgram(missingCaptureMove).valid(),
         "MIR verification should reject an owned capture whose ordinary "
         "move-state proof was removed");

  lang::MirProgram aliasedCaptureMove = ownedMoveCapture.mir;
  bool changedCaptureAlias = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           aliasedCaptureMove.functionInstances())) {
    std::optional<lang::MirValueId> movedCaptureValue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure &&
            instruction.operands.size() == 2 &&
            instruction.operands[1].kind == lang::MirOperandKind::Value) {
          movedCaptureValue = instruction.operands[1].value;
        }
      }
    }
    if (!movedCaptureValue) {
      continue;
    }
    const auto root =
        std::find_if(function.body.places.begin(), function.body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::Value &&
                              place.value == *movedCaptureValue;
                     });
    if (root == function.body.places.end()) {
      continue;
    }
    lang::MirPlace alias = *root;
    alias.id = function.body.places.size() + 1;
    function.body.places.push_back(std::move(alias));
    changedCaptureAlias = lang::rebuildMirValueUses(function.body);
    break;
  }
  expect(changedCaptureAlias &&
             !lang::verifyMirProgram(aliasedCaptureMove).valid(),
         "MIR verification should reject a second value-root path to an "
         "owned capture's moved initializer");

  lang::MirProgram forgedCapturePlace = ownedMoveCapture.mir;
  auto &forgedCaptureLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          forgedCapturePlace.lambdaInstances());
  bool changedCapturePlace = false;
  if (!forgedCaptureLambdas.empty()) {
    for (lang::MirPlace &place : forgedCaptureLambdas.front().body.places) {
      if (place.capture != 0) {
        place.capture = forgedCaptureLambdas.front().captureTypes.size() + 1;
        changedCapturePlace = true;
        break;
      }
    }
  }
  expect(changedCapturePlace &&
             !lang::verifyMirProgram(forgedCapturePlace).valid(),
         "MIR verification should reject a capture projection retargeted "
         "outside the exact closure environment");

  lang::MirProgram missingCapturePlace = ownedMoveCapture.mir;
  auto &missingCaptureLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          missingCapturePlace.lambdaInstances());
  bool removedCapturePlace = false;
  if (!missingCaptureLambdas.empty()) {
    for (lang::MirPlace &place : missingCaptureLambdas.front().body.places) {
      if (place.capture != 0) {
        place.capture = 0;
        removedCapturePlace = true;
        break;
      }
    }
  }
  expect(removedCapturePlace &&
             !lang::verifyMirProgram(missingCapturePlace).valid(),
         "MIR verification should reject an environment field whose exact "
         "capture projection was erased");

  const lang::FrontendResult parameterMoveCapture = lang::Frontend().analyze(
      "parameter-move-capture.gti", R"(
import std.memory;

class Owner {
  int value = 3;

public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  int read() { return this.value; }
};

int capture_parameter(Owner source) {
  auto action = [owned = std::move(source)]() -> int {
    return owned.read();
  };
  return action();
}

int main() { return capture_parameter(Owner()) - 3; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(parameterMoveCapture.canGenerateCode() &&
             parameterMoveCapture.hir.lambdaInstances().size() == 1 &&
             parameterMoveCapture.hir.lambdaInstances()
                     .front()
                     .captures.front()
                     .mode == lang::LambdaCaptureMode::Move &&
             lang::verifyMirProgram(parameterMoveCapture.mir).valid(),
         "a by-value parameter should be an exact owned move-capture source");

  const lang::FrontendResult invalidOwnedMoveCapture = lang::Frontend().analyze(
      "invalid-owned-move-capture.gti", R"(
import std.memory;

class Owner {
public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
};

class Immobile {
public:
  Immobile() {}
  Immobile(Immobile& other) = delete;
  Immobile(Immobile&& other) = delete;
};

class BorrowView {
  int& value;

public:
  BorrowView(int& value) : value(value) {}
};

int main() {
  Owner first{};
  auto moved = [owned = std::move(first)]() -> int { return 1; };
  auto use_after_move = [first]() -> int { return 2; };
  Owner second{};
  auto once = [owned = std::move(second)]() -> int { return 3; };
  auto twice = [again = std::move(second)]() -> int { return 4; };
  int value = 1;
  auto general = [renamed = value]() -> int { return renamed; };
  Immobile fixed{};
  auto immobile = [owned = std::move(fixed)]() -> int { return 5; };
  mut int number = 1;
  int& alias = number;
  BorrowView view{alias};
  auto borrowed = [owned = std::move(view)]() -> int { return 6; };
  return moved() + use_after_move() + once() + twice() + general() +
         immobile() + borrowed();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalidOwnedMoveCapture.canGenerateCode() &&
          hasDiagnostic(invalidOwnedMoveCapture.diagnostics,
                        "already been moved") &&
          hasDiagnostic(invalidOwnedMoveCapture.diagnostics,
                        "limited to the explicit owned-move form") &&
          hasDiagnostic(invalidOwnedMoveCapture.diagnostics, "is not movable"),
      "owned move capture should update source availability and reject "
      "general or non-movable init captures");

  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
int main() {
  auto bad_default = [=]() -> int { return 0; };
  auto bad_reference = [&value = source]() -> int { return value; };
  auto valid_syntax = [value = std::move(value)]() -> int { return 1; };
  int recovered = 1;
  return recovered;
}
)"));
  const lang::Program recovered = parser.parse();
  expect(parser.hadError() && !recovered.declarations().empty() &&
             hasDiagnostic(parser.errors(), "capture defaults") &&
             hasDiagnostic(parser.errors(),
                           "borrowing capture names an existing binding"),
         "the parser should reject capture defaults and borrowing init "
         "captures while "
         "recovering around the supported move-capture syntax");

  const std::string formatted = lang::Formatter().format(
      "int main(){int offset=1;auto add=[offset](int value)->int{return "
      "offset+value;};return add(1);}");
  expect(formatted.find("auto add = [offset](int value) -> int {\n") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "lambda syntax should receive stable C++-style formatting");
  const std::string moveCaptureFormatted = lang::Formatter().format(
      "int main(){Owner owner{};auto action=[owned=std::move(owner)]()->int{"
      "return owned.read();};return action();}");
  expect(moveCaptureFormatted.find("[owned = std::move(owner)]() -> int {\n") !=
                 std::string::npos &&
             lang::Formatter().format(moveCaptureFormatted) ==
                 moveCaptureFormatted,
         "owned move captures should receive stable C++-familiar formatting");
}

void testGeneratedDefaultTemporaryArgumentBorrows() {
  const auto frontend = lang::Frontend().analyze(
      "generated-default-temporary-arguments.gti", R"gti(
struct Implicit { public: Implicit() = default; };
struct Defaulted { Defaulted() = default; };
struct Cleanup { ~Cleanup() {}  public: Cleanup() = default; };
bool inspect(Implicit& value) { return true; }
bool inspect(Defaulted& value) { return true; }
bool inspect(Cleanup& value) { return true; }
void observe(bool value) {}
Implicit make() { return Implicit(); }
int main() {
  Implicit named = make();
  observe(inspect(Implicit()));
  observe(inspect(Defaulted()));
  observe(inspect(Cleanup()));
  return 0;
}
)gti");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "implicit/defaulted passive and cleanup-owning temporary arguments "
         "use the ordinary generated-default producer");
  if (!frontend.canGenerateCode())
    return;

  lang::HirFunctionInstanceId mainId = 0;
  for (const auto &instance : frontend.hir.functionInstances())
    if (instance.source && instance.source->name().lexeme == "main")
      mainId = instance.id;
  const auto *main = frontend.mir.findFunctionInstance(mainId);
  expect(main != nullptr, "generated temporary fixture retains its main body");
  if (!main)
    return;

  std::size_t stages = 0;
  for (const auto &block : main->body.blocks)
    for (const auto &input : block.instructions) {
      if (input.kind != lang::MirInstructionKind::CallInput ||
          input.callInputRole != lang::MirCallInputRole::Argument ||
          input.callInputKind != lang::HirCallInputKind::ReadTemporaryBorrow)
        continue;
      const auto *place = main->body.findPlace(input.operands.front().place);
      const auto materialization =
          place ? lang::findMirTemporaryMaterialization(main->body, *place,
                                                        input.hirValue)
                : lang::MirTemporaryMaterialization{};
      expect(materialization && materialization.producer &&
                 materialization.producer->kind ==
                     lang::MirInstructionKind::Construct &&
                 !materialization.producer->functionTarget &&
                 !materialization.producer->constructorTarget &&
                 materialization.producer->operands.empty(),
             "each default temporary retains its explicit generated Construct");
      ++stages;
    }
  expect(stages == 3, "all three generated temporary argument loans survive");
  auto invalidHir = frontend.hir;
  auto *hirMain = const_cast<lang::HirFunctionInstance *>(
      invalidHir.findFunctionInstance(mainId));
  bool changedHir = false;
  if (hirMain)
    for (auto &value : hirMain->body.values)
      if (value.kind == lang::HirValueKind::DirectInitializer &&
          !value.constructorTarget) {
        value.kind = lang::HirValueKind::Call;
        changedHir = true;
        break;
      }
  expect(
      changedHir &&
          !lang::verifyHirProgramPlans(frontend.semantics, invalidHir).valid(),
      "HIR retains the semantic generated construction selection");

  // A standalone result must carry the same proof, even without a later
  // temporary receiver or argument loan to expose a forged producer.
  auto invalidStandalone = frontend.mir;
  bool changedStandalone = false;
  for (const auto &candidate : invalidStandalone.functionInstances()) {
    auto *function = const_cast<lang::MirFunctionInstance *>(&candidate);
    if (function->id == mainId)
      continue;
    for (auto &block : function->body.blocks)
      for (auto &producer : block.instructions)
        if (producer.kind == lang::MirInstructionKind::Construct &&
            !producer.constructorTarget) {
          producer.kind = lang::MirInstructionKind::Call;
          changedStandalone = true;
        }
  }
  expect(changedStandalone &&
             !lang::verifyMirProgram(invalidStandalone).valid(),
         "standalone generated results cannot become unexplained calls");

  auto invalidFactory = frontend.mir;
  auto *factoryMain = const_cast<lang::MirFunctionInstance *>(
      invalidFactory.findFunctionInstance(mainId));
  bool changedFactory = false;
  if (factoryMain)
    for (auto &block : factoryMain->body.blocks)
      for (auto &call : block.instructions)
        if (call.kind == lang::MirInstructionKind::Call &&
            call.functionTarget &&
            call.info.type.kind == lang::SemanticType::Class) {
          call.functionTarget.reset();
          changedFactory = true;
          break;
        }
  expect(changedFactory && !lang::verifyMirProgram(invalidFactory).valid(),
         "a factory call cannot become construction by losing its target");
  expect(nativeBackendEmits(frontend),
         "generated temporary arguments use the verified native lifetime path");

  const auto rejects = [&](auto mutation) {
    auto program = frontend.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        program.findFunctionInstance(mainId));
    if (!function)
      return false;
    for (auto &block : function->body.blocks)
      for (auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputRole != lang::MirCallInputRole::Argument ||
            input.callInputKind != lang::HirCallInputKind::ReadTemporaryBorrow)
          continue;
        const auto *place =
            function->body.findPlace(input.operands.front().place);
        const auto materialization =
            place ? lang::findMirTemporaryMaterialization(
                        function->body, *place, input.hirValue)
                  : lang::MirTemporaryMaterialization{};
        if (!materialization)
          return false;
        auto *producer =
            const_cast<lang::MirInstruction *>(materialization.producer);
        mutation(program, function->body, input, *producer);
        return !lang::verifyMirProgram(program).valid();
      }
    return false;
  };
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.kind = lang::MirInstructionKind::Call;
         }),
         "generated construction cannot revert to a targetless ordinary call");
  expect(rejects([&](auto &, auto &, auto &, auto &producer) {
           producer.bodyTarget =
               lang::MirBodyAddress{lang::MirBodyKind::Function, mainId};
         }),
         "bodyless construction cannot acquire an unrelated body target");
  expect(rejects([](auto &, auto &, auto &input, auto &producer) {
           producer.loan = input.loan;
         }),
         "bodyless construction cannot acquire a result loan");
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.definedFailure.propagation =
               lang::FailurePropagationKind::Constructor;
         }),
         "bodyless generated construction cannot invent a failure edge");
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.info.type = lang::SemanticType::UInt64;
         }),
         "a target-free producer cannot substitute another result type");
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.parameterTypes.push_back(lang::SemanticType::UInt64);
           producer.operands.push_back(
               {.kind = lang::MirOperandKind::Constant,
                .literal = lang::Literal{std::uint64_t(1)},
                .type = lang::SemanticType::UInt64});
         }),
         "generated default construction cannot acquire runtime inputs");
  expect(rejects([](auto &program, auto &, auto &, auto &producer) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == producer.info.type)
               owner.defaultConstructor = lang::SpecialMemberStatus::Declared;
         }),
         "a target-free call cannot impersonate a nongenerated constructor");
  expect(rejects([](auto &program, auto &, auto &, auto &producer) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == producer.info.type)
               owner.generatedDefaultRequiresSchedule = true;
         }),
         "a target-free call cannot omit a required initializer schedule");
  expect(rejects([](auto &, auto &body, auto &input, auto &) {
           auto *loan = const_cast<lang::MirLoan *>(body.findLoan(*input.loan));
           loan->access = lang::AccessMode::Mutable;
         }),
         "generated default arguments do not relax read-only call-loan access");
  expect(rejects([](auto &, auto &body, auto &input, auto &) {
           for (auto &drop : body.dropObligations)
             if (drop.kind == lang::MirDropObligationKind::Value &&
                 drop.value == input.hirValue)
               drop.hirFullExpression = 0;
         }),
         "generated default arguments still require exact full-expression "
         "materialization and drop authority");

  const auto unionFrontend =
      lang::Frontend().analyze("generated-default-union-boundary.gti", R"gti(
union Overlay { mut uint64_t first; mut uint32_t second; };
Overlay zero() { return Overlay(); }
int main() {
  mut Overlay overlay{uint64_t(7)};
  unsafe { overlay.second = uint32_t(2); }
  return 0;
}
)gti");
  expect(unionFrontend.canGenerateCode() && unionFrontend.mirValid &&
             lang::verifyMirProgram(unionFrontend.mir).valid(),
         "exact first-field union construction remains distinct from an "
         "empty generated default");
  if (!unionFrontend.canGenerateCode())
    return;
  const auto rejectsUnion = [&](auto mutation) {
    auto program = unionFrontend.mir;
    for (const auto &original : unionFrontend.mir.functionInstances()) {
      auto *function = const_cast<lang::MirFunctionInstance *>(
          program.findFunctionInstance(original.id));
      for (auto &block : function->body.blocks)
        for (auto &construct : block.instructions) {
          if (construct.kind != lang::MirInstructionKind::Construct ||
              construct.constructorTarget || construct.operands.size() != 1)
            continue;
          mutation(program, construct);
          return !lang::verifyMirProgram(program).valid();
        }
    }
    return false;
  };
  expect(rejectsUnion([](auto &, auto &construct) {
           construct.operands.push_back(construct.operands.front());
           construct.parameterTypes.push_back(construct.parameterTypes.front());
         }),
         "a union cannot invent another initialization input");
  expect(rejectsUnion([](auto &, auto &construct) {
           construct.parameterTypes.front() = lang::SemanticType::UInt32;
           construct.operands.front() = {.kind = lang::MirOperandKind::Constant,
                                         .literal =
                                             lang::Literal{std::uint64_t(1)},
                                         .type = lang::SemanticType::UInt32};
         }),
         "a union initializer must use its first field's exact type");
  expect(rejectsUnion([](auto &program, auto &construct) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == construct.info.type)
               owner.kind = lang::ClassKind::Struct;
         }),
         "ordinary records cannot impersonate union-field construction");
  expect(rejectsUnion([](auto &program, auto &construct) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           std::erase_if(owners, [&](const auto &owner) {
             return owner.type == construct.info.type;
           });
         }),
         "union initialization requires its concrete owner");
  expect(rejectsUnion([](auto &program, auto &construct) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == construct.info.type)
               owner.unionLayout->fields.front().field =
                   owner.declaredFields.back().symbol;
         }),
         "union construction retains the exact declared field/layout identity");
  expect(rejectsUnion([](auto &, auto &construct) {
           construct.definedFailure.propagation =
               lang::FailurePropagationKind::Constructor;
         }),
         "passive union construction cannot invent a constructor failure");
  expect(
      rejectsUnion([](auto &, auto &construct) { construct.enumVariant = 0; }),
      "native union construction cannot acquire a payload-enum variant");
}

void testGeneratedDefaultConstructionContexts() {
  const auto frontend =
      lang::Frontend().analyze("generated-default-contexts.gti", R"gti(
namespace construction {
struct Token<T> { int number() { return 1; }  public: Token() = default; };
}
struct construction::Token<int> { int number() { return 2; }  public: Token() = default; };
struct construction::Token<char> { int number() { return 3; }  public: Token() = default; };
construction::Token<T> make_token<T>() { return construction::Token<T>(); }
struct Factory<T> {
  construction::Token<T> make() { return construction::Token<T>(); }
public:
  Factory() = default;
};
struct ConstructorContext<T> {
  construction::Token<T> token;
  ConstructorContext() : token(construction::Token<T>()) {}
  int number() { return this.token.number(); }
};
int nested_token<T>() {
  auto outer = []() -> int {
    auto inner = []() -> int {
      construction::Token<T> token = construction::Token<T>();
      return token.number();
    };
    return inner();
  };
  return outer();
}
int main() {
  construction::Token<int> integer = make_token<int>();
  construction::Token<char> character = make_token<char>();
  if (integer.number() != 2) { return 1; }
  if (character.number() != 3) { return 2; }
  Factory<int> factory = Factory<int>();
  construction::Token<int> member = factory.make();
  if (member.number() != 2) { return 3; }
  ConstructorContext<char> constructed{};
  if (constructed.number() != 3) { return 4; }
  if (nested_token<int>() != 2) { return 5; }
  if (nested_token<char>() != 3) { return 6; }
  return 0;
}
)gti");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "generic function, class, constructor and nested lambda construction "
         "select exact specialized owners");
  if (!frontend.canGenerateCode())
    return;
  const auto rejects = [&](auto mutation, std::string_view detail) {
    auto hir = frontend.hir;
    if (!mutation(hir))
      return false;
    const auto checked = lang::verifyHirProgramPlans(frontend.semantics, hir);
    return !checked.valid() &&
           std::any_of(checked.errors.begin(), checked.errors.end(),
                       [&](const auto &error) {
                         return error.find(detail) != std::string::npos;
                       });
  };
  expect(
      rejects(
          [](auto &hir) {
            for (const auto &candidate : hir.functionInstances()) {
              auto &function =
                  const_cast<lang::HirFunctionInstance &>(candidate);
              if (function.source &&
                  function.source->name().lexeme == "make_token" &&
                  function.typeArguments == std::vector<lang::SemanticType>{
                                                lang::SemanticType::Int32}) {
                function.typeArguments = {lang::SemanticType::Char};
                return true;
              }
            }
            return false;
          },
          "generated default value lost its exact construction selection"),
      "a function's mismatched generic arguments cannot retarget construction");
  expect(rejects(
             [](auto &hir) {
               std::optional<lang::HirValue> first;
               lang::HirValue *second = nullptr;
               for (const auto &candidate : hir.functionInstances()) {
                 auto &function =
                     const_cast<lang::HirFunctionInstance &>(candidate);
                 if (!function.source ||
                     function.source->name().lexeme != "nested_token")
                   continue;
                 for (auto &value : function.body.values)
                   if (value.kind == lang::HirValueKind::Lambda &&
                       value.lambdaTarget) {
                     if (function.typeArguments ==
                         std::vector<lang::SemanticType>{
                             lang::SemanticType::Int32})
                       first = value;
                     else if (function.typeArguments ==
                              std::vector<lang::SemanticType>{
                                  lang::SemanticType::Char})
                       second = &value;
                   }
               }
               if (!first || !second)
                 return false;
               second->lambdaTarget = first->lambdaTarget;
               second->info.type = first->info.type;
               return true;
             },
             "construction body has ambiguous instance arguments"),
         "sibling lambda creators cannot share incompatible construction "
         "contexts");
  expect(rejects(
             [](auto &hir) {
               for (const auto &function : hir.functionInstances())
                 for (const auto &value : function.body.values)
                   if (value.kind == lang::HirValueKind::Lambda &&
                       value.lambdaTarget) {
                     auto *lambda = const_cast<lang::HirLambda *>(
                         hir.findLambda(*value.lambdaTarget));
                     if (!lambda)
                       return false;
                     auto cycle = value;
                     cycle.id = 1000000;
                     lambda->body.values.push_back(std::move(cycle));
                     return true;
                   }
               return false;
             },
             "construction context contains a lambda creator cycle"),
         "a cyclic creator graph cannot infer construction authority");
}

void testTemporaryArgumentBorrows() {
  const std::string source = R"(
class Report {
  int value;

public:
  Report(int value) : value(value) {}
  int code() { return this.value; }
};

class Factory {
public:
  Factory() {}
  Report build(int value) { return Report(value); }
};

Report make_report(int value) { return Report(value); }
int inspect(Report& report) { return report.code(); }
int triple(int left, Report& report, int right) {
  return left + report.code() + right;
}
int combine(Report& left, Report& right) {
  return left.code() + right.code();
}
void observe(Report& report) {}
Report duplicate(Report& report) { return Report(report.code()); }
int generic<T>(T& value) { return 9; }
int choose(Report& report) { return 100 + report.code(); }
int choose(Report report) { return 200 + report.code(); }
int consume(int value) { return value; }
int side(int value) { return value; }

int main() {
  int direct = inspect(Report(1));
  int produced = inspect((make_report(2)));
  int nested = consume(inspect((Report(3))));
  int ordered = triple(side(4), Report(5), side(6));
  int multiple = combine(Report(7), make_report(8));
  observe(Report(11));
  int owned = duplicate(Report(12)).code();
  int concrete = generic(Report(9));
  int owning = choose(Report(10));
  Factory factory = Factory();
  int member_produced = inspect(factory.build(13));
  return direct + produced + nested + ordered + multiple + concrete + owning +
         member_produced;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("temporary-argument-borrows.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected temporary-argument diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "direct construction, GTI-call, grouping, nested, multiple, and "
         "concrete-generic temporary arguments should lower to verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto hirFunction =
      [&](std::string_view name) -> const lang::HirFunctionInstance * {
    const auto found =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [&](const lang::HirFunctionInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == name;
                     });
    return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
  };
  const lang::HirFunctionInstance *main = hirFunction("main");
  expect(main != nullptr,
         "the temporary-argument fixture should retain its main instance");
  if (main == nullptr) {
    return;
  }

  std::size_t temporaryCalls = 0;
  std::size_t temporaryArguments = 0;
  bool owningOverloadSelected = false;
  for (const lang::HirValue &value : main->body.values) {
    if (value.kind != lang::HirValueKind::Call || !value.callPlan) {
      continue;
    }
    std::vector<std::size_t> hirIndices;
    for (const lang::HirCallArgument &argument : value.callPlan->arguments) {
      if (argument.kind != lang::HirCallInputKind::ReadTemporaryBorrow) {
        continue;
      }
      ++temporaryArguments;
      hirIndices.push_back(argument.parameterIndex);
      const lang::HirValue *producer = main->body.findValue(argument.value);
      expect(argument.parameterType.kind == lang::SemanticType::Reference &&
                 argument.parameterType.referenceAccess ==
                     lang::AccessMode::ReadOnly &&
                 argument.parameterType.arguments.size() == 1 &&
                 producer != nullptr &&
                 producer->info.type ==
                     argument.parameterType.arguments.front() &&
                 producer->info.category == lang::ValueCategory::Value &&
                 !producer->info.traits.containsBorrowedState,
             "HIR should retain the exact read-only reference parameter and "
             "fresh self-contained producer for each ephemeral argument");
    }
    const auto *call = dynamic_cast<const lang::Call *>(value.source);
    const lang::ResolvedCallInfo *resolved =
        call == nullptr ? nullptr : frontend.semantics.findCall(*call);
    if (!hirIndices.empty()) {
      ++temporaryCalls;
      expect(resolved != nullptr &&
                 resolved->ephemeralBorrowArguments == hirIndices,
             "HIR temporary argument indices should copy the semantic "
             "overload decision exactly");
    }
    if (resolved != nullptr && resolved->declaration != nullptr &&
        resolved->declaration->name().lexeme == "choose") {
      owningOverloadSelected =
          resolved->ephemeralBorrowArguments.empty() &&
          resolved->parameterTypes.size() == 1 &&
          resolved->parameterTypes.front().kind == lang::SemanticType::Class;
    }
  }
  expect(temporaryCalls == 9 && temporaryArguments == 10 &&
             owningOverloadSelected,
         "the fallback tier should preserve every written temporary argument "
         "while an existing exact by-value overload wins first");

  const lang::MirFunctionInstance *mirMain =
      frontend.mir.findFunctionInstance(main->id);
  expect(mirMain != nullptr,
         "the temporary-argument fixture should retain its MIR main body");
  if (mirMain == nullptr) {
    return;
  }
  std::vector<lang::MirLoanId> argumentLoans;
  for (const lang::MirBlock &block : mirMain->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput ||
          instruction.callInputKind !=
              lang::HirCallInputKind::ReadTemporaryBorrow ||
          instruction.callInputRole != lang::MirCallInputRole::Argument) {
        continue;
      }
      const lang::MirPlace *place =
          instruction.operands.empty()
              ? nullptr
              : mirMain->body.findPlace(instruction.operands.front().place);
      const lang::MirLoan *loan =
          instruction.loan ? mirMain->body.findLoan(*instruction.loan)
                           : nullptr;
      const std::size_t drops =
          place == nullptr
              ? 0
              : static_cast<std::size_t>(std::count_if(
                    mirMain->body.dropObligations.begin(),
                    mirMain->body.dropObligations.end(),
                    [&](const lang::MirDropObligation &drop) {
                      return drop.kind == lang::MirDropObligationKind::Value &&
                             drop.value == instruction.hirValue &&
                             drop.place == place->id &&
                             drop.hirFullExpression != 0 &&
                             drop.fullExpression != 0;
                    }));
      expect(instruction.operands.size() == 1 &&
                 instruction.operands.front().kind ==
                     lang::MirOperandKind::BorrowRead &&
                 place != nullptr &&
                 place->root == lang::MirPlaceRootKind::Value &&
                 place->access == lang::AccessMode::Mutable &&
                 loan != nullptr &&
                 loan->kind == lang::MirLoanKind::CallArgument &&
                 loan->source == place->id &&
                 loan->access == lang::AccessMode::ReadOnly &&
                 loan->producedBy == instruction.callSite && !loan->escapes &&
                 drops == 1,
             "MIR should materialize one owned place, call-scoped read loan, "
             "and full-expression drop for each temporary argument");
      if (loan != nullptr) {
        argumentLoans.push_back(loan->id);
      }
    }
  }
  expect(argumentLoans.size() == 10,
         "MIR should retain all ten ephemeral argument loans");
  for (const lang::MirLoanId loan : argumentLoans) {
    std::size_t endings = 0;
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      endings += static_cast<std::size_t>(std::count_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == loan;
          }));
    }
    expect(endings >= 1,
           "each call-scoped temporary argument loan should end on every "
           "reachable post-invocation route");
  }
  expect(nativeBackendEmits(frontend),
         "the native backend should consume verified temporary argument slots "
         "without native lifetime inference");

  const auto mutableMirMain =
      [&](lang::MirProgram &program) -> lang::MirFunctionInstance * {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    return main->id == 0 || main->id > functions.size()
               ? nullptr
               : &functions[main->id - 1];
  };
  const auto firstTemporaryStage =
      [](lang::MirFunctionInstance &function) -> lang::MirInstruction * {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(function.body.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputKind ==
                lang::HirCallInputKind::ReadTemporaryBorrow &&
            instruction.callInputRole == lang::MirCallInputRole::Argument) {
          return &instruction;
        }
      }
    }
    return nullptr;
  };

  lang::MirProgram wrongIndex = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongIndex)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function)) {
      ++stage->callInputIndex;
    }
  }
  expect(!lang::verifyMirProgram(wrongIndex).valid(),
         "the verifier should reject a forged temporary argument index");

  lang::MirProgram wrongPlace = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongPlace)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function);
        stage != nullptr && !stage->operands.empty()) {
      auto &places =
          const_cast<std::vector<lang::MirPlace> &>(function->body.places);
      places[stage->operands.front().place - 1].access =
          lang::AccessMode::ReadOnly;
    }
  }
  expect(!lang::verifyMirProgram(wrongPlace).valid(),
         "the verifier should reject a temporary argument loan over a "
         "non-materializable place");

  lang::MirProgram wrongLoan = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongLoan)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function);
        stage != nullptr && stage->loan) {
      auto &loans =
          const_cast<std::vector<lang::MirLoan> &>(function->body.loans);
      loans[*stage->loan - 1].access = lang::AccessMode::Mutable;
    }
  }
  expect(!lang::verifyMirProgram(wrongLoan).valid(),
         "the verifier should reject a mutable or mismatched call-argument "
         "loan");

  lang::MirProgram wrongProducer = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongProducer)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function)) {
      stage->hirValue = 0;
    }
  }
  expect(!lang::verifyMirProgram(wrongProducer).valid(),
         "the verifier should reject a temporary argument detached from its "
         "exact producer identity");

  lang::MirProgram missingFullExpression = frontend.mir;
  if (lang::MirFunctionInstance *function =
          mutableMirMain(missingFullExpression)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function)) {
      auto &drops = const_cast<std::vector<lang::MirDropObligation> &>(
          function->body.dropObligations);
      const auto found =
          std::find_if(drops.begin(), drops.end(), [&](const auto &drop) {
            return drop.kind == lang::MirDropObligationKind::Value &&
                   drop.value == stage->hirValue;
          });
      if (found != drops.end()) {
        found->hirFullExpression = 0;
      }
    }
  }
  expect(!lang::verifyMirProgram(missingFullExpression).valid(),
         "the verifier should reject a temporary argument without its exact "
         "full-expression drop identity");

  lang::MirProgram missingEnd = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(missingEnd)) {
    lang::MirInstruction *stage = firstTemporaryStage(*function);
    const lang::MirLoanId loan =
        stage != nullptr && stage->loan ? *stage->loan : 0;
    if (loan != 0) {
      for (lang::MirBlock &block :
           const_cast<std::vector<lang::MirBlock> &>(function->body.blocks)) {
        auto &instructions =
            const_cast<std::vector<lang::MirInstruction> &>(block.instructions);
        const auto ending = std::find_if(
            instructions.begin(), instructions.end(),
            [&](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == loan;
            });
        if (ending != instructions.end()) {
          instructions.erase(ending);
          break;
        }
      }
    }
  }
  expect(!lang::verifyMirProgram(missingEnd).valid(),
         "the verifier should reject a missing immediate temporary argument "
         "loan ending");

  const auto rejected =
      [](std::string_view name, std::string_view source,
         const std::vector<std::filesystem::path> &preludes = {}) {
        // The prelude includes <std/meta>, so it needs the library root.
        return lang::Frontend().analyze(
            std::string(name), std::string(source), preludes, {},
            preludes.empty() ? std::vector<std::filesystem::path>{}
                             : std::vector{standardLibraryRoot()});
      };
  const lang::FrontendResult mutableReference =
      rejected("mutable-temporary-argument.gti", R"(
class Report { public: Report() {} };
void update(mut Report& report) {}
int main() { update(Report()); return 0; }
)");
  expect(!mutableReference.canGenerateCode() &&
             hasDiagnosticCode(mutableReference.diagnostics, "GTI-S2017"),
         "a mutable reference should remain unable to bind a temporary");

  const lang::FrontendResult localReference =
      rejected("local-temporary-reference.gti", R"(
class Report { public: Report() {} };
int main() { Report& alias = Report(); return 0; }
)");
  expect(!localReference.canGenerateCode() &&
             hasDiagnosticCode(localReference.diagnostics, "GTI-S2017"),
         "a local reference binding should not gain lifetime extension");

  const std::vector<std::filesystem::path> prelude{standardLibraryPrelude()};
  // An explicitly moved whole local now binds a read-only parameter as a
  // full-expression temporary, as an explicitly moved receiver does.
  const lang::FrontendResult explicitMove =
      rejected("moved-temporary-argument.gti", R"(
class Report { public: Report() {} };
void inspect(Report& report) {}
int main() {
  mut Report report = Report();
  inspect(std::move(report));
  return 0;
}
)",
               prelude);
  expect(explicitMove.canGenerateCode() &&
             lang::verifyMirProgram(explicitMove.mir).valid(),
         "an explicitly moved local should bind a read-only parameter as a "
         "call-scoped temporary");
  const lang::FrontendResult explicitMutableMove =
      rejected("moved-mutable-temporary-argument.gti", R"(
class Report { public: Report() {} };
void update(mut Report& report) {}
int main() {
  mut Report report = Report();
  update(std::move(report));
  return 0;
}
)",
               prelude);
  expect(!explicitMutableMove.canGenerateCode() &&
             hasDiagnosticCode(explicitMutableMove.diagnostics, "GTI-S2017"),
         "an explicit move from named storage should not count as fresh");

  const lang::FrontendResult compound =
      rejected("compound-temporary-argument.gti", R"(
class Report { public: Report() {} };
void inspect(Report& report) {}
int main() {
  bool select = true;
  inspect(select ? Report() : Report());
  return 0;
}
)");
  expect(compound.canGenerateCode() &&
             lang::verifyMirProgram(compound.mir).valid(),
         "a conditional class producer should bind an exact call-scoped "
         "read-only argument");

  const lang::FrontendResult borrowedState =
      rejected("borrowed-state-temporary-argument.gti", R"(
class View {
  int& value;
public:
  View(int& value) : value(value) {}
};
void inspect(View& view) {}
int main() {
  mut int value = 1;
  inspect(View(value));
  return 0;
}
)");
  expect(borrowedState.canGenerateCode() &&
             lang::verifyMirProgram(borrowedState.mir).valid() &&
             checkedTemporaryObserver(borrowedState, "inspect", true),
         "a fresh carrier with a proved stable dependency now supplies a "
         "nonescaping readonly argument");

  const lang::FrontendResult escapingResult =
      rejected("escaping-temporary-argument.gti", R"(
class Report { public: Report() {} };
Report& retain(Report& report) { return report; }
int main() { [[discard]] retain(Report()); return 0; }
)");
  expect(!escapingResult.canGenerateCode() &&
             hasDiagnosticCode(escapingResult.diagnostics, "GTI-S2017"),
         "a call result tied to the temporary parameter should remain "
         "ineligible");

  const lang::FrontendResult ambiguousFallback =
      rejected("ambiguous-temporary-argument.gti", R"(
class Report { public: Report() {} };
int inspect(Report& report) { return 1; }
int inspect<T>(T& report) { return 2; }
int main() { return inspect(Report()); }
)");
  expect(!ambiguousFallback.canGenerateCode() &&
             hasDiagnosticCode(ambiguousFallback.diagnostics, "GTI-S2013"),
         "ordinary ambiguity should remain authoritative within the "
         "ephemeral-borrow fallback tier");
}

} // namespace
} // namespace ownership

namespace borrowing_callbacks {
namespace {
void expireLoanBefore(lang::MirBody &body, lang::MirLoanId loan,
                      lang::MirInstructionId consumer) {
  lang::MirInstructionId last = 0;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      last = std::max(last, instruction.id);
  for (auto &block : body.blocks) {
    const auto position = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const auto &instruction) { return instruction.id == consumer; });
    if (position != block.instructions.end()) {
      block.instructions.insert(position,
                                {.id = last + 1,
                                 .kind = lang::MirInstructionKind::EndBorrow,
                                 .loan = loan});
      return;
    }
  }
}

bool callResultAliasProofTests() {
  using namespace lang;
  MirBody original;
  original.entry = 1;
  SemanticType reference = SemanticType::Reference;
  reference.arguments = {SemanticType::Int32};
  original.places.push_back({.id = 1,
                             .root = MirPlaceRootKind::Binding,
                             .binding = 1,
                             .type = reference});
  original.loans.push_back({.id = 1,
                            .kind = MirLoanKind::CallResult,
                            .source = 1,
                            .producedBy = 1,
                            .carriers = {1}});
  MirInstruction producer;
  producer.id = 1;
  producer.kind = MirInstructionKind::Call;
  producer.hirValue = 1;
  producer.loan = 1;
  producer.result = 1;
  producer.info.type = SemanticType::Int32;
  producer.info.category = ValueCategory::Place;
  MirInstruction initialize;
  initialize.id = 2;
  initialize.kind = MirInstructionKind::Initialize;
  initialize.destination = 1;
  initialize.operands = {
      {.kind = MirOperandKind::Loan, .loan = 1, .type = SemanticType::Int32}};
  MirBlock block;
  block.id = 1;
  block.reachable = true;
  block.instructions = {producer, initialize};
  block.terminator.kind = MirTerminatorKind::Return;
  original.blocks.push_back(block);
  const auto proof = [](const MirBody &body, std::size_t position) {
    return mir_detail::callResultReferenceAliases(
        body, body.loans.front(), body.blocks.front(), position);
  };
  if (!proof(original, 2)) {
    std::cerr << "exact call-result reference alias proof failed\n";
    return false;
  }
  auto runtimeAlias = original;
  runtimeAlias.places.push_back({.id = 2,
                                 .root = MirPlaceRootKind::Loan,
                                 .loan = 1,
                                 .type = SemanticType::Int32});
  runtimeAlias.blocks.front().instructions[1].operands.front().place = 2;
  if (!proof(runtimeAlias, 2)) {
    std::cerr << "exact runtime call-result alias proof failed\n";
    return false;
  }
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto changed = runtimeAlias;
    auto &referent = changed.places.back();
    if (mutation == 0)
      referent.loan = 2;
    else if (mutation == 1)
      referent.type = SemanticType::Bool;
    else if (mutation == 2)
      referent.projections.push_back({.kind = MirProjectionKind::Dereference});
    else
      referent.access = AccessMode::Mutable;
    if (proof(changed, 2)) {
      std::cerr << "accepted forged runtime call-result alias " << mutation
                << '\n';
      return false;
    }
  }
  for (int mutation = 0; mutation != 17; ++mutation) {
    auto changed = original;
    auto &instructions = changed.blocks.front().instructions;
    std::size_t consumer = 2;
    switch (mutation) {
    case 0:
      consumer = 1;
      break; // Consumer precedes alias initialization.
    case 1:
      std::swap(instructions[0], instructions[1]);
      break;
    case 2: // Root and operand agree, but not with the actual producer type.
      changed.places.front().type.arguments.front() = SemanticType::Bool;
      instructions[1].operands.front().type = SemanticType::Bool;
      break;
    case 3:
      changed.places.front().type.referenceAccess = AccessMode::Mutable;
      break;
    case 4:
      instructions[1].operands.front().loan = 2;
      break;
    case 5:
      instructions.push_back(initialize);
      consumer = 3;
      break;
    case 6:
      changed.blocks.front().terminator.targetArguments = {
          {.kind = MirOperandKind::Copy, .place = 1, .type = reference}};
      break;
    case 7:
      instructions.push_back({.id = 3,
                              .operands = {{.kind = MirOperandKind::Address,
                                            .place = 1,
                                            .type = reference}}});
      consumer = 3;
      break;
    case 8:
      instructions[0].hirValue = 2;
      break;
    case 9:
      instructions[0].info.access = AccessMode::Mutable;
      break;
    case 10:
      instructions.push_back(producer);
      consumer = 3;
      break;
    case 11:
      changed.loans.front().carriers.push_back(1);
      break;
    case 12:
      changed.loans.front().carriers.clear();
      break;
    case 13:
      consumer = 3;
      break;
    case 14:
      changed.blocks.front().terminator.switchTargets = {
          {.target = 1,
           .arguments = {
               {.kind = MirOperandKind::Copy, .place = 1, .type = reference}}}};
      break;
    case 15:
      instructions.push_back(initialize);
      instructions.back().kind = MirInstructionKind::Assign;
      consumer = 3;
      break;
    case 16:
      instructions.push_back(
          {.id = 3,
           .kind = MirInstructionKind::Move,
           .operands = {
               {.kind = MirOperandKind::Move, .place = 1, .type = reference}}});
      consumer = 3;
      break;
    }
    if (proof(changed, consumer)) {
      std::cerr << "accepted forged call-result alias proof " << mutation
                << '\n';
      return false;
    }
  }
  auto acrossBlocks = original;
  acrossBlocks.blocks.front().terminator.kind = MirTerminatorKind::Goto;
  acrossBlocks.blocks.front().terminator.target = 2;
  MirBlock consumer;
  consumer.id = 2;
  consumer.reachable = true;
  consumer.terminator.kind = MirTerminatorKind::Return;
  acrossBlocks.blocks.push_back(consumer);
  if (!mir_detail::callResultReferenceAliases(acrossBlocks,
                                              acrossBlocks.loans.front(),
                                              acrossBlocks.blocks.back(), 0)) {
    std::cerr << "dominating call-result reference alias proof failed\n";
    return false;
  }
  // The initializer on only one branch must not authorize the join.
  acrossBlocks.blocks.front().instructions.pop_back();
  acrossBlocks.blocks.front().terminator.kind = MirTerminatorKind::Branch;
  acrossBlocks.blocks.front().terminator.elseTarget = 3;
  acrossBlocks.blocks[1].instructions = {initialize};
  acrossBlocks.blocks[1].terminator.kind = MirTerminatorKind::Goto;
  acrossBlocks.blocks[1].terminator.target = 3;
  consumer.id = 3;
  acrossBlocks.blocks.push_back(consumer);
  if (mir_detail::callResultReferenceAliases(acrossBlocks,
                                             acrossBlocks.loans.front(),
                                             acrossBlocks.blocks.back(), 0)) {
    std::cerr << "accepted nondominating call-result alias initialization\n";
    return false;
  }
  return true;
}

bool referenceInitializerBodyProofTests() {
  using namespace lang;
  const auto analyzed =
      Frontend().analyze("reference-initializer-body-proof.gti", R"gti(
int global_value = 3;
int& independent() { return global_value; }
int& relay(int& value) { return value; }
class View {
  int& value;
public:
  View(int& value) : value(relay(value)) {}
  int read() { return this.value; }
};
int main() {
  int owner = 7;
  View view{owner};
  return view.read() + independent() - 10;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  const MirFunctionInstance *global = nullptr;
  for (const auto &function : analyzed.mir.functionInstances())
    if (function.returnBorrowOrigin == BorrowOriginKind::Global &&
        function.parameterTypes.empty())
      global = &function;
  if (!global)
    return false;
  bool checked = false;
  for (const auto &constructor : analyzed.mir.constructorInstances())
    for (const auto &initializer : constructor.initializers) {
      const auto *call = mir_detail::referenceInitializerCall(
          analyzed.mir, constructor, initializer);
      if (!call)
        continue;
      const auto *original =
          analyzed.mir.findFunctionInstance(*call->functionTarget);
      if (!original || original->returnType != global->returnType ||
          original->parameterBindings.size() != 1)
        return false;
      const auto formal = std::find_if(
          original->body.places.begin(), original->body.places.end(),
          [&](const auto &place) {
            return place.root == MirPlaceRootKind::Binding &&
                   place.projections.empty() &&
                   place.binding == original->parameterBindings.front();
          });
      if (formal == original->body.places.end())
        return false;
      auto changed = analyzed.mir;
      auto *helper = const_cast<MirFunctionInstance *>(
          changed.findFunctionInstance(original->id));
      helper->body = global->body;
      helper->body.placeDomain = original->body.placeDomain;
      for (auto &place : helper->body.places)
        if (place.key)
          place.key->domain = helper->body.placeDomain;
      for (auto &block : helper->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.ownership)
            instruction.ownership->place.domain = helper->body.placeDomain;
      auto unusedFormal = *formal;
      unusedFormal.id = helper->body.places.size() + 1;
      helper->body.places.push_back(unusedFormal);
      // A genuinely valid global-returning body, not a dangling/unknown place.
      // The incoming formal is unused; signature and claimed parameter-origin
      // contract are deliberately unchanged from the real relay.
      const auto bodyProof = verifyMirBody(helper->body, helper->id);
      if (!bodyProof.valid()) {
        for (const auto &error : bodyProof.errors)
          std::cerr << "non-relay mutation invalid locally: " << error.message
                    << '\n';
        return false;
      }
      const auto *unchangedConstructor =
          changed.findConstructorInstance(constructor.id);
      if (!mir_detail::referenceInitializerCall(changed, *unchangedConstructor,
                                                initializer)) {
        std::cerr
            << "non-relay mutation changed the declared initializer shape\n";
        return false;
      }
      const auto programProof = verifyMirProgram(changed);
      if (std::none_of(
              programProof.errors.begin(), programProof.errors.end(),
              [&](const auto &error) {
                return error.bodyKind == MirBodyKind::Function &&
                       error.owner == helper->id &&
                       error.message.find(
                           "argument-dependent return does not originate") !=
                           std::string::npos;
              })) {
        std::cerr << "non-relay helper was not rejected by its actual "
                     "return-origin contract\n";
        for (const auto &error : programProof.errors)
          std::cerr << error.message << '\n';
        return false;
      }
      checked = true;
    }
  return checked;
}

bool namedRestorationReturnTests() {
  using namespace lang;
  const auto application = [](const std::string &name,
                              const std::string &source) {
    const auto entry = std::filesystem::temp_directory_path() / name;
    return Frontend().analyze(
        entry, source, {standardLibraryPrelude()},
        {{std::filesystem::weakly_canonical(entry).string(), source}},
        {standardLibraryRoot()});
  };
  const auto analyzed = application("named-restoration-return-proof.gti", R"gti(
import std.memory;
struct Reader {
  int& value;
  Reader(int& source) : value(source) {}
  int read() { return this.value; }
};
struct Writer {
  mut int& value;
  Writer(mut int& source) : value(source) {}
  void increment() mut { this.value += 1; }
};
unsafe Reader& restore_reader(std::erased_object& object) {
  unsafe { Reader& restored = std::restore_ref<Reader>(object); return restored; }
}
unsafe mut Writer& restore_writer(mut std::erased_object& object) {
  unsafe { mut Writer& restored = std::restore_mut<Writer>(object); return restored; }
}
int main() {
  int value = 9;
  Reader reader{value};
  std::erased_object& object = std::erase_ref(reader);
  unsafe {
    Reader& restored = restore_reader(object);
    if (restored.read() != 9) return 1;
  }
  mut int count = 4;
  mut Writer writer{count};
  mut std::erased_object& mutable_object = std::erase_mut(writer);
  unsafe {
    mut Writer& restored = restore_writer(mutable_object);
    restored.increment();
  }
  return count - 5;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &function : analyzed.mir.functionInstances()) {
      if (verifyMirBody(function.body, function.id).valid())
        continue;
      std::cerr << "restore body " << function.id
                << " summary=" << function.restoresReferenceResult << '\n';
      for (const auto &loan : function.body.loans)
        std::cerr << " loan " << loan.id << " kind=" << int(loan.kind)
                  << " source=" << loan.source << " parent=" << loan.parent
                  << " stored=" << loan.storedDependency
                  << " entry=" << loan.entry << " producer=" << loan.producedBy
                  << " carriers=" << loan.carriers.size() << '\n';
      for (const auto &place : function.body.places)
        std::cerr << " place " << place.id << " root=" << int(place.root)
                  << " binding=" << place.binding
                  << " type=" << int(place.type.kind)
                  << " projections=" << place.projections.size()
                  << " available=" << place.initiallyAvailable << '\n';
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.loan || instruction.destination) {
            std::cerr << " instruction " << instruction.id
                      << " kind=" << int(instruction.kind)
                      << " loan=" << instruction.loan.value_or(0)
                      << " hir=" << instruction.hirValue
                      << " restore=" << instruction.restoresReferenceResult
                      << " fields=" << instruction.restoredBorrowFields.size()
                      << " dest=" << instruction.destination.value_or(0)
                      << '\n';
            for (const auto &operand : instruction.operands)
              std::cerr << "  operand kind=" << int(operand.kind)
                        << " loan=" << operand.loan
                        << " place=" << operand.place
                        << " value=" << operand.value
                        << " type=" << int(operand.type.kind) << '\n';
          }
    }
    return false;
  }
  std::size_t checked = 0;
  for (const auto &function : analyzed.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *output = instruction.loan
                                 ? function.body.findLoan(*instruction.loan)
                                 : nullptr;
        const auto *input =
            instruction.operands.size() == 1 &&
                    instruction.operands.front().kind == MirOperandKind::Loan
                ? function.body.findLoan(instruction.operands.front().loan)
                : nullptr;
        if (instruction.kind != MirInstructionKind::Borrow || !output ||
            !input || output->kind != MirLoanKind::Return ||
            input->kind != MirLoanKind::CallResult || input->carriers.empty())
          continue;
        ++checked;
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto changed = analyzed.mir;
          auto *target = const_cast<MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &retained = target->body.loans[input->id - 1];
          auto &published = target->body.loans[output->id - 1];
          if (mutation == 0)
            published.storedDependency = 0;
          else if (mutation == 1)
            retained.parent = 0;
          else if (mutation == 2) {
            auto &witness = target->body.loans[retained.storedDependency - 1];
            witness.access = witness.access == AccessMode::ReadOnly
                                 ? AccessMode::Mutable
                                 : AccessMode::ReadOnly;
          } else if (mutation == 3)
            retained.lifecycleBound = true;
          else if (mutation == 4)
            retained.additionalDependencies.push_back(
                {.source = retained.source});
          else {
            MirInstructionId last = 0;
            for (const auto &candidateBlock : target->body.blocks)
              for (const auto &candidate : candidateBlock.instructions)
                last = std::max(last, candidate.id);
            auto &instructions = target->body.blocks[block.id - 1].instructions;
            const auto position =
                std::find_if(instructions.begin(), instructions.end(),
                             [&](const auto &candidate) {
                               return candidate.id == instruction.id;
                             });
            instructions.insert(position,
                                {.id = last + 1,
                                 .kind = MirInstructionKind::EndBorrow,
                                 .hirValue = instruction.hirValue,
                                 .loan = input->id});
          }
          if (verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged named-restoration return " << mutation
                      << '\n';
            return false;
          }
        }
      }
  if (checked != 2) {
    std::cerr << "named restoration did not exercise both read and mutable "
                 "alias transfers: "
              << checked << '\n';
    return false;
  }
  for (const auto &[name, source] :
       std::vector<std::pair<std::string, std::string>>{
           {"restoration-local-owner", R"gti(
import std.memory;
struct Reader { int& value; Reader(int& source) : value(source) {} };
Reader& bad(int& source) {
  Reader local{source};
  std::erased_object& object = std::erase_ref(local);
  unsafe { Reader& restored = std::restore_ref<Reader>(object); return restored; }
}
int main() { return 0; }
)gti"},
           {"restoration-access-upgrade", R"gti(
import std.memory;
struct Reader { int& value; Reader(int& source) : value(source) {} };
unsafe mut Reader& bad(std::erased_object& object) {
  unsafe { Reader& restored = std::restore_ref<Reader>(object); return restored; }
}
int main() { return 0; }
)gti"}}) {
    const auto rejected = application(name + ".gti", source);
    if (rejected.canGenerateCode() ||
        std::none_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2017";
                     })) {
      std::cerr << "missing source lifetime rejection for " << name << '\n';
      return false;
    }
  }
  return true;
}

bool receiverCaptureTests() {
  const auto accepted =
      lang::Frontend().analyze("receiver-captures-mir.gti", R"gti(
class Counter {
  mut int value = 4;
public:
  Counter() = default;
  int read() { return this.value; }
  int shared() {
    auto callback = [&this]() -> int { return this.read(); };
    auto copy = callback;
    return copy();
  }
  int exclusive() mut {
    mut auto callback = [mut &this]() -> int { this.value += 1; return this.value; };
    int result = callback();
    this.value += 1;
    return result;
  }
};
int main() {
  mut Counter counter{};
  if (counter.shared() != 4) return 1;
  if (counter.exclusive() != 5) return 2;
  return counter.read() - 6;
}
)gti");
  if (!accepted.canGenerateCode() ||
      !lang::verifyMirProgram(accepted.mir).valid()) {
    for (const auto &diagnostic : accepted.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  bool shared = false;
  bool exclusive = false;
  for (const auto &lambda : accepted.mir.lambdaInstances()) {
    if (lambda.captureTypes.size() != 1 ||
        lambda.captureTypes.front().kind != lang::SemanticType::Reference)
      return false;
    shared |=
        lambda.captureModes.front() == lang::LambdaCaptureMode::BorrowRead;
    exclusive |=
        lambda.captureModes.front() == lang::LambdaCaptureMode::BorrowMut;
    auto changed = accepted.mir;
    auto *target =
        const_cast<lang::MirLambdaInstance *>(changed.findLambda(lambda.id));
    target->captureModes.front() = lang::LambdaCaptureMode::Copy;
    if (lang::verifyMirProgram(changed).valid()) {
      std::cerr << "accepted forged receiver capture ownership\n";
      return false;
    }
  }
  if (!shared || !exclusive)
    return false;
  const std::pair<std::string_view, std::string_view> rejected[] = {
      {"shared-write", R"gti(class C { mut int n = 0; public: C() = default;
        void run() mut { auto f = [&this]() -> void { this.n += 1; }; f(); }
      }; int main() { mut C c{}; c.run(); })gti"},
      {"mutable-readonly-receiver",
       R"gti(class C { mut int n = 0; public: C() = default;
        void run() { mut auto f = [mut &this]() -> void { this.n += 1; }; f(); }
      }; int main() { C c{}; c.run(); })gti"},
      {"shared-conflict", R"gti(class C { mut int n = 0; public: C() = default;
        int run() mut { auto f = [&this]() -> int { return this.n; }; this.n = 3; return f(); }
      }; int main() { mut C c{}; return c.run(); })gti"},
      {"exclusive-conflict",
       R"gti(class C { mut int n = 0; public: C() = default;
        int run() mut { mut auto f = [mut &this]() -> int { return this.n; }; int n = this.n; return f() + n; }
      }; int main() { mut C c{}; return c.run(); })gti"},
      {"static-receiver", R"gti(class C { public: static int run() {
        auto f = [&this]() -> int { return 1; }; return f(); }
      }; int main() { return C::run(); })gti"},
      {"no-receiver",
       R"gti(int main() { auto f = [&this]() -> int { return 1; }; return f(); })gti"},
      {"missing-capture", R"gti(class C { int n = 0; public: C() = default;
        int run() { auto f = []() -> int { return this.n; }; return f(); }
      }; int main() { C c{}; return c.run(); })gti"},
      {"duplicate-receiver", R"gti(class C { public: C() = default;
        int run() { auto f = [&this, &this]() -> int { return 1; }; return f(); }
      }; int main() { C c{}; return c.run(); })gti"},
      {"copy-exclusive", R"gti(class C { mut int n = 0; public: C() = default;
        int run() mut { mut auto f = [mut &this]() -> int { return this.n; }; mut auto copy = f; return copy(); }
      }; int main() { mut C c{}; return c.run(); })gti"},
      {"nested-receiver", R"gti(class C { int n = 0; public: C() = default;
        int run() { auto f = [&this]() -> int {
          auto inner = [&this]() -> int { return this.n; }; return inner();
        }; return f(); }
      }; int main() { C c{}; return c.run(); })gti"},
      {"captured-reference-result",
       R"gti(class C { int n = 0; public: C() = default;
        int run() { auto f = [&this]() -> int& { return this.n; }; return f(); }
      }; int main() { C c{}; return c.run(); })gti"},
      {"value-receiver", R"gti(class C { public: C() = default;
        int run() { auto f = [this]() -> int { return 1; }; return f(); }
      }; int main() { C c{}; return c.run(); })gti"},
  };
  for (const auto &[name, source] : rejected) {
    const auto result =
        lang::Frontend().analyze(std::string(name), std::string(source));
    const std::string_view expectedCode =
        name == "shared-write" ? "GTI-S2000"
        : name == "shared-conflict" || name == "exclusive-conflict"
            ? "GTI-S2017"
        : name == "static-receiver" || name == "no-receiver" ||
                name == "missing-capture"
            ? "GTI-S2027"
            : "";
    if (!expectedCode.empty() &&
        std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &d) {
                       return d.code == expectedCode &&
                              d.primary.end > d.primary.start;
                     })) {
      std::cerr << "receiver diagnostic missing: " << name << " expected "
                << expectedCode << '\n';
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
      return false;
    }
    if (result.canGenerateCode() || result.diagnostics.empty() ||
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto &d) { return d.code == "GTI-B0001"; })) {
      std::cerr << "receiver rejection failed: " << name << '\n';
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
      return false;
    }
  }
  return true;
}

} // namespace

int runAll(int argc, char **argv) {
  if (!receiverCaptureTests())
    return 18;
  if (!callResultAliasProofTests())
    return 10;
  if (argc == 2 && std::string_view(argv[1]) == "--alias-proof-only") {
    std::cout
        << "PASS exact alias producer, order, access and storage proofs\n";
    return 0;
  }
  if (!referenceInitializerBodyProofTests())
    return 16;
  if (argc == 2 && std::string_view(argv[1]) == "--relay-proof-only") {
    std::cout << "PASS initializer helper body-origin proof\n";
    return 0;
  }
  if (!namedRestorationReturnTests())
    return 17;
  if (argc == 2 && std::string_view(argv[1]) == "--restoration-proof-only") {
    std::cout << "PASS named restoration return lifetime proofs\n";
    return 0;
  }
  if (argc != 1)
    return 15;
  const auto application = [](const std::string &name,
                              const std::string &source) {
    const auto entry = std::filesystem::temp_directory_path() / name;
    return lang::Frontend().analyze(
        entry, source, {standardLibraryPrelude()},
        {{std::filesystem::weakly_canonical(entry).string(), source}},
        {standardLibraryRoot()});
  };
  const auto erased = application("erased-user-wrapper-mir.gti", R"gti(
import std.memory;
import std.functional;
class Inspection {
  std::erased_object& object;
public:
  Inspection(int& object) : object(std::erase_ref(object)) {}
  int read() {
    unsafe { int& value = std::restore_ref<int>(this.object); return value; }
  }
};
class Reader {
  int& value;
public:
  Reader(int& value) : value(value) {}
  int read() { return this.value; }
};
class HiddenReader {
  std::erased_object& object;
public:
  HiddenReader(Reader& original, Reader& unrelated) : object(std::erase_ref(original)) {}
};
Reader& restore_reader(std::erased_object& object) {
  unsafe { Reader& reader = std::restore_ref<Reader>(object); return reader; }
}
int read_erased(std::erased_object& object) {
  unsafe { Reader& reader = std::restore_ref<Reader>(object); return reader.read(); }
}
int main() {
  int owner = 7; Inspection view{owner};
  Reader original{owner};
  int other_owner = 19;
  Reader unrelated{other_owner};
  HiddenReader correlated{original, unrelated};
  std::erased_object& hidden = std::erase_ref(original);
  std::function_ptr<int(std::erased_object&)> opaque_pack{read_erased};
  if (opaque_pack(hidden) != 7) return 3;
  Reader& restored = restore_reader(hidden);
  if (restored.read() != 7) return 1;
  auto reader = [&owner](int value) -> int { return owner + value; };
  std::function_ref<int(int)> callback{reader};
  mut int count = 0;
  mut auto writer = [mut &count](int value) -> int { count += value; return count; };
  mut std::function_ref<int(int) mut> update{writer};
  return view.read() + callback(1) + update(2) - 17;
}
)gti");
  if (!erased.canGenerateCode()) {
    for (const auto &diagnostic : erased.diagnostics)
      std::cerr << diagnostic.message << '\n';
    const auto verification = lang::verifyMirProgram(erased.mir);
    for (const auto &error : verification.errors)
      std::cerr << "verify " << int(error.bodyKind) << ':' << error.owner
                << " b" << error.block << " i" << error.instruction << ' '
                << error.message << '\n';
    const auto dump = [&](const auto &instance) {
      const auto &body = instance.body;
      if (std::none_of(verification.errors.begin(), verification.errors.end(),
                       [&](const auto &error) {
                         return error.bodyKind == body.kind &&
                                error.owner == instance.id;
                       }))
        return;
      std::cerr << "body " << int(body.kind) << ':' << instance.id << '\n';
      for (const auto &loan : body.loans)
        std::cerr << "  loan " << loan.id << " kind=" << int(loan.kind)
                  << " source=" << loan.source << " parent=" << loan.parent
                  << " access=" << int(loan.access) << " entry=" << loan.entry
                  << " escapes=" << loan.escapes
                  << " producer=" << loan.producedBy << '\n';
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions) {
          std::cerr << "  i" << instruction.id << " k=" << int(instruction.kind)
                    << " intrinsic=" << int(instruction.intrinsic)
                    << " op=" << int(instruction.operation)
                    << " result=" << instruction.result.value_or(0)
                    << " loan=" << instruction.loan.value_or(0)
                    << " type=" << int(instruction.info.type.kind)
                    << " restore=" << instruction.restoresReferenceResult
                    << " fields=" << instruction.restoredBorrowFields.size()
                    << " target=" << instruction.functionTarget.value_or(0)
                    << " origin=" << int(instruction.borrowOrigin) << ':'
                    << instruction.borrowArgument << " operands=";
          for (const auto &operand : instruction.operands)
            std::cerr << int(operand.kind) << ':' << int(operand.type.kind)
                      << ":v" << operand.value << ":p" << operand.place << ":l"
                      << operand.loan << ',';
          std::cerr << '\n';
        }
    };
    for (const auto &instance : erased.mir.functionInstances())
      dump(instance);
    for (const auto &instance : erased.mir.constructorInstances())
      dump(instance);
    for (const auto &instance : erased.mir.lambdaInstances())
      dump(instance);
    return 7;
  }
  bool erasedChecked = false;
  bool restoredFieldsChecked = false;
  for (const auto &function : erased.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::RestoreReference)
          continue;
        erasedChecked = true;
        const bool hasFields = !instruction.restoredBorrowFields.empty();
        restoredFieldsChecked = restoredFieldsChecked || hasFields;
        for (int mutation = 0; mutation != (hasFields ? 8 : 3); ++mutation) {
          auto changed = erased.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &instructions = target->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(instructions.begin(), instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
          if (mutation == 0)
            selected->unsafeOperation = lang::UnsafeOperationKind::None;
          else if (mutation == 1)
            selected->parameterTypes.front().referenceAccess =
                lang::AccessMode::Mutable;
          else if (mutation == 2)
            selected->borrowOrigin = lang::BorrowOriginKind::None;
          else if (mutation == 3)
            selected->restoredBorrowFields.clear();
          else if (mutation == 4)
            selected->restoredBorrowFields.front().fields.front() = 0;
          else if (mutation == 5)
            selected->restoredBorrowFields.front()
                .referenceType.referenceAccess =
                selected->restoredBorrowFields.front()
                            .referenceType.referenceAccess ==
                        lang::AccessMode::Mutable
                    ? lang::AccessMode::ReadOnly
                    : lang::AccessMode::Mutable;
          else if (mutation == 6)
            target->body.loans[*selected->loan - 1].storedDependency = 0;
          else
            target->body.loans[*selected->loan - 1].parent = 0;
          if (lang::verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged erased reference " << mutation
                      << '\n';
            return 8;
          }
        }
      }
  if (!erasedChecked || !restoredFieldsChecked)
    return 9;
  bool aliasReturnChecked = false;
  bool opaquePackChecked = false;
  bool transformedDependencyChecked = false;
  bool unrelatedDependencyChecked = false;
  for (const auto &function : erased.mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *output = instruction.loan
                                 ? function.body.findLoan(*instruction.loan)
                                 : nullptr;
        const auto *input =
            instruction.operands.size() == 1 &&
                    instruction.operands.front().kind ==
                        lang::MirOperandKind::Loan
                ? function.body.findLoan(instruction.operands.front().loan)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Borrow || !output ||
            !input || output->kind != lang::MirLoanKind::Return ||
            input->kind != lang::MirLoanKind::CallResult ||
            input->carriers.empty())
          continue;
        aliasReturnChecked = true;
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        lang::MirInstructionId last = 0;
        for (const auto &candidateBlock : target->body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            last = std::max(last, candidate.id);
        auto &instructions = target->body.blocks[block.id - 1].instructions;
        const auto position =
            std::find_if(instructions.begin(), instructions.end(),
                         [&](const auto &candidate) {
                           return candidate.id == instruction.id;
                         });
        instructions.insert(position,
                            {.id = last + 1,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .hirValue = instruction.hirValue,
                             .loan = input->id});
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted expired call-result alias return\n";
          return 11;
        }
      }
    for (const auto &loan : function.body.loans) {
      const auto *source = function.body.findPlace(loan.source);
      if (!loan.entry || loan.kind != lang::MirLoanKind::Parameter ||
          loan.access != lang::AccessMode::ReadOnly || !source ||
          source->projections.size() != 1 ||
          source->projections.front().kind !=
              lang::MirProjectionKind::PackElement)
        continue;
      opaquePackChecked = true;
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        auto &entry = target->body.loans[loan.id - 1];
        auto &place = target->body.places[source->id - 1];
        if (mutation == 0)
          place.projections.front().constantIndex = 999;
        else if (mutation == 1)
          place.type = lang::SemanticType::Bool;
        else if (mutation == 2)
          entry.access = lang::AccessMode::Mutable;
        else if (mutation == 3)
          entry.source = 0;
        else if (mutation == 4)
          entry.parent = entry.id;
        else {
          const auto *entryBlock = target->body.findBlock(target->body.entry);
          if (!entryBlock || entryBlock->instructions.empty()) {
            std::cerr << "opaque pack mutation has no entry consumer\n";
            return 12;
          }
          expireLoanBefore(target->body, entry.id,
                           entryBlock->instructions.front().id);
        }
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted forged opaque pack entry " << mutation << '\n';
          return 12;
        }
      }
    }
  }
  for (const auto &constructor : erased.mir.constructorInstances())
    for (const auto &initializer : constructor.initializers) {
      const auto *call = lang::mir_detail::referenceInitializerCall(
          erased.mir, constructor, initializer);
      const auto *result =
          call && call->loan ? constructor.body.findLoan(*call->loan) : nullptr;
      if (!result || !result->storedDependency)
        continue;
      transformedDependencyChecked = true;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirConstructorInstance *>(
            changed.findConstructorInstance(constructor.id));
        // Matching call/publication metadata cannot invent or erase the
        // independent original owner authority. A separate scalar fixture
        // checks a valid-body, signature-identical non-relay replacement.
        if (mutation == 2) {
          expireLoanBefore(target->body, result->storedDependency, call->id);
        } else {
          for (auto &loan : target->body.loans)
            if (loan.storedDependency == result->storedDependency)
              loan.storedDependency = mutation == 0 ? 0 : result->id;
        }
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted forged transformed constructor authority "
                    << mutation << '\n';
          return 13;
        }
      }
      const auto unrelated =
          std::find_if(constructor.body.loans.begin(),
                       constructor.body.loans.end(), [&](const auto &loan) {
                         return loan.entry &&
                                loan.kind == lang::MirLoanKind::Stored &&
                                loan.id != result->storedDependency;
                       });
      if (unrelated != constructor.body.loans.end()) {
        unrelatedDependencyChecked = true;
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirConstructorInstance *>(
            changed.findConstructorInstance(constructor.id));
        for (auto &loan : target->body.loans)
          if (loan.storedDependency == result->storedDependency)
            loan.storedDependency = unrelated->id;
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted a different valid caller-owned constructor "
                       "dependency\n";
          return 13;
        }
      }
    }
  if (!aliasReturnChecked || !opaquePackChecked ||
      !transformedDependencyChecked || !unrelatedDependencyChecked) {
    std::cerr << "missing erased borrow boundary mutation coverage "
              << aliasReturnChecked << opaquePackChecked
              << transformedDependencyChecked << unrelatedDependencyChecked
              << '\n';
    return 14;
  }
  const auto exclusive =
      lang::Frontend().analyze("exclusive-borrowed-callback-mir.gti", R"gti(
int main() {
  mut int value = 4;
  mut auto change = [mut &value]() -> void { value += 1; };
  change();
  return value - 5;
}
)gti");
  if (!exclusive.canGenerateCode()) {
    for (const auto &diagnostic : exclusive.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 6;
  }
  const auto analyzed =
      lang::Frontend().analyze("borrowed-callback-mir.gti", R"gti(
int main() {
  int first = 4;
  int second = 9;
  auto difference = [&first, &second]() -> int { return second - first; };
  return difference() - 5;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  if (!lang::verifyMirProgram(analyzed.mir).valid())
    return 2;
  const auto &lambda = analyzed.mir.lambdaInstances().front();
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto changed = analyzed.mir;
    auto *target =
        const_cast<lang::MirLambdaInstance *>(changed.findLambda(lambda.id));
    if (mutation == 0)
      target->captureModes.front() = lang::LambdaCaptureMode::Copy;
    else if (mutation == 1)
      target->capability = lang::CallableInvocationCapability::Mutable;
    else
      target->captureTypes.front().referenceAccess = lang::AccessMode::Mutable;
    if (lang::verifyMirProgram(changed).valid()) {
      std::cerr << "accepted forged capture representation " << mutation
                << '\n';
      return 3;
    }
  }
  bool checked = false;
  for (const auto &function : analyzed.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Closure ||
            !instruction.loan)
          continue;
        checked = true;
        for (int mutation = 0; mutation < 4; ++mutation) {
          auto changed = analyzed.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &instructions = target->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(instructions.begin(), instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
          auto &loan = target->body.loans[*instruction.loan - 1];
          if (mutation == 0)
            selected->loan.reset();
          else if (mutation == 1)
            loan.additionalDependencies.clear();
          else if (mutation == 2)
            loan.additionalDependencies.front().designatedFields =
                loan.designatedFields;
          else
            selected->operands.front().kind = lang::MirOperandKind::Copy;
          if (lang::verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged closure dependency " << mutation
                      << '\n';
            return 4;
          }
        }
      }
  return checked ? 0 : 5;
}
} // namespace borrowing_callbacks

namespace borrowing_composition {
namespace {

lang::FrontendResult analyzeBorrowFixture(std::string_view name) {
  const auto path = standardLibraryRoot().parent_path() /
                    "tests/ui/ownership/borrowing_composition" / name;
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
  // RFC 0004: a free factory over several reference inputs has no elided
  // result region (Rust rejects the same signature), so the mixed carrier
  // comes from one supplier: a mutable receiver that owns every referent.
  const auto frontend = lang::Frontend().analyze("mixed-return-origins.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  int& read() { return this.first; }
  mut int& write() mut { return this.second; }
};
class Owner {
public:
  mut int a = 40;
  mut int b = 0;
  mut int c = 2;
  Owner() {}
  Services factory() mut { return Services(this.a, this.b, this.c); }
};

int main() {
  mut Owner owner{};
  {
    mut auto services = owner.factory();
    mut int& target = services.write();
    target = 40;
    int first = services.read();
    if (first != 40) { return 1; }
  }
  return owner.a + owner.b + owner.c - 82;
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
      // The factory's signature contract borrows its whole receiver; an
      // exclusive returned field makes that borrow exclusive for callers.
      if (!function.owner ||
          function.returnBorrowOrigin != lang::BorrowOriginKind::Receiver ||
          function.returnType.kind != lang::SemanticType::Class ||
          function.returnBorrowAccess != lang::AccessMode::Mutable)
        continue;
      forgedReturn = true;
      auto changed = frontend.mir;
      auto *factory = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id));
      factory->returnBorrowAccess = lang::AccessMode::ReadOnly;
      for (const auto &caller : changed.functionInstances()) {
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(caller.id));
        for (auto &block : instance->body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.functionTarget == function.id) {
              instruction.borrowAccess = lang::AccessMode::ReadOnly;
              if (instruction.loan)
                instance->body.loans[*instruction.loan - 1].access =
                    lang::AccessMode::ReadOnly;
            }
      }
      expect(!lang::verifyMirProgram(changed).valid(),
             "matching caller/callee metadata cannot forge a shared borrow "
             "of a receiver that publishes an exclusive field");
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
          instruction.borrowOrigin = lang::BorrowOriginKind::Receiver;
          instruction.borrowAccess = lang::AccessMode::Mutable;
          forgedNonCall = true;
        }
      expect(forgedNonCall && !lang::verifyMirProgram(malformed).valid(),
             "non-call instructions cannot smuggle returned-borrow contracts");
    }
  expect(forgedReturn, "return-contract forgery exercised a mapped factory");
  expect(erasedConstruction,
         "constructor-contract mutation exercised a publication");
}

} // namespace
} // namespace borrowing_composition

namespace complete_borrow {
namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

std::filesystem::path repositoryRoot() {
  return std::filesystem::path(__FILE__)
      .parent_path()
      .parent_path()
      .parent_path();
}

lang::FrontendResult analyze(const std::string &source) {
  const auto root = repositoryRoot();
  auto result = lang::Frontend().analyze(
      root / "tests" / "complete-borrow-summary.gti", source,
      {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), "result-summary fixture must lower");
  return result;
}

lang::HirFunctionInstanceId functionId(const lang::FrontendResult &result,
                                       const std::string &name) {
  for (const auto &function : result.hir.functionInstances())
    if (function.source && function.source->name().lexeme == name)
      return function.id;
  return lang::HirFunctionInstanceId{0};
}

lang::MirFunctionInstance &mutableFunction(lang::MirProgram &program,
                                           lang::HirFunctionInstanceId id) {
  return *const_cast<lang::MirFunctionInstance *>(
      program.findFunctionInstance(id));
}

void testGlobalOriginContract() {
  const auto root = repositoryRoot();
  (void)root;
  // Static storage is its own origin: the contract names the global, not an
  // argument, and the verifier rejects a lost, substituted or strengthened
  // global from the provisional program onward.
  const auto result = analyze(R"gti(
enum Slot { item(int& value), empty };
int global_value = 19;
Slot borrow_global(bool present) {
  if (!present) { return Slot::empty; }
  return Slot::item(global_value);
}
bool occupied(Slot value) {
  switch (value) {
  case Slot::item: return true;
  case Slot::empty: return false;
  }
}
int main() { auto value = borrow_global(true); return occupied(value) ? 0 : 1; }
)gti");
  const auto id = functionId(result, "borrow_global");
  const auto *function = result.mir.findFunctionInstance(id);
  const bool globalContract =
      function && function->completeReturnBorrow &&
      function->completeReturnBorrow->dependencies.size() == 1 &&
      function->completeReturnBorrow->dependencies.front().origin ==
          lang::BorrowOriginKind::Global &&
      function->completeReturnBorrow->dependencies.front().globalRoot != 0;
  expect(result.canGenerateCode() && globalContract,
         "global-borrowing carrier names its static storage origin");
  if (result.canGenerateCode() && globalContract) {
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto malformed = result.mir;
      auto &dependency = mutableFunction(malformed, id)
                             .completeReturnBorrow->dependencies.front();
      if (mutation == 0)
        dependency.globalRoot = 0;
      else if (mutation == 1)
        dependency.globalRoot += 1000;
      else if (mutation == 2) {
        dependency.origin = lang::BorrowOriginKind::Argument;
        dependency.globalRoot = 0;
      } else
        dependency.access = lang::AccessMode::Mutable;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "global summary rejects a lost, substituted or strengthened "
             "static origin " +
                 std::to_string(mutation));
    }
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = result.hir, .mir = result.mir, .level = level});
      expect(optimized.valid(),
             "optimizer preserves the global result contract");
    }
  }
}

void testWholeInputFieldContract() {
  std::ifstream input(
      repositoryRoot() /
      "tests/ui/borrow/carrier_receivers/whole_carrier_subview.gti");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  expect(!source.empty(), "whole-input carrier fixture is available");
  const auto result = analyze(source);
  if (!result.canGenerateCode())
    return;
  const auto id = functionId(result, "main");
  const auto *function = result.mir.findFunctionInstance(id);
  bool checked = false;
  bool checkedOwner = false;
  for (const auto &block : function->body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Call ||
          !instruction.loan || instruction.borrowFields.size() != 2 ||
          !std::all_of(instruction.borrowFields.begin(),
                       instruction.borrowFields.end(), [](const auto &field) {
                         return field.origin ==
                                    lang::BorrowOriginKind::Receiver &&
                                field.sourceFields.empty();
                       }))
        continue;
      const auto *loan = function->body.findLoan(*instruction.loan);
      expect(loan && loan->additionalDependencies.size() == 1,
             "whole-input result retains both designated fields");
      if (!loan || loan->additionalDependencies.size() != 1)
        continue;
      checked = true;
      auto lostField = result.mir;
      mutableFunction(lostField, id)
          .body.loans[loan->id - 1]
          .additionalDependencies.clear();
      expect(!lang::verifyMirProgram(lostField).valid(),
             "whole-input result cannot omit its second field dependency");
      for (const auto &owner : function->body.loans) {
        if (owner.id == loan->id || owner.additionalDependencies.size() != 1)
          continue;
        checkedOwner = true;
        auto lostOwner = result.mir;
        mutableFunction(lostOwner, id)
            .body.loans[owner.id - 1]
            .additionalDependencies.clear();
        expect(!lang::verifyMirProgram(lostOwner).valid(),
               "physical input address cannot hide a missing external owner");
      }
    }
  expect(checked, "whole-input result forgery exercised a carrier subview");
  expect(checkedOwner,
         "whole-input forgery exercised external owner retention");
}

void testDependencySourceIdentity() {
  const auto root = repositoryRoot();
  (void)root;
  // Two calls through one reference formal each name its referent by
  // their own place. A moved carrier's set may name either of those equal
  // paths, but never a structurally different source.
  const auto result = analyze(R"gti(
enum lookup_state<T> { found(T payload), missing };
class lookup<T> {
  mut lookup_state<T> state;
public:
  implicit lookup(T value) : state(lookup_state<T>::found(std::move(value))) {}
};
class lookup<T&> {
  mut lookup_state<T&> state;
public:
  implicit lookup(T& value) : state(lookup_state<T&>::found(value)) {}
  T take_or(T fallback) && {
    switch (this.state) {
    case lookup_state<T&>::found(auto& value): return value;
    case lookup_state<T&>::missing: return fallback;
    }
  }
};
lookup<int&> find(int& owner) { return owner; }
int twice(int& owner) {
  auto first = find(owner);
  auto second = find(owner);
  int moved = std::move(second).take_or(0);
  return std::move(first).take_or(0) + moved;
}
int main() { int owner = 3; return twice(owner) == 6 ? 0 : 1; }
)gti");
  expect(result.canGenerateCode(),
         "equal dependency paths through distinct places are one source");
  if (result.canGenerateCode()) {
    const auto id = functionId(result, "twice");
    const auto *function = result.mir.findFunctionInstance(id);
    std::size_t substituted = 0;
    for (const auto &value : function->body.values) {
      if (!value.borrowDependencies ||
          value.borrowDependencies->dependencies.empty())
        continue;
      const auto *source = function->body.findPlace(
          value.borrowDependencies->dependencies.front().source);
      if (!source || source->root != lang::MirPlaceRootKind::Binding ||
          source->projections.empty())
        continue;
      const auto root =
          std::find_if(function->body.places.begin(),
                       function->body.places.end(), [&](const auto &place) {
                         return place.root == lang::MirPlaceRootKind::Binding &&
                                place.binding == source->binding &&
                                place.projections.empty();
                       });
      if (root == function->body.places.end())
        continue;
      auto malformed = result.mir;
      auto &changed = mutableFunction(malformed, id);
      changed.body.values[value.id - 1]
          .borrowDependencies->dependencies.front()
          .source = root->id;
      ++substituted;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "a dependency cannot substitute a structurally different "
             "source");
    }
    expect(substituted != 0,
           "two-carrier fixture exercises dependency source identity");
  }
}

void testPayloadSummaryForgeries() {
  const auto root = repositoryRoot();
  (void)root;
  // This is a shape-query fixture, not a well-typed admitted program. The
  // separate program and live-flow proofs cannot be replaced by this query.
  lang::MirBody body;
  body.entry = 1;
  body.blocks.resize(2);
  body.blocks[0].id = 1;
  body.blocks[1].id = 2;
  body.places.resize(3);
  body.places[0].id = 1;
  body.places[0].root = lang::MirPlaceRootKind::Binding;
  body.places[0].binding = 1;
  body.places[0].type = lang::SemanticType::enumType(1);
  body.places[1].id = 2;
  body.places[1].root = lang::MirPlaceRootKind::Value;
  body.places[1].value = 1;
  body.places[2].id = 3;
  body.places[2].root = lang::MirPlaceRootKind::Binding;
  body.places[2].binding = 2;
  body.values.push_back({.id = 1, .definitionBlock = 2, .definition = 1});
  body.loans.resize(2);
  body.loans[0].id = 1;
  body.loans[0].kind = lang::MirLoanKind::Stored;
  body.loans[0].carriers = {1};
  body.loans[1].id = 2;
  body.loans[1].producedBy = 1;
  body.loans[1].carriers = {2};
  body.loans[1].referenceSelection = lang::MirReferenceSelection{
      .value = 1, .carrier = 1, .transferredFrom = 1};
  auto &edge = body.blocks[0].terminator;
  edge.kind = lang::MirTerminatorKind::Switch;
  edge.value =
      lang::MirOperand{.kind = lang::MirOperandKind::BorrowRead, .place = 1};
  lang::SwitchCaseValue tag;
  tag.enumOwner = 1;
  edge.switchTargets.push_back({.value = tag, .target = 2});
  auto &instructions = body.blocks[1].instructions;
  instructions.resize(4);
  auto &extract = instructions[0];
  extract.id = 1;
  extract.hirValue = 1;
  extract.result = 1;
  extract.operation = lang::MirOperation::PayloadExtract;
  extract.enumOwner = 1;
  extract.enumVariant = 0;
  extract.payloadIndex = 0;
  extract.payloadExtractionKind =
      lang::PayloadExtractionKind::TransferReference;
  extract.operands = {{.kind = lang::MirOperandKind::Move,
                       .place = 1,
                       .type = body.places[0].type}};
  instructions[1].id = 2;
  instructions[1].kind = lang::MirInstructionKind::EndBorrow;
  instructions[1].loan = 1;
  instructions[2].id = 3;
  instructions[2].kind = lang::MirInstructionKind::Borrow;
  instructions[2].loan = 2;
  instructions[2].operands = {
      {.kind = lang::MirOperandKind::BorrowRead, .place = 2}};
  instructions[3].id = 4;
  instructions[3].kind = lang::MirInstructionKind::Initialize;
  instructions[3].destination = 3;
  instructions[3].operands = {{.kind = lang::MirOperandKind::Loan, .loan = 2}};
  const auto proof =
      lang::mir_detail::selectedReferenceHandoff(body, body.loans[1]);
  expect(proof && proof->retirement == 2 && proof->endPosition == 4 &&
             proof->successors == std::vector<lang::MirLoanId>{2},
         "closed handoff shape caches before retirement and publishes before "
         "use");
  for (int mutation = 0; mutation != 10; ++mutation) {
    auto malformed = body;
    auto &prefix = malformed.blocks[1].instructions;
    if (mutation == 0)
      std::swap(prefix[0], prefix[1]);
    if (mutation == 1)
      prefix[1].kind = lang::MirInstructionKind::Call;
    if (mutation == 2)
      prefix[1].loan = 2;
    if (mutation == 3)
      prefix[2].operands[0].place = 3;
    if (mutation == 4)
      prefix.pop_back();
    if (mutation == 5)
      prefix[0].payloadIndex = 1;
    if (mutation == 6)
      malformed.blocks[0].terminator.switchTargets[0].value->value.magnitude =
          1;
    if (mutation == 7)
      malformed.loans[0].carriers.clear();
    if (mutation == 8)
      malformed.places[2].binding = 3;
    if (mutation == 9)
      prefix.insert(prefix.begin() + 1, prefix.front());
    expect(!lang::mir_detail::selectedReferenceHandoff(malformed,
                                                       malformed.loans[1]),
           "handoff rejects reordered/missing/substituted/duplicated setup "
           "identities");
  }
  // A body-local fixture supplies an already established external authority.
  // It does not claim the sealed enum/formal inventory of a complete program.
  // This checks the permission transition separately from the shape query.
  auto live = body;
  live.kind = lang::MirBodyKind::Function;
  live.blocks.resize(3);
  live.blocks[2].id = 3;
  live.blocks[0].terminator.target = 3;
  live.blocks[0].terminator.value->type = live.places[0].type;
  live.blocks[0].terminator.switchTargets[0].value->kind =
      lang::SwitchCaseKind::Enumerator;
  live.blocks[0].terminator.switchTargets[0].value->type = live.places[0].type;
  live.blocks[1].terminator.kind = lang::MirTerminatorKind::Return;
  live.blocks[2].terminator.kind = lang::MirTerminatorKind::Return;
  live.places[0].initiallyAvailable = true;
  live.places[1].type = lang::SemanticType::Int32;
  const auto reference =
      lang::SemanticType::referenceTo(lang::SemanticType::Int32);
  live.places[2].type = reference;
  live.places.push_back({.id = 4,
                         .root = lang::MirPlaceRootKind::Binding,
                         .binding = 3,
                         .type = lang::SemanticType::Int32,
                         .initiallyAvailable = true});
  live.loans[0].source = 4;
  live.loans[0].entry = true;
  live.loans[0].carriers.push_back(3);
  live.loans[1].source = 4;
  live.loans[1].referenceSelection->fields = {1};
  live.loans[1].referenceSelection->conditions = {
      {.enumeration = live.places[0].type, .variant = 0}};
  live.loans[1].referenceSelection->reference = reference;
  auto &setup = live.blocks[1].instructions;
  setup[0].info.type = lang::SemanticType::Int32;
  setup[0].info.category = lang::ValueCategory::Place;
  live.values[0].info = setup[0].info;
  live.values[0].sourceValue = setup[0].hirValue;
  setup[2].operands[0].type = reference;
  setup[3].operands[0].type = reference;
  setup[3].operands[0].place = 2;
  setup.push_back(
      {.id = 5, .kind = lang::MirInstructionKind::EndBorrow, .loan = 2});
  live.blocks[2].instructions.push_back(
      {.id = 6, .kind = lang::MirInstructionKind::EndBorrow, .loan = 1});
  lang::rebuildMirReachability(live);
  expect(lang::rebuildMirValueUses(live), "handoff fixture value uses index");
  const auto liveResult = lang::verifyMirBody(live);
  for (const auto &error : liveResult.errors)
    std::cerr << "live handoff fixture block " << error.block << " instruction "
              << error.instruction << ": " << error.message << '\n';
  expect(liveResult.valid(),
         "exact consuming prefix transfers an active external capability");
  auto expired = live;
  expired.blocks[0].instructions.push_back(
      {.id = 7, .kind = lang::MirInstructionKind::EndBorrow, .loan = 1});
  expect(lang::rebuildMirValueUses(expired) &&
             !lang::verifyMirBody(expired).valid(),
         "an earlier retirement cannot authorize a later handoff");
  auto invented = live;
  invented.loans[1].producedBy = 0;
  expect(!lang::verifyMirBody(invented).valid(),
         "a handoff producer cannot pretend to be a parentless later alias");
  auto substituted = live;
  substituted.places.push_back({.id = 5,
                                .root = lang::MirPlaceRootKind::Binding,
                                .binding = 4,
                                .type = lang::SemanticType::Int32,
                                .initiallyAvailable = true});
  substituted.loans[1].source = 5;
  expect(!lang::verifyMirBody(substituted).valid(),
         "handoff cannot substitute a same-typed external owner");
  auto stronger = live;
  stronger.loans[1].access = lang::AccessMode::Mutable;
  expect(!lang::verifyMirBody(stronger).valid(),
         "handoff cannot strengthen its former group's shared access");
}

} // namespace
} // namespace complete_borrow

GTI_UNIT_TEST(ownership_lambdas) {
  ownership::testLambdas();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_unique_owner_swap) {
  ownership::testUniqueOwnerSwap();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_consuming_methods_and_value_operators) {
  ownership::testConsumingMethodsAndValueOperators();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_global_borrow_returns) {
  ownership::testGlobalBorrowReturns();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_exclusive_reborrow_loan_graph) {
  ownership::testExclusiveReborrowLoanGraph();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_indexed_place_moves_and_initialization) {
  ownership::testIndexedPlaceMovesAndInitialization();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_generated_default_temporary_argument_borrows) {
  ownership::testGeneratedDefaultTemporaryArgumentBorrows();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_temporary_argument_borrows) {
  ownership::testTemporaryArgumentBorrows();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_generated_default_construction_contexts) {
  ownership::testGeneratedDefaultConstructionContexts();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownership_restricted_member_operators) {
  ownership::testRestrictedMemberOperators();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(borrowing_callbacks_mir_contracts) {
  char name[] = "gti_unit_tests";
  char *arguments[] = {name, nullptr};
  return borrowing_callbacks::runAll(1, arguments);
}

GTI_UNIT_TEST(borrowing_composition_check_nested_constructor_arguments) {
  borrowing_composition::checkNestedConstructorArguments();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(borrowing_composition_check_mixed_return_origins) {
  borrowing_composition::checkMixedReturnOrigins();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(borrowing_composition_check_projected_mixed_reborrows) {
  borrowing_composition::checkProjectedMixedReborrows();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(complete_borrow_global_origin_contract) {
  complete_borrow::testGlobalOriginContract();
  return complete_borrow::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(complete_borrow_whole_input_field_contract) {
  complete_borrow::testWholeInputFieldContract();
  return complete_borrow::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(complete_borrow_dependency_source_identity) {
  complete_borrow::testDependencySourceIdentity();
  return complete_borrow::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(complete_borrow_payload_summary_forgeries) {
  complete_borrow::testPayloadSummaryForgeries();
  return complete_borrow::failures == 0 ? 0 : 1;
}
