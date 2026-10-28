#include "compiler_test_fixture.h"
#include "gti/mir_effects.h"
#include "gti/optimization/alias.h"

namespace {

const std::string declarations = R"gti(
#include <std/utility>
struct Counter { mut int value = 4;  public: Counter() = default; };
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

void checkTextReferenceDependencies() {
  const std::string prefix = R"gti(
#include <std/string>
struct TextReader {
  std::string_view& text;
  TextReader(std::string_view& input) : text(input) {}
  std::size_t size() { return this.text.size(); }
  std::string_view read() { return this.text; }
};
std::size_t text_size(std::string_view& input, std::string_view& other) {
  TextReader reader{input};
  return reader.size() + other.size();
}
std::string_view& relay_text(std::string_view& input) { return input; }
std::size_t relayed_size(std::string_view& input) {
  TextReader reader{relay_text(input)};
  std::string_view copied = reader.read();
  return copied.size();
}
)gti";
  const auto run = [&](std::string body) {
    return lang::Frontend().analyze("text-reference-dependencies.gti",
                                    prefix + body, {standardLibraryPrelude()},
                                    {}, {standardLibraryRoot()});
  };
  const auto frontend = run(R"gti(
int main() {
  std::string first = std::string("first");
  std::string second = std::string("second");
  std::string_view a = first.view();
  std::string_view b = second.view();
  std::string_view& alias = relay_text(a);
  TextReader direct{alias};
  TextReader nested{relay_text(alias)};
  if (direct.size() != std::size_t(5) || nested.size() != std::size_t(5))
    return 1;
  return int(text_size(a, b) + relayed_size(a)) - 16;
}
)gti");
  expect(frontend.canGenerateCode(),
         "a text reference retains both its physical view and external owner");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  try {
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "verified text-reference adapters reach the production backend");
  } catch (const std::logic_error &error) {
    expect(false, std::string("text-reference backend: ") + error.what());
  }
  bool checkedEntry = false;
  bool checkedWrongOwner = false;
  bool checkedEndpoint = false;
  bool checkedReturnedReference = false;
  bool checkedCopiedReturn = false;
  bool checkedRetainedAccessor = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::StringView &&
        function.returnBorrowOrigin == lang::BorrowOriginKind::Receiver) {
      for (const auto &block : function.body.blocks) {
        if (!block.terminator.returnLoan)
          continue;
        auto missing = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            missing.findFunctionInstance(function.id));
        changed->body.blocks[block.id - 1].terminator.returnLoan.reset();
        expect(!lang::verifyMirProgram(missing).valid(),
               "a copied text-reference field cannot erase its return owner");
        checkedCopiedReturn = true;
      }
    }
    for (const auto &loan : function.body.loans) {
      const auto *parent = function.body.findLoan(loan.parent);
      if (loan.kind == lang::MirLoanKind::Stored && parent &&
          parent->kind == lang::MirLoanKind::CallResult &&
          !parent->carriers.empty() && loan.storedDependency != 0) {
        auto detached = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            detached.findFunctionInstance(function.id));
        changed->body.loans[loan.id - 1].parent = 0;
        expect(!lang::verifyMirProgram(detached).valid(),
               "a stored reference cannot detach its named accessor parent");
        checkedRetainedAccessor = true;
      }
      if (loan.kind == lang::MirLoanKind::CallResult &&
          loan.storedDependency != 0) {
        const auto *dependency = function.body.findLoan(loan.storedDependency);
        const auto *origin =
            dependency ? function.body.findPlace(dependency->source) : nullptr;
        if (origin && origin->type.kind == lang::SemanticType::Reference &&
            origin->type.arguments.size() == 1 &&
            origin->type.arguments.front().kind ==
                lang::SemanticType::StringView) {
          auto missing = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              missing.findFunctionInstance(function.id));
          changed->body.loans[loan.id - 1].storedDependency = 0;
          expect(!lang::verifyMirProgram(missing).valid(),
                 "a returned text reference cannot lose its external owner");
          checkedReturnedReference = true;
        }
      }
      const auto *source = function.body.findPlace(loan.source);
      if (!loan.entry || loan.kind != lang::MirLoanKind::Parameter || !source ||
          source->type.kind != lang::SemanticType::Reference ||
          source->type.arguments.size() != 1 ||
          source->type.arguments.front().kind != lang::SemanticType::StringView)
        continue;
      const auto *dependency = function.body.findLoan(loan.storedDependency);
      expect(dependency && dependency->entry &&
                 dependency->kind == lang::MirLoanKind::Stored &&
                 dependency->access == lang::AccessMode::ReadOnly &&
                 dependency->source == loan.source &&
                 dependency->designatedFields.empty(),
             "text reference entry has an exact independent read-only owner "
             "witness");
      if (!dependency)
        continue;
      auto missing = frontend.mir;
      auto *missingFunction = const_cast<lang::MirFunctionInstance *>(
          missing.findFunctionInstance(function.id));
      missingFunction->body.loans[loan.id - 1].storedDependency = 0;
      expect(!lang::verifyMirProgram(missing).valid(),
             "a text reference may not silently drop its owner witness");
      auto clearedTraits = missing;
      auto *clearedFunction = const_cast<lang::MirFunctionInstance *>(
          clearedTraits.findFunctionInstance(function.id));
      clearedFunction->body.places[loan.source - 1]
          .traits.containsBorrowedState = false;
      expect(!lang::verifyMirProgram(clearedTraits).valid(),
             "clearing a text reference trait cannot forge a literal-backed "
             "source");
      auto upgraded = frontend.mir;
      auto *upgradedFunction = const_cast<lang::MirFunctionInstance *>(
          upgraded.findFunctionInstance(function.id));
      upgradedFunction->body.loans[dependency->id - 1].access =
          lang::AccessMode::Mutable;
      expect(!lang::verifyMirProgram(upgraded).valid(),
             "an opaque text dependency grants no mutable owner authority");
      checkedEntry = true;
      for (const auto &other : function.body.loans) {
        if (!other.entry || other.kind != lang::MirLoanKind::Stored ||
            other.source == dependency->source)
          continue;
        auto forged = frontend.mir;
        auto *forgedFunction = const_cast<lang::MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        forgedFunction->body.loans[loan.id - 1].storedDependency = other.id;
        expect(!lang::verifyMirProgram(forged).valid(),
               "an unrelated text owner cannot satisfy a reference dependency");
        checkedWrongOwner = true;
        break;
      }
      for (const auto &block : function.body.blocks) {
        const auto shellEnd = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == loan.id;
            });
        const auto ownerEnd = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == dependency->id;
            });
        if (shellEnd == block.instructions.end() ||
            ownerEnd == block.instructions.end())
          continue;
        expect(shellEnd < ownerEnd,
               "the text owner outlives its reference shell");
        auto premature = function.body;
        auto &instructions = premature.blocks[block.id - 1].instructions;
        std::swap(instructions[shellEnd - block.instructions.begin()].loan,
                  instructions[ownerEnd - block.instructions.begin()].loan);
        expect(!lang::verifyMirBody(premature, function.id).valid(),
               "text owner retirement before its live reference is rejected");
        checkedEndpoint = true;
        break;
      }
    }
  }
  expect(checkedEntry && checkedWrongOwner && checkedEndpoint &&
             checkedReturnedReference && checkedCopiedReturn &&
             checkedRetainedAccessor,
         "text reference fixtures exercise missing, forged and early owner "
         "retirement");
  for (const std::string invalid :
       {"int main() { mut std::string owner = std::string(\"text\"); "
        "std::string_view view = owner.view(); "
        "std::string_view& alias = relay_text(view); "
        "TextReader reader{relay_text(alias)}; owner.clear(); "
        "return int(reader.size()); }",
        "int main() { std::string owner = std::string(\"text\"); "
        "mut std::string_view view = owner.view(); "
        "std::string_view& alias = relay_text(view); TextReader reader{alias}; "
        "view = \"changed\"; return int(reader.size()); }",
        "int main() { mut std::string owner = std::string(\"text\"); "
        "std::string_view view = owner.view(); TextReader reader{view}; "
        "owner.clear(); return int(reader.size()); }",
        "int main() { std::string owner = std::string(\"text\"); "
        "mut std::string_view view = owner.view(); TextReader reader{view}; "
        "view = \"changed\"; return int(reader.size()); }",
        "int main() { mut std::string owner = std::string(\"text\"); "
        "std::string_view view = owner.view(); TextReader reader{view}; "
        "std::string_view copy = reader.read(); owner.clear(); "
        "return int(copy.size()); }",
        "std::string_view escape_copy() { "
        "std::string owner = std::string(\"text\"); "
        "std::string_view view = owner.view(); TextReader reader{view}; "
        "return reader.read(); }",
        "TextReader escape() { std::string owner = std::string(\"text\"); "
        "std::string_view view = owner.view(); return TextReader(view); }"}) {
    const auto rejected = run(invalid);
    expect(!rejected.semanticValid &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "checked text reference rejects invalidation or escape before MIR");
  }
}

