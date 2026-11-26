// RFC 0003 shared borrowed storage: MIR/HIR structural and forged-program
// checks of retained text and owned storage. Moved unchanged from the former
// ownerless_text_contracts_tests.cpp; the source-observable cases of the
// same tests are tests/ui/ownership/text_provenance/ownerless cases.
#include "../../src/compiler/mir_borrow_audits.h"
#include "../../src/compiler/mir_carrier_flow.h"
#include "../../src/compiler/mir_storage_effects.h"
#include "../../src/compiler/mir_text_contracts.h"
#include "../../src/compiler/mir_verification_queries.h"
#include "../compiler_test_fixture.h"
#include "gti/hir.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

namespace {
// Cases over about 2 s of CPU run only in the daily slow tier
// (gti_unit_tests_slow sets GTI_UNIT_SLOW=1); per-commit runs skip them.
[[maybe_unused]] bool slowTier() {
  const char *value = std::getenv("GTI_UNIT_SLOW");
  return value != nullptr && std::string_view(value) == "1";
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (diagnostic.code != "GTI-B0001")
      continue;
    for (const auto &function : result.hir.functionInstances()) {
      const auto marker =
          "body kind " +
          std::to_string(static_cast<int>(lang::MirBodyKind::Function)) +
          ", owner " + std::to_string(function.id) + ",";
      if (diagnostic.message.find(marker) == std::string::npos)
        continue;
      const auto *info = result.semantics.findFunction(function.declaration);
      std::cerr << "FAILED FUNCTION " << function.id << ' '
                << (info ? info->qualifiedName : "<unknown>") << '\n';
      if (const auto *lowered = result.mir.findFunctionInstance(function.id))
        std::cerr << lang::MirPrinter().print(lowered->body);
    }
  }
}
lang::FrontendResult analyzeText(std::string source,
                                 bool reachableOnly = false) {
  lang::FrontendOptions options;
  options.lowerReachableOnly = reachableOnly;
  options.lowerLibraryReachableOnly = reachableOnly;
  if (reachableOnly)
    options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  return lang::Frontend(options).analyze(
      "ownerless-text-contracts.gti", std::move(source),
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

bool functionError(const lang::MirVerificationResult &result,
                   lang::HirFunctionInstanceId function,
                   std::string_view reason) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.bodyKind == lang::MirBodyKind::Function &&
                              error.owner == function &&
                              error.message.find(reason) != std::string::npos;
                     });
}

const std::string kReferenceAccessors = R"gti(
import std.string;
mut std::string_view kept = "";
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view& ref() { return this.text; }
  int& kindRef() { return this.kind; }
  int& kindOf() { return this.kindRef(); }
  std::string_view& alias() { return this.ref(); }
};
class Box<T> {
public:
  T value;
  Box(T value) : value(value) {}
  T& get() { return this.value; }
};
void keep(Token& token) { kept = token.ref(); }
uint64_t width(Token& token) { return token.ref().size(); }
)gti";

// A payload-enum field reached through the receiver's accessor holds what
// the field does: an arm returning its text payload by reference relays it,
// and one binding it by value places the same ownerless requirement as
// `switch (this.word)` would, which MIR derives through the accessor's body.
const std::string kAccessorSubjects = R"gti(
import std.panic;
import std.string;
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
class Line {
public:
  Word word;
  Line(std::string_view value) : word(Word::text(value)) {}
  Word& wordRef() { return this.word; }
  std::string_view& viaPlace() {
    switch (this.word) {
    case Word::text(auto& v): return v;
    case Word::none: std::panic("none");
    }
  }
  std::string_view& viaCall() {
    switch (this.wordRef()) {
    case Word::text(auto& v): return v;
    case Word::none: std::panic("none");
    }
  }
  std::string_view firstCall() {
    switch (this.wordRef()) {
    case Word::text(v): return v;
    case Word::none: return "";
    }
  }
};
)gti";

// Any use of an accessor's result but a switch on it reads the field it
// returns: passing it on or copying it out may carry a text payload away, so
// it requires the field's payloads ownerless, in semantics and in MIR. The
// accessor itself, whose every return borrows the field, requires nothing.
const std::string kAccessorEscapes = R"gti(
import std.string;
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
void keepWord(Word& w) {
  switch (w) {
  case Word::text(v): kept = v; break;
  case Word::none: break;
  }
}
class Line {
public:
  Word word;
  Line(std::string_view value) : word(Word::text(value)) {}
  Word& wordRef() { return this.word; }
  Word& pick(int index) { return this.word; }
  void passOut() { keepWord(this.wordRef()); }
  void passPick() { keepWord(this.pick(0)); }
  void copyOut() {
    Word w = this.wordRef();
    switch (w) {
    case Word::text(v): kept = v; break;
    case Word::none: break;
    }
  }
};
)gti";

// The source function whose qualified name is `name`.
const lang::MirFunctionInstance *
namedFunction(const lang::FrontendResult &result, std::string_view name) {
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        info != nullptr && info->qualifiedName == name)
      return &function;
  }
  return nullptr;
}

// Whether a program is rejected at source with `code` and no internal error.
bool rejectedAtSource(const lang::FrontendResult &result,
                      const std::string &code) {
  return !result.canGenerateCode() &&
         hasDiagnosticCode(result.diagnostics, code) &&
         std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      });
}

// Semantics never nests dynamic text in an array (L-OWN-02), so a view copied
// out of an array holds what the array holds: ownerless text, wherever the
// array lives. MIR proves it independently: each element write, an array
// literal's included, is a checked text store, and an element read is then
// an ownerless result proof. A user-defined record's array field and a
// lambda's result follow the same rules.
const std::string kArrayTextStorage = R"gti(
import std.string;
struct Shelf {
  std::string_view titles[2];
  int count;
  Shelf() : titles({"x", "yz"}), count(2) {}
};
std::string_view table[3] = {"zero", "one", "two"};
mut std::string_view kept = "";
std::string_view pick(std::string_view names[3], uint64_t index) {
  return names[index];
}
std::string_view fromTable(uint64_t index) { return table[index]; }
std::string_view fromLocal(uint64_t index) {
  std::string_view local[2] = {"left", "right"};
  return local[index];
}
std::string_view fromGrid(uint64_t row, uint64_t column) {
  std::string_view grid[2][2] = {{"a", "b"}, {"c", "d"}};
  return grid[row][column];
}
std::string_view fromShelf(Shelf shelf, uint64_t index) {
  return shelf.titles[index];
}
std::string_view fromDefault(uint64_t index) {
  std::string_view empty[2] = {};
  return empty[index];
}
std::string_view place(std::string_view text, uint64_t at) {
  mut std::string_view slots[2] = {"first", "second"};
  slots[at] = text;
  return slots[at];
}
uint64_t stamp(std::string_view text, uint64_t at) {
  std::string_view copy = text;
  mut std::string_view slots[2] = {"stored", "spare"};
  slots[at] = "stamped";
  return slots[at].size() + copy.size();
}
std::string_view labelled(std::string_view prefix) {
  auto label = [](std::string_view unused) -> std::string_view {
    std::string_view copy = unused;
    return "label";
  };
  std::string_view labels[2] = {label(prefix), "other"};
  return labels[0];
}
std::string_view throughReferences() {
  mut std::string_view words[2] = {"a", "bc"};
  for (mut std::string_view& word : words) {
    word = "q";
  }
  std::string_view& last = words[1];
  return last == "q" ? words[0] : "missed";
}
uint64_t guard() {
  std::string owner{"dynamic"};
  std::string_view view = owner.view();
  std::string_view copy = view;
  mut std::string_view slots[2] = {"first", "second"};
  slots[0] = "guarded";
  return slots[0].size() + copy.size();
}
)gti";

const std::string kCapturedTextEscapes = R"gti(
import std.string;
import std.functional;
struct Note {
  std::string_view text;
  int line;
};
class Token {
public:
  std::string_view text;
  int n;
  Token(std::string_view text, int n) : text(text), n(n) {}
};
class Label {
public:
  mut std::string_view text;
  Label() : text("") {}
  void set(std::string_view text) mut { this.text = text; }
};
enum class Message { text(std::string_view value), none };
mut std::string_view kept = "";
void keep(std::string_view text) { kept = text; }
std::string_view same(std::string_view text) { return text; }
Token make(std::string_view text) { return Token(text, 1); }
std::string_view call<F>(F view) { return view(); }
)gti";

// Each `marker`'s last `anchor` starts a GTI-S2045 naming `reason`.
struct CapturedTextEscape {
  std::string_view marker;
  std::string_view anchor;
  std::string_view reason;
};

// A virtual call's text result is its root's contract, and an override keeps
// it; a lambda's result is what its returns hold; a generic method whose
// result its instance makes text relays at each call as the instance does.
// So an erased callable over a literal lambda returns ownerless text through
// a user-defined interface and through std::function, and MIR verifies it.
const std::string kErasedTextResults = R"gti(
import std.functional;
import std.polymorphic;
import std.string;
interface Labeler {
  std::string_view label();
};
class Fixed : public Labeler {
public:
  Fixed() {}
  std::string_view label() override { return "fixed"; }
};
class Wrapped<F> : public Labeler {
  F fn;
public:
  Wrapped(F fn) : fn(fn) {}
  std::string_view label() override { return this.fn(); }
};
class Erased {
  mut std::inline_polymorphic<Labeler, 32, 8> target{};
public:
  Erased() {}
  void set<F>(F fn) mut { this.target.replace(Wrapped<F>(fn)); }
  std::string_view operator()() { return this.target.get().label(); }
};
class Echo<R> {
public:
  int n;
  Echo() : n(0) {}
  R same(R text) { return text; }
  R operator()(R text) { return text; }
};
mut std::string_view kept = "";
)gti";

// An override's text result keeps its root's contract in the override's own
// instance of the root's class: under a generic root that relays its
// argument, each override relays that argument or returns ownerless text,
// and each call keeps the argument's owner borrowed (L-OWN-02).
const std::string kGenericVirtualRoot = R"gti(
import std.string;
class Picker<T> {
public:
  int n;
  Picker() : n(0) {}
  virtual T pick(T a) { return a; }
};
class Literal : public Picker<std::string_view> {
public:
  Literal() : Picker<std::string_view>() {}
  std::string_view pick(std::string_view a) override { return "literal"; }
};
class Same<T> : public Picker<T> {
public:
  Same() : Picker<T>() {}
  T pick(T a) override { return a; }
};
)gti";

// An overloaded operator's text result relays its receiver or an argument
// as its summary says, as a method's does (L-OWN-02): the relayed owner stays
// borrowed while the result is used, and an argument holding ownerless text
// relays nothing.
const std::string kOperatorTextRelays = R"gti(
import std.string;
class Same {
public:
  Same() {}
  std::string_view operator()(std::string_view s) { return s; }
  std::string_view operator[](std::string_view s) { return s; }
};
class Plain {
  mut std::string s;
public:
  Plain() : s("plain") {}
  std::string_view operator()() { return this.s.view(); }
  void grow() mut { this.s.append("!"); }
};
class Echo<R> {
public:
  int n;
  Echo() : n(0) {}
  R operator()(R text) { return text; }
};
)gti";

// A view field a base class declares is the derived value's field too. A
// class with a base keeps its text fields ownerless, so a derived method,
// virtual or not, returns an inherited view field as ownerless text, as a
// root's own method returns its field (L-OWN-02). A derived constructor that
// passes a formal to a base constructor storing it therefore requires
// ownerless text there, at every construction and through a function that
// forwards to it; MIR derives that requirement independently.
const std::string kInheritedTextFields = R"gti(
import std.string;
class Base {
public:
  std::string_view text;
  Base(std::string_view text) : text(text) {}
  virtual std::string_view name() { return "base"; }
};
class Derived : public Base {
public:
  Derived(std::string_view text) : Base(text) {}
  std::string_view name() override { return this.text; }
  std::string_view plain() { return this.text; }
};
class Holder<T> {
public:
  std::string_view text;
  T value;
  Holder(std::string_view text, T value) : text(text), value(value) {}
  virtual std::string_view name() { return this.text; }
};
class Special<T> : public Holder<T> {
public:
  Special(T value) : Holder<T>("special", value) {}
  std::string_view name() override { return this.text; }
};
Derived make(std::string_view text) { return Derived(text); }
)gti";

void testMultipleTextOwners() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
struct Labels<lifetime a, lifetime b> {
  std::string_view<a> first;
  std::string_view<b> second;
  Labels(std::string_view<a> x, std::string_view<b> y):first(x),second(y){}
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  Labels labels{first.view(), second.view()};
  auto copy = labels;
  auto moved = std::move(labels);
  return copy.first == "first" && copy.second == "second" &&
         moved.first == "first" && moved.second == "second" ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a shared aggregate retains two text owners");
  if (!result.canGenerateCode())
    return;
  bool tested = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.dependentTextPublication ||
            instruction.borrowFields.size() != 2 || !instruction.loan)
          continue;
        tested = true;
        for (int mutation = 0; mutation < 6; ++mutation) {
          auto changed = result.mir;
          auto &body = const_cast<lang::MirFunctionInstance *>(
                           changed.findFunctionInstance(function.id))
                           ->body;
          auto &instructions = body.findBlock(block.id)->instructions;
          auto *altered = &*std::find_if(
              instructions.begin(), instructions.end(),
              [&](const auto &item) { return item.id == instruction.id; });
          auto *loan =
              const_cast<lang::MirLoan *>(body.findLoan(*instruction.loan));
          if (mutation == 0)
            altered->borrowFields.pop_back();
          else if (mutation == 1)
            altered->borrowFields.back().parameter =
                altered->borrowFields.front().parameter;
          else if (mutation == 2)
            altered->borrowFields.back().resultFields =
                altered->borrowFields.front().resultFields;
          else if (mutation == 3)
            loan->additionalDependencies.clear();
          else if (mutation == 4) {
            expect(!loan->additionalDependencies.empty(),
                   "second owner is retained");
            if (loan->additionalDependencies.empty())
              continue;
            loan->additionalDependencies.front().source = loan->source;
          } else {
            expect(!loan->additionalDependencies.empty(),
                   "second field is retained");
            if (loan->additionalDependencies.empty())
              continue;
            loan->additionalDependencies.front().designatedFields =
                loan->designatedFields;
          }
          expect(!lang::verifyMirProgram(changed).valid(),
                 "every published field and its actual owner must be verified");
        }
      }
    }
  }
  expect(tested,
         "two-owner publication is represented by a verified loan group");
}

