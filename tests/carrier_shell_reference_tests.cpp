#include "compiler_test_fixture.h"
#include "gti/optimization/alias.h"

namespace {

const std::string declarations = R"gti(
#include <std/utility>
struct Counter { mut int value = 4; };
class Shared {
  Counter& counter;
  mut int offset = 2;
public:
  Shared(Counter& source) : counter(source) {}
  int read() { return this.counter.value + this.offset; }
  void advance() mut { this.offset += 1; }
};
class Exclusive {
  mut Counter& counter;
public:
  Exclusive(mut Counter& source) : counter(source) {}
  int read() { return this.counter.value; }
  void advance() mut { this.counter.value += 1; }
};
class SharedPair {
  Counter& first;
  Counter& second;
public:
  SharedPair(Counter& left, Counter& right) : first(left), second(right) {}
  int read() { return this.first.value + this.second.value; }
};
struct Holder {
  mut Exclusive stored;
  Holder(Exclusive value) : stored(std::move(value)) {}
  mut Exclusive& get() mut { return this.stored; }
  Exclusive& inspect() { return this.stored; }
};
mut Exclusive& relay(mut Exclusive& value) { return value; }
Exclusive& observe(Exclusive& value) { return value; }
)gti";

lang::FrontendResult analyze(const std::string &body) {
  return lang::Frontend().analyze(
      "carrier-shell-reference.gti",
      declarations + "\nint main() {\n" + body + "\n}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

void checkLocalReferences() {
  const std::vector<std::string> programs{
      "Counter owner{}; Shared view{owner}; Shared& observed = view; "
      "return observed.read() - 6;",
      "mut Counter owner{}; mut Exclusive view{owner}; "
      "mut Exclusive& observed = view; observed.advance(); "
      "return observed.read() - 5;",
      "mut Counter owner{}; Exclusive view{owner}; "
      "Exclusive& observed = view; return observed.read() - 4;",
      "Counter owner{}; mut Shared view{owner}; mut Shared& observed = view; "
      "observed.advance(); return observed.read() - 7;",
      "mut Counter owner{}; Exclusive view{owner}; "
      "mut Holder holder{std::move(view)}; "
      "mut Exclusive& observed = holder.stored; observed.advance(); "
      "return observed.read() - 5;",
      "mut Counter owner{}; mut Holder holder{Exclusive(owner)}; "
      "holder.get().advance(); mut Exclusive& observed = holder.get(); "
      "observed.advance(); return observed.read() - 6;",
      "mut Counter owner{}; Holder holder{Exclusive(owner)}; "
      "Exclusive& observed = holder.inspect(); return observed.read() - 4;",
      "mut Counter owner{}; mut Exclusive view{owner}; "
      "mut Exclusive& observed = relay(view); observed.advance(); "
      "return observed.read() - 5;",
      "mut Counter owner{}; Exclusive view{owner}; "
      "Exclusive& observed = observe(view); return observed.read() - 4;",
      "Counter owner{}; Shared view{owner}; Shared& reference = view; "
      "Shared copied = reference; return copied.read() - 6;",
      "Counter owner{}; mut Shared view{owner}; mut Shared& reference = view; "
      "Shared copied = reference; return copied.read() - 6;",
      "Counter owner{}; Shared view{owner}; "
      "auto callback = [](Shared& reference) -> int { return reference.read(); "
      "}; "
      "return callback(view) - 6;",
      "Counter first{}; Counter second{}; SharedPair view{first, second}; "
      "SharedPair& reference = view; return reference.read() - 8;"};
  for (const auto &program : programs) {
    const auto frontend = analyze(program);
    expect(frontend.canGenerateCode(),
           "carrier shell references preserve independent owner authority: " +
               program);
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    bool separateShell = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        if (loan.storedDependency == 0)
          continue;
        const auto *dependency = function.body.findLoan(loan.storedDependency);
        separateShell =
            separateShell || ((loan.kind == lang::MirLoanKind::Local ||
                               loan.kind == lang::MirLoanKind::CallResult) &&
                              dependency != nullptr &&
                              dependency->kind == lang::MirLoanKind::Stored &&
                              loan.source != dependency->source);
      }
    }
    for (const auto &lambda : frontend.mir.lambdaInstances()) {
      for (const auto &loan : lambda.body.loans) {
        const auto *dependency = lambda.body.findLoan(loan.storedDependency);
        const auto *source = lambda.body.findPlace(loan.source);
        if (loan.kind != lang::MirLoanKind::Parameter || !loan.entry ||
            dependency == nullptr ||
            dependency->kind != lang::MirLoanKind::Stored ||
            !dependency->entry || dependency->designatedFields.empty() ||
            source == nullptr || dependency->source != loan.source ||
            source->root != lang::MirPlaceRootKind::Binding ||
            source->type.kind != lang::SemanticType::Reference)
          continue;
        for (std::size_t index = 0; index < lambda.parameterBindings.size();
             ++index)
          separateShell = separateShell ||
                          (lambda.parameterBindings[index] == source->binding &&
                           lambda.parameterTypes[index] == source->type);
      }
    }
    expect(separateShell && lang::verifyMirProgram(frontend.mir).valid(),
           "a reference protects both the physical shell and its stored loan");
    try {
      expect(!gti_test::emitCpp(frontend).contents.empty(),
             "verified carrier shell references reach the production backend");
    } catch (const std::logic_error &error) {
      expect(false, std::string("carrier shell backend: ") + error.what());
    }
  }
}