// A view read through a checked text reference keeps its external owner. It
// can be returned under the receiver dependency or copied from an exact
// reference formal, but never becomes an ownerless value that could be stored
// by value or escape that owner.
void checkTextReferenceReads() {
  const std::string prefix = R"gti(
#include <std/string>
class Named {
  std::string_view name;
public:
  Named(std::string_view name) : name(name) {}
};
std::string_view relay(std::string_view text) { return text; }
struct TextReader {
  std::string_view& text;
  TextReader(std::string_view& input) : text(input) {}
  std::string_view view() { return this.text; }
  std::size_t size() { return this.text.size(); }
};
std::size_t copied_size(std::string_view& input) {
  std::string_view copy = input;
  return copy.size();
}
)gti";
  const auto run = [&](std::string body) {
    return lang::Frontend().analyze("text-reference-reads.gti", prefix + body,
                                    {standardLibraryPrelude()}, {},
                                    {standardLibraryRoot()});
  };
  const auto frontend = run(R"gti(
int main() {
  std::string owner = std::string("owner");
  std::string_view view = owner.view();
  TextReader reader{view};
  std::string_view read = reader.view();
  return int(read.size() + copied_size(view) + reader.size()) - 15;
}
)gti");
  expect(frontend.canGenerateCode(),
         "a text reference field returns, and a reference formal copies, its "
         "dependent view");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "text reference reads verify");
  try {
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "text reference reads reach the production backend");
  } catch (const std::logic_error &error) {
    expect(false, std::string("text-reference reads backend: ") + error.what());
  }
  bool checkedReturn = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    // Only a receiver whose own text reference supplies a stored owner entry;
    // ordinary owning-text relays keep their existing receiver loans.
    if (function.returnType.kind != lang::SemanticType::StringView ||
        function.returnBorrowOrigin != lang::BorrowOriginKind::Receiver ||
        std::none_of(function.body.loans.begin(), function.body.loans.end(),
                     [](const auto &loan) {
                       return loan.entry &&
                              loan.kind == lang::MirLoanKind::Stored;
                     }))
      continue;
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.returnLoan)
        continue;
      const auto *published =
          function.body.findLoan(*block.terminator.returnLoan);
      const bool fromStoredOwner =
          published != nullptr &&
          std::any_of(function.body.loans.begin(), function.body.loans.end(),
                      [&](const auto &loan) {
                        return loan.entry &&
                               loan.kind == lang::MirLoanKind::Stored &&
                               loan.access == lang::AccessMode::ReadOnly &&
                               loan.source == published->source;
                      });
      expect(published && published->kind == lang::MirLoanKind::Return &&
                 published->escapes && fromStoredOwner,
             "a returned text reference read publishes the receiver's stored "
             "text owner");
      auto missing = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          missing.findFunctionInstance(function.id));
      changed->body.blocks[block.id - 1].terminator.returnLoan.reset();
      expect(!lang::verifyMirProgram(missing).valid(),
             "a returned text reference read cannot drop its escaping loan");
      checkedReturn = true;
    }
  }
  expect(checkedReturn, "a receiver text reference return is exercised");
  for (const std::string invalid :
       {"struct Holder { std::string_view& text; "
        "Holder(std::string_view& input) : text(input) {} "
        "Named launder() { return Named(this.text); } }; "
        "int main() { return 0; }",
        "struct Holder { std::string_view& text; "
        "Holder(std::string_view& input) : text(input) {} "
        "Named launder() { return Named(relay(this.text)); } }; "
        "int main() { return 0; }",
        "struct Holder { std::string_view& text; "
        "Holder(std::string_view& input) : text(input) {} "
        "Named launder() { std::string_view copy = this.text; "
        "return Named(copy); } }; int main() { return 0; }",
        "int main() { mut std::string owner = std::string(\"text\"); "
        "std::string_view view = owner.view(); TextReader reader{view}; "
        "std::string_view read = reader.view(); owner.clear(); "
        "return int(read.size()); }",
        "std::string_view escape() { std::string owner = "
        "std::string(\"text\"); std::string_view view = owner.view(); "
        "TextReader reader{view}; return reader.view(); } "
        "int main() { return 0; }"}) {
    const auto rejected = run(invalid);
    expect(!rejected.semanticValid &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "a text reference read cannot launder, outlive or escape its owner");
  }
  // A fresh adapter over a dynamic view borrows the exact view shell and keeps
  // that shell's own owner loan, as a bound adapter's contract does.
  const auto temporaries = run(R"gti(
std::size_t count(TextReader reader) { return reader.size(); }
TextReader make(std::string_view& input) { return TextReader(input); }
int main() {
  std::string owner = std::string("owner");
  std::string_view view = owner.view();
  return int(count(TextReader(view)) + count(make(view)) + make(view).size()) -
         15;
}
)gti");
  expect(temporaries.canGenerateCode() &&
             lang::verifyMirProgram(temporaries.mir).valid(),
         "fresh text-reference adapters verify as temporary arguments and "
         "receivers");
  std::size_t shellLoans = 0;
  for (const auto &function : temporaries.mir.functionInstances())
    for (const auto &loan : function.body.loans) {
      const auto *source = function.body.findPlace(loan.source);
      const auto *owner = function.body.findLoan(loan.storedDependency);
      if (loan.entry || loan.kind != lang::MirLoanKind::Stored || !source ||
          source->root != lang::MirPlaceRootKind::Binding ||
          !source->projections.empty() ||
          source->type.kind != lang::SemanticType::StringView || !owner ||
          owner->kind != lang::MirLoanKind::Stored)
        continue;
      ++shellLoans;
      auto detached = temporaries.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          detached.findFunctionInstance(function.id));
      changed->body.loans[loan.id - 1].storedDependency = 0;
      expect(!lang::verifyMirProgram(detached).valid(),
             "a text-reference adapter cannot drop its view's owner loan");
    }
  expect(shellLoans >= 3,
         "constructor, factory and receiver temporaries borrow the view shell");
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

void checkOwningAccessorAdapters() {
  const std::string program = declarations + R"gti(
#include <std/memory>
class Adapter {
  mut Counter& target;
public:
  Adapter(mut Counter& source) : target(source) {}
  void advance() mut { this.target.value += 1; }
  int read() { return this.target.value; }
};
int main() {
  mut auto owner = std::make_unique<Counter>();
  {
    mut Adapter adapter{*owner};
    adapter.advance();
    if (adapter.read() != 5) { return 1; }
  }
  return owner->value - 5;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "owning-accessor-adapter.gti", program, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "stored adapter retains owning accessor result");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (frontend.canGenerateCode()) {
    try {
      expect(!gti_test::emitCpp(frontend).contents.empty(),
             "owning accessor adapter reaches the verified MIR backend");
    } catch (const std::logic_error &error) {
      expect(false,
             std::string("owning accessor adapter backend: ") + error.what());
    }
    bool checkedParent = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        const auto *parent = function.body.findLoan(loan.parent);
        if (loan.kind != lang::MirLoanKind::Stored || parent == nullptr ||
            parent->kind != lang::MirLoanKind::CallResult ||
            loan.carriers.empty())
          continue;
        auto missingParent = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            missingParent.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1].parent = 0;
        expect(!lang::verifyMirProgram(missingParent).valid(),
               "stored owning accessor adapter cannot lose its active parent");
        checkedParent = true;
      }
    }
    expect(checkedParent, "owning accessor adapter preserves its result chain");
  }
  for (const std::string mutation : {"owner.reset();", "owner->value = 99;",
                                     "auto moved = std::move(owner);"}) {
    std::string invalid = program;
    invalid.insert(invalid.find("    adapter.advance();"),
                   "    " + mutation + "\n");
    const auto rejected = lang::Frontend().analyze(
        "invalid-owning-accessor.gti", invalid, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    expect(!rejected.semanticValid,
           "retained accessor adapter prevents owner invalidation: " +
               mutation);
  }
}