void testCarrierFlowOnLoweredProgram() {
  const auto result = analyzeText(R"gti(
int main() {
  mut int owner = 4;
  int& view = owner;
  int independent = view;
  owner = 5;
  return independent - 4;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a scalar copy releases its input borrow before a write");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr && !main->body.loans.empty(),
         "the source regression contains a real loan");
  if (!main)
    return;
  const auto flow = lang::deriveMirCarrierFlow(main->body);
  expect(flow.complete,
         "ordinary lowered reference/copy/store flow is complete");
  expect(std::any_of(flow.reads.begin(), flow.reads.end(),
                     [](const auto &item) {
                       return item.second && !item.second->empty();
                     }),
         "lowered uses are associated with their actual loan groups");
}

void testReplacementDependencyVerification() {
  const auto result = analyzeText(R"gti(
import std.string;
int main() {
  std::string owner{"text"};
  mut std::string_view view = owner.view();
  view = "literal";
  return int(view.size());
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "ownerless replacement is a valid control");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "replacement control has a lowered body");
  if (!main)
    return;
  lang::MirLoanId dependency = 0;
  lang::MirOperand borrowed;
  for (const auto &block : main->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::StringView) {
        dependency = *instruction.loan;
        borrowed = {.kind = lang::MirOperandKind::Value,
                    .value = *instruction.result,
                    .type = instruction.info.type};
      }
  expect(dependency != 0, "the control has an external view dependency");
  if (!dependency)
    return;
  auto changed = main->body;
  bool replaced = false;
  for (auto &block : changed.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Assign &&
          instruction.operands.size() == 1 && instruction.destination &&
          changed.findPlace(*instruction.destination)->type.kind ==
              lang::SemanticType::StringView) {
        instruction.operands.front() = borrowed;
        replaced = true;
      }
  expect(replaced, "the mutation replaces the actual view store's input");
  if (!replaced)
    return;
  // Move the dependency's endpoint to the edited store. The valid control
  // may already retire it before assigning the literal; leaving that endpoint
  // would test a duplicate EndBorrow instead of the forged value reuse.
  for (auto &block : changed.blocks)
    std::erase_if(block.instructions, [&](const auto &instruction) {
      return instruction.kind == lang::MirInstructionKind::EndBorrow &&
             instruction.loan == dependency;
    });
  lang::MirInstructionId next = 1;
  for (const auto &block : changed.blocks)
    for (const auto &instruction : block.instructions)
      next = std::max(next, instruction.id + 1);
  bool endedBeforeStore = false;
  for (auto &block : changed.blocks) {
    const auto store = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const auto &instruction) {
          return instruction.kind == lang::MirInstructionKind::Assign &&
                 instruction.operands.size() == 1 &&
                 instruction.operands.front().kind ==
                     lang::MirOperandKind::Value &&
                 instruction.operands.front().value == borrowed.value;
        });
    if (store == block.instructions.end())
      continue;
    block.instructions.insert(store,
                              {.id = next++,
                               .kind = lang::MirInstructionKind::EndBorrow,
                               .loan = dependency});
    endedBeforeStore = true;
  }
  expect(endedBeforeStore,
         "the mutation explicitly ends its dependency before the store");
  if (!endedBeforeStore)
    return;
  expect(lang::rebuildMirValueUses(changed),
         "the edited store has coherent operand-use indexing");
  // Reusing its SSA value must not resurrect it, even if the carrier-list
  // annotation is erased. Inspect the specific rejection, not just failure.
  const_cast<lang::MirLoan *>(changed.findLoan(dependency))->carriers.clear();
  // Reconstruct the source-defined string constructor and accessor effects;
  // a body-only query intentionally treats those retained contents as unknown.
  const auto checked =
      lang::verifyMirBody(changed, main->id, nullptr, &result.mir);
  const bool rejected = std::any_of(
      checked.errors.begin(), checked.errors.end(), [](const auto &error) {
        return error.borrow.kind ==
                   lang::MirBorrowRejectionKind::ExpiredDependency &&
               error.message.find(
                   "stored value uses unavailable dependency loan") !=
                   std::string::npos;
      });
  if (!rejected) {
    const auto *record = changed.findLoan(dependency);
    std::cerr << "replacement dependency " << dependency
              << " kind=" << int(record->kind) << " parent=" << record->parent
              << " stored=" << record->storedDependency
              << " entry=" << record->entry << " escapes=" << record->escapes
              << " lifecycle=" << record->lifecycleBound << '\n';
    for (const auto &error : checked.errors)
      std::cerr << "verification: " << error.message << '\n';
    const auto flow = lang::deriveMirCarrierFlow(changed, &result.mir);
    for (const auto &block : changed.blocks)
      for (const auto &instruction : block.instructions) {
        const auto reads = flow.reads.find(instruction.id);
        if ((instruction.loan && *instruction.loan == dependency) ||
            (reads != flow.reads.end() && reads->second &&
             std::find(reads->second->begin(), reads->second->end(),
                       dependency) != reads->second->end()))
          std::cerr << "dependency site block=" << block.id
                    << " instruction=" << instruction.id
                    << " kind=" << int(instruction.kind) << '\n';
      }
  }
  expect(rejected, "actual stored-value flow rejects a retired dependency "
                   "without carrier metadata");
}

void testExplicitEmptyCarrierProof() {
  const auto result = analyzeText(R"gti(
enum Reading<lifetime a> { empty, value(int&<a> item) };
Reading<static> empty_reading() {
  auto first = Reading<static>::empty;
  auto copied = first;
  return copied;
}
int main() {
  auto reading = empty_reading();
  return 0;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a copied explicitly empty alternative remains publishable without a "
         "loan");
}

void testArrayResultLiteralReturnProof() {
  const auto result = analyzeText(R"gti(
std::string_view<a>[2] paired<lifetime a>(std::string_view<a> first,
                                        std::string_view<a> second) {
  return {"first", "second"};
}
int main() { auto values = paired("one", "two"); return values[0] == "first" ? 0 : 1; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "a declared array lifetime contract permits "
                                   "an actually ownerless result");
  if (!result.canGenerateCode())
    return;
  const auto *function = namedFunction(result, "paired");
  expect(function != nullptr, "the array-return function reaches MIR");
  if (!function)
    return;
  auto changed = result.mir;
  auto *forged = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(function->id));
  forged->returnBorrowParameters = {0, 0};
  expect(!lang::verifyMirProgram(changed).valid(),
         "duplicate array-return suppliers are not a valid return contract");
}

void testInlineArrayCallSupplierSet() {
  for (const auto &arguments : {std::string{"first.view(), second.view()"},
                                std::string{"\"first\", second.view()"},
                                std::string{"first.view(), \"second\""}}) {
    const auto result = analyzeText(R"gti(
import std.string;
std::string_view<a>[2] paired<lifetime a>(std::string_view<a> first,
                                        std::string_view<a> second) {
  return {"first", "second"};
}
int main() {
  std::string first{"first"}; std::string second{"second"};
  auto values = paired()gti" + arguments +
                                    R"gti();
  return values[0] == "first" ? 0 : 1;
}
)gti");
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(result.canGenerateCode(),
           "array return contracts retain actual suppliers including an "
           "ownerless primary");
    if (!result.canGenerateCode())
      continue;
    const auto *callee = namedFunction(result, "paired");
    const auto *caller = namedFunction(result, "main");
    expect(callee && caller, "the supplier-set call reaches MIR");
    if (!callee || !caller)
      continue;
    bool inspected = false;
    for (const auto &block : caller->body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            call.functionTarget != callee->id || !call.loan)
          continue;
        inspected = true;
        auto changed = result.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         changed.findFunctionInstance(caller->id))
                         ->body;
        auto *loan = const_cast<lang::MirLoan *>(body.findLoan(*call.loan));
        expect(loan != nullptr,
               "borrowed actuals produce a result dependency group");
        if (!loan)
          continue;
        if (arguments == "first.view(), second.view()") {
          expect(!loan->additionalDependencies.empty(),
                 "two actual owners survive the declared return contract");
          loan->additionalDependencies.clear();
          expect(!lang::verifyMirProgram(changed).valid(),
                 "the verifier rejects a result group that drops the secondary "
                 "owner");
        }
      }
    expect(inspected, "the declared call publishes its actual owner set");
  }
}

