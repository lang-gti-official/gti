#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
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

lang::FrontendResult analyze(std::string_view source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("expected_ergonomics.gti",
                                  std::string(source),
                                  {library / "prelude.gti"}, {}, {library});
}

void diagnostics(const lang::FrontendResult &result) {
  std::cerr << "phases: source=" << result.sourceValid
            << " syntax=" << result.syntaxValid
            << " semantics=" << result.semanticValid
            << " HIR=" << result.hirValid
            << " failure=" << result.failureMetadataValid
            << " MIR=" << result.mirValid << '\n';
  for (const auto &error :
       lang::verifyHirProgramPlans(result.semantics, result.hir).errors)
    std::cerr << "HIR: " << error << '\n';
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  for (const auto &function : result.mir.functionInstances()) {
    if (function.tracksCaller != function.body.hasCallerLocation) {
      const auto *hir = result.hir.findFunctionInstance(function.id);
      std::cerr << "MIR caller mismatch: " << function.id << " "
                << (hir && hir->source ? hir->source->name().lexeme
                                       : "<generated>")
                << " instance=" << function.tracksCaller
                << " body=" << function.body.hasCallerLocation << '\n';
    }
  }
}

void reject(const std::string &source, std::string_view code,
            std::string_view anchor, std::string_view explanation) {
  const auto result = analyze(source);
  check(!result.canGenerateCode() && !result.semanticValid,
        "invalid expected operation stops in semantics");
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == code; });
  if (found == result.diagnostics.end()) {
    diagnostics(result);
    check(false, "expected semantic diagnostic is present");
    return;
  }
  const auto start = source.rfind(anchor);
  check(found->primary.source ==
                std::filesystem::weakly_canonical(
                    std::filesystem::absolute("expected_ergonomics.gti"))
                    .string() &&
            found->primary.start == start &&
            found->primary.end == start + anchor.size(),
        "diagnostic retains the precise offending token and source");
  check(found->message.find(explanation) != std::string::npos ||
            std::any_of(found->hints.begin(), found->hints.end(),
                        [&](const auto &hint) {
                          return hint.find(explanation) != std::string::npos;
                        }),
        "diagnostic explains the failed operation contract");
  check(found->fixes.empty(), "no fix guesses ownership or mapper intent");
  check(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code.starts_with("GTI-B");
                     }),
        "source errors do not escape to the backend");
}

// As reject, and the diagnostic is the program's only error: nothing is
// reported about consequences of the failure inside the prelude.
void rejectOnly(const std::string &source, std::string_view code,
                std::string_view anchor, std::string_view explanation) {
  const auto result = analyze(source);
  const auto errors = std::count_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.severity == lang::DiagnosticSeverity::Error;
      });
  if (errors != 1)
    diagnostics(result);
  check(errors == 1, "the failure is one error, without consequences");
  reject(source, code, anchor, explanation);
}