void checkNestedAdapterDependencies() {
  const std::string program = declarations + R"gti(
class First {
  mut Exclusive& target;
public:
  First(mut Exclusive& source) : target(source) {}
  void advance() mut { this.target.advance(); }
  int read() { return this.target.read(); }
};
class Second {
  mut First& target;
public:
  Second(mut First& source) : target(source) {}
  void advance() mut { this.target.advance(); }
  int read() { return this.target.read(); }
};
int main() {
  mut Counter owner{};
  mut Exclusive view{owner};
  mut First first{view};
  mut Second second{first};
  second.advance();
  return second.read() - 5;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "nested-adapter-dependencies.gti", program, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "nested adapters preserve every stored dependency");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  bool checkedChain = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *dependency = function.body.findLoan(loan.storedDependency);
      if (dependency == nullptr || dependency->storedDependency == 0)
        continue;
      checkedChain = true;
      expect(
          lang::mirLoanProtectedSources(function.body, loan.id).size() >= 3,
          "optimization sees every protected source in nested adapter chain");
      auto cyclic = function.body;
      cyclic.loans[dependency->id - 1].storedDependency = loan.id;
      expect(lang::mirLoanProtectedSources(cyclic, loan.id).empty() &&
                 !lang::verifyMirBody(cyclic, function.id).valid(),
             "dependency cycles fail closed in both verifier and optimization "
             "query");
      auto missingDependency = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          missingDependency.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].storedDependency = 0;
      expect(!lang::verifyMirProgram(missingDependency).valid(),
             "nested adapter cannot drop its intermediate dependency");
    }
  }
  expect(checkedChain, "nested adapters exercise a transitive stored chain");
  for (const std::string mutation :
       {"owner.value = 99;", "view.advance();", "first.advance();",
        "auto moved = std::move(first);"}) {
    std::string invalid = program;
    invalid.insert(invalid.find("  second.advance();"), "  " + mutation + "\n");
    const auto rejected = lang::Frontend().analyze(
        "invalid-nested-adapter.gti", invalid, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    expect(!rejected.semanticValid,
           "nested adapter preserves ancestor lifetime: " + mutation);
  }
}

void checkIoConceptDiagnostics() {
  const std::string declarations = R"gti(
#include <std/io>
class WrongResult {
public:
  int read_some(mut std::mutable_span<uint8_t>& output) mut { return 0; }
public:
  WrongResult() = default;
};
class ReadonlyOutput {
public:
  expected<std::size_t, std::io_errc> read_some(std::span<uint8_t>& output) mut {
    return std::size_t(0);
  }
public:
  ReadonlyOutput() = default;
};
class MissingFlush {
public:
  expected<std::size_t, std::io_errc> write_some(std::span<uint8_t>& input) mut {
    return input.size();
  }
public:
  MissingFlush() = default;
};
)gti";
  const std::vector<std::string> calls{
      "mut WrongResult source{}; mut std::byte_buffer bytes{std::size_t(1)}; "
      "mut auto output = bytes.as_mutable_span(); auto result = "
      "std::io::read_exact(source, output);",
      "mut ReadonlyOutput source{}; mut std::byte_buffer "
      "bytes{std::size_t(1)}; "
      "mut auto output = bytes.as_mutable_span(); auto result = "
      "std::io::read_exact(source, output);",
      "mut MissingFlush sink{}; std::byte_buffer bytes{std::size_t(1)}; "
      "auto input = bytes.as_span(); auto result = std::io::write_all(sink, "
      "input);"};
  for (const auto &call : calls) {
    const auto frontend = lang::Frontend().analyze(
        "invalid-io-concept.gti",
        declarations + "\nint main() { " + call + " return 0; }\n",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!frontend.semanticValid &&
               std::any_of(frontend.diagnostics.begin(),
                           frontend.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code.starts_with("GTI-S");
                           }) &&
               std::none_of(frontend.diagnostics.begin(),
                            frontend.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-B0001";
                            }),
           "invalid I/O protocol is diagnosed before MIR: " + call);
    if (frontend.semanticValid ||
        std::none_of(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code.starts_with("GTI-S");
                     }))
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

void checkOwningElementSlice() {
  const auto canonical = lang::Frontend().analyze(
      standardLibraryRoot().parent_path() /
          "tests/fixtures/mutable_span_owned.gti",
      std::nullopt, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(canonical.canGenerateCode(),
         "mutable owning-element slice fixture verifies");
  if (!canonical.canGenerateCode()) {
    for (const auto &diagnostic : canonical.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const std::string program = R"gti(
#include <std/vector>
#include <std/string>
struct Record {
  mut std::string name;
  Record(std::string text) : name(std::move(text)) {}
};
int main() {
  mut std::vector<Record> records{};
  records.push_back(Record(std::string("a")));
  records.push_back(Record(std::string("b")));
  {
    mut auto slice = records.as_mutable_span();
    mut Record& first = slice.front();
    first.name.append("!");
  }
  if (records[std::size_t(0)].name != "a!" || records[std::size_t(1)].name != "b") {
    return 1;
  }
  return 0;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "owning-element-slice.gti", program, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "mutable slice permits owning element member mutation");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  for (const std::string mutation :
       {"records.clear();", "auto moved = std::move(records);",
        "mut auto alias = records.as_mutable_span();"}) {
    std::string invalid = program;
    invalid.insert(invalid.find("    first.name.append"),
                   "    " + mutation + "\n");
    const auto rejected = lang::Frontend().analyze(
        "invalid-owning-element-slice.gti", invalid, {standardLibraryPrelude()},
        {}, {standardLibraryRoot()});
    expect(!rejected.semanticValid,
           "owning-element slice forbids owner invalidation: " + mutation);
  }
}

void checkProjectedOwningMemberRetainedResults() {
  const std::string declarations = R"gti(
#include <std/string>
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int read() { return this.value; }
};
struct Leaf {
  mut std::string storage = std::string("owned");
  mut int value = 4;
  int& reference() { return this.value; }
  View view() { return View(this.value); }
public:
  Leaf() = default;
};
struct Record { mut Leaf leaf{};  public: Record() = default; };
class Owner {
  mut Record record{};
public:
  mut Record& get() mut { return this.record; }
  void change() mut { this.record.leaf.value += 1; }
public:
  Owner() = default;
};
)gti";
  for (const bool borrowedValue : {false, true}) {
    const std::string binding =
        borrowedValue ? "View retained = owner.get().leaf.view();"
                      : "int& retained = owner.get().leaf.reference();";
    const std::string observation =
        borrowedValue ? "retained.read()" : "retained";
    const std::string program =
        declarations + "\nint main() { mut Owner owner{}; { " + binding +
        " if (" + observation +
        " != 4) { return 1; } } owner.change(); return 0; }\n";
    const auto frontend = lang::Frontend().analyze(
        "projected-owning-member-retained.gti", program,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(frontend.canGenerateCode(),
           "owning projected receiver preserves a retained " +
               std::string(borrowedValue ? "borrowed value" : "reference"));
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    bool checkedEarlyEnd = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (function.entryKind == lang::ProgramEntryKind::None)
        continue;
      for (const auto &loan : function.body.loans) {
        const auto *parent = function.body.findLoan(loan.parent);
        if (parent == nullptr ||
            parent->kind != lang::MirLoanKind::CallResult ||
            loan.carriers.empty())
          continue;
        for (const auto &block : function.body.blocks) {
          const auto endpoint =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.kind ==
                                        lang::MirInstructionKind::EndBorrow &&
                                    instruction.loan == loan.id;
                           });
          if (endpoint == block.instructions.end())
            continue;
          auto earlyEnd = function.body;
          auto &changed = earlyEnd.blocks[block.id - 1]
                              .instructions[static_cast<std::size_t>(
                                  endpoint - block.instructions.begin())];
          changed.loan = parent->id;
          expect(!lang::verifyMirBody(earlyEnd, function.id).valid(),
                 "owning member receiver cannot end while its retained child "
                 "is live");
          checkedEarlyEnd = true;
          break;
        }
      }
    }
    expect(checkedEarlyEnd,
           "projected retained result exercises its accessor parent");
    for (const std::string mutation :
         {"owner.change();", "auto moved = std::move(owner);"}) {
      std::string invalid = program;
      invalid.insert(invalid.find(" if ("), " " + mutation);
      const auto rejected = lang::Frontend().analyze(
          "invalid-projected-owning-member.gti", invalid,
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
      expect(!rejected.semanticValid,
             "retained projected member blocks owner invalidation: " +
                 mutation);
    }
  }
}