void testReferenceArrayExternalEntry() {
  const auto result = analyzeText(R"gti(
std::string_view<a> select_view<lifetime a, lifetime shell>(
    std::string_view<a>[2]&<shell> values, int index) {
  return values[index];
}
int main() { return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "a reference-array formal separates element "
                                   "owners from physical storage");
  if (!result.canGenerateCode())
    return;
  const auto *function = namedFunction(result, "select_view");
  expect(function != nullptr, "the reference-array function reaches MIR");
  if (!function)
    return;
  bool inspected = false;
  for (const auto &entry : function->body.loans) {
    if (!entry.entry || entry.kind != lang::MirLoanKind::Parameter ||
        !entry.storedDependency)
      continue;
    inspected = true;
    auto changed = result.mir;
    auto &body = const_cast<lang::MirFunctionInstance *>(
                     changed.findFunctionInstance(function->id))
                     ->body;
    auto *external =
        const_cast<lang::MirLoan *>(body.findLoan(entry.storedDependency));
    expect(external != nullptr,
           "the array reference has an external dependency witness");
    if (!external)
      continue;
    external->access = lang::AccessMode::Mutable;
    expect(!lang::verifyMirProgram(changed).valid(),
           "an array of shared views cannot manufacture mutable external "
           "authority");
  }
  expect(inspected,
         "the reference-array formal has a distinct data dependency");
}

void testExpiredStoredReturn() {
  using namespace lang;
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.entry = 1;
  body.places = {{.id = 1,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = SemanticType::Int32,
                  .access = AccessMode::Mutable,
                  .initiallyAvailable = true},
                 {.id = 2,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 2,
                  .type = SemanticType::StringView,
                  .access = AccessMode::Mutable,
                  .traits = {.containsBorrowedState = true}}};
  body.loans = {{.id = 1, .kind = MirLoanKind::Stored, .source = 1}};
  body.blocks = {
      {.id = 1,
       .instructions =
           {{.id = 1,
             .kind = MirInstructionKind::Borrow,
             .operands = {{.kind = MirOperandKind::BorrowRead, .place = 1}},
             .loan = 1},
            {.id = 2,
             .kind = MirInstructionKind::Initialize,
             .destination = 2,
             .operands = {{.kind = MirOperandKind::Loan, .loan = 1}}},
            {.id = 3, .kind = MirInstructionKind::EndBorrow, .loan = 1}},
       .terminator = {.kind = MirTerminatorKind::Return,
                      .value = MirOperand{.kind = MirOperandKind::Copy,
                                          .place = 2,
                                          .type = SemanticType::StringView}}}};
  const auto verify = [](const MirBody &candidate) {
    return verifyMirPersistentStorageLoanFlow(candidate, 0, {});
  };
  auto discarded = body;
  discarded.blocks[0].terminator.value.reset();
  expect(
      verify(discarded).valid(),
      "an initialized borrowed carrier may be discarded after its loan ends");
  const auto expired = verify(body);
  expect(std::any_of(expired.errors.begin(), expired.errors.end(),
                     [](const auto &error) {
                       return error.borrow.kind ==
                                  MirBorrowRejectionKind::Escape &&
                              error.borrow.loan == 1;
                     }),
         "returning an expired initialized carrier is an escape without any "
         "replacement store");
  auto ownerless = body;
  ownerless.blocks[0].instructions[1].operands = {
      {.kind = MirOperandKind::Constant, .type = SemanticType::StringView}};
  expect(
      verify(ownerless).valid(),
      "an unrelated ended loan does not prevent returning an ownerless value");
}

void testConditionalStoredValueFlow() {
  // Exercise the canonical loan-flow engine independently of source admission
  // and lowering's binding state. Source acceptance is tested separately;
  // these mutations must reject invalid MIR regardless of its producer.
  using namespace lang;
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.entry = 1;
  body.places = {{.id = 1,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = SemanticType::Int32,
                  .access = AccessMode::Mutable,
                  .initiallyAvailable = true},
                 {.id = 2,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 2,
                  .type = SemanticType::StringView,
                  .access = AccessMode::Mutable,
                  .traits = {.containsBorrowedState = true},
                  .initiallyAvailable = true}};
  body.loans = {
      {.id = 1, .kind = MirLoanKind::Stored, .source = 1, .carriers = {2}}};
  body.blocks = {
      {.id = 1,
       .instructions = {{.id = 1,
                         .kind = MirInstructionKind::Initialize,
                         .destination = 2,
                         .operands = {{.kind = MirOperandKind::Constant}}}},
       .terminator = {.kind = MirTerminatorKind::Branch,
                      .target = 2,
                      .elseTarget = 3}},
      {.id = 2,
       .instructions =
           {{.id = 2,
             .kind = MirInstructionKind::Borrow,
             .operands = {{.kind = MirOperandKind::BorrowRead, .place = 1}},
             .loan = 1},
            {.id = 3,
             .kind = MirInstructionKind::Assign,
             .destination = 2,
             .operands = {{.kind = MirOperandKind::Loan, .loan = 1}}}},
       .terminator = {.kind = MirTerminatorKind::Goto, .target = 4}},
      {.id = 3, .terminator = {.kind = MirTerminatorKind::Goto, .target = 4}},
      {.id = 4,
       .instructions =
           {{.id = 4,
             .kind = MirInstructionKind::Load,
             .operands = {{.kind = MirOperandKind::Copy, .place = 2}}},
            {.id = 5, .kind = MirInstructionKind::EndBorrow, .loan = 1}},
       .terminator = {.kind = MirTerminatorKind::Return}}};
  const auto verify = [](const MirBody &candidate) {
    return verifyMirPersistentStorageLoanFlow(candidate, 0, {});
  };
  const auto checked = verify(body);
  for (const auto &error : checked.errors)
    std::cerr << "conditional stored control: " << error.message << '\n';
  expect(checked.valid(), "a valid carried value supports ordinary reads after "
                          "a conditional store");
  auto twoOwners = body;
  auto otherOwner = twoOwners.places.front();
  otherOwner.id = 3;
  otherOwner.binding = 3;
  twoOwners.places.push_back(otherOwner);
  twoOwners.loans.push_back(
      {.id = 2, .kind = MirLoanKind::Stored, .source = 3, .carriers = {2}});
  twoOwners.blocks[2].instructions = {
      {.id = 6,
       .kind = MirInstructionKind::Borrow,
       .operands = {{.kind = MirOperandKind::BorrowRead, .place = 3}},
       .loan = 2},
      {.id = 7,
       .kind = MirInstructionKind::Assign,
       .destination = 2,
       .operands = {{.kind = MirOperandKind::Loan, .loan = 2}}}};
  twoOwners.blocks[3].instructions.push_back(
      {.id = 8, .kind = MirInstructionKind::EndBorrow, .loan = 2});
  expect(verify(twoOwners).valid(),
         "a branch join preserves a value supplied by either of two owners");
  for (const MirPlaceId owner : {MirPlaceId{1}, MirPlaceId{3}}) {
    auto invalid = twoOwners;
    invalid.blocks[3].instructions.insert(
        invalid.blocks[3].instructions.begin(),
        {.id = 9,
         .kind = MirInstructionKind::Assign,
         .destination = owner,
         .operands = {{.kind = MirOperandKind::Constant}}});
    expect(!verify(invalid).valid(),
           "each potential owner remains protected after a two-owner join");
  }
  for (auto &loan : twoOwners.loans)
    loan.carriers.clear();
  expect(verify(twoOwners).valid(),
         "two-owner join proof is reconstructed without carrier annotations");
  auto loop = body;
  loop.blocks[1].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 4};
  const auto repeated = verify(loop);
  for (const auto &error : repeated.errors)
    std::cerr << "repeated shared store: " << error.message << '\n';
  expect(repeated.valid(),
         "one shared stored producer may execute repeatedly on a backedge");
  auto duplicated = loop;
  auto secondProducer = duplicated.blocks[1].instructions.front();
  secondProducer.id = 6;
  duplicated.blocks[1].instructions.insert(
      duplicated.blocks[1].instructions.begin() + 1, secondProducer);
  expect(!verify(duplicated).valid(),
         "distinct producers cannot reuse a live stored group identifier");
  auto expiredCopy = loop;
  auto copySlot = expiredCopy.places[1];
  copySlot.id = 3;
  copySlot.binding = 3;
  expiredCopy.places.push_back(copySlot);
  expiredCopy.blocks[0].instructions.push_back(
      {.id = 6,
       .kind = MirInstructionKind::Initialize,
       .destination = 3,
       .operands = {{.kind = MirOperandKind::Constant}}});
  expiredCopy.blocks[1].instructions.insert(
      expiredCopy.blocks[1].instructions.begin() + 1,
      {.id = 7,
       .kind = MirInstructionKind::Load,
       .operands = {{.kind = MirOperandKind::Copy, .place = 3}}});
  expiredCopy.blocks[1].instructions.push_back(
      {.id = 8,
       .kind = MirInstructionKind::Assign,
       .destination = 3,
       .operands = {{.kind = MirOperandKind::Copy, .place = 2}}});
  expiredCopy.blocks[1].instructions.push_back(
      {.id = 9, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  expiredCopy.blocks[3].instructions.clear();
  const auto expired = verify(expiredCopy);
  expect(
      std::any_of(
          expired.errors.begin(), expired.errors.end(),
          [](const auto &error) {
            return error.borrow.kind ==
                       lang::MirBorrowRejectionKind::ExpiredDependency &&
                   error.borrow.loan == 1 &&
                   error.message.find(
                       "stored value uses unavailable dependency loan 1") !=
                       std::string::npos;
          }),
      "a new iteration cannot revive a copy expired on the previous iteration");
  auto exclusiveLoop = loop;
  exclusiveLoop.loans[0].access = AccessMode::Mutable;
  exclusiveLoop.blocks[1].instructions[0].operands[0].kind =
      MirOperandKind::BorrowWrite;
  expect(!verify(exclusiveLoop).valid(),
         "repeated shared production does not grant exclusive reactivation");
  auto changed = body;
  changed.loans[0].carriers.clear();
  expect(verify(changed).valid(),
         "actual stored-value proof does not depend on carrier annotations");
  changed = body;
  changed.loans.push_back({.id = 2,
                           .storedDependency = 1,
                           .kind = MirLoanKind::CallResult,
                           .source = 2});
  changed.blocks[3].instructions[0].kind = MirInstructionKind::Call;
  changed.blocks[3].instructions[0].loan = 2;
  changed.blocks[3].instructions.insert(
      changed.blocks[3].instructions.begin() + 1,
      {.id = 6, .kind = MirInstructionKind::EndBorrow, .loan = 2});
  const auto publication = verify(changed);
  expect(std::any_of(publication.errors.begin(), publication.errors.end(),
                     [](const auto &error) {
                       return error.message.find(
                                  "requires active stored dependency loan 1") !=
                              std::string::npos;
                     }),
         "ordinary value proof does not authorize incomplete borrowed-result "
         "publication");
  changed = body;
  changed.blocks[3].instructions[0].operands = {
      {.kind = MirOperandKind::Loan, .loan = 1}};
  expect(!verify(changed).valid(),
         "a naked loan ID is not created on the ownerless predecessor");
  changed = body;
  std::swap(changed.blocks[3].instructions[0],
            changed.blocks[3].instructions[1]);
  expect(
      !verify(changed).valid(),
      "ending the conditional dependency before its carried read is rejected");
  changed = body;
  changed.blocks[3].instructions.erase(changed.blocks[3].instructions.begin() +
                                       1);
  expect(!verify(changed).valid(),
         "conditional dependencies still require an endpoint before body exit");
  changed = body;
  changed.blocks[3].instructions.insert(
      changed.blocks[3].instructions.begin(),
      {.id = 6,
       .kind = MirInstructionKind::Assign,
       .destination = 1,
       .operands = {{.kind = MirOperandKind::Constant}}});
  expect(!verify(changed).valid(),
         "a conditionally retained owner is protected against mutation");
  changed = body;
  changed.loans[0].access = AccessMode::Mutable;
  changed.blocks[1].instructions[0].operands[0].kind =
      MirOperandKind::BorrowWrite;
  expect(
      !verify(changed).valid(),
      "conditional value flow does not grant exclusive-loan join permission");
}

void testInferredArrayReturnSuppliers() {
  const auto result = analyzeText(R"gti(
import std.string;
std::string_view pick(std::string_view first[2], std::string_view second[2]) {
  return first[0];
}
int main() {
  std::string_view first[2] = {"first", "other"};
  std::string_view second[2] = {"second", "other"};
  return pick(first, second) == "first" ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "array element return infers its supplying parameter");
  if (!result.canGenerateCode())
    return;
  const auto *pick = namedFunction(result, "pick");
  expect(pick && pick->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             pick->returnBorrowParameter == 0,
         "array return identifies the first supplier rather than its "
         "same-typed neighbor");
  if (!pick)
    return;
  auto changed = result.mir;
  auto *forged = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(pick->id));
  forged->returnBorrowParameter = 1;
  forged->returnBorrowParameters = {1};
  expect(functionError(lang::verifyMirProgram(changed), pick->id,
                       "inline text array returns a dependency outside its "
                       "declared suppliers"),
         "an inferred array return cannot substitute a same-typed unrelated "
         "supplier");
}

void testArrayOwnerlessPublicationContract() {
  const auto result = analyzeText(R"gti(
import std.string;
mut std::string_view published = "";
void publish(std::string_view values[2]) { published = values[0]; }
int main() {
  std::string_view values[2] = {"first", "second"};
  publish(values);
  return published == "first" ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "literal array contents can reach static storage");
  if (!result.canGenerateCode())
    return;
  const auto *publisher = namedFunction(result, "publish");
  expect(publisher &&
             publisher->ownerlessTextParameters == std::vector<std::size_t>{0},
         "publishing an array element requires ownerless contents from its "
         "formal");
  if (!publisher)
    return;
  auto changed = result.mir;
  const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(publisher->id))
      ->ownerlessTextParameters.clear();
  expect(
      functionError(lang::verifyMirProgram(changed), publisher->id,
                    "ownerless text input contract differs"),
      "an array publisher cannot erase its independently derived requirement");
}

void testBorrowedArrayProjectionProof() {
  const auto result = analyzeText(R"gti(
import std.string;
int main() {
  std::string first{"first"}; std::string second{"second"};
  std::string_view views[2] = {first.view(), second.view()};
  return views[0] == "first" && views[1] == "second" ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "borrowed array literals retain all element owners");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "borrowed array proof has a main body");
  if (!main)
    return;
  auto changed = main->body;
  for (auto &loan : changed.loans)
    loan.carriers.clear();
  std::size_t checked = 0;
  for (const auto &place : changed.places) {
    if (place.type.kind != lang::SemanticType::StringView ||
        place.root != lang::MirPlaceRootKind::Binding ||
        place.projections.empty() ||
        !std::all_of(place.projections.begin(), place.projections.end(),
                     [](const auto &part) {
                       return part.kind == lang::MirProjectionKind::Index;
                     }))
      continue;
    ++checked;
    expect(!lang::mir_detail::isOwnerlessTextStorageProjection(result.mir,
                                                               changed, place),
           "actual array stores prevent forged ownerless reads without carrier "
           "annotations");
  }
  expect(checked >= 2,
         "both indexed borrowed elements have independent projection checks");
}

void testGenericArrayReceiverRetention() {
  const auto result = analyzeText(R"gti(
struct Entries<T, uint64_t Capacity> {
  mut T values[Capacity];
  mut uint64_t used;
  Entries():values({}),used(0){}
  bool insert(T value) mut {
    if (this.used == Capacity) { return false; }
    this.values[this.used] = value;
    this.used += 1;
    return true;
  }
  bool forward(T value) mut { return this.insert(value); }
};
int main() {
  mut Entries<std::string_view, 3> values{};
  if (!values.insert("first") || !values.forward("second")) { return 1; }
  mut Entries<int, 3> integers{};
  return integers.insert(1) && integers.forward(2) ? 0 : 2;
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || info->qualifiedName.find("Entries::") != 0)
        continue;
      std::cerr << "generic method " << function.id << ' '
                << info->qualifiedName << " declared ownerless inputs:";
      for (const auto index : function.ownerlessTextParameters)
        std::cerr << ' ' << index;
      std::cerr << "\n";
    }
  }
  expect(result.canGenerateCode(),
         "generic inline storage accepts its ownerless control");
  if (!result.canGenerateCode())
    return;
  for (const auto name : {"Entries::insert", "Entries::forward"}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr,
           "the instantiated generic storage method reaches MIR");
    if (!function)
      continue;
    const auto effects =
        lang::deriveMirRetainingStoreEffects(*function, &result.mir);
    expect(effects.complete && effects.effects.size() == 1 &&
               effects.effects.front().destination ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::This, 0} &&
               effects.effects.front().source ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                        function->parameterBindings.front()},
           "conditional indexed writes and forwarding retain their argument in "
           "the receiver");
  }
}

void testGenericFieldReceiverRetention() {
  const auto result = analyzeText(R"gti(
struct Cell<T> {
  mut T value;
  Cell(T initial):value(initial){}
  void put(T input) mut { this.value = input; }
  bool forward(T input) mut { this.put(input); return true; }
};
int main() {
  mut Cell<std::string_view> text{"initial"};
  if (!text.forward("stored")) { return 1; }
  mut Cell<int> number{1};
  return number.forward(2) && number.value == 2 ? 0 : 2;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "generic field forwarding preserves conditional contracts without "
         "constraining integers");
  if (!result.canGenerateCode())
    return;
  bool textSeen = false;
  bool integerSeen = false;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (!info || info->qualifiedName != "Cell::forward" ||
        function.parameterTypes.size() != 1)
      continue;
    const auto effects =
        lang::deriveMirRetainingStoreEffects(function, &result.mir);
    if (function.parameterTypes.front().kind ==
        lang::SemanticType::StringView) {
      textSeen = true;
      expect(effects.complete && effects.effects.size() == 1,
             "generic text field forwarding retains its input");
    } else if (function.parameterTypes.front().kind ==
               lang::SemanticType::Int32) {
      integerSeen = true;
      expect(effects.complete && effects.effects.empty() &&
                 function.ownerlessTextParameters.empty(),
             "the concrete integer field has no text or retention obligation");
    }
  }
  expect(textSeen && integerSeen,
         "both generic field specializations were checked");
}

void testGenericArrayElementReturnContract() {
  const auto result =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
          .analyze("generic-array-element-return.gti", R"gti(
import std.string;
struct Entries<T, uint64_t Capacity> {
  mut T values[Capacity];
  Entries():values({}){}
  T at(uint64_t index) { return this.values[index]; }
};
int main() {
  Entries<std::string_view, 3> text{};
  Entries<int, 3> numbers{};
  return text.at(0) == "" && numbers.at(0) == 0 ? 0 : 1;
}
)gti",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.semanticValid || !result.hirValid)
    printDiagnostics(result);
  expect(result.semanticValid && result.hirValid,
         "the generic array getter reaches checked HIR");
  bool text = false;
  bool scalar = false;
  for (const auto &function : result.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "at")
      continue;
    if (function.returnType.kind == lang::SemanticType::StringView) {
      text = true;
      expect(function.returnBorrowOrigin == lang::BorrowOriginKind::Receiver &&
                 function.returnBorrowFields.size() == 1 &&
                 function.returnBorrowFields.front().resultFields.empty() &&
                 function.returnBorrowFields.front().sourceFields.size() == 1,
             "an indexed text getter exports the stored field's external "
             "dependency");
    } else if (function.returnType.kind == lang::SemanticType::Int32) {
      scalar = true;
      expect(function.returnBorrowOrigin == lang::BorrowOriginKind::None &&
                 function.returnBorrowFields.empty(),
             "an ordinary scalar element does not acquire a text lifetime "
             "contract");
    }
  }
  expect(text && scalar,
         "both concrete generic getter specializations were inspected");
}