void testDiagnostics() {
  const std::string producer = "expected<int, int> read() { return 7; }\n";
  for (const std::string member : {"transform", "and_then"}) {
    const std::string mapper =
        member == "transform"
            ? "int map(int value) { return value; }\n"
            : "expected<int, int> map(int value) { return value; }\n";
    // Consuming source members report the general consuming-receiver
    // diagnostic at the call's closing parenthesis.
    reject(producer + mapper +
               "int main() { auto result = read(); /* 😀 */ [[discard]] "
               "result." +
               member + "(map); return 0; }\n",
           "GTI-S2022", ")", "std::move");
    reject(producer +
               "int map(bool value) { return 0; }\n"
               "int main() { /* 😀 */ [[discard]] read()." +
               member + "(map); return 0; }\n",
           "GTI-S2090", "map",
           "exactly matches the required callable signature");
  }
  reject(producer + "int main() { auto result = read(); [[discard]] "
                    "result.expect(\"required\"); return 0; }\n",
         "GTI-S2022", ")", "std::move");
  // expect(std::string_view) is an ordinary source parameter.
  reject(producer + "int main() { [[discard]] read().expect(42); return 0; }\n",
         "GTI-S2003", "42", "std::string_view");
  // and_then's own requirement accepts only a result of the same family and
  // error type; the unsatisfied requirement is reported at the call.
  reject(producer +
             "expected<int, bool> map(int value) { return value; }\n"
             "int main() { [[discard]] read().and_then(map); return 0; }\n",
         "GTI-S2029", ")", "expected<int32_t, bool>");
  reject(producer +
             "int map(int value) { return value; }\n"
             "int main() { [[discard]] read().and_then(map); return 0; }\n",
         "GTI-S2029", ")", "int32_t");
  // The prelude's concept states the mapper contract instead of naming its
  // private conformance blocks.
  for (const std::string result : {"expected<int, bool>", "int"}) {
    const auto mismatch =
        analyze(producer + result + " map(int value) { return value; }\n" +
                "int main() { [[discard]] read().and_then(map); return 0; }\n");
    const auto found = std::find_if(
        mismatch.diagnostics.begin(), mismatch.diagnostics.end(),
        [](const auto &diagnostic) { return diagnostic.code == "GTI-S2029"; });
    check(found != mismatch.diagnostics.end() && found->hints.size() == 1 &&
              found->hints.front() ==
                  "An 'and_then' mapper must return an 'expected' with the "
                  "same error type as the result it maps.",
          "and_then explains its mapper contract in one hint");
  }
  // A reference-returning mapper would make the consuming member return a
  // borrow its consumed receiver cannot keep alive. That selected signature
  // is rejected once, naming the result, before its arguments are checked.
  rejectOnly("int value = 7; int& map(int input) { return value; }\n" +
                 producer +
                 "int main() { [[discard]] read().transform(map); return 0; "
                 "}\n",
             "GTI-S2022", ")",
             "A consuming method cannot return a reference or tracked "
             "borrowed state; this call returns 'expected<int32_t&, "
             "int32_t>'.");
  // A callable class has no reflectable result, so the call names no result
  // type; it is rejected once, before any library instance is formed.
  for (const std::string member : {"transform", "transform_error"}) {
    rejectOnly(producer +
                   "struct Mapper { Mapper() = default; int operator()(int value) { return value; "
                   "} };\n"
                   "int main() { Mapper mapper{}; [[discard]] read()." +
                   member + "(mapper); return 0; }\n",
               "GTI-S2023", ")", "does not select an overloaded operator()");
  }
  // The result class's own assertion rejects a void error type where the
  // program requires that instance; its layout and members add nothing.
  rejectOnly("int main() { expected<int, void> result = 1; return 0; }\n",
             "GTI-S2079", "result",
             "Static assertion failed. An 'expected' error type cannot be "
             "'void'.");
  // A nested conditional carrier that a library operator cannot store is
  // reported at the assignment that requested the operator.
  const std::string nestedCarrier = R"(int main() {
  mut expected<expected<int&, int>, int> nested = unexpected(1);
  {
    int local = 5;
    expected<int&, int> inner = local;
    nested = std::move(inner);
  }
  return 0;
}
)";
  rejectOnly(nestedCarrier, "GTI-S2067", "=",
             "conditionally retains references");
  const auto nested = analyze(nestedCarrier);
  check(!nested.diagnostics.empty() &&
            std::any_of(nested.diagnostics.front().related.begin(),
                        nested.diagnostics.front().related.end(),
                        [](const lang::RelatedDiagnostic &related) {
                          return related.message ==
                                     "Reported inside this library instance." &&
                                 related.span.source.ends_with("prelude.gti");
                        }),
        "the payload error keeps its library location as related information");
  // A void success passes no value, so a one-parameter mapper's call arity
  // fails inside the selected mapping instance, reported at the request.
  reject("expected<void, int> read() { return; }\n"
         "int main() { [[discard]] read().transform([](int value) -> int { "
         "return value; }); return 0; }\n",
         "GTI-S2005", "transform", "expects 1 argument");
  // expected<T, E> is an ordinary source class, so an owning payload enum is
  // an admitted mapping result rather than a carrier containment error.
  const auto payloadResult =
      analyze("struct Resource { ~Resource() {}  public: Resource() = default; };\n"
              "enum class Packet { item(Resource value), idle };\n"
              "expected<int, int> read() { return 7; }\n"
              "Packet map(int input) { return Packet::item(Resource()); }\n"
              "int main() { [[discard]] read().transform(map); return 0; }\n");
  if (!payloadResult.canGenerateCode())
    diagnostics(payloadResult);
  check(payloadResult.canGenerateCode() &&
            lang::verifyMirProgram(payloadResult.mir).valid(),
        "an owning payload enum is an ordinary expected mapping result");
  const auto moved = analyze(
      producer +
      "int main() { auto result = read(); [[discard]] "
      "std::move(result).expect(\"required\"); return result.value(); }\n");
  check(!moved.canGenerateCode() &&
            std::any_of(moved.diagnostics.begin(), moved.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.message.find("moved") !=
                                 std::string::npos;
                        }),
        "expect consumes the complete named expected availability");
}