void checkSharedChildOfExclusiveCarrier() {
  const std::string program = R"gti(
class ReadView {
  int& value;
public:
  ReadView(int& source) : value(source) {}
  int read() { return this.value; }
};
class WriteView {
  mut int& value;
public:
  WriteView(mut int& source) : value(source) {}
  ReadView inspect() { return ReadView(this.value); }
  void set(int n) mut { mut int& slot = this.value; slot = n; }
};
int main() {
  mut int owner = 4;
  {
    mut WriteView writer{owner};
    {
      ReadView reader = writer.inspect();
      ReadView copy = reader;
      if (copy.read() != 4 || reader.read() != 4) { return 1; }
    }
    writer.set(7);
  }
  return owner == 7 ? 0 : 2;
}
)gti";
  const auto frontend = lang::Frontend().analyze("shared-child.gti", program,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "shared child retains exclusive carrier dependency");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.message << '\n';
    }
    std::cerr << lang::MirPrinter().print(frontend.mir);
  }
  for (const std::string mutation : {"writer.set(99);", "owner = 99;",
                                     "WriteView stolen = std::move(writer);"}) {
    std::string invalid = program;
    invalid.insert(invalid.find("      if (copy.read()"), mutation + "\n");
    const auto rejected = lang::Frontend().analyze(
        "shared-child-conflict.gti", invalid, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    expect(!rejected.semanticValid && std::none_of(rejected.diagnostics.begin(),
                                                   rejected.diagnostics.end(),
                                                   [](const auto &diagnostic) {
                                                     return diagnostic.code ==
                                                            "GTI-B0001";
                                                   }),
           "shared child aliases block parent/owner invalidation: " + mutation);
  }
  bool hasChild = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || loan.parent == 0 ||
          loan.access != lang::AccessMode::ReadOnly || loan.carriers.size() < 2)
        continue;
      const auto *parent = function.body.findLoan(loan.parent);
      if (parent == nullptr || parent->access != lang::AccessMode::Mutable)
        continue;
      hasChild = true;
      auto malformed = function.body;
      malformed.loans[loan.id - 1].parent = loan.id;
      expect(!lang::verifyMirBody(malformed, function.id).valid(),
             "MIR rejects a cyclic parent for the aliased shared child");
    }
  }
  expect(hasChild, "MIR retains shared aliases under their exclusive parent");
}

void checkMappedChildThroughCarrierReference() {
  const std::string program = R"gti(
class ReadView {
  int& value;
public:
  ReadView(int& source) : value(source) {}
  int read() { return this.value; }
};
class WriteView {
  mut int& storage;
public:
  WriteView(mut int& source) : storage(source) {}
  ReadView inspect() { return ReadView(this.storage); }
  void set(int next) mut { mut int& slot = this.storage; slot = next; }
};
int inspect_reference(mut WriteView& writer) {
  ReadView reader = writer.inspect();
  ReadView copy = reader;
  int sum = copy.read() + reader.read();
  writer.set(9);
  return sum - 8;
}
int main() {
  mut int owner = 4;
  { mut WriteView writer{owner};
    int result = inspect_reference(writer);
    if (result != 0) { return result; }
  }
  return owner == 9 ? 0 : 1;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "mapped-carrier-reference-child.gti", program, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "a mapped shared adapter retains the selected external owner through "
         "a mutable carrier reference");
  bool checked = false;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source == nullptr ||
        function.source->name().lexeme != "inspect_reference")
      continue;
    for (const auto &child : function.body.loans) {
      if (child.kind != lang::SemanticLoanKind::StoredValue ||
          child.parent == 0 || child.carriers.size() != 2)
        continue;
      const auto parent =
          std::find_if(function.body.loans.begin(), function.body.loans.end(),
                       [&](const auto &candidate) {
                         return candidate.semanticLoan == child.parent;
                       });
      expect(parent != function.body.loans.end() &&
                 parent->kind == lang::SemanticLoanKind::StoredValue &&
                 parent->access == lang::AccessMode::Mutable &&
                 child.storedDependency == 0 &&
                 child.designatedFields.size() == 1 &&
                 child.place.projections.size() == 1 &&
                 child.place.projections.front().kind ==
                     lang::PlaceProjectionKind::Field &&
                 parent->designatedFields ==
                     std::vector<lang::SymbolId>{
                         child.place.projections.front().field} &&
                 child.designatedFields != parent->designatedFields,
             "shared aliases preserve distinct source and result fields "
             "without duplicating their parent as a shell dependency");
      checked = true;
    }
  }
  expect(checked, "the mapped adapter and its copy share one exact child loan");
  bool checkedMir = false;
  bool checkedShellEndpoint = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto alias =
        std::find_if(function.body.loans.begin(), function.body.loans.end(),
                     [](const auto &loan) {
                       return loan.entry && loan.storedDependency != 0;
                     });
    if (alias == function.body.loans.end())
      continue;
    const auto child =
        std::find_if(function.body.loans.begin(), function.body.loans.end(),
                     [&](const auto &loan) {
                       return loan.kind == lang::MirLoanKind::Stored &&
                              !loan.entry &&
                              loan.parent == alias->storedDependency &&
                              loan.carriers.size() == 2;
                     });
    if (child == function.body.loans.end())
      continue;
    checkedMir = true;
    expect(lang::verifyMirBody(function.body, function.id).valid(),
           "finishing a mapped child preserves its independently live input "
           "shell and external-owner loan");
    for (const auto &block : function.body.blocks) {
      const auto shellEnd = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const auto &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == alias->id;
          });
      const auto ownerEnd = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const auto &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == alias->storedDependency;
          });
      if (shellEnd == block.instructions.end() ||
          ownerEnd == block.instructions.end())
        continue;
      expect(shellEnd < ownerEnd,
             "the input shell ends before its retained external owner");
      auto premature = function.body;
      auto &instructions = premature.blocks[block.id - 1].instructions;
      std::swap(instructions[shellEnd - block.instructions.begin()].loan,
                instructions[ownerEnd - block.instructions.begin()].loan);
      const auto rejected = lang::verifyMirBody(premature, function.id);
      expect(std::any_of(rejected.errors.begin(), rejected.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "ended before its carrier-shell loan") !=
                                  std::string::npos;
                         }),
             "MIR independently rejects premature external-owner retirement");
      checkedShellEndpoint = true;
      break;
    }
    auto detached = function.body;
    detached.loans[alias->id - 1].storedDependency = 0;
    expect(!lang::verifyMirBody(detached, function.id).valid(),
           "a mapped carrier result requires the selected alias's exact "
           "external parent, not just an overlapping source");
    auto wrongDestination = function.body;
    wrongDestination.loans[child->id - 1].designatedFields =
        function.body.loans[child->parent - 1].designatedFields;
    expect(!lang::verifyMirBody(wrongDestination, function.id).valid(),
           "a mapped carrier result cannot publish the input field as its "
           "destination field");
    auto wrongAccess = function.body;
    wrongAccess.loans[child->id - 1].access = lang::AccessMode::Mutable;
    expect(!lang::verifyMirBody(wrongAccess, function.id).valid(),
           "a mapped shared result cannot acquire exclusive access");
    auto missingSourceField = function.body;
    bool removedSourceField = false;
    for (auto &block : missingSourceField.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.loan == child->id &&
            instruction.borrowFields.size() == 1) {
          instruction.borrowFields.front().sourceFields.clear();
          removedSourceField = true;
        }
    expect(removedSourceField &&
               !lang::verifyMirBody(missingSourceField, function.id).valid(),
           "an alias dependency alone cannot replace the selected field map");
  }
  expect(checkedMir, "the mapped carrier result exercises MIR parent proofs");
  expect(checkedShellEndpoint,
         "the input shell and its external owner have verified endpoints");
  for (const auto *mutation :
       {"writer.set(99);",
        "mut WriteView& sibling = writer; sibling.set(99);"}) {
    auto invalid = program;
    invalid.insert(invalid.find("  int sum ="), std::string(mutation) + "\n");
    const auto rejected = lang::Frontend().analyze(
        "mapped-carrier-reference-conflict.gti", invalid,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!rejected.semanticValid &&
               std::any_of(rejected.diagnostics.begin(),
                           rejected.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2017";
                           }) &&
               std::none_of(rejected.diagnostics.begin(),
                            rejected.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-B0001";
                            }),
           "a shared result suspends parent access, including a sibling alias, "
           "until both shared aliases finish");
  }
}