void testGenericArrayElementReturnProof() {
  const auto result = analyzeText(R"gti(
import std.string;
struct Entries<T, uint64_t Capacity> {
  mut T values[Capacity];
  Entries():values({}){}
  T at(uint64_t index) { return this.values[index]; }
};
int main() {
  Entries<std::string_view, 3> text{};
  Entries<int, 3> numbers{};
  return text.at(0) == "" && numbers.at(0) == 0 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "the generic text-array getter has an independently verified return "
         "proof");
  if (!result.canGenerateCode())
    return;
  bool inspected = false;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::StringView ||
        function.returnBorrowFields.empty())
      continue;
    // Find the actual array-field relation rather than depending on a generic
    // specialization's generated name or ordinal.
    bool arrayField = false;
    for (const auto &relation : function.returnBorrowFields)
      for (const auto field : relation.sourceFields)
        for (const auto &record : result.mir.classInstances())
          for (const auto &member : record.declaredFields)
            if (member.symbol == field &&
                member.type.kind == lang::SemanticType::Array)
              arrayField = true;
    if (!arrayField)
      continue;
    inspected = true;
    auto changed = result.mir;
    auto *getter = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id));
    getter->returnBorrowOrigin = lang::BorrowOriginKind::None;
    getter->returnBorrowFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "an indexed getter cannot erase its external stored-text return "
           "relation");
  }
  expect(inspected,
         "the array getter's return relation was independently challenged");
}

void testRetainingStoreContractSeparation() {
  const auto result = analyzeText(R"gti(
import std.string;
mut std::string_view persistent = "";
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void retain(std::string_view input) mut { this.value = input; }
  void retain_and_escape(std::string_view input) mut {
    this.value = input;
    persistent = input;
  }
};
int main() {
  mut Holder holder{};
  holder.retain("first");
  holder.retain_and_escape("second");
  return 0;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "the store-contract controls compile with literal inputs");
  if (!result.canGenerateCode())
    return;
  for (const auto &[name, permitted] :
       {std::pair{"Holder::retain", true},
        std::pair{"Holder::retain_and_escape", false}}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr, "the store-contract method reaches MIR");
    if (!function)
      continue;
    auto changed = result.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function->id));
    target->ownerlessTextParameters.clear();
    expect(lang::verifyMirProgram(changed).valid() == permitted,
           permitted ? "an independently modeled retaining write can discharge "
                       "its own ownerless requirement"
                     : "a retaining write cannot discharge a separate "
                       "persistent escape of the same input");
  }
}

void testRetainingNamedCallableEscape() {
  const std::string declarations = R"gti(
import std.string;
import std.functional;
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void put(std::string_view input) mut { this.value = input; }
};
void retain(mut Holder& target, std::string_view input) { target.put(input); }
void literal(mut Holder& target, std::string_view input) { target.put("literal"); }
)gti";
  const auto control = analyzeText(declarations + R"gti(
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{literal};
  mut Holder holder{};
  std::string owner{"short"}; store(holder, owner.view());
  return holder.value == "literal" ? 0 : 1;
}
)gti");
  if (!control.canGenerateCode())
    printDiagnostics(control);
  expect(control.canGenerateCode(), "a named callable storing only literal "
                                    "text has no input-retention obligation");
  const auto direct = analyzeText(declarations + R"gti(
int main() {
  std::string owner{"direct"};
  mut Holder holder{}; retain(holder, owner.view());
  return holder.value == "direct" ? 0 : 1;
}
)gti");
  if (!direct.canGenerateCode())
    printDiagnostics(direct);
  expect(direct.canGenerateCode(),
         "direct calls preserve a named function's retaining-input relation");
  const auto escape = analyzeText(declarations + R"gti(
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{retain};
  mut Holder holder{};
  { std::string owner{"short"}; store(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const bool safe = rejectedAtSource(escape, "GTI-S2045");
  if (!safe) {
    std::cerr << "named retaining callable escape code generation: "
              << escape.canGenerateCode() << '\n';
    printDiagnostics(escape);
  }
  expect(safe,
         "a named function pointer cannot erase an input-retention obligation");
  const auto uncertain = analyzeText(declarations + R"gti(
extern "C" { void opaque_effect(); }
void retain_opaque(mut Holder& target, std::string_view input) {
  target.put(input);
  opaque_effect();
}
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{retain_opaque};
  mut Holder holder{};
  { std::string owner{"short"}; store(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const auto *opaque = namedFunction(uncertain, "retain_opaque");
  expect(opaque &&
             !lang::deriveMirRetainingStoreEffects(*opaque, &uncertain.mir)
                  .complete,
         "the opaque-call control reaches MIR with an incomplete retention "
         "summary");
  if (!rejectedAtSource(uncertain, "GTI-S2045"))
    printDiagnostics(uncertain);
  expect(rejectedAtSource(uncertain, "GTI-S2045"),
         "an incomplete effect summary cannot certify retention-free callable "
         "conversion");
}

void testRetainingCallConditionalCleanupProof() {
  const auto result = analyzeText(R"gti(
import std.string;
struct Sink {
  mut std::string_view text;
  Sink():text(""){}
  void set(std::string_view value) mut { this.text = value; }
};
int main() {
  std::string owner{"text"};
  std::string_view view = owner.view();
  mut Sink sink{};
  sink.set("literal");
  return int(view.size());
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "conditional call cleanup has a valid source control");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  const auto *setter = namedFunction(result, "Sink::set");
  expect(main && setter, "the cleanup proof has both source bodies");
  if (!main || !setter)
    return;
  auto body = main->body;
  lang::MirLoanId dependency = 0;
  lang::MirOperand borrowed;
  lang::MirInstructionId next = 1;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      next = std::max(next, instruction.id + 1);
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::StringView) {
        dependency = *instruction.loan;
        borrowed = {.kind = lang::MirOperandKind::Value,
                    .value = *instruction.result,
                    .type = instruction.info.type};
      }
    }
  expect(dependency != 0, "the control produces an external dependency");
  if (!dependency)
    return;
  // Exercise loan-flow verification separately from the still-closed source
  // storage contract. Replace the literal setter input with the real view SSA.
  bool changedInput = false;
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget == setter->id &&
          instruction.operands.size() == 1) {
        auto *input = &instruction.operands.front();
        if (input->kind == lang::MirOperandKind::Value)
          for (auto &inputBlock : body.blocks)
            for (auto &stage : inputBlock.instructions)
              if (stage.result == input->value &&
                  stage.kind == lang::MirInstructionKind::CallInput &&
                  stage.operands.size() == 1)
                input = &stage.operands.front();
        *input = borrowed;
        changedInput = true;
      }
  expect(changedInput, "the setter receives the represented dependency");
  if (!changedInput)
    return;
  const auto proof = [&](const lang::MirBody &candidate,
                         const lang::MirProgram *program) {
    return lang::verifyMirPersistentStorageLoanFlow(
        candidate, main->id, {}, nullptr, nullptr, {}, program);
  };
  expect(proof(body, &result.mir).valid(),
         "the call's actual retained input is live");
  body.findBlock(body.entry)
      ->instructions.insert(body.findBlock(body.entry)->instructions.begin(),
                            {.id = next,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .loan = dependency});
  expect(!proof(body, nullptr).valid(),
         "body-only verification cannot invent a retaining call contract");
  const auto checked = proof(body, &result.mir);
  if (!checked.valid())
    for (const auto &error : checked.errors)
      std::cerr << error.message << '\n';
  expect(checked.valid(),
         "proven call retention permits cleanup before a conditional producer");
  // Retained shared groups may end between lexical drops and their boundary
  // marker. Their endpoints have no runtime action; loan flow must validate
  // them without losing the boundary's exact runtime drop sequence.
  auto cleanup = body;
  bool insertedEndpoint = false;
  for (auto &block : cleanup.blocks) {
    for (auto position = block.instructions.begin();
         position != block.instructions.end(); ++position)
      if (position->cleanupBoundaryEnd &&
          !cleanup.cleanupBoundaries[position->cleanupBoundaryEnd - 1]
               .obligations.empty()) {
        block.instructions.insert(position,
                                  {.id = next + 1,
                                   .kind = lang::MirInstructionKind::EndBorrow,
                                   .loan = dependency});
        insertedEndpoint = true;
        break;
      }
    if (insertedEndpoint)
      break;
  }
  expect(insertedEndpoint,
         "the control has a cleanup boundary with runtime drops");
  expect(lang::rebuildMirValueUses(cleanup),
         "the changed call input's SSA uses are rebuilt");
  const auto cleanupProof =
      lang::verifyMirBody(cleanup, main->id, nullptr, &result.mir);
  if (!cleanupProof.valid())
    for (const auto &error : cleanupProof.errors)
      std::cerr << error.message << '\n';
  expect(cleanupProof.valid(),
         "checked shared endpoints preserve the exact cleanup drop sequence");
  auto nonRetaining = result.mir;
  auto *changedSetter = const_cast<lang::MirFunctionInstance *>(
      nonRetaining.findFunctionInstance(setter->id));
  for (auto &block : changedSetter->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Assign)
        instruction.tombstone = true;
  expect(!proof(body, &nonRetaining).valid(),
         "removing the callee store removes conditional cleanup authority");
}

void testRetainingStoreEffectsOnLoweredProgram() {
  const auto result = analyzeText(R"gti(
import std.string;
struct TextSlot {
  mut std::string_view text;
  TextSlot():text(""){}
  void set(std::string_view value) mut { this.text = value; }
  void forward(std::string_view value) mut { this.set(value); }
  void repeat(std::string_view first, std::string_view second, int depth) mut {
    if (depth > 0) { this.repeat(second, first, depth - 1); }
    this.set(first);
  }
};
int main() {
  mut TextSlot slot{};
  slot.repeat("first", "second", 2);
  slot.forward("earlier");
  slot.set("literal");
  return slot.text == "literal" ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "the existing ownerless setter control compiles");
  if (!result.canGenerateCode())
    return;
  const auto *setter = namedFunction(result, "TextSlot::set");
  expect(setter != nullptr, "the setter has a source MIR body");
  if (!setter)
    return;
  const auto effects = lang::deriveMirRetainingStoreEffects(*setter);
  expect(effects.complete && effects.effects.size() == 1,
         "the setter's retaining effect is reconstructed from its real MIR");
  if (effects.effects.size() == 1)
    expect(effects.effects[0].destination ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::This, 0} &&
               effects.effects[0].source ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                        setter->parameterBindings.front()},
           "the setter stores its input into its receiver");
  const auto *forwarder = namedFunction(result, "TextSlot::forward");
  expect(forwarder != nullptr, "the retaining helper has a source MIR body");
  if (forwarder) {
    const auto composed =
        lang::deriveMirRetainingStoreEffects(*forwarder, &result.mir);
    expect(
        composed.complete && composed.effects.size() == 1 &&
            composed.effects[0].destination ==
                lang::MirCarrierRoot{lang::MirPlaceRootKind::This, 0} &&
            composed.effects[0].source ==
                lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                     forwarder->parameterBindings.front()},
        "a source helper preserves the nested setter's retaining obligation");
  }
  const auto *recursive = namedFunction(result, "TextSlot::repeat");
  expect(recursive != nullptr, "the recursive retaining helper is lowered");
  if (recursive) {
    const auto summary =
        lang::deriveMirRetainingStoreEffects(*recursive, &result.mir);
    expect(summary.complete && summary.effects.size() == 2,
           "recursive source helper converges to both permuted inputs");
    for (std::size_t index = 0; index < 2; ++index)
      expect(std::find(summary.effects.begin(), summary.effects.end(),
                       lang::MirRetainingStoreEffect{
                           {lang::MirPlaceRootKind::This, 0},
                           {lang::MirPlaceRootKind::Binding,
                            recursive->parameterBindings[index]}}) !=
                 summary.effects.end(),
             "recursive source helper preserves each receiver/input relation");
  }
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "the setter has an ordinary source caller");
  if (!main)
    return;
  const auto callerFlow = lang::deriveMirCarrierFlow(main->body, &result.mir);
  bool applied = false;
  for (const auto &block : main->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget == setter->id) {
        const auto targets = callerFlow.storeTargets.find(instruction.id);
        const auto inputs = callerFlow.storeInputs.find(instruction.id);
        applied =
            targets != callerFlow.storeTargets.end() &&
            targets->second.size() == 1 &&
            targets->second.front().kind == lang::MirPlaceRootKind::Binding &&
            inputs != callerFlow.storeInputs.end() && inputs->second &&
            inputs->second->empty();
      }
  expect(applied,
         "the callee effect is substituted into the caller's actual receiver");
  // Exercise the same verified callee with represented dynamic inputs and
  // independently check source provenance before the receiver union.
  lang::MirBody caller;
  caller.entry = 1;
  for (std::size_t id = 1; id <= 4; ++id)
    caller.places.push_back(
        {.id = id, .root = lang::MirPlaceRootKind::Binding, .binding = id});
  caller.loans = {{.id = 1, .kind = lang::MirLoanKind::Stored, .source = 2},
                  {.id = 2, .kind = lang::MirLoanKind::Stored, .source = 3}};
  const lang::MirOperand receiver{.kind = lang::MirOperandKind::Copy,
                                  .place = 1};
  caller.blocks = {
      {.id = 1,
       .instructions =
           {{.id = 1,
             .kind = lang::MirInstructionKind::Initialize,
             .destination = 1,
             .operands = {{.kind = lang::MirOperandKind::Constant}}},
            {.id = 2,
             .kind = lang::MirInstructionKind::Call,
             .receiver = receiver,
             .operands = {{.kind = lang::MirOperandKind::Loan, .loan = 1}},
             .functionTarget = setter->id},
            {.id = 3,
             .kind = lang::MirInstructionKind::Initialize,
             .destination = 4,
             .operands = {receiver}},
            {.id = 4,
             .kind = lang::MirInstructionKind::Call,
             .receiver = receiver,
             .operands = {{.kind = lang::MirOperandKind::Loan, .loan = 2}},
             .functionTarget = setter->id},
            {.id = 5,
             .kind = lang::MirInstructionKind::Load,
             .operands = {receiver}},
            {.id = 6,
             .kind = lang::MirInstructionKind::Load,
             .operands = {{.kind = lang::MirOperandKind::Copy, .place = 4}}}},
       .terminator = {.kind = lang::MirTerminatorKind::Return}}};
  const auto retained = lang::deriveMirCarrierFlow(caller, &result.mir);
  const lang::MirCarrierRoot formalInput{lang::MirPlaceRootKind::Binding,
                                         setter->parameterBindings.front()};
  expect(retained.retainingInputs.at(2).at(formalInput) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1}} &&
             retained.retainingInputs.at(4).at(formalInput) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{2}},
         "retaining calls preserve each actual source proof before receiver "
         "union");
  expect(retained.complete &&
             retained.reads.at(5) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1, 2}} &&
             retained.reads.at(6) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1}},
         "successive retaining calls widen the receiver while copies keep "
         "independent owners");
  caller.blocks[0].instructions.insert(
      caller.blocks[0].instructions.begin() + 1,
      {.id = 7,
       .kind = lang::MirInstructionKind::CallInput,
       .callInputKind = lang::HirCallInputKind::MutableBorrow,
       .result = 1,
       .operands = {receiver}});
  for (auto &instruction : caller.blocks[0].instructions)
    if (instruction.kind == lang::MirInstructionKind::Call)
      instruction.receiver =
          lang::MirOperand{.kind = lang::MirOperandKind::Value, .value = 1};
  const auto staged = lang::deriveMirCarrierFlow(caller, &result.mir);
  expect(staged.complete && staged.reads.at(5) == retained.reads.at(5) &&
             staged.reads.at(6) == retained.reads.at(6) &&
             staged.reads.at(4) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1, 2}},
         "staging a borrowed receiver preserves its physical destination");
  caller.blocks[0].instructions[1].callInputKind =
      lang::HirCallInputKind::CopyValue;
  const auto copiedReceiver = lang::deriveMirCarrierFlow(caller, &result.mir);
  expect(!copiedReceiver.complete && !copiedReceiver.storeTargets.contains(2),
         "an independent receiver copy must not alias the original storage");
}