void testPipeline() {
  const auto result = analyze(R"(
expected<int, int> read(bool fail) {
  if (fail) { return unexpected(3); }
  return 7;
}
expected<void, int> perform(bool fail) {
  if (fail) { return unexpected(3); }
  return;
}
int increment(int value) { return value + 1; }
void observe(int value) {}
int fresh() { return 11; }
void finish() {}
expected<int, int> next(int value) { return value + 2; }
expected<void, int> done(int value) { return; }
expected<int, int> start() { return 13; }
expected<void, int> stop() { return; }
T required<T, E>(expected<T, E> value) {
  return std::move(value).expect("required value");
}
int main() {
  auto first = read(false).transform(increment).and_then(next);
  auto failed = read(true).transform(increment).and_then(next);
  auto void_output = read(false).transform(observe);
  auto void_input = perform(false).transform(fresh);
  auto void_both = perform(false).transform(finish);
  auto void_chain = read(false).and_then(done);
  auto from_void = perform(false).and_then(start);
  auto all_void = perform(false).and_then(stop);
  auto lambda = read(false).transform([](int value) -> int { return value + 1; });
  auto chained_lambda = read(false).and_then([](int value) -> expected<int, int> {
    return value + 2;
  });
  std::move(void_output).expect("observe");
  std::move(void_both).expect("finish");
  std::move(void_chain).expect("done");
  std::move(all_void).expect("stop");
  return required<int, int>(std::move(first)) + failed.error() +
      std::move(void_input).expect("fresh") +
      std::move(from_void).expect("start") +
      std::move(lambda).expect("lambda") +
      std::move(chained_lambda).expect("chain") - 54;
}
)");
  if (!result.canGenerateCode())
    diagnostics(result);
  check(result.canGenerateCode(), "all expected ergonomics shapes lower");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyMirProgram(result.mir).valid(),
        "expected ergonomics produce verified MIR");
  unsigned trackedDeclarations = 0;
  for (const auto &function : result.mir.functionInstances()) {
    check(function.body.hasCallerLocation == function.tracksCaller,
          "all callable shells preserve their declaration's caller contract");
    if (function.tracksCaller &&
        function.definitionKind == lang::MirDefinitionKind::Declaration)
      ++trackedDeclarations;
    if (function.neverReturns &&
        function.definitionKind == lang::MirDefinitionKind::Declaration)
      check(function.body.blocks.size() == 1 &&
                function.body.blocks.front().terminator.kind ==
                    lang::MirTerminatorKind::Unreachable,
            "terminal declaration shells have no fabricated return edge");
  }
  check(
      trackedDeclarations > 0,
      "the compiler-private panic declaration exercises a tracked body shell");
  check(lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
        "expected ergonomics retain verified selected HIR plans");
  // A void mapper is an ordinary statement of the source
  // expected_mapping<void, E>::invoke body, ordered before its result
  // construction by that body itself; no separate sequencing plan exists.
  unsigned terminalArms = 0;
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      // expect() is a source member: its terminal arm is the panic inside
      // each selected expect instance.
      if (value.intrinsic == lang::IntrinsicKind::Panic &&
          function.source != nullptr &&
          function.source->name().lexeme == "expect") {
        ++terminalArms;
        auto malformed = result.hir;
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &candidate : changed->body.values)
          if (candidate.id == value.id)
            candidate.neverReturns = false;
        check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
              "expect cannot acquire a normal failure-arm continuation");
      }
    }
  }
  // One shared instance per selected result type: expected<int, int> and
  // expected<void, int>.
  check(terminalArms == 2, "terminal expect arms are present");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      check(!gti_test::emitCppText(result, standard).empty(),
            "expected operations use the production MIR backend in both "
            "standards");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      check(false, "expected operations fit verified backend vocabulary");
    }
  }
}