void checkNestedCarrierAccess() {
  const std::string program = R"gti(
class CellView {
  mut int& item;
public:
  CellView(mut int& source) : item(source) {}
  mut int& operator[](int index) mut { return this.item; }
  int read() { return this.item; }
};
class Device {
  mut CellView view;
  mut int offset = 0;
  mut int local = 0;
public:
  Device(CellView source) : view(std::move(source)) {}
  void write(int value) mut {
    mut CellView scratch{this.local};
    scratch[0] = value;
    this.view[this.offset] = value;
  }
  int read() { return this.view.read(); }
};
class Adapter {
  mut Device& target;
public:
  Adapter(mut Device& source) : target(source) {}
  void write(int value) mut { this.target.write(value); }
  int read() { return this.target.read(); }
};
int main() {
  mut int value = 1;
  {
    mut Device device{CellView(value)};
    {
      mut Adapter adapter{device};
      adapter.write(7);
      if (adapter.read() != 7) { return 1; }
    }
    device.write(8);
  }
  return value == 8 ? 0 : 2;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "nested-carrier-adapter.gti", program, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "nested carrier reference adapter verifies");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  for (const std::string mutation : {"device.write(99);", "value = 99;",
                                     "Device moved = std::move(device);"}) {
    auto invalid = program;
    invalid.insert(invalid.find("      adapter.write(7);"), mutation + "\n");
    const auto rejected = lang::Frontend().analyze(
        "nested-carrier-conflict.gti", invalid, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    expect(!rejected.semanticValid,
           "adapter retains shell and owner: " + mutation);
  }
  bool checkedPath = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      if (!loan.entry || loan.designatedFields.size() < 2)
        continue;
      auto malformed = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].designatedFields.pop_back();
      expect(
          !lang::verifyMirProgram(malformed).valid(),
          "nested carrier authority requires the exact reference field path");
      checkedPath = true;
    }
  }
  expect(checkedPath, "nested carrier test covers abstract reference paths");
}

void checkReadOnlyConstructorDependencies() {
  for (const std::string carrier : {"Shared", "Exclusive"}) {
    const std::string program = "#include <std/panic>\n" + declarations +
                                "\nusing Carrier = " + carrier + R"gti(;
class Marker {
public:
  Marker(bool fail) { if (fail) { std::panic("initializer failure"); } }
  ~Marker() {}
public:
  Marker() = default;
};
class Adapter {
  Carrier& target;
  Marker marker;
public:
  Adapter(Carrier& source, bool fail_initialization, bool fail_body)
      : target(source), marker(Marker(fail_initialization)) {
    if (fail_body) { std::panic("body failure"); }
  }
  int read() { return this.target.read(); }
};
int main() {
  mut Counter owner{};
  mut Carrier view{owner};
  Adapter adapter{view, false, false};
  return adapter.read();
}
)gti";
    const auto frontend = lang::Frontend().analyze(
        "readonly-carrier-constructor.gti", program, {standardLibraryPrelude()},
        {}, {standardLibraryRoot()});
    expect(frontend.canGenerateCode(),
           "read-only constructor shell retains external dependency: " +
               carrier);
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    if (carrier == "Exclusive") {
      std::string projected = program;
      const std::string local = "  Adapter adapter{view, false, false};";
      projected.replace(projected.find(local), local.size(),
                        "  mut Holder holder{std::move(view)};\n"
                        "  Adapter adapter{holder.stored, false, false};");
      const auto field = lang::Frontend().analyze(
          "projected-carrier-adapter.gti", projected,
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
      expect(field.canGenerateCode(),
             "constructor shell preserves exact field projection type");
      if (!field.canGenerateCode())
        for (const auto &diagnostic : field.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    bool checkedPublication = false;
    std::size_t checkedFailureEdges = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      for (const auto &loan : constructor.body.loans) {
        if (loan.kind != lang::MirLoanKind::Stored || !loan.escapes ||
            loan.storedField == 0 || loan.storedDependency == 0 ||
            loan.access != lang::AccessMode::ReadOnly)
          continue;
        checkedPublication = true;
        auto missingDependency = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            missingDependency.findConstructorInstance(constructor.id));
        changed->body.loans[loan.id - 1].storedDependency = 0;
        expect(!lang::verifyMirProgram(missingDependency).valid(),
               "published constructor shell cannot lose its owner dependency");
        for (const auto &block : constructor.body.blocks) {
          if (block.terminator.kind !=
              lang::MirTerminatorKind::PropagateFailure)
            continue;
          const auto shellEnd =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.kind ==
                                        lang::MirInstructionKind::EndBorrow &&
                                    instruction.loan == loan.id;
                           });
          if (shellEnd == block.instructions.end())
            continue;
          const auto ownerEnd =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.kind ==
                                        lang::MirInstructionKind::EndBorrow &&
                                    instruction.loan == loan.storedDependency;
                           });
          expect(ownerEnd != block.instructions.end() && shellEnd < ownerEnd,
                 "failed construction retires the shell before its external "
                 "owner");
          for (const auto ended : {loan.id, loan.storedDependency}) {
            auto malformed = constructor.body;
            auto &failure = malformed.blocks[block.id - 1];
            std::erase_if(failure.instructions, [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == ended;
            });
            expect(
                !lang::verifyMirBody(malformed, constructor.id).valid(),
                "constructor failure must retire both published dependencies");
          }
          ++checkedFailureEdges;
        }
      }
    }
    expect(checkedPublication, "read-only adapter publishes a dual dependency");
    expect(
        checkedFailureEdges >= 2,
        "initializer and body failure exercise constructor dependency cleanup");
    for (const std::string mutation : {"view.advance();", "owner.value = 99;",
                                       "Carrier moved = std::move(view);"}) {
      auto invalid = program;
      invalid.insert(invalid.find("  return adapter.read();"), mutation + "\n");
      const auto rejected = lang::Frontend().analyze(
          "readonly-carrier-constructor-conflict.gti", invalid,
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
      expect(!rejected.semanticValid,
             "read-only adapter preserves shell and external owner (" +
                 carrier + "): " + mutation);
    }
  }
}