void testOwnedStorageReferenceArgument() {
  const auto result = analyzeText(R"gti(
import std.memory;
import std.string;
int inspect<T>(T& storage) { return 0; }
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe { std::initialize_owned_at(storage.data(), storage, owner.view()); }
  return inspect(storage);
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "a retained dependency does not replace a "
                                   "storage owner's reference identity");
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "owned storage reference probe has a main body");
  bool checkedShell = false;
  if (main)
    for (const auto &block : main->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputKind == lang::HirCallInputKind::ReadBorrow &&
            instruction.callInputRole == lang::MirCallInputRole::Argument &&
            instruction.callInputIndex == 0 &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind ==
                lang::MirOperandKind::BorrowRead)
          checkedShell = true;
  expect(checkedShell, "reference input borrows the physical owner instead of "
                       "its stored external dependency");
}

void testOwnedStorageBorrowedRecord() {
  const auto result = analyzeText(R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  unsafe {
    Reading* address = storage.data();
    std::initialize_owned_at(address, storage, Reading(owner));
    auto selected = std::borrow_owned_at(address, storage);
    return selected.read() == 42 ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  if (const auto *initializer =
          namedFunction(result, "std::initialize_owned_at")) {
    const auto summary =
        lang::deriveMirRetainingStoreEffects(*initializer, &result.mir);
    expect(summary.complete && !summary.effects.empty(),
           "generated record cleanup preserves the initialization retention "
           "summary");
    // An unmodeled replacement cleanup must not inherit the pure-drop fact.
    auto changed = result.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(initializer->id));
    bool mutated = false;
    for (auto &block : function->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Drop) {
          instruction.kind = lang::MirInstructionKind::CleanupBeforeAssign;
          mutated = true;
        }
    expect(
        mutated &&
            !lang::deriveMirRetainingStoreEffects(*function, &changed).complete,
        "unknown cleanup does not acquire a non-retaining summary");
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
}

void testOwnedStorageReadWrapper() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
unsafe T&<a> read_slot<T, Owner, lifetime a>(const T* address, Owner&<a> owner) {
  unsafe { return std::borrow_owned_at(address, owner); }
}
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe {
    std::string_view* address = storage.data();
    std::initialize_owned_at(address, storage, owner.view());
    std::string_view selected = read_slot(address, storage);
    return selected == "borrowed" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name : {"main", "read_slot", "std::borrow_owned_at",
                            "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  const auto *reader = namedFunction(result, "std::borrow_owned_at");
  expect(reader != nullptr, "wrapper contents proof reaches its source reader");
  if (!reader)
    return;
  const auto *wrapper = namedFunction(result, "read_slot");
  expect(wrapper != nullptr, "contents-forwarding wrapper is represented");
  if (!wrapper)
    return;
  auto cycle = result.mir;
  auto *cyclicWrapper = const_cast<lang::MirFunctionInstance *>(
      cycle.findFunctionInstance(wrapper->id));
  for (auto &block : cyclicWrapper->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.functionTarget == reader->id)
        instruction.functionTarget = wrapper->id;
  bool checkedContents = false;
  for (const auto &loan : wrapper->body.loans)
    if (lang::mir_detail::ownedContentEntry(wrapper->body, loan, &result.mir)) {
      checkedContents = true;
      const auto *cyclicLoan = cyclicWrapper->body.findLoan(loan.id);
      expect(cyclicLoan && !lang::mir_detail::ownedContentEntry(
                               cyclicWrapper->body, *cyclicLoan, &cycle),
             "a helper cycle cannot invent an owner-contents primitive");
    }
  expect(checkedContents,
         "helper contents entry reaches its concrete primitive");
  auto changed = result.mir;
  auto *callee = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(reader->id));
  for (auto &block : callee->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.intrinsic == lang::IntrinsicKind::ObjectBorrowOwned)
        instruction.intrinsic = lang::IntrinsicKind::RawBorrow;
  expect(!lang::verifyMirProgram(changed).valid(),
         "helper signatures cannot replace the concrete owner-contents proof");
}

void testOwnedStorageMultipleOwners() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
class RawSlots<std::layout_queryable T> {
public:
  mut std::uninitialized<T> left{};
  mut std::uninitialized<T> right{};
  RawSlots() = default;
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  mut RawSlots<std::string_view> storage{};
  unsafe {
    std::string_view* left = storage.left.data();
    std::string_view* right = storage.right.data();
    std::initialize_owned_at(left, storage, first.view());
    std::initialize_owned_at(right, storage, second.view());
    std::string_view selected = std::borrow_owned_at(right, storage);
    return selected == "second" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
}

void testOwnedStorageMultipleRecords() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
class RawSlots<std::layout_queryable T> {
public:
  mut std::uninitialized<T> left{};
  mut std::uninitialized<T> right{};
  RawSlots() = default;
};
int main() {
  int first = 11; int second = 22;
  mut RawSlots<Reading> storage{};
  unsafe {
    Reading* left = storage.left.data();
    Reading* right = storage.right.data();
    std::initialize_owned_at(left, storage, Reading(first));
    std::initialize_owned_at(right, storage, Reading(second));
    Reading selected = std::borrow_owned_at(right, storage);
    return selected.read() == 22 ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  auto forged = result.mir;
  bool changed = false;
  const auto *reader = namedFunction(result, "std::borrow_owned_at");
  expect(reader != nullptr, "multiple-record accessor is represented");
  if (!reader)
    return;
  auto *callee = const_cast<lang::MirFunctionInstance *>(
      forged.findFunctionInstance(reader->id));
  for (auto &block : callee->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.intrinsic == lang::IntrinsicKind::ObjectBorrowOwned) {
        instruction.intrinsic = lang::IntrinsicKind::RawBorrow;
        changed = true;
      }
  expect(changed, "multiple-record read uses the explicit contents accessor");
  expect(!lang::verifyMirProgram(forged).valid(),
         "multiple record owners do not authorize an unproved raw accessor");
}

void testOwnedStorageReferenceReceiver() {
  // The elements are views, so the formal's referent can hold external
  // owners and the formal gets an external-contents entry. A referent that
  // cannot hold value dependencies (std::vector<std::string>) has no such
  // entry since borrowed-carrier shells were narrowed to those records.
  for (const int projected : {0, 1, 2}) {
    std::string source = R"gti(
import std.string;
import std.vector;
std::string clone_first(std::vector<std::string_view>& values) {
  return std::string(values[std::size_t(0)]);
}
int main() {
  std::string kept = std::string("kept");
  mut std::vector<std::string_view> values{};
  values.push_back(kept.view());
  return clone_first(values).size() == 4 ? 0 : 1;
}
)gti";
    if (projected) {
      const auto insert = source.find("std::string clone_first");
      source.insert(insert,
                    "struct State { std::vector<std::string_view> values; "
                    "State(std::vector<std::string_view> values) : "
                    "values(std::move(values)) {} };\n");
      const auto signature =
          source.find("clone_first(std::vector<std::string_view>& values)");
      source.replace(
          signature,
          std::string("clone_first(std::vector<std::string_view>& values)")
              .size(),
          "clone_first(State& state)");
      source.replace(source.find("(values["), std::string("(values[").size(),
                     "(state.values[");
      source.replace(
          source.find("return clone_first(values)"),
          std::string("return clone_first(values)").size(),
          "State state{std::move(values)}; return clone_first(state)");
    }
    if (projected == 2)
      source.replace(source.find("state.values[std::size_t(0)]"),
                     std::string("state.values[std::size_t(0)]").size(),
                     "state.values.back()");
    const auto result = analyzeText(source, true);
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(
        result.canGenerateCode(),
        "owned contents accessed through a reference formal's receiver lower");
    if (!result.canGenerateCode())
      return;
    const auto *function = namedFunction(result, "clone_first");
    expect(function != nullptr, "reference receiver helper is represented");
    if (!function)
      return;
    bool checked = false;
    for (const auto &loan : function->body.loans) {
      if (!lang::mir_detail::ownedContentEntry(function->body, loan,
                                               &result.mir))
        continue;
      auto forged = function->body;
      bool changed = false;
      for (auto &place : forged.places)
        if (!place.projections.empty() &&
            place.projections.front().kind ==
                lang::MirProjectionKind::Dereference) {
          place.projections.front().kind = lang::MirProjectionKind::Index;
          changed = true;
        }
      expect(changed, "reference receiver has an exact dereference place");
      expect(
          !lang::mir_detail::ownedContentEntry(forged, loan, &result.mir),
          "an arbitrary projection cannot witness the reference formal owner");
      if (projected == 2) {
        auto wrongStage = function->body;
        bool changedStage = false;
        for (auto &block : wrongStage.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.callInputRole == lang::MirCallInputRole::Receiver) {
              instruction.callSite = 0;
              changedStage = true;
            }
        expect(changedStage,
               "method receiver has an explicit call-input stage");
        expect(
            !lang::mir_detail::ownedContentEntry(wrongStage, loan, &result.mir),
            "a stage from another call cannot witness the physical owner");
      }
      checked = true;
    }
    expect(checked,
           "reference receiver has a verified external-contents entry");
  }
}

void testOwnedStorageReceiverWrite() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "generic storage insertion compiles");
  for (const auto name :
       {"RawBox::write", "std::initialize_owned_at", "main"}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr, "storage insertion has a source body");
    if (!function)
      continue;
    const auto effects =
        lang::deriveMirRetainingStoreEffects(*function, &result.mir);
    if (std::string_view(name) == "RawBox::write")
      expect(effects.complete && effects.effects.size() == 1 &&
                 effects.effects.front().destination.kind ==
                     lang::MirPlaceRootKind::This &&
                 effects.effects.front().source ==
                     lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                          function->parameterBindings.front()},
             "generic storage insertion retains its input in the receiver");
    if (std::string_view(name) == "RawBox::write") {
      auto unknown = *function;
      bool changed = false;
      for (auto &block : unknown.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.unsafeOperation != lang::UnsafeOperationKind::None &&
              instruction.functionTarget) {
            instruction.functionTarget.reset();
            changed = true;
          }
      expect(changed, "insertion delegates to an unsafe source helper");
      expect(
          !lang::deriveMirRetainingStoreEffects(unknown, &result.mir).complete,
          "an unsafe call without a checked target keeps unknown effects");
    }
    if (!effects.complete && std::string_view(name) != "main")
      std::cerr << "TRACE " << name << '\n'
                << lang::MirPrinter().print(function->body);
  }
}