void testOwnedPipeline() {
  const auto result = analyze(R"(
#include <std/panic>
struct Resource {
  int id;
  Resource(int value) : id(value) {}
  ~Resource() { std::println(id); }
};
expected<Resource, Resource> read(bool fail) {
  if (fail) { return unexpected(Resource(2)); }
  return Resource(1);
}
Resource required(bool fail) { return read(fail).expect("required"); }
int consume(Resource value) { return value.id; }
[[noreturn]] void terminal(Resource value) { std::panic("terminal"); }
int main() {
  auto resource = read(false).expect("local");
  auto returned = required(false);
  auto skipped = read(true).transform(terminal);
  return consume(read(false).expect("argument")) + resource.id + returned.id +
      skipped.error().id - 5;
}
)");
  if (!result.canGenerateCode())
    diagnostics(result);
  check(result.canGenerateCode(),
        "owned expected results and terminal mappers lower");
  if (!result.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      check(!gti_test::emitCppText(result, standard).empty(),
            "owned expect supports local, return and argument publication");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      check(false, "owned expected operations fit verified backend vocabulary");
    }
  }
}

void testMovedScalarObserver() {
  const auto result = analyze(R"(
struct Ticket {
  int identity;
  Ticket(int value) : identity(value) {}
};
expected<Ticket, int> issue(bool fail) {
  if (fail) { return unexpected(17); }
  return Ticket(7);
}
expected<int, int> read(bool fail) {
  auto ticket = issue(fail);
  if (!ticket) { return unexpected(std::move(ticket).error()); }
  Ticket value = std::move(ticket).value();
  return value.identity;
}
int main() { return read(false).value() + read(true).error() - 24; }
)");
  if (!result.canGenerateCode())
    diagnostics(result);
  check(result.canGenerateCode(), "moved Expected scalar observer lowers");
  if (!result.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      check(!gti_test::emitCppText(result, standard).empty(),
            "cleanup-free Expected supports scalar and class extraction");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      check(false, "moved Expected observer fits verified backend vocabulary");
    }
  }
}

void testFreshExpectedArguments(unsigned mode) {
  const bool mixed = mode != 0;
  std::string source = R"(
struct Resource {
  int id;
  Resource(int value) : id(value) { int checked = 1 / value; }
  ~Resource() { std::println(id); }
};
expected<Resource, int> make(int id) { return Resource(id); }
Resource make_resource(int id) { return Resource(id); }
)";
  source += mode == 2 ? R"(
void consume(expected<Resource, int> first, Resource second, Resource third) {}
void exercise() { consume(make(1), Resource(2), Resource(3)); }
)"
            : mixed   ? R"(
void consume(expected<Resource, int> first, Resource second, Resource third) {}
void exercise() { consume(make(1), make_resource(2), make_resource(3)); }
)"
                      : R"(
void consume(expected<Resource, int> first, expected<Resource, int> second) {}
void exercise() { consume(make(1), make(2)); }
)";
  source += R"(