void checkReferenceCopies() {
  for (const std::string function :
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
  for (const std::string bad :
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

// A moved borrow carrier relocates as the addresses it holds (owner decision
// 2026-10-28): its generated value move is a relocation, while its
// source-visible relocation stays unavailable, and a loan it carries through
// a move never borrows its own storage.
void checkMovedCarrierRelocation() {
  const auto frontend = lang::Frontend().analyze("moved-carrier.gti", R"gti(
interface Gauge { int read(); };
class Scaled : public Gauge {
  int value;
  int& scale;
public:
  Scaled(int value, int& scale) : value(value), scale(scale) {}
  int read() override { return this.value * this.scale; }
};
int main() {
  int scale = 3;
  Scaled first{2, scale};
  Scaled moved = std::move(first);
  return moved.read() - 6;
}
)gti",
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a moved borrow carrier verifies");
  if (!frontend.canGenerateCode())
    return;
  bool checkedKinds = false;
  for (const auto &instance : frontend.mir.classInstances()) {
    if (!instance.traits.containsBorrowedState || !instance.polymorphic)
      continue;
    expect(instance.traits.relocation == lang::RelocationKind::Unavailable &&
               lang::mirTypeRelocationKind(frontend.mir, instance.type) ==
                   lang::RelocationKind::Unavailable &&
               lang::mirTypeMoveRelocationKind(frontend.mir, instance.type) ==
                   lang::RelocationKind::Structural,
           "a carrier's value move relocates while std::relocatable stays "
           "false");
    checkedKinds = true;
  }
  expect(checkedKinds, "the carrier class instance was inspected");
  bool checkedSelfBorrow = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone || stage.kind != lang::MirInstructionKind::Move ||
            stage.operands.size() != 1 ||
            !stage.info.traits.containsBorrowedState)
          continue;
        const auto *moved = body.findPlace(stage.operands.front().place);
        if (moved == nullptr || moved->binding == 0)
          continue;
        for (const auto &loan : body.loans) {
          if (loan.kind != lang::MirLoanKind::Stored || loan.entry ||
              std::find(loan.carriers.begin(), loan.carriers.end(),
                        moved->binding) == loan.carriers.end())
            continue;
          auto selfBorrow = body;
          selfBorrow.loans[loan.id - 1].source = moved->id;
          const auto verification =
              lang::verifyMirBody(selfBorrow, function.id);
          expect(std::any_of(verification.errors.begin(),
                             verification.errors.end(),
                             [](const auto &error) {
                               return error.message ==
                                      "moved borrow carrier stores a loan of "
                                      "its own storage";
                             }),
                 "a carrier holding a loan of its own storage cannot move");
          checkedSelfBorrow = true;
        }
      }
  }
  expect(checkedSelfBorrow, "the moved carrier's stored loan was mutated");
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

// A reference of a base or interface type to a borrow carrier borrows the
// carrier object as a reference of the carrier's own type does: the base hides
// the stored references, not the object's lifetime. A local binding, a call
// relaying an upcast argument and an upcast return each keep the carrier
// borrowed while the view is used, so moving it then is GTI-S2017, and moving
// it after the view's last use is valid. MIR rejects a carrier reference
// backed only by the carrier's Stored loan, which designates the external
// referent rather than the carrier.
const std::string upcastCarriers = R"gti(
#include <std/utility>
interface Gauge { int32_t read(); };
class Scaled : public Gauge {
  int32_t value;
  int32_t& scale;
public:
  Scaled(int32_t v, int32_t& s) : value(v), scale(s) {}
  int32_t read() override { return this.value * this.scale; }
};
class Base {
public:
  int32_t value;
  Base(int32_t v) : value(v) {}
  int32_t get() { return this.value; }
};
class Plain : public Base {
  int32_t& scale;
public:
  Plain(int32_t v, int32_t& s) : Base(v), scale(s) {}
  int32_t read() { return this.value * this.scale; }
};
Gauge& pick(Gauge& gauge) { return gauge; }
Gauge& up(Scaled& scaled) { return scaled; }
int32_t sample(Gauge& gauge) { return gauge.read(); }
)gti";

lang::FrontendResult analyzeUpcast(const std::string &body) {
  return lang::Frontend().analyze(
      "upcast-carrier-references.gti",
      upcastCarriers + "int main() {\n  int32_t scale = 3;\n" + body + "\n}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

bool rejectedBorrow(const lang::FrontendResult &result) {
  return !result.canGenerateCode() &&
         hasDiagnosticCode(result.diagnostics, "GTI-S2017") &&
         std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      });
}

void checkUpcastCarrierReferences() {
  for (const std::string view :
       {"Gauge& view = direct;", "Gauge& view = pick(direct);",
        "Gauge& view = up(direct);"}) {
    const auto valid =
        analyzeUpcast("  Scaled direct{2, scale};\n  " + view +
                      "\n  int32_t first = view.read();\n"
                      "  Scaled moved = std::move(direct);\n"
                      "  return first + moved.read() + sample(moved);");
    if (!valid.canGenerateCode())
      for (const auto &diagnostic : valid.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
           "an upcast view may end before its carrier moves: " + view);
    expect(
        rejectedBorrow(analyzeUpcast("  Scaled direct{2, scale};\n  " + view +
                                     "\n  Scaled moved = std::move(direct);\n"
                                     "  return view.read() + moved.read();")),
        "an upcast view keeps its carrier borrowed: " + view);
  }
  expect(rejectedBorrow(analyzeUpcast("  Plain direct{2, scale};\n"
                                      "  Base& view = direct;\n"
                                      "  Plain moved = std::move(direct);\n"
                                      "  return view.get() + moved.read();")),
         "a base-class view keeps its carrier borrowed");
  const auto valid = analyzeUpcast("  Scaled direct{2, scale};\n"
                                   "  Gauge& view = direct;\n"
                                   "  return view.read();");
  expect(valid.canGenerateCode(), "an interface view of a carrier lowers");
  if (!valid.canGenerateCode())
    return;
  // Backing the view by the carrier's Stored loan, as lowering did before,
  // leaves the carrier itself unprotected.
  bool mutated = false;
  auto changed = valid.mir;
  for (const auto &function : valid.mir.functionInstances()) {
    auto &body = const_cast<lang::MirFunctionInstance *>(
                     changed.findFunctionInstance(function.id))
                     ->body;
    const auto stored = std::find_if(
        body.loans.begin(), body.loans.end(), [](const auto &loan) {
          return loan.kind == lang::MirLoanKind::Stored;
        });
    if (stored == body.loans.end())
      continue;
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions) {
        const auto *destination = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
        if (mutated ||
            instruction.kind != lang::MirInstructionKind::Initialize ||
            destination == nullptr ||
            destination->type.kind != lang::SemanticType::Reference ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan)
          continue;
        instruction.operands.front().loan = stored->id;
        mutated = true;
      }
  }
  expect(mutated, "the interface view is initialized from a carrier loan");
  expect(hasMirVerificationError(lang::verifyMirProgram(changed),
                                 "reference to a borrow carrier is not "
                                 "backed by a loan on the carrier"),
         "MIR rejects a carrier reference backed by its Stored loan");
}