void testOwnedStorageReceiverRead() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    std::initialize_owned_at(box.slot.data(), box, owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name : {"main", "RawBox::read", "std::borrow_owned_at",
                            "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  const auto *reader = namedFunction(result, "RawBox::read");
  expect(reader != nullptr, "receiver reader has a checked source body");
  if (!reader)
    return;
  bool checked = false;
  for (const auto &loan : reader->body.loans)
    if (lang::mir_detail::ownedContentEntry(reader->body, loan, &result.mir)) {
      checked = true;
      auto forged = loan;
      forged.access = lang::AccessMode::Mutable;
      expect(!lang::mir_detail::ownedContentEntry(reader->body, forged,
                                                  &result.mir),
             "shared receiver contents do not authorize mutable dependencies");
    }
  expect(checked,
         "receiver contents are independently traced to the primitive");
}

void testOwnedStorageBorrowedRead() {
  const auto result = analyzeText(R"gti(
import std.memory;
import std.string;
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe {
    std::string_view* address = storage.data();
    std::initialize_owned_at(address, storage, owner.view());
    std::string_view selected = std::borrow_owned_at(address, storage);
    return selected == "borrowed" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  const auto *reader = namedFunction(result, "std::borrow_owned_at");
  expect(reader != nullptr, "owner-aware reader has a checked source body");
  if (!reader)
    return;
  bool foundRead = false;
  for (const auto &block : reader->body.blocks)
    for (const auto &read : block.instructions) {
      if (read.intrinsic != lang::IntrinsicKind::ObjectBorrowOwned ||
          !read.loan)
        continue;
      foundRead = true;
      const auto *resultLoan = reader->body.findLoan(*read.loan);
      expect(resultLoan && resultLoan->storedDependency != 0,
             "owner-aware read carries a separate contents entry");
      if (!resultLoan || !resultLoan->storedDependency)
        continue;
      for (int mutation = 0; mutation < 3; ++mutation) {
        auto changed = result.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         changed.findFunctionInstance(reader->id))
                         ->body;
        if (mutation == 0) {
          // An ordinary raw address assertion does not establish contents.
          for (auto &candidateBlock : body.blocks)
            for (auto &candidate : candidateBlock.instructions)
              if (candidate.id == read.id)
                candidate.intrinsic = lang::IntrinsicKind::RawBorrow;
        } else if (mutation == 1) {
          for (auto &loan : body.loans)
            if (loan.id == resultLoan->id)
              loan.storedDependency = 0;
        } else {
          for (auto &loan : body.loans)
            if (loan.id == resultLoan->storedDependency)
              loan.access = lang::AccessMode::Mutable;
        }
        expect(!lang::verifyMirProgram(changed).valid(),
               "missing or forged owner-content authority is rejected");
      }
    }
  expect(foundRead, "owner-aware reader lowers to the explicit primitive");
}

void testProcessAllocatorRetention() {
  const auto frontend = lang::Frontend().analyze("allocator-retention.gti",
                                                 "int main() { return 0; }");
  expect(frontend.canGenerateCode(), "allocator retention fixture compiles");
  using Operation = lang::ProcessMemoryRuntimeOperation;
  for (unsigned value = static_cast<unsigned>(Operation::NativeSizeMax);
       value < static_cast<unsigned>(Operation::Count); ++value) {
    const auto operation = static_cast<Operation>(value);
    const auto *signature = lang::processMemoryRuntimeSignature(operation);
    if (!signature)
      continue;
    lang::MirFunctionInstance function;
    function.id = 1;
    function.definitionKind = lang::MirDefinitionKind::Declaration;
    function.linkage = lang::LanguageLinkage::C;
    function.externalSymbol = signature->symbol;
    function.returnType = signature->returnType;
    function.parameterTypes = signature->parameters;
    function.processMemoryOperation = operation;
    const bool selected = lang::processMemoryRuntimeSelectedForTarget(
        operation, frontend.mir.target().os);
    const auto summary =
        lang::deriveMirRetainingStoreEffects(function, &frontend.mir);
    expect(
        summary.complete == selected && summary.effects.empty(),
        "only target-selected exact allocator operations prove non-retention");
    auto changed = function;
    changed.externalSymbol += "_untrusted";
    expect(
        !lang::deriveMirRetainingStoreEffects(changed, &frontend.mir).complete,
        "an arbitrary native symbol cannot borrow the allocator contract");
    changed = function;
    changed.processMemoryOperation = Operation::None;
    expect(
        !lang::deriveMirRetainingStoreEffects(changed, &frontend.mir).complete,
        "an unclassified native declaration remains unknown");
    changed = function;
    changed.parameterTypes.push_back(lang::SemanticType::referenceTo(
        lang::SemanticType::StringView, lang::AccessMode::Mutable));
    expect(
        !lang::deriveMirRetainingStoreEffects(changed, &frontend.mir).complete,
        "a changed allocator signature cannot conceal checked reference "
        "retention");
  }
}

void testRetainingLocalConstruction() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class Scratch {
  int value;
public:
  Scratch(int input) : value(input) {}
};
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { Scratch scratch{7}; unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "local construction preserves a retaining method contract");
  const auto *writer = namedFunction(result, "RawBox::write");
  expect(writer != nullptr,
         "retaining method with local construction is lowered");
  if (!writer)
    return;
  expect(lang::deriveMirRetainingStoreEffects(*writer, &result.mir).complete,
         "a checked constructor writing only private storage has no outward "
         "retention");
  // A constructor body that writes a formal is not private construction.
  // Mutate the already-lowered body to ensure that this proof checks actual
  // destinations, not just the constructor's nominal identity.
  auto outward = result.mir;
  bool outwardWrite = false;
  for (const auto &block : writer->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.constructorTarget) {
        auto *constructor = const_cast<lang::MirConstructorInstance *>(
            outward.findConstructorInstance(*instruction.constructorTarget));
        if (!constructor || constructor->parameterBindings.empty())
          continue;
        lang::MirPlaceId formalPlace = 0;
        for (const auto &place : constructor->body.places)
          if (place.root == lang::MirPlaceRootKind::Binding &&
              place.projections.empty() &&
              place.binding == constructor->parameterBindings.front())
            formalPlace = place.id;
        for (auto &candidateBlock : constructor->body.blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (formalPlace &&
                candidate.kind == lang::MirInstructionKind::Initialize) {
              candidate.destination = formalPlace;
              outwardWrite = true;
            }
      }
  expect(outwardWrite &&
             !lang::deriveMirRetainingStoreEffects(*writer, &outward).complete,
         "a constructor with an outward formal write is not classified as "
         "private");
  auto unknown = *writer;
  bool changed = false;
  for (auto &block : unknown.body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          instruction.constructorTarget) {
        instruction.constructorTarget.reset();
        changed = true;
      }
  expect(
      changed &&
          !lang::deriveMirRetainingStoreEffects(unknown, &result.mir).complete,
      "construction without an inspectable target cannot claim no outward "
      "stores");
}

void testRetainingLocalCleanup() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class Scratch {
  mut int value;
public:
  Scratch(int input) : value(input) {}
  ~Scratch() { this.value = 0; }
};
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { Scratch scratch{7}; unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "private cleanup preserves a retaining method contract");
  const auto *writer = namedFunction(result, "RawBox::write");
  expect(writer != nullptr, "retaining method with local cleanup is lowered");
  if (!writer)
    return;
  expect(lang::deriveMirRetainingStoreEffects(*writer, &result.mir).complete,
         "an inspected destructor with only private writes has no outward "
         "retention");
  auto unknown = result.mir;
  bool changed = false;
  for (const auto &destructor : result.mir.destructorInstances())
    if (destructor.definitionKind == lang::MirDefinitionKind::Source) {
      const_cast<lang::MirDestructorInstance *>(
          unknown.findDestructorInstance(destructor.id))
          ->definitionKind = lang::MirDefinitionKind::Declaration;
      changed = true;
    }
  expect(changed &&
             !lang::deriveMirRetainingStoreEffects(*writer, &unknown).complete,
         "an unavailable destructor body cannot claim private cleanup");
}

void testRetainingExplicitCleanup() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class Scratch {
  mut int value;
public:
  Scratch(int input) : value(input) {}
  ~Scratch() { this.value = 0; }
};
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { mut std::uninitialized<Scratch> scratch{}; unsafe { std::initialize_owned_at(scratch.data(), scratch, Scratch(7)); std::destroy_at(scratch.data()); } unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "typed raw cleanup preserves a retaining method contract");
  const auto *writer = namedFunction(result, "RawBox::write");
  const auto *destroyer = namedFunction(result, "std::destroy_at");
  expect(writer && destroyer,
         "explicit destruction and retaining writer are lowered");
  if (!writer || !destroyer)
    return;
  const auto writerEffects =
      lang::deriveMirRetainingStoreEffects(*writer, &result.mir);
  if (!writerEffects.complete)
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || function.definitionKind != lang::MirDefinitionKind::Source)
        continue;
      const auto effects =
          lang::deriveMirRetainingStoreEffects(function, &result.mir);
      if (!effects.complete) {
        std::cerr << "CLEANUP INCOMPLETE " << info->qualifiedName
                  << " id=" << function.id << '\n';
        if (info->qualifiedName == "RawBox::write" ||
            info->qualifiedName == "std::initialize_owned_at" ||
            info->qualifiedName == "std::destroy_at")
          std::cerr << lang::MirPrinter().print(function.body);
      }
    }
  if (!writerEffects.complete)
    for (const auto &block : writer->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.constructorTarget) {
          const auto *constructor = result.mir.findConstructorInstance(
              *instruction.constructorTarget);
          if (constructor) {
            std::cerr << "CLEANUP CONSTRUCTOR " << constructor->id
                      << " definition="
                      << static_cast<int>(constructor->definitionKind)
                      << " generated-default=" << constructor->generatedDefault
                      << '\n';
            std::cerr << lang::MirPrinter().print(constructor->body);
          }
        }
  expect(
      writerEffects.complete,
      "explicit cleanup derives its retention from the exact destroyed type");
  auto scheduled = result.mir;
  bool changedDefault = false;
  std::vector<const lang::MirBody *> constructionBodies{&writer->body};
  std::vector<lang::HirConstructorInstanceId> visitedConstructors;
  for (std::size_t index = 0; index < constructionBodies.size(); ++index)
    for (const auto &block : constructionBodies[index]->blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.constructorTarget &&
            std::find(visitedConstructors.begin(), visitedConstructors.end(),
                      *instruction.constructorTarget) ==
                visitedConstructors.end()) {
          visitedConstructors.push_back(*instruction.constructorTarget);
          if (const auto *constructor = result.mir.findConstructorInstance(
                  *instruction.constructorTarget))
            constructionBodies.push_back(&constructor->body);
        }
        if (lang::mir_detail::generatedDefaultConstruction(instruction))
          for (const auto &owner : result.mir.classInstances())
            if (owner.type == instruction.info.type) {
              const_cast<lang::MirClassInstance *>(
                  scheduled.findClassInstance(owner.id))
                  ->generatedDefaultRequiresSchedule = true;
              changedDefault = true;
            }
      }
  expect(
      changedDefault &&
          !lang::deriveMirRetainingStoreEffects(*writer, &scheduled).complete,
      "generated construction cannot ignore a required initializer schedule");
  for (unsigned mutation = 0; mutation != 2; ++mutation) {
    auto changed = *destroyer;
    bool found = false;
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy) {
          expect(instruction.elementCleanup.has_value(),
                 "source destructor has an exact cleanup recipe");
          if (mutation == 0)
            instruction.elementCleanup.reset();
          else
            instruction.unsafeOperation = lang::UnsafeOperationKind::None;
          found = true;
        }
    expect(found && !lang::deriveMirRetainingStoreEffects(changed, &result.mir)
                         .complete,
           "missing cleanup or invalid lifetime-operation shape cannot prove "
           "non-retention");
  }
}

void testSymbolicAggregateRetention() {
  const auto result = analyzeText(R"gti(
class Payload {
  int value;
public:
  Payload(int input) : value(input) {}
};
class Box {
  mut Payload value{0};
public:
  Box() = default;
  void set(Payload input) mut { this.value = input; }
};
int main() { mut Box box{}; box.set(Payload(7)); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "ordinary aggregate assignment compiles");
  const auto *setter = namedFunction(result, "Box::set");
  expect(setter != nullptr, "aggregate setter is lowered");
  if (!setter)
    return;
  const auto original = setter->body;
  const auto effects =
      lang::deriveMirRetainingStoreEffects(*setter, &result.mir);
  expect(effects.complete && effects.effects.size() == 1 &&
             effects.effects.front().destination.kind ==
                 lang::MirPlaceRootKind::This &&
             effects.effects.front().source ==
                 lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                      setter->parameterBindings.front()},
         "aggregate contents are modeled symbolically even without nominal "
         "borrowed fields");
  expect(setter->body == original,
         "summary-only tokens do not mutate executable MIR");
}

void testNestedLifecycleRetention() {
  const auto result = analyzeText(R"gti(
import std.utility;
void construct_helper() {}
void cleanup_helper() {}
class Scratch {
public:
  Scratch() { construct_helper(); }
  ~Scratch() { cleanup_helper(); }
};
class Payload {
public:
  int value;
  Payload(int input) : value(input) {}
  ~Payload() { cleanup_helper(); }
};
void assign(mut Payload& target, Payload input) {
  Scratch scratch{};
  target = std::move(input);
}
int main() { mut Payload target{0}; assign(target, Payload(7)); return target.value; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "retaining store with nested lifecycle helpers compiles");
  const auto *writer = namedFunction(result, "assign");
  expect(writer != nullptr, "nested lifecycle retaining writer is lowered");
  if (!writer)
    return;
  const auto valid = lang::deriveMirRetainingStoreEffects(*writer, &result.mir);
  expect(valid.complete && valid.effects.size() == 1,
         "lifecycle helpers preserve the actual retaining transfer");
  for (unsigned mutation = 0; mutation != 2; ++mutation) {
    auto changed = *writer;
    bool found = false;
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CleanupBeforeAssign) {
          if (mutation == 0)
            instruction.replacementCleanup.reset();
          else
            instruction.replacementPair = 0;
          found = true;
        }
    expect(found && !lang::deriveMirRetainingStoreEffects(changed, &result.mir)
                         .complete,
           "replacement cleanup needs its exact recipe and paired commit");
  }
  for (const auto name : {"construct_helper", "cleanup_helper"}) {
    const auto *helper = namedFunction(result, name);
    expect(helper != nullptr, "nested lifecycle helper has an instance");
    if (!helper)
      continue;
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(helper->id))
        ->definitionKind = lang::MirDefinitionKind::Declaration;
    expect(
        !lang::deriveMirRetainingStoreEffects(*writer, &changed).complete,
        "unknown nested lifecycle helper invalidates the enclosing transfer");
  }
}

void testReturnedAggregateRetention() {
  const auto result = analyzeText(R"gti(
class Payload { public: int value; Payload(int input) : value(input) {} };
Payload relay(Payload input) { return input; }
Payload choose(Payload first, Payload second, bool select) {
  if (select) { return relay(first); }
  return relay(second);
}
Payload fresh() { return Payload(3); }
class Box {
  mut Payload value{0};
public:
  Box() = default;
  void set(Payload first, Payload second, bool select) mut {
    this.value = choose(first, second, select);
  }
};
int main() { mut Box box{}; box.set(Payload(1), Payload(2), true); return fresh().value; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "aggregate result forwarding compiles");
  const auto *relay = namedFunction(result, "relay");
  const auto *choose = namedFunction(result, "choose");
  const auto *fresh = namedFunction(result, "fresh");
  const auto *setter = namedFunction(result, "Box::set");
  expect(relay && choose && fresh && setter,
         "aggregate return helpers are lowered");
  if (!relay || !choose || !fresh || !setter)
    return;
  const auto selected =
      lang::deriveMirRetainingStoreEffects(*choose, &result.mir);
  const auto independent =
      lang::deriveMirRetainingStoreEffects(*fresh, &result.mir);
  const auto stored =
      lang::deriveMirRetainingStoreEffects(*setter, &result.mir);
  expect(selected.complete && selected.returnedOrigins &&
             selected.returnedOrigins->size() == 2,
         "conditional helper results preserve both input origins");
  expect(independent.complete && independent.returnedOrigins &&
             independent.returnedOrigins->empty(),
         "fresh aggregate result has a proved empty dependency set");
  expect(stored.complete && stored.effects.size() == 2,
         "retaining stores compose through aggregate-returning helpers");
  auto changed = result.mir;
  auto *changedRelay = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(relay->id));
  bool mutated = false;
  for (auto &block : changedRelay->body.blocks)
    if (block.terminator.kind == lang::MirTerminatorKind::Return &&
        block.terminator.value) {
      block.terminator.value->kind = lang::MirOperandKind::Value;
      block.terminator.value->value = 999999;
      mutated = true;
    }
  expect(mutated &&
             !lang::deriveMirRetainingStoreEffects(*changedRelay, &changed)
                  .returnedOrigins,
         "an unproved return value is unknown, never an empty origin set");
  expect(!lang::deriveMirRetainingStoreEffects(*setter, &changed).complete,
         "unknown return provenance blocks the downstream retaining store");
}