void checkReferenceCopies() {
  for (const std::string &function :
       {"int copied(Shared& source) { Shared local = source; return "
        "local.read(); }",
        "int copied(mut Shared& source) { Shared local = source; return "
        "local.read(); }",
        "int copied(mut Shared& source) { Shared local = source; "
        "source.advance(); "
        "return local.read(); }",
        "int copied(Shared& source) { Shared first = source; Shared second = "
        "first; "
        "return second.read(); }",
        "Shared copied(Shared& source) { Shared local = source; return local; "
        "}",
        "Shared copied(Shared& source) { return source; }"}) {
    const bool returnsCopy = function.starts_with("Shared copied");
    const auto frontend = lang::Frontend().analyze(
        "carrier-reference-copy.gti",
        declarations + function +
            "\nint main() { Counter owner{}; mut Shared view{owner}; "
            "return copied(view)" +
            (returnsCopy ? ".read()" : "") + " - 6; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(frontend.canGenerateCode(),
           "copying a shared view retains its external owner: " + function);
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "copied reference carrier publications are independently verified");
    if (returnsCopy) {
      bool checkedRetirement = false;
      for (const auto &function : frontend.mir.functionInstances()) {
        if (function.returnType.kind != lang::SemanticType::Class)
          continue;
        for (const auto &loan : function.body.loans) {
          if (!loan.entry || loan.kind != lang::MirLoanKind::Stored)
            continue;
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          bool erased = false;
          for (auto &block : changed->body.blocks)
            erased = std::erase_if(
                         block.instructions,
                         [&](const auto &instruction) {
                           return instruction.kind ==
                                      lang::MirInstructionKind::EndBorrow &&
                                  instruction.loan == loan.id;
                         }) != 0 ||
                     erased;
          if (!erased)
            continue;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "actual copied-value return cannot omit its stored-owner "
                 "retirement");
          bool rejectedByLoweredBoundary = false;
          try {
            (void)gti_test::emitCpp(frontend, malformed, malformed);
          } catch (const std::logic_error &error) {
            rejectedByLoweredBoundary =
                std::string_view(error.what()).find("lowered program") !=
                std::string_view::npos;
          }
          expect(rejectedByLoweredBoundary,
                 "production lowering rejects missing copy-return retirement "
                 "before C++ representation admission");
          checkedRetirement = true;
        }
      }
      expect(checkedRetirement,
             "copied return retirement mutation exercised actual source MIR");
    }
    try {
      expect(!gti_test::emitCpp(frontend).contents.empty(),
             "copied reference carrier reaches production backend");
    } catch (const std::logic_error &error) {
      expect(false,
             std::string("copied reference carrier backend: ") + error.what());
    }
  }
}

void checkLocalReferentBindings() {
  const auto frontend = lang::Frontend().analyze(
      "carrier-local-referent.gti", R"gti(
class WriteView {
  mut int& item;
public:
  WriteView(mut int& source) : item(source) {}
  void write(int value) mut { mut int& target = this.item; target = value; }
  int read() { int& target = this.item; return target; }
};
int main() {
  mut int value = 1;
  { mut WriteView view{value}; view.write(42); if (view.read() != 42) return 1; }
  return value - 42;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "local references to stored referents preserve the concrete field "
         "dereference rather than reconstructing the overlap-only place");
  if (!frontend.canGenerateCode())
    return;
  try {
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "mutable and shared stored-referent bindings reach the backend");
  } catch (const std::logic_error &error) {
    expect(false,
           std::string("local stored-referent backend: ") + error.what());
  }
  bool checkedDereference = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *source = function.body.findPlace(loan.source);
      if (loan.kind != lang::MirLoanKind::Local || source == nullptr ||
          source->root != lang::MirPlaceRootKind::This ||
          source->projections.size() != 2 ||
          source->projections.front().kind != lang::MirProjectionKind::Field ||
          source->projections.back().kind !=
              lang::MirProjectionKind::Dereference)
        continue;
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      changed->body.places[source->id - 1].projections.pop_back();
      bool rejected = false;
      try {
        (void)gti_test::emitCpp(frontend, malformed, malformed);
      } catch (const std::logic_error &) {
        rejected = true;
      }
      expect(rejected,
             "removing the exact reference-field dereference cannot pass "
             "the production lowered boundary");
      checkedDereference = true;
    }
  }
  expect(checkedDereference,
         "stored-referent local loans expose concrete dereference evidence");
}