void checkAbstractCarrierAliasResult() {
  const auto frontend = lang::Frontend().analyze(
      "abstract-carrier-alias-result.gti", R"gti(
class SelectedWindow {
  int& selected_value;
public:
  SelectedWindow(int& source) : selected_value(source) {}
  int read() { return this.selected_value; }
};
class Window {
  int& value;
public:
  Window(int& source) : value(source) {}
  SelectedWindow select() { return SelectedWindow(this.value); }
  int read() { return this.value; }
};
int inspect(Window input) {
  Window& alias = input;
  SelectedWindow selected = alias.select();
  return selected.read();
}
int main() { int owner = 7; return inspect(Window(owner)) - 7; }
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      frontend.canGenerateCode(),
      "mapped results through entry carrier aliases preserve external owner");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            instruction.borrowFields.size() != 1 || !instruction.loan ||
            instruction.info.type.kind != lang::SemanticType::Class)
          continue;
        const auto *result = function.body.findLoan(*instruction.loan);
        const auto alias = std::find_if(
            function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &loan) {
              const auto *dependency =
                  function.body.findLoan(loan.storedDependency);
              return loan.kind == lang::MirLoanKind::Local && dependency &&
                     dependency->entry &&
                     dependency->kind == lang::MirLoanKind::Stored && result &&
                     result->source == dependency->source;
            });
        if (alias == function.body.loans.end())
          continue;
        auto wrongSource = frontend.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         wrongSource.findFunctionInstance(function.id))
                         ->body;
        // The runtime alias shell is not the entry's external referent.
        // A verifier must not accept the new abstract-source path by simply
        // dropping either side of the caller's exact field contract.
        const auto shell = std::find_if(
            body.places.begin(), body.places.end(), [&](const auto &place) {
              return place.root == lang::MirPlaceRootKind::Binding &&
                     !alias->carriers.empty() &&
                     place.binding == alias->carriers.front() &&
                     place.projections.empty();
            });
        expect(shell != body.places.end(),
               "carrier alias has a physical shell");
        if (shell == body.places.end())
          continue;
        body.loans[result->id - 1].source = shell->id;
        expect(!lang::verifyMirProgram(wrongSource).valid(),
               "mapped alias result cannot substitute its carrier shell");
        auto wrongPath = frontend.mir;
        auto &pathBody = const_cast<lang::MirFunctionInstance *>(
                             wrongPath.findFunctionInstance(function.id))
                             ->body;
        pathBody.loans[result->id - 1].designatedFields =
            instruction.borrowFields.front().sourceFields;
        expect(!lang::verifyMirProgram(wrongPath).valid(),
               "mapped alias result cannot substitute its input field for its "
               "destination field");
        checked = true;
      }
    }
  }
  expect(checked,
         "unrelated user carrier exercises abstract entry alias proof");
}

// A carrier built from the view an accessor returned stores a reference to
// that returned referent, whether the result is a temporary or bound to a
// reference first. MIR keeps the referent under the accessor's result loan
// and holds exactly that loan's stored dependency.
void checkAccessorReturnedTextCarriers() {
  const auto frontend = lang::Frontend().analyze(
      "accessor-returned-text-carriers.gti", R"gti(
#include <std/string>
#include <std/optional>
class Box {
public:
  std::string_view text;
  Box(std::string_view text) : text(text) {}
  std::string_view& get() { return this.text; }
};
class Cursor {
  std::string_view& view;
public:
  Cursor(std::string_view& view) : view(view) {}
  std::size_t size() { return this.view.size(); }
};
Cursor open(std::string_view& view) { return Cursor(view); }
int main() {
  std::string owner = std::string("owned");
  Box owned = Box(owner.view());
  Box literal = Box("literal");
  auto first = owned.get().begin();
  auto second = literal.get().begin();
  std::string_view& named = literal.get();
  auto third = named.begin();
  Cursor cursor = open(owned.get());
  std::optional<std::string_view> maybe =
      std::optional<std::string_view>(owner.view());
  auto fourth = maybe.value().begin();
  if (*first != 'o' || *second != 'l' || *third != 'l' ||
      cursor.size() != std::size_t(5) || *fourth != 'o')
    return 1;
  return 0;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "carriers of accessor-returned views construct verified MIR");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  try {
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "accessor-returned text carriers reach the production backend");
  } catch (const std::logic_error &error) {
    expect(false,
           std::string("accessor text carrier backend: ") + error.what());
  }
  const auto rejectedWith = [](const lang::MirProgram &program,
                               std::string_view message) {
    const auto result = lang::verifyMirProgram(program);
    return std::any_of(
        result.errors.begin(), result.errors.end(), [&](const auto &error) {
          return error.message.find(message) != std::string::npos;
        });
  };
  std::size_t owned = 0;
  std::size_t ownerless = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *accessor = function.body.findLoan(loan.parent);
      const auto *referent = function.body.findPlace(loan.source);
      if (loan.kind != lang::MirLoanKind::Stored || accessor == nullptr ||
          accessor->kind != lang::MirLoanKind::CallResult ||
          referent == nullptr ||
          referent->root != lang::MirPlaceRootKind::Loan ||
          referent->loan != accessor->id || !referent->projections.empty())
        continue;
      expect(loan.storedDependency == accessor->storedDependency,
             "an accessor-referent carrier holds its result's dependency");
      const auto mutate = [&](const auto &change) {
        auto malformed = frontend.mir;
        change(const_cast<lang::MirFunctionInstance *>(
                   malformed.findFunctionInstance(function.id))
                   ->body.loans[loan.id - 1]);
        return malformed;
      };
      if (loan.storedDependency != 0) {
        ++owned;
        expect(rejectedWith(
                   mutate([](auto &changed) { changed.storedDependency = 0; }),
                   "carrier shell loan does not preserve its exact "
                   "stored dependency"),
               "an owner-backed accessor carrier cannot drop its dependency");
      } else {
        // Ownerless text is proved only at the exact returned referent under
        // its result loan, never at the accessor's owner.
        ++ownerless;
        expect(rejectedWith(mutate([](auto &changed) { changed.parent = 0; }),
                            "carrier reference result requires one exact "
                            "stored dependency"),
               "ownerless accessor text is proved only under its result loan");
        expect(rejectedWith(mutate([&](auto &changed) {
                              changed.source = accessor->source;
                            }),
                            "carrier reference result requires one exact "
                            "stored dependency"),
               "ownerless accessor text cannot move to the accessor's owner");
      }
    }
  }
  expect(owned >= 3 && ownerless >= 2,
         "temporary, named, user and optional accessor carriers are checked");

  // The carrier keeps the accessor's owner and the text's owner borrowed.
  const std::string box = R"gti(
#include <std/string>
#include <std/optional>
class Box {
public:
  mut std::string_view text;
  Box(std::string_view text) : text(text) {}
  std::string_view& get() { return this.text; }
};
int main() {
  mut std::string owner = std::string("owned");
)gti";
  for (const auto &[label, body] :
       {std::pair<std::string_view, std::string_view>{
            "text owner",
            "Box b = Box(owner.view()); auto it = b.get().begin(); "
            "owner.push_back('x'); return *it == 'o' ? 0 : 1;"},
        {"accessor owner",
         "mut Box b = Box(\"literal\"); auto it = b.get().begin(); "
         "b.text = \"other\"; return *it == 'l' ? 0 : 1;"},
        {"optional owner",
         "std::optional<std::string_view> o = "
         "std::optional<std::string_view>(owner.view()); auto it = "
         "o.value().begin(); owner.push_back('x'); return *it == 'o' ? 0 "
         ": 1;"}}) {
    const auto rejected = lang::Frontend().analyze(
        "accessor-text-carrier-mutation.gti", box + std::string(body) + "\n}\n",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!rejected.canGenerateCode() &&
               std::all_of(rejected.diagnostics.begin(),
                           rejected.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2017";
                           }) &&
               !rejected.diagnostics.empty(),
           std::string("an accessor text carrier keeps its ") +
               std::string(label) + " borrowed");
  }
}