void testReturnedTextRetention() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
T relay<T>(T input) { return std::move(input); }
class Holder {
  mut std::string_view text = "";
public:
  Holder() = default;
  void set(std::string_view input) mut { this.text = relay(input); }
};
int main() { mut Holder holder{}; holder.set("borrowed"); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "returned borrowed text can enter retaining storage");
  const auto *writer = namedFunction(result, "Holder::set");
  const auto *relay = namedFunction(result, "relay");
  expect(writer && relay,
         "borrowed-text helper and retaining method are lowered");
  if (!writer || !relay)
    return;
  const auto summary =
      lang::deriveMirRetainingStoreEffects(*writer, &result.mir);
  if (!summary.complete) {
    const auto returned =
        lang::deriveMirRetainingStoreEffects(*relay, &result.mir);
    std::cerr << "TEXT RETURN helper complete=" << returned.complete
              << " origins="
              << (returned.returnedOrigins
                      ? std::to_string(returned.returnedOrigins->size())
                      : "unknown")
              << '\n';
    std::cerr << lang::MirPrinter().print(relay->body);
    std::cerr << lang::MirPrinter().print(writer->body);
  }
  expect(summary.complete && summary.effects.size() == 1,
         "returned text preserves its input origin in the retaining summary");
  const auto concrete = lang::deriveMirCarrierFlow(writer->body, &result.mir);
  bool keptResultLoan = false;
  for (const auto &block : writer->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan)
        for (const auto &[store, inputs] : concrete.storeInputs) {
          (void)store;
          keptResultLoan =
              keptResultLoan ||
              (inputs && std::find(inputs->begin(), inputs->end(),
                                   *instruction.loan) != inputs->end());
        }
  expect(keptResultLoan,
         "concrete text storage still carries the actual result loan");
  auto changed = result.mir;
  const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(relay->id))
      ->definitionKind = lang::MirDefinitionKind::Declaration;
  expect(
      !lang::deriveMirRetainingStoreEffects(*writer, &changed).complete,
      "an unknown returning helper cannot justify a symbolic borrowed store");
}

void testFailureSnapshotRetention() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.panic;
void fail(std::string_view message) { std::panic(message); }
int main() { fail("snapshot"); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "borrowed panic message compiles");
  const auto *relay = namedFunction(result, "fail");
  expect(relay &&
             lang::deriveMirRetainingStoreEffects(*relay, &result.mir).complete,
         "failure snapshot does not retain its borrowed message");
  bool found = false;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::Panic) {
          found = true;
          expect(lang::mir_detail::explicitFailure(result.mir, instruction),
                 "panic snapshot has an exact failure contract");
          for (unsigned mutation = 0; mutation != 3; ++mutation) {
            auto changed = function;
            for (auto &changedBlock : changed.body.blocks)
              for (auto &call : changedBlock.instructions)
                if (call.id == instruction.id) {
                  if (mutation == 0)
                    call.localFailureSites.clear();
                  else if (mutation == 1)
                    call.parameterTypes.clear();
                  else
                    call.rawMemoryAccess = true;
                  expect(
                      !lang::mir_detail::explicitFailure(result.mir, call),
                      "malformed failure snapshot cannot prove non-retention");
                }
            expect(!lang::deriveMirRetainingStoreEffects(changed, &result.mir)
                        .complete,
                   "retention analysis rejects an unproved failure snapshot");
          }
        }
  expect(found, "panic snapshot is lowered for the mutation regressions");
}

void testGrowableBorrowedRecordReturn() {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "growable-record-return.gti", R"gti(
import std.vector;
class Reading<lifetime a> {
  int&<a> value;
public:
  Reading(int&<a> input) : value(input) {}
  int read() { return this.value; }
};
Reading<a> pick<lifetime a>(int&<a> first, int&<a> second) {
  mut std::vector<Reading<a>> values{};
  values.push_back(Reading<a>(first)); values.push_back(Reading<a>(second));
  Reading<a> selected = values[0];
  values.reserve(32);
  return selected;
}
int main() {
  int first = 11; int second = 22;
  Reading selected = pick(first, second);
  return selected.read() == 11 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info && info->qualifiedName == "pick")
        std::cerr << "RECORD RETURN " << info->qualifiedName
                  << " id=" << function.id << '\n'
                  << lang::MirPrinter().print(function.body);
    }
  }
  expect(result.canGenerateCode(),
         "copied borrowed record outlives container storage");
  if (result.canGenerateCode()) {
    const auto *pick = namedFunction(result, "pick");
    expect(pick != nullptr, "copied record return has a source body");
    if (pick) {
      auto changed = result.mir;
      auto *limited = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(pick->id));
      limited->returnBorrowParameters = {0};
      const auto verification = lang::verifyMirProgram(changed);
      // Complete summary reconstruction can reject the inconsistent supplier
      // claim before the copied-value endpoint check. Require a rejection in
      // pick's own return proof, not merely a mismatching caller annotation.
      const bool rejected =
          functionError(
              verification, pick->id,
              "copied record return uses an undeclared or local supplier") ||
          functionError(verification, pick->id,
                        "complete result dependency summary lacks independent "
                        "evidence for every reachable normal return");
      if (!rejected) {
        std::cerr << "RETURN SUPPLIER MUTATION valid=" << verification.valid()
                  << '\n';
        for (const auto &error : verification.errors)
          std::cerr << "RETURN SUPPLIER ERROR owner=" << error.owner << " "
                    << error.message << '\n';
      }
      expect(rejected,
             "return publication cannot discard another possible supplier");
    }
  }
}

void testCopiedReferenceParameterProof() {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "copy-reference-proof.gti", R"gti(
import std.string;
T copy_borrowed<T>(T& value) requires std::copy_constructible<T> { return value; }
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int number = 42;
  Reading original{number};
  Reading record = copy_borrowed(original);
  std::string owner{"borrowed"};
  std::string_view view = owner.view();
  std::string_view copied = copy_borrowed(view);
  return copied == "borrowed" && record.read() == 42 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode())
    printDiagnostics(result);
  std::size_t found = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (!info || info->qualifiedName != "copy_borrowed")
      continue;
    ++found;
    expect(lang::mir_detail::copiedReferenceParameter(function) == 0,
           "whole-value copy proves contents forwarding from its formal");
    auto changed = function;
    changed.parameterTypes[0].referenceAccess = lang::AccessMode::Mutable;
    expect(!lang::mir_detail::copiedReferenceParameter(changed),
           "mutable reference must not receive shared-copy proof");
    changed = function;
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Load)
          instruction.operands.front().kind = lang::MirOperandKind::Move;
    expect(!lang::mir_detail::copiedReferenceParameter(changed),
           "moving is not independently proved to be a contents copy");
    changed = function;
    for (auto &loan : changed.body.loans)
      if (loan.entry && loan.kind == lang::MirLoanKind::Stored)
        loan.kind = lang::MirLoanKind::Parameter;
    expect(!lang::mir_detail::copiedReferenceParameter(changed),
           "physical reference authority is not stored-contents authority");
  }
  expect(found == 2, "generic copy helper has view and user-record MIR bodies");
  expect(result.canGenerateCode(),
         "ordinary shared-reference copying is valid");
}

std::string ownedConstructionSource() {
  return R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
void put(Reading* destination, mut int& owner, int& input) {
  unsafe { std::construct_owned_at<Reading, int, int&>(destination, owner, input); }
}
int main() {
  Reading* destination = nullptr;
  mut int owner = 0;
  int number = 11;
  put(destination, owner, number);
  return 0;
}
)gti";
}

void testOwnedConstructionHir() {
  const auto source = ownedConstructionSource();
  const auto analyze = [&](const std::string &text) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
        .analyze("owned-construction-hir.gti", text, {standardLibraryPrelude()},
                 {}, {standardLibraryRoot()});
  };
  const auto result = analyze(source);
  if (!result.semanticValid || !result.hirValid)
    printDiagnostics(result);
  expect(result.semanticValid && result.hirValid,
         "owner-aware construction selects the borrowed record constructor in "
         "HIR");
  bool found = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.intrinsic == lang::IntrinsicKind::ObjectConstructOwned) {
        found = true;
        expect(
            value.constructorTarget.has_value() &&
                value.parameterTypes.size() == 3 &&
                value.parameterTypes[1].kind == lang::SemanticType::Reference &&
                value.parameterTypes[1].referenceAccess ==
                    lang::AccessMode::Mutable &&
                value.parameterTypes[2].kind == lang::SemanticType::Reference,
            "storage owner remains separate from the constructor reference "
            "parameter");
      }
  expect(found, "the owner-aware operation survives HIR lowering");
  auto readonly = source;
  readonly.replace(readonly.find("mut int& owner"),
                   std::string("mut int& owner").size(), "int& owner");
  const auto rejectedOwner = analyze(readonly);
  expect(!rejectedOwner.semanticValid,
         "owner-aware construction rejects a shared destination owner");
  auto raw = source;
  const std::string call =
      "construct_owned_at<Reading, int, int&>(destination, owner, input)";
  raw.replace(raw.find(call), call.size(),
              "construct_at<Reading, int&>(destination, input)");
  const auto rejectedRaw = analyze(raw);
  expect(!rejectedRaw.semanticValid,
         "ordinary raw construction still rejects checked borrowed payloads");
}

void testOwnedMutableBorrowHir() {
  const std::string source = R"gti(
import std.memory;
class Reading { public: int& value; Reading(int& input) : value(input) {} };
unsafe mut Reading& access(Reading* address, mut int& owner) {
  unsafe { return std::borrow_owned_mut_at(address, owner); }
}
int main() {
  Reading* address = nullptr;
  mut int owner = 0;
  unsafe { mut Reading& selected = access(address, owner); }
  return 0;
}
)gti";
  const auto analyze = [&](const std::string &text) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
        .analyze("owned-mutable-borrow-hir.gti", text,
                 {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  const auto result = analyze(source);
  if (!result.semanticValid || !result.hirValid)
    printDiagnostics(result);
  expect(result.semanticValid && result.hirValid,
         "exclusive owner-aware access preserves its checked HIR contract");
  bool found = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.intrinsic == lang::IntrinsicKind::ObjectBorrowOwnedMut) {
        found = true;
        expect(
            value.parameterTypes.size() == 2 &&
                value.parameterTypes[0].pointerAccess ==
                    lang::AccessMode::Mutable &&
                value.parameterTypes[1].referenceAccess ==
                    lang::AccessMode::Mutable &&
                value.borrowAccess == lang::AccessMode::Mutable,
            "mutable access requires a writable address and exclusive owner");
      }
  expect(found, "mutable owner-aware access reaches its exact primitive");
  auto readonlyOwner = source;
  readonlyOwner.replace(readonlyOwner.find("mut int& owner"),
                        std::string("mut int& owner").size(), "int& owner");
  expect(!analyze(readonlyOwner).semanticValid,
         "shared owners cannot authorize mutable owned access");
  auto readonlyAddress = source;
  readonlyAddress.replace(readonlyAddress.find("access(Reading* address"),
                          std::string("access(Reading* address").size(),
                          "access(const Reading* address");
  expect(!analyze(readonlyAddress).semanticValid,
         "read-only pointers cannot authorize mutable owned access");
}

void testOwnedConstructionMir() {
  const auto result = lang::Frontend().analyze(
      "owned-construction-mir.gti", ownedConstructionSource(),
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "owner-aware source construction reaches verified MIR");
  if (!result.canGenerateCode())
    return;
  bool found = false;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::ObjectConstructOwned)
          continue;
        found = true;
        expect(lang::mir_detail::objectConstruct(result.mir, instruction),
               "the selected owner-aware construction has an exact structural "
               "proof");
        auto sharedOwner = instruction;
        sharedOwner.parameterTypes[1].referenceAccess =
            lang::AccessMode::ReadOnly;
        sharedOwner.operands[1].type = sharedOwner.parameterTypes[1];
        expect(
            !lang::mir_detail::objectConstruct(result.mir, sharedOwner),
            "a shared owner cannot replace the exclusive construction witness");
        auto missingOwner = instruction;
        missingOwner.parameterTypes.erase(missingOwner.parameterTypes.begin() +
                                          1);
        missingOwner.operands.erase(missingOwner.operands.begin() + 1);
        expect(!lang::mir_detail::objectConstruct(result.mir, missingOwner),
               "construction cannot erase its owner parameter");
        auto missingConstructor = instruction;
        missingConstructor.constructorTarget.reset();
        expect(
            !lang::mir_detail::objectConstruct(result.mir, missingConstructor),
            "ordinary borrowed construction cannot erase its selected "
            "constructor");
        missingOwner.intrinsic = lang::IntrinsicKind::ObjectConstruct;
        expect(
            !lang::mir_detail::objectConstruct(result.mir, missingOwner),
            "raw construction cannot inherit owner-aware payload permission");
      }
  expect(found,
         "the MIR test exercises the owner-aware construction operation");
}