int main() { exercise(); return 0; }
)";
  const auto result = analyze(source);
  if (!result.canGenerateCode())
    diagnostics(result);
  check(result.canGenerateCode(), "fresh owning expected arguments lower");
  if (!result.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      check(!gti_test::emitCppText(result, standard).empty(),
            "fresh expected results transfer directly to source function "
            "parameters");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      check(false, "fresh Expected argument fits verified backend vocabulary");
    }
  }
  lang::HirFunctionInstanceId exercise = 0;
  lang::HirFunctionInstanceId producer = 0;
  lang::HirFunctionInstanceId consumer = 0;
  for (const auto &function : result.hir.functionInstances()) {
    if (!function.source)
      continue;
    if (function.source->name().lexeme == "exercise")
      exercise = function.id;
    if (function.source->name().lexeme == "make")
      producer = function.id;
    if (function.source->name().lexeme == "consume")
      consumer = function.id;
  }
  check(exercise != 0 && producer != 0 && consumer != 0,
        "argument fixture retains exact source function identities");
  if (exercise == 0 || producer == 0 || consumer == 0)
    return;
  const std::size_t argumentCount = mixed ? 3 : 2;
  const std::size_t mutatedArgument = mixed ? 1 : 0;
  for (unsigned mutation = 0; mutation != 7; ++mutation) {
    auto malformed = result.mir;
    auto &body = const_cast<lang::MirFunctionInstance *>(
                     malformed.findFunctionInstance(exercise))
                     ->body;
    lang::MirInstruction *call = nullptr;
    lang::MirBlockId consumerBlock = 0;
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.functionTarget == consumer) {
          call = &instruction;
          consumerBlock = block.id;
        }
    check(call && call->operands.size() == argumentCount &&
              call->lifecycle.size() == argumentCount,
          "each fresh Expected has one exact transfer to the selected call");
    if (!call || call->operands.size() != argumentCount ||
        call->lifecycle.size() != argumentCount)
      continue;
    const auto *value = body.findValue(call->operands[mutatedArgument].value);
    lang::MirDropObligationId sourceDrop = 0;
    // Each fresh class argument, expected included, is staged as a prepared
    // parameter whose obligation the call transfers.
    if (value != nullptr)
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.id == value->definition &&
              instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.preparedParameterDrop)
            sourceDrop = *instruction.preparedParameterDrop;
    const auto transfer = std::find_if(
        call->lifecycle.begin(), call->lifecycle.end(),
        [&](const auto &event) { return event.source == sourceDrop; });
    check(sourceDrop != 0 && transfer != call->lifecycle.end(),
          "mutation targets the exact Expected or class argument obligation");
    if (sourceDrop == 0 || transfer == call->lifecycle.end())
      continue;
    if (mutation == 0)
      call->lifecycle.erase(transfer);
    if (mutation == 1) {
      std::erase_if(body.dropObligations,
                    [&](const auto &drop) { return drop.id == sourceDrop; });
    }
    if (mutation == 2)
      call->operands[mutatedArgument + 1] = call->operands[mutatedArgument];
    if (mutation == 3)
      call->parameterTypes[mutatedArgument] = lang::SemanticType::Int32;
    if (mutation == 4)
      call->functionTarget = producer;
    if (mutation == 5) {
      lang::MirBlock bypass;
      for (const auto &block : body.blocks)
        bypass.id = std::max(bypass.id, block.id);
      ++bypass.id;
      bypass.terminator.kind = lang::MirTerminatorKind::Goto;
      bypass.terminator.target = consumerBlock;
      body.entry = bypass.id;
      body.blocks.push_back(std::move(bypass));
      lang::rebuildMirReachability(body);
    }
    if (mutation == 6)
      call->lifecycle.push_back(*transfer);
    check(lang::rebuildMirValueUses(body),
          "mutation rebuilds exact use records");
    check(!lang::verifyMirProgram(malformed).valid(),
          "Expected argument rejects lost transfer/drop, duplicate use, wrong "
          "parameter/target identity and non-dominating publication");
  }
}
} // namespace

int main() {
  testDiagnostics();
  testPipeline();
  testOwnedPipeline();
  testMovedScalarObserver();
  testFreshExpectedArguments(0);
  testFreshExpectedArguments(1);
  testFreshExpectedArguments(2);
  return failures == 0 ? 0 : 1;
}