void checkSharedAccessorRange() {
  const auto fixture = standardLibraryRoot().parent_path() /
                       "tests/fixtures/shared_accessor_range.gti";
  const auto frontend = lang::Frontend().analyze(fixture, std::nullopt,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "read-only traversal through a borrowed carrier retains the exact "
         "iterator result");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto prefix = source.substr(0, source.find("int main("));
  // expected<T&, E>::value() is an ordinary source accessor, so the fixture's
  // ranges borrow accessor call results. A range over a borrowed carrier's
  // stored reference still has the stored shared parent.
  const auto stored = lang::Frontend().analyze(
      "shared-stored-range.gti", prefix + R"(
struct Elements {
  std::vector<Item>& items;
  Elements(std::vector<Item>& items) : items(items) {}
};
int main() {
  mut Bag owner{};
  {
    Elements borrowed{owner.elements()};
    mut int sum = 0;
    for (auto& item : borrowed.items) { sum += item.read(); }
    if (sum != 15) { return 1; }
  }
  owner.clear();
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(stored.canGenerateCode() && lang::verifyMirProgram(stored.mir).valid(),
         "read-only traversal of a stored carrier reference verifies");
  bool checked = false;
  for (const auto &function : stored.mir.functionInstances()) {
    for (const auto &range : function.body.rangeIterations) {
      const auto *element = function.body.findLoan(range.elementLoan);
      const auto *parent = function.body.findLoan(range.rangeLoan);
      if (!element || !parent || element->storedDependency != parent->id ||
          parent->kind != lang::MirLoanKind::Stored)
        continue;
      expect(element->kind == lang::MirLoanKind::CallResult &&
                 element->access == lang::AccessMode::ReadOnly &&
                 element->parent == 0 &&
                 parent->access == lang::AccessMode::ReadOnly &&
                 !element->entry && !element->escapes &&
                 !element->carriers.empty(),
             "shared range element keeps its call loan and exact stored "
             "owner dependency through the source-defined iterator");
      const auto *iterator = function.body.findPlace(range.iterator);
      const auto *binding = function.body.findPlace(range.element);
      const lang::MirInstruction *producer = nullptr;
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.loan == element->id)
            producer = &instruction;
      expect(iterator && binding && producer &&
                 binding->traits.containsBorrowedState &&
                 !producer->info.traits.containsBorrowedState &&
                 producer->hirValue == range.elementAccessSource &&
                 std::find(parent->carriers.begin(), parent->carriers.end(),
                           iterator->binding) != parent->carriers.end(),
             "ordinary owning range elements have nominal unborrowed traits "
             "despite their hidden reference bindings");
      if (!iterator || !producer)
        continue;
      for (int mutation = 0; mutation < 12; ++mutation) {
        auto invalid = stored.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         invalid.findFunctionInstance(function.id))
                         ->body;
        auto &changed = body.loans[element->id - 1];
        if (mutation == 0)
          changed.kind = lang::MirLoanKind::Local;
        else if (mutation == 1)
          changed.access = lang::AccessMode::Mutable;
        else if (mutation == 3)
          changed.storedDependency = 0;
        else if (mutation == 2) {
          // Ending the range immediately after dereference must not revoke
          // the authority still required by the live returned element.
          lang::MirInstructionId last = 0;
          for (const auto &block : body.blocks)
            for (const auto &instruction : block.instructions)
              last = std::max(last, instruction.id);
          bool inserted = false;
          for (auto &block : body.blocks) {
            for (auto it = block.instructions.begin();
                 it != block.instructions.end(); ++it) {
              if (it->loan == element->id &&
                  it->kind == lang::MirInstructionKind::Call) {
                block.instructions.insert(
                    it + 1, {.id = last + 1,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .loan = parent->id});
                inserted = true;
                break;
              }
            }
            if (inserted)
              break;
          }
          expect(inserted, "range element has an exact call producer");
        } else if (mutation == 4) {
          std::erase(body.loans[parent->id - 1].carriers, iterator->binding);
        } else if (mutation == 5) {
          body.loans[parent->id - 1].kind = lang::MirLoanKind::Local;
        } else if (mutation == 6) {
          changed.source = range.rangeCarrier;
        } else if (mutation == 7) {
          changed.producedBy = 0;
        } else if (mutation == 8) {
          body.rangeIterations[range.id - 1].elementAccessSource++;
        } else if (mutation == 9) {
          body.rangeIterations[range.id - 1].dereferenceFunction++;
        } else {
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == producer->id) {
                if (mutation == 10)
                  instruction.receiver->place = range.rangeCarrier;
                else
                  instruction.info.traits.containsBorrowedState = true;
              }
        }
        expect(!lang::verifyMirProgram(invalid).valid(),
               "shared range rejects forged traits, producer, cursor "
               "authority, dependency or early owner retirement: " +
                   std::to_string(mutation));
      }
      checked = true;
      break;
    }
    if (checked)
      break;
  }
  expect(checked, "borrowed accessor range exercises a stored shared parent");

  for (const auto &operation :
       {"owner.clear();", "auto moved = std::move(owner);"}) {
    const auto invalid = lang::Frontend().analyze(
        "shared-accessor-invalidation.gti",
        prefix +
            "int main() { mut Bag owner{}; auto borrowed = lookup(owner); "
            "for (auto& item : borrowed.value().elements()) { " +
            operation + " return item.read(); } return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(invalid.syntaxValid && !invalid.semanticValid &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2017") &&
               !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001"),
           "retained shared element still prevents owner mutation/movement");
  }
  const auto escape = lang::Frontend().analyze(
      "shared-accessor-escape.gti",
      prefix +
          "Item& escape() { Bag owner{}; auto borrowed = lookup(owner); "
          "for (auto& item : borrowed.value().elements()) { return item; } "
          "return owner.elements()[std::size_t(0)]; } int main() { return 0; }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(escape.syntaxValid && !escape.semanticValid &&
             hasDiagnosticCode(escape.diagnostics, "GTI-S2017") &&
             !hasDiagnosticCode(escape.diagnostics, "GTI-B0001"),
         "a returned range element cannot outlive its local owner");
}

} // namespace

int main() {
  checkUpcastCarrierReferences();
  checkSharedAccessorRange();
  checkAccessorReturnedTextCarriers();
  checkAbstractCarrierAliasResult();
  checkProjectedOwningMemberRetainedResults();
  checkOwningElementSlice();
  checkOwningAccessorAdapters();
  checkNestedAdapterDependencies();
  checkIoConceptDiagnostics();
  checkTextReferenceDependencies();
  checkTextReferenceReads();
  checkLocalReferences();
  checkSharedChildOfExclusiveCarrier();
  checkMappedChildThroughCarrierReference();
  checkNestedCarrierAccess();
  checkReadOnlyConstructorDependencies();
  const auto span = lang::Frontend().analyze(
      standardLibraryRoot().parent_path() / "tests/fixtures/mutable_span.gti",
      std::nullopt, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(span.canGenerateCode(),
         "mutable span subview and shared reborrow verify");
  if (!span.canGenerateCode()) {
    std::cerr << "source=" << span.sourceValid << " syntax=" << span.syntaxValid
              << " semantics=" << span.semanticValid << " hir=" << span.hirValid
              << " mir=" << span.mirValid << '\n';
    for (const auto &diagnostic : span.diagnostics)
      std::cerr << diagnostic.message << '\n';
    for (const auto &error :
         lang::verifyHirProgramPlans(span.semantics, span.hir).errors)
      std::cerr << error << '\n';
  }
  bool checkedStoredDependency = false;
  for (const auto &function : span.mir.functionInstances()) {
    const auto *source = span.hir.findFunctionInstance(function.id);
    if (source == nullptr)
      continue;
    for (const auto &loan : function.body.loans) {
      const bool retainsIterator = std::any_of(
          source->body.statements.begin(), source->body.statements.end(),
          [&](const auto &statement) {
            return statement.rangeIteration &&
                   std::find(loan.carriers.begin(), loan.carriers.end(),
                             statement.rangeIteration->iterator) !=
                       loan.carriers.end();
          });
      if (!retainsIterator || loan.parent == 0 || loan.storedDependency == 0)
        continue;
      auto missingDependency = function.body;
      missingDependency.loans[loan.id - 1].storedDependency = 0;
      expect(!lang::verifyMirBody(missingDependency, function.id).valid(),
             "the retained range iterator must preserve its external owner");
      auto missingParent = function.body;
      missingParent.loans[loan.id - 1].parent = 0;
      expect(!lang::verifyMirBody(missingParent, function.id).valid(),
             "the retained range iterator must preserve the borrowed receiver "
             "lifetime");
      checkedStoredDependency = true;
    }
  }
  expect(checkedStoredDependency,
         "mutable-view iteration exercises dual dependency");
  const auto io = lang::Frontend().analyze(
      standardLibraryRoot().parent_path() / "tests/fixtures/composable_io.gti",
      std::nullopt, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(io.canGenerateCode(), "I/O composes stored and reference borrowing");
  if (!io.canGenerateCode()) {
    for (const auto &diagnostic : io.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  checkReferenceCopies();
  checkLocalReferentBindings();
  checkInvalidations();
  checkShellEscapes();
  checkDependencyMutations();
  checkProtectedEscapeAnalysis();
  checkMovedCarrierRelocation();
  return failures == 0 ? 0 : 1;
}