void testDynamicTextCleanupCapacity() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.panic;
struct Observing {
  std::string_view text;
  Observing() : text("literal") {}
  ~Observing() { if (this.text != "literal") { std::panic("bad text"); } }
};
struct Passive {
  std::string_view text;
  std::string owned;
  Passive() : text("literal"), owned("owned") {}
};
struct OwnedOnly {
  int value;
  OwnedOnly() : value(1) {}
  ~OwnedOnly() {}
};
enum PassiveChoice { text(std::string_view view), owned(std::string value), none };
enum ObservingChoice { value(Observing item), none };
struct PassiveEnum {
  PassiveChoice choice;
  PassiveEnum() : choice(PassiveChoice::none) {}
};
struct ObservingEnum {
  ObservingChoice choice;
  ObservingEnum() : choice(ObservingChoice::none) {}
};
int main() {
  PassiveEnum passive_enum{};
  ObservingEnum observing_enum{};
  Observing observing{};
  Passive passive{};
  OwnedOnly owned{};
  return observing.text == passive.text && owned.value == 1 ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "cleanup capacity preserves literal-only record construction");
  unsigned checked = 0;
  for (const auto &owner : result.mir.classInstances()) {
    const auto *type = result.semantics.findClassType(owner.declaration);
    if (!type)
      continue;
    if (type->qualifiedName == "Observing" ||
        type->qualifiedName == "ObservingEnum") {
      ++checked;
      expect(!owner.traits.containsBorrowedState &&
                 owner.lifecycleBoundBorrowCarrier,
             "observing cleanup retains dynamic text dependencies absent "
             "static reference traits");
      auto changed = result.mir;
      const_cast<lang::MirClassInstance *>(changed.findClassInstance(owner.id))
          ->lifecycleBoundBorrowCarrier = false;
      expect(!lang::verifyMirProgram(changed).valid(),
             "MIR independently rejects omitted dynamic cleanup retention");
    } else if (type->qualifiedName == "Passive" ||
               type->qualifiedName == "OwnedOnly" ||
               type->qualifiedName == "PassiveEnum") {
      ++checked;
      expect(!owner.lifecycleBoundBorrowCarrier,
             "passive field cleanup or a record without borrowed capacity does "
             "not pin dependencies");
    }
  }
  expect(checked == 5, "cleanup-capacity regression checks direct and "
                       "enum-nested structural cases");
}

void testBorrowedArrayCleanupContainment() {
  const auto result = analyzeText(R"gti(
mut int witness = 0;
class Observed {
  int& source;
public:
  Observed(int& input) : source(input) {}
  ~Observed() { witness += this.source; }
};
int main() {
  int first = 11;
  int second = 22;
  Observed values[2] = {Observed(first), Observed(second)};
  return 0;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(
      result.canGenerateCode(),
      "borrowed array elements retain dependencies through observing cleanup");
  if (!result.canGenerateCode())
    return;
  bool checked = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Aggregate ||
            instruction.info.type.kind != lang::SemanticType::Array ||
            instruction.operands.size() != 2 ||
            instruction.lifecycle.size() != 3)
          continue;
        checked = true;
        for (int forgery = 0; forgery < 3; ++forgery) {
          auto changed = result.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          for (auto &changedBlock : target->body.blocks)
            for (auto &candidate : changedBlock.instructions) {
              if (candidate.id != instruction.id)
                continue;
              if (forgery == 0) {
                candidate.lifecycle.erase(candidate.lifecycle.begin());
              } else if (forgery == 1) {
                candidate.operands[1] = candidate.operands[0];
              } else {
                candidate.lifecycle.back().target =
                    candidate.lifecycle.front().source;
              }
            }
          expect(!lang::verifyMirProgram(changed).valid(),
                 "array cleanup rejects missing, unrelated or wrong-result "
                 "element transfers");
        }
      }
    }
  }
  expect(checked,
         "observing array exposes an exact aggregate cleanup transfer");
}

void testTextFieldInitializerStage() {
  const auto result = analyzeText(R"gti(
import std.memory;
struct Label {
  std::string_view text;
  Label(std::string_view input) : text(input) {}
};
int main() {
  Label selected{"literal"};
  return selected.text == "literal" ? 0 : 1;
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "text field construction retains its ordinary source contract");
  if (!result.canGenerateCode())
    return;
  std::size_t stages = 0;
  for (const auto &constructor : result.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &stage : block.instructions) {
        if (!stage.constructorInitializer ||
            stage.kind != lang::MirInstructionKind::Initialize ||
            stage.info.type.kind != lang::SemanticType::StringView)
          continue;
        ++stages;
        const auto flow =
            lang::deriveMirCarrierFlow(constructor.body, &result.mir);
        const auto targets = flow.storeTargets.find(stage.id);
        expect(targets != flow.storeTargets.end() &&
                   targets->second.size() == 1 &&
                   targets->second.front().kind == lang::MirPlaceRootKind::This,
               "constructor text publication is an explicit receiver store");
        auto changed = result.mir;
        auto *altered = const_cast<lang::MirConstructorInstance *>(
            changed.findConstructorInstance(constructor.id));
        for (auto &changedBlock : altered->body.blocks)
          for (auto &changedStage : changedBlock.instructions)
            if (changedStage.id == stage.id)
              changedStage.constructorInitializer = 0;
        expect(!lang::verifyMirProgram(changed).valid(),
               "text field initialization must retain its exact constructor "
               "stage identity");
      }
  expect(stages == 1, "one text field initializer publishes through MIR");
}

void testGrowableBorrowedRecordAccess() {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "growable-record-access.gti", R"gti(
import std.vector;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int first = 11; int second = 22;
  mut std::vector<Reading> values{};
  values.push_back(Reading(first)); values.push_back(Reading(second));
  values.reserve(32);
  std::vector<Reading>& shared = values;
  return shared[0].read() == 11 && shared[1].read() == 22 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info &&
          (info->qualifiedName == "main" ||
           info->qualifiedName == "std::vector::__gti_operator_subscript" ||
           info->qualifiedName == "std::detail::prefix_owner::read_unchecked" ||
           info->qualifiedName ==
               "gti_internal::prefix_slots::read_unchecked" ||
           info->qualifiedName == "std::borrow_owned_at"))
        std::cerr << "RECORD ACCESS " << info->qualifiedName
                  << " id=" << function.id << '\n'
                  << lang::MirPrinter().print(function.body);
    }
  }
  expect(result.canGenerateCode(),
         "shared vector access preserves multiple borrowed-record owners");
}

void testGrowableBorrowedRetention() {
  // Match executable compilation while isolating the instantiated insertion
  // path. Full library checking also exercises unrelated generic operations.
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "growable-retention.gti", R"gti(
import std.string;
import std.vector;
int main() {
  std::string owner{"borrowed"};
  mut std::vector<std::string_view> values{};
  values.push_back(owner.view());
  return values.size() == 1 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || function.definitionKind != lang::MirDefinitionKind::Source)
        continue;
      const auto effects =
          lang::deriveMirRetainingStoreEffects(function, &result.mir);
      if (!effects.complete) {
        std::cerr << "INCOMPLETE " << info->qualifiedName
                  << " id=" << function.id << '\n';
        if (info->qualifiedName == "gti_internal::try_allocate_objects" ||
            info->qualifiedName == "gti_internal::raw_buffer::try_allocate") {
          const auto flow =
              lang::deriveMirCarrierFlow(function.body, &result.mir);
          std::cerr << "ALLOCATION concrete effects=" << flow.effectsComplete
                    << '\n';
          for (const auto &block : function.body.blocks)
            for (const auto &instruction : block.instructions)
              if (instruction.functionTarget) {
                const auto *callee = result.mir.findFunctionInstance(
                    *instruction.functionTarget);
                const auto *declaration =
                    callee ? result.semantics.findFunction(callee->declaration)
                           : nullptr;
                if (callee && declaration) {
                  const auto summary = lang::deriveMirRetainingStoreEffects(
                      *callee, &result.mir);
                  std::cerr
                      << "ALLOCATION call i" << instruction.id << " "
                      << declaration->qualifiedName
                      << " complete=" << summary.complete
                      << " stores=" << summary.effects.size() << " returned="
                      << (summary.returnedOrigins
                              ? std::to_string(summary.returnedOrigins->size())
                              : "unknown")
                      << '\n';
                }
              }
        }
        if (info->qualifiedName == "std::vector::push_back" ||
            info->qualifiedName == "std::vector::reserve" ||
            info->qualifiedName == "std::vector::ensure_capacity_for_one" ||
            info->qualifiedName == "gti_internal::try_allocate_objects" ||
            info->qualifiedName == "gti_internal::raw_buffer::try_allocate")
          std::cerr << lang::MirPrinter().print(function.body);
      }
    }
  }
  expect(result.canGenerateCode(),
         "growable insertion retains a dynamic text owner");
  const auto *push = namedFunction(result, "std::vector::push_back");
  expect(push != nullptr, "growable insertion has a source body");
  if (push) {
    const auto effects =
        lang::deriveMirRetainingStoreEffects(*push, &result.mir);
    expect(effects.complete,
           "growable insertion publishes complete retaining effects");
  }
}

} // namespace

#include "unit_test.h"

#include <cstdlib>
#include <string_view>

GTI_UNIT_TEST(ownerless_multiple_text_owners) {
  if (!slowTier())
    return 77;
  testMultipleTextOwners();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_carrier_flow_on_lowered_program) {
  testCarrierFlowOnLoweredProgram();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_replacement_dependency_verification) {
  if (!slowTier())
    return 77;
  testReplacementDependencyVerification();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_explicit_empty_carrier_proof) {
  testExplicitEmptyCarrierProof();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_array_result_literal_return_proof) {
  testArrayResultLiteralReturnProof();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_inline_array_call_supplier_set) {
  if (!slowTier())
    return 77;
  testInlineArrayCallSupplierSet();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_reference_array_external_entry) {
  testReferenceArrayExternalEntry();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_expired_stored_return) {
  testExpiredStoredReturn();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_conditional_stored_value_flow) {
  testConditionalStoredValueFlow();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_inferred_array_return_suppliers) {
  testInferredArrayReturnSuppliers();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_array_ownerless_publication_contract) {
  testArrayOwnerlessPublicationContract();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_borrowed_array_projection_proof) {
  testBorrowedArrayProjectionProof();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_generic_array_receiver_retention) {
  testGenericArrayReceiverRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_generic_field_receiver_retention) {
  testGenericFieldReceiverRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_generic_array_element_return_contract) {
  testGenericArrayElementReturnContract();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_generic_array_element_return_proof) {
  if (!slowTier())
    return 77;
  testGenericArrayElementReturnProof();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_store_contract_separation) {
  if (!slowTier())
    return 77;
  testRetainingStoreContractSeparation();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_named_callable_escape) {
  if (!slowTier())
    return 77;
  testRetainingNamedCallableEscape();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_call_conditional_cleanup_proof) {
  if (!slowTier())
    return 77;
  testRetainingCallConditionalCleanupProof();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_store_effects_on_lowered_program) {
  if (!slowTier())
    return 77;
  testRetainingStoreEffectsOnLoweredProgram();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_reference_argument) {
  if (!slowTier())
    return 77;
  testOwnedStorageReferenceArgument();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_borrowed_record) {
  testOwnedStorageBorrowedRecord();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_read_wrapper) {
  if (!slowTier())
    return 77;
  testOwnedStorageReadWrapper();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_multiple_owners) {
  if (!slowTier())
    return 77;
  testOwnedStorageMultipleOwners();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_multiple_records) {
  if (!slowTier())
    return 77;
  testOwnedStorageMultipleRecords();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_reference_receiver) {
  if (!slowTier())
    return 77;
  testOwnedStorageReferenceReceiver();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_receiver_write) {
  if (!slowTier())
    return 77;
  testOwnedStorageReceiverWrite();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_receiver_read) {
  if (!slowTier())
    return 77;
  testOwnedStorageReceiverRead();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_storage_borrowed_read) {
  if (!slowTier())
    return 77;
  testOwnedStorageBorrowedRead();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_process_allocator_retention) {
  testProcessAllocatorRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_local_construction) {
  if (!slowTier())
    return 77;
  testRetainingLocalConstruction();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_local_cleanup) {
  if (!slowTier())
    return 77;
  testRetainingLocalCleanup();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_retaining_explicit_cleanup) {
  if (!slowTier())
    return 77;
  testRetainingExplicitCleanup();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_symbolic_aggregate_retention) {
  testSymbolicAggregateRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_nested_lifecycle_retention) {
  testNestedLifecycleRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_returned_aggregate_retention) {
  testReturnedAggregateRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_returned_text_retention) {
  if (!slowTier())
    return 77;
  testReturnedTextRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_failure_snapshot_retention) {
  if (!slowTier())
    return 77;
  testFailureSnapshotRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_growable_borrowed_record_return) {
  testGrowableBorrowedRecordReturn();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_copied_reference_parameter_proof) {
  testCopiedReferenceParameterProof();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_construction_hir) {
  testOwnedConstructionHir();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_mutable_borrow_hir) {
  testOwnedMutableBorrowHir();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_owned_construction_mir) {
  testOwnedConstructionMir();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_dynamic_text_cleanup_capacity) {
  testDynamicTextCleanupCapacity();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_borrowed_array_cleanup_containment) {
  testBorrowedArrayCleanupContainment();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_text_field_initializer_stage) {
  testTextFieldInitializerStage();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_growable_borrowed_record_access) {
  testGrowableBorrowedRecordAccess();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ownerless_growable_borrowed_retention) {
  testGrowableBorrowedRetention();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(persistent_storage_audit_preserves_structural_errors) {
  const auto result = analyzeText(R"gti(
mut int first = 1;
mut int second = 2;
int main() {
  int& a = first;
  int& b = second;
  return a + b;
}
)gti");
  const auto *main = namedFunction(result, "main");
  expect(result.canGenerateCode() && main && main->body.loans.size() == 2,
         "the fixture has two independent global reference loans");
  if (!main || main->body.loans.size() != 2)
    return 1;
  auto changed = result.mir;
  auto &body = const_cast<lang::MirFunctionInstance *>(
                   changed.findFunctionInstance(main->id))
                   ->body;
  body.loans.back().parent = body.loans.front().id;
  expect(!lang::verifyMirProgram(changed).valid(),
         "a reborrow of unrelated storage fails MIR verification");
  expect(lang::auditMirPersistentStorageBorrows(changed).empty(),
         "a malformed loan does not diagnose a source-level global conflict");
  return failures == 0 ? 0 : 1;
}