void checkInvalidations() {
  const std::vector<std::string> programs{
      "Counter owner{}; Shared view{owner}; Shared& observed = view; "
      "auto moved = std::move(view); return observed.read() + moved.read();",
      "Counter owner{}; mut Shared view{owner}; Shared& observed = view; "
      "view.advance(); return observed.read();",
      "mut Counter owner{}; Shared view{owner}; Shared& observed = view; "
      "owner.value += 1; return observed.read();",
      "mut Counter owner{}; Exclusive view{owner}; Exclusive& observed = view; "
      "observed.advance(); return observed.read();",
      "mut Counter owner{}; mut Exclusive view{owner}; "
      "mut Exclusive& first = view; mut Exclusive& second = view; "
      "first.advance(); return second.read();",
      "Counter first{}; mut Counter second{}; SharedPair view{first, second}; "
      "SharedPair& reference = view; second.value += 1; return "
      "reference.read();"};
  for (const auto &program : programs) {
    const auto frontend = analyze(program);
    expect(!frontend.canGenerateCode(),
           "invalid carrier shell access is rejected at source: " + program);
    expect(std::any_of(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code.starts_with("GTI-S");
                       }) &&
               std::none_of(frontend.diagnostics.begin(),
                            frontend.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-B0001";
                            }),
           "borrow conflicts are semantic diagnostics, not backend ICEs");
  }
}

void checkShellEscapes() {
  for (const std::string &bad :
       {"Shared& bad(Shared& source) { Shared local = source; return local; }",
        "Shared& bad(Shared source) { return source; }",
        "class Leaker { Shared stored; public: "
        "Leaker(Shared source) : stored(source) {} "
        "Shared& bad() { Shared local = this.stored; return local; } };"}) {
    const auto frontend = lang::Frontend().analyze(
        "carrier-shell-escape.gti",
        declarations + bad + "\nint main() { return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!frontend.canGenerateCode(),
           "local and by-value shells never escape: " + bad);
    const bool byValue = bad == "Shared& bad(Shared source) { return source; }";
    const auto found =
        std::find_if(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                     [byValue](const auto &diagnostic) {
                       return byValue
                                  ? diagnostic.code == "GTI-S2085"
                                  : diagnostic.code == "GTI-S2017" &&
                                        diagnostic.message.find(
                                            "not a local or by-value object") !=
                                            std::string::npos;
                     });
    expect(found != frontend.diagnostics.end(),
           "shell escape has its source lifetime diagnostic");
    if (found != frontend.diagnostics.end())
      expect(!found->hints.empty() && found->fixes.empty(),
             "shell escape suggests lifetime choices without guessing an "
             "ownership edit");
  }
}

void checkDependencyMutations() {
  const auto frontend = analyze(
      "mut Counter one{}; mut Exclusive view{one}; mut Exclusive& local = "
      "view; "
      "local.advance(); mut Counter two{}; mut Holder holder{Exclusive(two)}; "
      "mut Exclusive& called = holder.get(); called.advance(); return "
      "called.read() - 5;");
  expect(frontend.canGenerateCode(), "dependency mutation seed verifies");
  if (!frontend.canGenerateCode())
    return;
  std::unordered_set<lang::MirLoanKind> mutatedKinds;
  bool mutatedPaths = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      if (loan.storedDependency != 0) {
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1].storedDependency = 0;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "independent program proof rejects erased external dependency");
        mutatedKinds.insert(loan.kind);
      }
      if (loan.entry && loan.kind == lang::MirLoanKind::Stored &&
          !loan.designatedFields.empty()) {
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1].designatedFields.clear();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "independent program proof rejects erased abstract field path");
        mutatedPaths = true;
      }
    }
  }
  for (const auto kind :
       {lang::MirLoanKind::Local, lang::MirLoanKind::Parameter,
        lang::MirLoanKind::CallResult, lang::MirLoanKind::Return})
    expect(mutatedKinds.contains(kind),
           "every physical carrier reference family is mutated");
  expect(mutatedPaths, "abstract entry field paths were independently mutated");
}

void checkProtectedEscapeAnalysis() {
  const auto frontend =
      analyze("mut Counter owner{}; mut Exclusive view{owner}; "
              "mut Exclusive& reference = relay(view); reference.advance(); "
              "return reference.read() - 5;");
  expect(frontend.canGenerateCode(), "carrier escape analysis seed verifies");
  if (!frontend.canGenerateCode())
    return;
  bool checkedIndependentOwner = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto analysis = lang::analyzeMirPlaceAliasEscape(
        frontend.mir,
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
    for (const auto &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::CallResult ||
          loan.storedDependency == 0)
        continue;
      const auto sources =
          lang::mirLoanProtectedSources(function.body, loan.id);
      if (sources.size() < 2)
        continue;
      for (const auto source : sources)
        expect(source <= analysis.escapes.size() &&
                   analysis.escapes[source - 1].has(
                       lang::MirPlaceEscapeReason::ExactCall),
               "exact reference calls retain shell and external owner escape "
               "facts for optimization");
      checkedIndependentOwner = true;
    }
  }
  expect(checkedIndependentOwner,
         "optimizer evidence includes a distinct external owner, not only an "
         "abstract formal source");
}

} // namespace

int main() {
  checkLocalReferences();
  checkReferenceCopies();
  checkLocalReferentBindings();
  checkInvalidations();
  checkShellEscapes();
  checkDependencyMutations();
  checkProtectedEscapeAnalysis();
  return failures == 0 ? 0 : 1;
}
