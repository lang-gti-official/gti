#include "../src/compiler/mir_verification_queries.h"
#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir.h"
#include "gti/mir_printer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool valid, const std::string &message) {
  if (!valid) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult
analyze(const std::string &source,
        lang::RawPointerAuditMode mode = lang::RawPointerAuditMode::Off,
        bool reportLibraryAuditFindings = false) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  lang::FrontendOptions options;
  options.rawPointerAudit = mode;
  options.reportLibraryAuditFindings = reportLibraryAuditFindings;
  return lang::Frontend(options).analyze(root / "raw-borrow-test.gti", source,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}
void show(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
void negative(const std::string &name, const std::string &source,
              const std::string &fragment,
              lang::RawPointerAuditMode mode = lang::RawPointerAuditMode::Off) {
  const auto result = analyze(source, mode);
  const bool matched = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.message.find(fragment) != std::string::npos;
      });
  if (result.canGenerateCode() || !matched)
    show(result);
  expect(!result.canGenerateCode() && matched, name);
}
void auditNegative(const std::string &name, const std::string &source,
                   const std::string &fragment) {
  const auto result = analyze(source, lang::RawPointerAuditMode::Strict);
  const auto entry =
      std::filesystem::path(__FILE__).parent_path().parent_path() /
      "raw-borrow-test.gti";
  const bool matched = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return (d.code == "GTI-S2101" || d.code == "GTI-S2102" ||
                d.code == "GTI-S2103") &&
               d.primary.source == entry.string() &&
               d.severity == lang::DiagnosticSeverity::Error &&
               d.message.find(fragment) != std::string::npos;
      });
  if (result.canGenerateCode() || !matched)
    show(result);
  expect(!result.canGenerateCode() && matched, name);
}
lang::FrontendResult auditAnalyze(const std::string &source) {
  // Positive fixtures prove application provenance, not complete optional
  // analysis of every included generic library body. Keep dependency findings
  // visible, require mandatory acceptance, and reject any finding at the
  // application source. Negative cases independently require Strict promotion
  // at that same source rather than accidentally matching a dependency error.
  auto result = analyze(source, lang::RawPointerAuditMode::Warn);
  const auto entry =
      std::filesystem::path(__FILE__).parent_path().parent_path() /
      "raw-borrow-test.gti";
  const bool sourceFinding = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.primary.source == entry.string() &&
               (d.code == "GTI-S2101" || d.code == "GTI-S2102" ||
                d.code == "GTI-S2103");
      });
  if (sourceFinding)
    show(result);
  expect(!sourceFinding,
         "positive raw-audit fixture has no application finding");
  return result;
}
void auditDependencySeverity() {
  // The audit analyses standard-library bodies for the provenance that flows
  // into application code, but a finding located inside those compiler-trusted
  // units describes the library's own implementation. By default it is not
  // reported to an application in any mode, so Strict has nothing to promote
  // there. The opt-in restores the complete report, and Strict then promotes
  // exactly the findings Warn shows.
  const std::string source =
      "#include <std/memory>\nint main() { return 0; }\n";
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() /
      "stdlib/std/memory.gti";
  const auto libraryFindings = [&](const lang::FrontendResult &result) {
    std::vector<const lang::Diagnostic *> findings;
    for (const auto &diagnostic : result.diagnostics)
      if (diagnostic.primary.source == library.string() &&
          (diagnostic.code == "GTI-S2101" || diagnostic.code == "GTI-S2102" ||
           diagnostic.code == "GTI-S2103"))
        findings.push_back(&diagnostic);
    return findings;
  };
  const auto warned = analyze(source, lang::RawPointerAuditMode::Warn);
  const auto strict = analyze(source, lang::RawPointerAuditMode::Strict);
  if (!libraryFindings(warned).empty() || !libraryFindings(strict).empty() ||
      !warned.canGenerateCode() || !strict.canGenerateCode())
    show(strict);
  expect(libraryFindings(warned).empty() && libraryFindings(strict).empty() &&
             warned.canGenerateCode() && strict.canGenerateCode(),
         "audit findings inside compiler-trusted library units are not "
         "reported to an application by default");

  const auto warnedAll = analyze(source, lang::RawPointerAuditMode::Warn, true);
  const auto strictAll =
      analyze(source, lang::RawPointerAuditMode::Strict, true);
  const auto warnedFindings = libraryFindings(warnedAll);
  bool promoted = !warnedFindings.empty();
  for (const auto *diagnostic : warnedFindings) {
    promoted =
        promoted && diagnostic->severity == lang::DiagnosticSeverity::Warning &&
        std::any_of(strictAll.diagnostics.begin(), strictAll.diagnostics.end(),
                    [&](const auto &other) {
                      return other.primary == diagnostic->primary &&
                             other.code == diagnostic->code &&
                             other.message == diagnostic->message &&
                             other.severity == lang::DiagnosticSeverity::Error;
                    });
  }
  if (!promoted || !warnedAll.canGenerateCode() || strictAll.canGenerateCode())
    show(strictAll);
  expect(promoted && warnedAll.canGenerateCode() &&
             !strictAll.canGenerateCode(),
         "opting in reports library findings in Warn and Strict promotes "
         "exactly those findings");
}
void auditOutputParity() {
  const std::pair<std::string, std::string> cases[] = {
      {"raw field lookup does not register later methods", R"(
struct Slot {
  mut const int* pointer = nullptr;
  const int* take() mut {
    const int* previous = this.pointer;
    this.clear();
    return previous;
  }
  void clear() mut { this.pointer = nullptr; }
public:
  Slot() = default;
};
int main() {
  int owner = 7;
  mut Slot slot{};
  slot.pointer = &owner;
  const int* value = slot.take();
  unsafe { return *value - 7; }
}
)"},
      {"nested constructor fields declared after the caller", R"(
int main() {
  int live = 7;
  Wrapper wrapper{&live};
  unsafe { return *wrapper.inner.pointer - 7; }
}
struct Wrapper {
  Holder inner;
  Wrapper(const int* input) : inner(Holder(input)) {}
};
struct Holder {
  const int* pointer = nullptr;
  Holder(const int* input) : pointer(input) {}
public:
  Holder() = default;
};
)"},
      {"scalar receiver mutation with a retained address", R"(
struct Box {
  mut int value = 7;
  void bump() mut { this.value += 1; }
public:
  Box() = default;
};
int main() {
  mut Box box{};
  const int* pointer = &box.value;
  box.bump();
  unsafe { return *pointer - 8; }
}
)"},
      {"generic and public borrowed-from witnesses", R"(
#include <std/memory>
[[borrowed_from(owner)]]
unsafe T& observe<T>(const T* address, T& owner) {
  unsafe { return *address; }
}
int main() {
  int owner = 7;
  const int* address = &owner;
  unsafe {
    int& first = observe<int>(address, owner);
    int& second = std::assume_ref(address, owner);
    return first + second - 14;
  }
}
)"},
      {"checked loan endpoints in branches and loops", R"(
#include <std/memory>
int inspect(bool choose) {
  mut int owner = 7;
  const int* address = &owner;
  if (choose) {
    unsafe {
      int& view = std::assume_ref(address, owner);
      int copy = view;
      owner = copy + 1;
    }
  } else {
    int& view = owner;
    int copy = view;
    owner = copy + 2;
  }
  for (mut int i = 0; i < 2; ++i) {
    unsafe {
      int& view = std::assume_ref(address, owner);
      int copy = view;
      owner = copy + 1;
    }
  }
  return owner;
}
int main() { return inspect(true) - 10; }
)"},
      {"native pointer adaptation and checked failure schedule", R"(
extern "C" { int32_t native_read(const int32_t* address); }
int32_t quotient(int32_t divisor) { return 42 / divisor; }
int main() {
  int32_t value = 7;
  unsafe { return quotient(native_read(&value)); }
}
)"}};
  for (const auto &[name, source] : cases) {
    const auto off = analyze(source, lang::RawPointerAuditMode::Off);
    const auto warn = analyze(source, lang::RawPointerAuditMode::Warn);
    if (!off.canGenerateCode())
      show(off);
    if (!warn.canGenerateCode())
      show(warn);
    expect(off.canGenerateCode() && warn.canGenerateCode(),
           name + " is valid in both audit modes");
    if (!off.canGenerateCode() || !warn.canGenerateCode())
      continue;
    expect(lang::MirPrinter().print(off.mir) ==
               lang::MirPrinter().print(warn.mir),
           name + " has identical mandatory MIR including loans and failures");
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
      for (const auto level :
           {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
        const auto ordinary = gti_test::emitCppText(
            off, standard, lang::TargetInfo::host(), level);
        const auto audited = gti_test::emitCppText(
            warn, standard, lang::TargetInfo::host(), level);
        expect(!ordinary.empty() && ordinary == audited,
               name + " emits identical C++ with audit Off and Warn");
      }
  }
}
void canonicalConstructorFields() {
  const std::string caller = R"(
int main() { Holder value{}; unsafe { return *value.pointer; } }
)";
  const std::string declaration = R"(
struct Holder {
  mut const int* pointer = nullptr;
  Holder() { int dead = 7; this.pointer = &dead; }
};
)";
  auditNegative("later constructor field identity preserves expired output",
                caller + declaration, "scope has ended");
  auditNegative("earlier constructor field identity preserves expired output",
                declaration + caller, "scope has ended");
}
void assertionReset() {
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
[[borrowed_from(owner)]] unsafe int& observe(const int* address, int& owner) {
  unsafe { return *address; }
}
int main() { return 0; }
)"));
  auto program = parser.parse();
  const auto *function =
      lang::stmtAs<lang::FunctionDecl>(program.declarations().front().get());
  const auto *block = function
                          ? lang::stmtAs<lang::BlockStmt>(
                                function->body()->statements().front().get())
                          : nullptr;
  const auto *returned =
      block ? lang::stmtAs<lang::ReturnStmt>(block->statements().front().get())
            : nullptr;
  expect(returned != nullptr, "reset regression has a raw assertion return");
  if (!returned)
    return;
  lang::SemanticVisitor semantic;
  expect(semantic.check(program) && semantic.model().findRawReferenceAssertion(
                                        *returned->value()) != nullptr,
         "first request records its raw assertion");
  lang::Lexer nextLexer;
  lang::Parser nextParser(nextLexer.scan("int main() { return 0; }"));
  auto next = nextParser.parse();
  expect(semantic.check(next) && semantic.model().findRawReferenceAssertion(
                                     *returned->value()) == nullptr,
         "semantic request reset discards raw assertion facts");
}

void ownerViewBoundaries() {
  const auto rejected = [](const std::string &name, const std::string &source) {
    const auto result = analyze(source);
    const bool semanticError =
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto &d) {
                      return d.phase == lang::DiagnosticPhase::Semantics &&
                             d.severity == lang::DiagnosticSeverity::Error;
                    });
    if (result.canGenerateCode() || !semanticError)
      show(result);
    expect(!result.canGenerateCode() && semanticError, name);
  };
  rejected("shared bridge cannot escape local witness", R"(
#include <std/memory>
int& escape() { int owner = 7;
  unsafe { return std::assume_ref(&owner, owner); } }
int main() { return 0; }
)");
  rejected("exclusive bridge cannot escape local witness", R"(
#include <std/memory>
mut int& escape() { mut int owner = 7;
  unsafe { return std::assume_mut(&owner, owner); } }
int main() { return 0; }
)");
  rejected("shared bridge prevents conflicting owner mutation", R"(
#include <std/memory>
int main() { mut int owner = 7; unsafe {
  int& view = std::assume_ref(&owner, owner);
  owner = 8;
  return view;
} }
)");
  rejected("exclusive bridge prevents a competing checked borrow", R"(
#include <std/memory>
int main() { mut int owner = 7; const int* address = &owner; unsafe {
  mut int& view = std::assume_mut(&owner, owner);
  int& other = std::assume_ref(address, owner);
  view = 8;
  return other;
} }
)");
  const std::string ownerView = R"(
#include <std/memory>
class Box {
  mut std::storage<int> slots;
public:
  Box(std::storage<int> slots) : slots(std::move(slots)) {}
  int& value() { return this.slots.get(std::size_t(0)); }
  void release() mut { this.slots.release(); }
};
class Reading {
  Box& owner;
  const int* address = nullptr;
public:
  Reading(Box& owner) : owner(owner), address(&owner.value()) {}
  int read() { unsafe { return std::assume_ref(this.address, this.owner); } }
};
int main() {
  std::allocator allocator{};
  mut auto allocated = allocator.try_storage<int>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int> slots = std::move(allocated).value();
  slots.construct(std::size_t(0), 42);
  mut Box owner{std::move(slots)};
  Reading reading{owner};
)";
  rejected("raw-backed view prevents allocation release before last use",
           ownerView + "owner.release(); return reading.read(); }");
  rejected("raw-backed view prevents owner movement before last use",
           ownerView +
               "Box moved = std::move(owner); return reading.read(); }");
  const auto accepted = analyze(
      ownerView +
      "int result = reading.read(); owner.release(); return result - 42; }");
  if (!accepted.canGenerateCode())
    show(accepted);
  expect(accepted.canGenerateCode(),
         "raw-backed view permits release after its last checked use");
}

void bridge() {
  const auto result = analyze(R"(
#include <std/memory>
struct Witness { mut int value = 42;  public: Witness() = default; };
unsafe int& relay(const int* address, Witness& witness) {
  const int* alias = address;
  unsafe { return std::assume_ref(alias, witness); }
}
int main() {
  mut Witness owner{};
  unsafe {
    int* address = &owner.value;
    { mut int& edit = std::assume_mut(address, owner); edit = 43; }
    int& read = relay(address, owner);
    return read - 43;
  }
}
)");
  if (!result.canGenerateCode())
    show(result);
  expect(result.canGenerateCode(),
         "unrelated owner witnesses read and mutable borrowing");
  if (!result.canGenerateCode())
    return;
  std::size_t bridges = 0;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::RawBorrow ||
            instruction.intrinsic == lang::IntrinsicKind::RawBorrowMut) {
          ++bridges;
          expect(instruction.borrowArgument == 1 && instruction.loan != 0,
                 "MIR keeps exact owner argument and checked loan");
          expect(
              lang::mir_detail::rawBorrowResultLoan(function.body, instruction),
              "shared verifier/backend gate admits the exact bridge");
          for (int mutation = 0; mutation != 6; ++mutation) {
            auto forged = instruction;
            switch (mutation) {
            case 0:
              forged.borrowArgument = 0;
              break;
            case 1:
              forged.operands.pop_back();
              break;
            case 2:
              forged.loan.reset();
              break;
            case 3:
              forged.unsafeOperation = lang::UnsafeOperationKind::None;
              break;
            case 4:
              forged.info.access =
                  forged.info.access == lang::AccessMode::Mutable
                      ? lang::AccessMode::ReadOnly
                      : lang::AccessMode::Mutable;
              break;
            case 5:
              forged.parameterTypes[1] = forged.parameterTypes[0];
              break;
            }
            expect(
                !lang::mir_detail::rawBorrowResultLoan(function.body, forged),
                "bridge rejects forged witness, loan, access or authorization");
            auto malformed = result.mir;
            auto &body = const_cast<lang::MirBody &>(
                malformed.findFunctionInstance(function.id)->body);
            for (auto &changedBlock : body.blocks)
              for (auto &changed : changedBlock.instructions)
                if (changed.id == instruction.id)
                  changed = forged;
            expect(!lang::verifyMirProgram(malformed).errors.empty(),
                   "whole-program verifier rejects the malformed bridge");
          }
        }
  expect(bridges >= 2, "read and mutable bridges reach MIR");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
    expect(!gti_test::emitCppText(result, standard).empty(),
           "verified bridge has a C++20/C++23 production consumer");
}
void producedResults() {
  const auto ordinaryGeneric = auditAnalyze(R"(
#include <std/memory>
T forward_exact<T>(T input) { return input; }
int main() { mut int value = 7;
  void* erased = std::erase_address(&value);
  void* forwarded = forward_exact<void*>(erased);
  unsafe { auto restored = std::typed_address<int>(forwarded); return *restored; }
}
)");
  if (!ordinaryGeneric.canGenerateCode())
    show(ordinaryGeneric);
  expect(ordinaryGeneric.canGenerateCode(),
         "ordinary concrete generic source outputs retain usable provenance");
  auditNegative(
      "raw-output argument pack does not erase unresolved source facts",
      R"(
#include <std/memory>
T forward_exact<T, Args...>(Args... args) { return T(args...); }
int main() { mut int value = 7;
  void* erased = std::erase_address(&value);
  void* forwarded = forward_exact<void*>(erased);
  unsafe { auto wrong = std::typed_address<double>(forwarded); return int(*wrong); }
}
)",
      "completed source-result");
  auditNegative("constructor-produced local cannot become an owner assertion",
                R"(
#include <std/memory>
class Address {
public:
  mut const int* pointer = nullptr;
  Address() { int local = 1; this.pointer = &local; }
};
int main() { int witness = 7; Address address{};
  unsafe { int& value = std::assume_ref(address.pointer, witness); return value; } }
)",
                "scope has ended");
  auditNegative("callee-produced expired origin cannot become unknown", R"(
#include <std/memory>
int main() {
  int witness = 9;
  const int* pointer = dangling();
  unsafe { int& value = std::assume_ref(pointer, witness); return value; }
}
const int* dangling() { int local = 1; return &local; }
)",
                "scope has ended");
  auditNegative("callee-produced global origin requires its actual witness", R"(
#include <std/memory>
int global = 1;
int main() {
  int witness = 9;
  const int* pointer = source();
  unsafe { int& value = std::assume_ref(pointer, witness); return value; }
}
const int* source() { return &global; }
)",
                "may originate outside its checked owner witness");
  auditNegative("callee-produced aggregate field cannot hide an expired origin",
                R"(
#include <std/memory>
struct Address { const int* pointer = nullptr;  public: Address() = default; };
int main() {
  int witness = 9;
  auto address = source();
  unsafe { int& value = std::assume_ref(address.pointer, witness); return value; }
}
Address source() { int local = 1; return {.pointer = &local}; }
)",
                "scope has ended");
  auditNegative(
      "concrete generic source result preserves expired parameter storage",
      R"(
#include <std/memory>
int main() {
  int witness = 9;
  const int* pointer = source<int>(1);
  unsafe { int& value = std::assume_ref(pointer, witness); return value; }
}
const T* source<T>(T input) { return &input; }
)",
      "scope has ended");
  const auto global = auditAnalyze(R"(
#include <std/memory>
int global = 7;
int main() {
  const int* pointer = source();
  unsafe { int& value = std::assume_ref(pointer, global); return value; }
}
const int* source() { return &global; }
)");
  if (!global.canGenerateCode())
    show(global);
  expect(global.canGenerateCode(),
         "callee-produced global accepts its matching witness");
}

void repeatedSemanticRequest() {
  lang::Lexer firstLexer;
  lang::Parser firstParser(firstLexer.scan(R"(
[[borrowed_from(witness)]] unsafe int& observe(const int* pointer, int& witness) {
  unsafe { return *pointer; }
}
int main() { int value = 7;
  unsafe { int& result = observe(&value, value); return result; }
}
)"));
  auto first = firstParser.parse();
  lang::Lexer secondLexer;
  lang::Parser secondParser(secondLexer.scan("int main() { return 0; }"));
  auto second = secondParser.parse();
  lang::SemanticVisitor semantic;
  expect(semantic.check(first),
         "first reused semantic request records raw call uses");
  expect(semantic.check(second),
         "semantic request reset clears raw borrow call uses");
}

void nestedQuerySymbols() {
  auditNegative(
      "nested source queries preserve repeated and inherited symbol identities",
      R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { mut Holder out{}; outer(out);
  unsafe { return std::assume_ref(out.pointer, out); } }
void outer(mut Holder& out) { middle(out); middle(out); }
void middle(mut Holder& out) { last<int>(out, 1); last<int>(out, 2); }
void last<T>(mut Holder& out, T unused) { int gone = 1; out.pointer = &gone; }
)",
      "scope has ended");
}

void taggedRawOrigins() {
  for (const auto &source : {R"(
#include <std/memory>
expected<const int*, int> dangling() { int dead = 7; return &dead; }
int main() { auto wrapped = dangling(); if (!wrapped) { return 1; }
  unsafe { return std::assume_ref(wrapped.value(), wrapped); } })",
                             R"(
#include <std/memory>
expected<int, const int*> dangling() { int dead = 7; return unexpected(&dead); }
int main() { auto wrapped = dangling(); if (wrapped) { return 1; }
  unsafe { return *wrapped.error(); } })",
                             R"(
#include <std/memory>
struct Address { const int* pointer = nullptr;  public: Address() = default; };
expected<Address, int> dangling() { int dead = 7; Address result{.pointer = &dead}; return result; }
int main() { auto wrapped = dangling(); if (!wrapped) { return 1; }
  unsafe { return std::assume_ref(wrapped.value().pointer, wrapped); } })",
                             R"(
#include <std/memory>
struct Address { const int* pointer = nullptr;  public: Address() = default; };
enum class Event { address(Address value), empty };
Event dangling() { int dead = 7; Address result{.pointer = &dead}; return Event::address(result); }
int main() { auto event = dangling(); switch (event) {
  case Event::address(value): unsafe { return *value.pointer; }
  case Event::empty: return 0; } })",
                             R"(
#include <std/memory>
class Base { public: mut const int* pointer = nullptr;  public: Base() = default; };
class Derived : public Base { public: Derived() = default; };
Derived dangling() { int dead = 7; mut Derived result{}; result.pointer = &dead; return result; }
int main() { auto wrapped = dangling(); unsafe { return *wrapped.pointer; } })"})
    auditNegative("tagged and inherited raw fields preserve callee expiry",
                  source, "scope has ended");
  for (const auto &source : {R"(
#include <std/memory>
expected<const int*, int> address_of(int& owner) { return &owner; }
int main() { int live = 7; auto wrapped = address_of(live); if (!wrapped) { return 1; }
  unsafe { return std::assume_ref(wrapped.value(), live); } })",
                             R"(
#include <std/memory>
int main() { int live = 7; expected<const int*, int> wrapped = &live;
  unsafe { return std::assume_ref(wrapped.value(), live); } })",
                             R"(
#include <std/memory>
expected<int, const int*> address_of(int& owner) { return unexpected(&owner); }
int main() { int live = 7; auto wrapped = address_of(live); if (wrapped) { return 1; }
  unsafe { return std::assume_ref(wrapped.error(), live); } })",
                             R"(
#include <std/memory>
enum class Event { address(const int* value), empty };
Event address_of(int& owner) { return Event::address(&owner); }
int main() { int live = 7; auto event = address_of(live); switch (event) {
  case Event::address(value): unsafe { return std::assume_ref(value, live); }
  case Event::empty: return 0; } })",
                             R"(
#include <std/memory>
class Base { public: mut const int* pointer = nullptr;  public: Base() = default; };
class Derived : public Base { public: Derived() = default; };
Derived address_of(int& owner) { mut Derived result{}; result.pointer = &owner; return result; }
int main() { int live = 7; auto wrapped = address_of(live);
  unsafe { return std::assume_ref(wrapped.pointer, live); } })",
                             R"(
expected<const int*, int> dangling() { int dead = 7; return &dead; }
int main() { auto wrapped = dangling(); auto copy = wrapped;
  return copy.value() == wrapped.value() ? 0 : 1; })"}) {
    const auto result = auditAnalyze(source);
    if (!result.canGenerateCode())
      show(result);
    expect(result.canGenerateCode(),
           "tagged raw observers retain the actual owner and inert transport");
  }
}

void sourceRawWriteControlFlow() {
  const std::string globals = R"(
#include <std/memory>
int witness = 7;
mut const int* pointer = &witness;
void bad() { int gone = 1; pointer = &gone; }
void reset() { pointer = &witness; }
void unchanged() {}
)";
  for (const auto &body :
       {"if (run) { bad(); } else { reset(); }",
        "bad(); while (run) { reset(); run = false; }",
        "bad(); run ? reset() : unchanged();",
        "bad(); [[discard]] run && ((pointer = &witness) == nullptr);",
        "while (run) { unsafe { [[discard]] std::assume_ref(pointer, witness); "
        "} bad(); run = false; }"})
    auditNegative("global raw writes preserve conditional and backedge origins",
                  globals + "int probe(mut bool run) { " + body +
                      " unsafe { return std::assume_ref(pointer, witness); } } "
                      "int main() { return probe(false); }",
                  "scope has ended");
  const auto resetAfterControl = auditAnalyze(globals + R"(
int probe(bool run) { if (run) { bad(); } reset();
  unsafe { return std::assume_ref(pointer, witness); } }
int main() { return probe(true); })");
  if (!resetAfterControl.canGenerateCode())
    show(resetAfterControl);
  expect(resetAfterControl.canGenerateCode(),
         "an unconditional exact global reset after control flow stays strong");
  for (const auto &body :
       {"unsafe { bad(address); } address = &second; count -= 1;",
        "unsafe { [[discard]] std::assume_ref(second.pointer, second); "
        "bad(address); } address = &second; count -= 1;"})
    auditNegative("source raw writes follow later-iteration alias destinations",
                  R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr; };
unsafe void bad(Holder* out) { int gone = 1; unsafe { out->pointer = &gone; } }
int probe(mut int count) { mut Holder first{}; mut Holder second{};
  mut auto address = &first; while (count > 0) { )" +
                      std::string(body) + R"( }
  unsafe { return std::assume_ref(second.pointer, second); } }
int main() { return probe(2); })",
                  "scope has ended");
  auditNegative("abstract source dispatch cannot become foreign unknown writes",
                globals + R"(
class Writer { public: virtual void write() = 0;  public: Writer() = default; };
class Bad : public Writer { public: void write() override { bad(); }  public: Bad() = default; };
int probe(Writer& writer) { writer.write();
  unsafe { return std::assume_ref(pointer, witness); } }
int main() { Bad writer{}; return probe(writer); })",
                "completed source-result");
  auditNegative(
      "readonly virtual receivers preserve stored mutable raw aliases",
      R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
class Writer { public: Holder* out = nullptr;
  Writer(Holder* value) : out(value) {}
  virtual void write() { int gone = 1; unsafe { out->pointer = &gone; } }  public: Writer() = default; };
int main() { mut Holder out{}; Writer writer{&out}; writer.write();
  unsafe { return std::assume_ref(out.pointer, out); } })",
      "completed source-result");
}

void staticFieldRawWrites() {
  // A class static field is program storage like a namespace global: direct,
  // summarized and in-class writes all reach later reads of the same field.
  const std::string holder = R"(
#include <std/memory>
int witness = 7;
struct Holder {
  static mut const int* slot = &witness;
  static void keep(const int* address) { slot = address; }
  static const int* current() { return slot; }
public:
  Holder() = default;
};
)";
  const std::string setter =
      "void set() { int gone = 1; Holder::slot = &gone; }\n";
  const std::string getter = "const int* get() { return Holder::slot; }\n";
  const std::string use = R"(
int main() { set(); auto address = get();
  unsafe { int& value = std::assume_ref(address, witness); return value; } }
)";
  for (const bool earlier : {false, true})
    auditNegative("class static field raw write effects are declaration-order "
                  "independent",
                  holder +
                      (earlier ? setter + getter + use : use + setter + getter),
                  "scope has ended");
  auditNegative("a class static field retains an expired direct write",
                holder + R"(
int main() { { int gone = 1; Holder::slot = &gone; }
  auto address = Holder::slot;
  unsafe { int& value = std::assume_ref(address, witness); return value; } })",
                "scope has ended");
  auditNegative("an unqualified static member write reaches its class's reads",
                holder + R"(
int main() { { int gone = 1; Holder::keep(&gone); }
  auto address = Holder::current();
  unsafe { int& value = std::assume_ref(address, witness); return value; } })",
                "scope has ended");
  for (const auto &source : {holder + R"(
int main() { int value = 7; Holder::slot = &value; auto address = Holder::slot;
  unsafe { return std::assume_ref(address, value); } })",
                             holder + R"(
int main() { set(); Holder::keep(&witness); auto address = Holder::current();
  unsafe { return std::assume_ref(address, witness); } })" +
                                 setter}) {
    const auto result = auditAnalyze(source);
    if (!result.canGenerateCode())
      show(result);
    expect(result.canGenerateCode(),
           "live and reset class static field raw writes stay usable");
  }
}

void sourceRawWrites() {
  const std::string globals = R"(
#include <std/memory>
int witness = 7;
mut const int* pointer = &witness;
)";
  const std::string setter = "void set() { int gone = 1; pointer = &gone; }\n";
  const std::string getter = "const int* get() { return pointer; }\n";
  const std::string use = R"(
int main() { set(); auto address = get();
  unsafe { int& value = std::assume_ref(address, witness); return value; } }
)";
  for (const bool earlier : {false, true})
    auditNegative("global raw write effects are declaration-order independent",
                  globals +
                      (earlier ? setter + getter + use : use + setter + getter),
                  "scope has ended");
  for (const auto &source : {
           R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { mut Holder out{}; set(out); unsafe { return std::assume_ref(out.pointer, out); } }
void set(mut Holder& out) { int gone = 1; out.pointer = &gone; })",
           R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
mut Holder out{};
int main() { set(out); unsafe { return std::assume_ref(out.pointer, out); } }
void set(mut Holder& out) { int gone = 1; out.pointer = &gone; })",
           R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { mut Holder out{}; unsafe { set(&out); return std::assume_ref(out.pointer, out); } }
unsafe void set(Holder* out) { int gone = 1; unsafe { out->pointer = &gone; } })",
           R"(#include <std/memory>
int witness = 7; mut const int* pointer = &witness;
int main() { relay(); unsafe { return std::assume_ref(pointer, witness); } }
void relay() { set(); }
void set() { int gone = 1; pointer = &gone; })"})
    auditNegative("source-produced raw writes preserve callee expiry", source,
                  "scope has ended");
  const std::vector<std::string> valid = {
      globals + setter + getter +
          R"(int main() { auto address = get(); unsafe { return std::assume_ref(address, witness); } })",
      globals +
          R"(int main() { set(); reset(); auto address = get(); unsafe { return std::assume_ref(address, witness); } })" +
          setter + getter + "void reset() { pointer = &witness; }",
      R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { int value = 7; mut Holder out{}; set(out, value); unsafe { return std::assume_ref(out.pointer, value); } }
void set(mut Holder& out, int& input) { out.pointer = &input; })",
      R"(#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { int value = 7; mut Holder out{}; unsafe { set(&out, value); return std::assume_ref(out.pointer, value); } }
unsafe void set(Holder* out, int& input) { unsafe { out->pointer = &input; } })"};
  for (const auto &source : valid) {
    const auto result = auditAnalyze(source);
    if (!result.canGenerateCode())
      show(result);
    expect(result.canGenerateCode(), "uncalled setters and exact valid source "
                                     "writes/reset preserve usable raw facts");
  }
  auditNegative(
      "conditional reset cannot erase a previous expired global write",
      globals +
          R"(int main() { set(); reset(false); unsafe { return std::assume_ref(pointer, witness); } })" +
          setter +
          "void reset(bool choose) { if (choose) { pointer = &witness; } }",
      "scope has ended");
  for (const auto &body :
       {std::string(
            "struct Writer { Writer() { int gone = 1; pointer = &gone; } };"),
        std::string("struct Writer { ~Writer() { int gone = 1; pointer = "
                    "&gone; }  public: Writer() = default; };")}) {
    const auto result = auditAnalyze(
        globals + body +
        "int main() { unsafe { return std::assume_ref(pointer, witness); } }");
    if (!result.canGenerateCode())
      show(result);
    expect(result.canGenerateCode(),
           "unselected lifecycle bodies do not execute global raw writes");
  }
  const auto uncalledLambda = auditAnalyze(globals + R"(
int main() { auto writer = []() -> void { int gone = 1; pointer = &gone; };
  unsafe { return std::assume_ref(pointer, witness); } })");
  if (!uncalledLambda.canGenerateCode())
    show(uncalledLambda);
  expect(uncalledLambda.canGenerateCode(),
         "uncalled lambda does not execute global raw writes");
  auditNegative("selected constructor imports known callee-local writes",
                globals + R"(
struct Writer { Writer() { int gone = 1; pointer = &gone; } };
int main() { Writer writer{}; unsafe { return std::assume_ref(pointer, witness); } })",
                "scope has ended");
  auditNegative(
      "conditional constructor initializers cannot erase a known expired write",
      globals + R"(
struct Factory {
  Factory(int unused) { int gone = 1; pointer = &gone; }
  Factory(bool unused) { pointer = &witness; }
public:
  Factory() = default;
};
struct Outer { Factory field;
  Outer(bool choose) : field(choose ? Factory(0) : Factory(true)) {} };
int main() { Outer value{true}; unsafe { return std::assume_ref(pointer, witness); } })",
      "scope has ended");
  auditNegative("selected constructor imports mapped formal writes", R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
struct Writer { Writer(mut Holder& out) { int gone = 1; out.pointer = &gone; }  public: Writer() = default; };
int main() { mut Holder out{}; Writer writer{out}; unsafe { return std::assume_ref(out.pointer, out); } })",
                "scope has ended");
  auditNegative("called lambda writer retains unresolved source facts",
                globals + R"(
int main() { auto writer = []() -> void { int gone = 1; pointer = &gone; }; writer();
  unsafe { return std::assume_ref(pointer, witness); } })",
                "completed source-result");
  auditNegative("source destructor cleanup retains unresolved global writes",
                globals + R"(
struct Writer { ~Writer() { int gone = 1; pointer = &gone; }  public: Writer() = default; };
int main() { { Writer writer{}; } unsafe { return std::assume_ref(pointer, witness); } })",
                "completed source-result");
  auditNegative("source raw formal relay preserves callee expiry", R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
unsafe void set(Holder* out) { int gone = 1; unsafe { out->pointer = &gone; } }
unsafe void relay(Holder* out) { unsafe { set(out); } }
int main() { mut Holder out{}; unsafe { relay(&out); return std::assume_ref(out.pointer, out); } })",
                "scope has ended");
  auditNegative(
      "captured raw alias writer retains unresolved destination facts", R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { mut Holder out{}; auto address = &out;
  auto writer = [address]() -> void { int gone = 1; unsafe { address->pointer = &gone; } };
  writer(); unsafe { return std::assume_ref(out.pointer, out); } })",
      "completed source-result");
  auditNegative("source destructor follows stored raw aliases", R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
struct Writer { Holder* address = nullptr;
  Writer(Holder* input) : address(input) {}
  ~Writer() { int gone = 1; unsafe { this.address->pointer = &gone; } }  public: Writer() = default; };
int main() { mut Holder out{}; { Writer writer{&out}; }
  unsafe { return std::assume_ref(out.pointer, out); } })",
                "completed source-result");
  const auto exactCaptures = auditAnalyze(R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { int witness = 7; mut Holder changed{}; mut Holder untouched{.pointer = &witness};
  mut auto address = &changed;
  auto writer = [address]() -> void { int gone = 1; unsafe { address->pointer = &gone; } };
  address = &untouched; writer();
  unsafe { return std::assume_ref(untouched.pointer, witness); } })");
  if (!exactCaptures.canGenerateCode())
    show(exactCaptures);
  expect(exactCaptures.canGenerateCode(),
         "captured raw aliases use capture-time facts and do not poison "
         "unrelated holders");
  for (const auto &source : {R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
struct Alias { Holder* value = nullptr;  public: Alias() = default; };
void set(Alias alias) { int gone = 1; unsafe { alias.value->pointer = &gone; } }
int main() { mut Holder out{}; Alias alias{.value = &out}; set(alias);
  unsafe { return std::assume_ref(out.pointer, out); } })",
                             R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
struct Alias { Holder* value = nullptr;
  void set() { int gone = 1; unsafe { this.value->pointer = &gone; } }  public: Alias() = default; };
int main() { mut Holder out{}; Alias alias{.value = &out}; alias.set();
  unsafe { return std::assume_ref(out.pointer, out); } })"})
    auditNegative(
        "source writes follow stored formal and readonly receiver raw aliases",
        source, "scope has ended");
  auditNegative("source callback wrapper preserves captured raw writer effects",
                R"(
#include <std/memory>
#include <std/functional>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
int main() { mut Holder out{}; auto address = &out;
  auto writer = [address]() -> void { int gone = 1; unsafe { address->pointer = &gone; } };
  std::invoke_once(writer); unsafe { return std::assume_ref(out.pointer, out); } })",
                "completed source-result");
  const auto validStoredAlias = auditAnalyze(R"(
#include <std/memory>
struct Holder { mut const int* pointer = nullptr;  public: Holder() = default; };
struct Alias { Holder* value = nullptr;  public: Alias() = default; };
void set(Alias alias, int& witness) { unsafe { alias.value->pointer = &witness; } }
int main() { int witness = 7; mut Holder out{}; Alias alias{.value = &out}; set(alias, witness);
  unsafe { return std::assume_ref(out.pointer, witness); } })");
  if (!validStoredAlias.canGenerateCode())
    show(validStoredAlias);
  expect(validStoredAlias.canGenerateCode(),
         "exact stored formal aliases preserve valid setter origins");
}
void rawInvocationBoundaries() {
  auditNegative("derived raw fields cannot disappear behind a base reference",
                R"(
class Base { public: virtual int read() = 0;  public: Base() = default; };
class Derived : public Base {
public:
  mut const int* pointer = nullptr;
  int read() override { unsafe { return *this.pointer; } }
public:
  Derived() = default;
};
int main() {
  mut Derived value{};
  { int dead = 7; value.pointer = &dead; }
  Base& base = value;
  return base.read();
})",
                "scope has ended");
  auditNegative("foreign call cannot certify an inert source wrapper", R"(
extern "C" { int foreign_read(const int* pointer); }
int read(const int* pointer) { unsafe { return foreign_read(pointer); } }
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  return read(pointer);
})",
                "scope has ended");
  auditNegative("foreign call cannot certify an inert captured callable", R"(
extern "C" { int foreign_read(const int* pointer); }
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  auto read = [pointer]() -> int { unsafe { return foreign_read(pointer); } };
  return read();
})",
                "scope has ended");
  const std::string arrow = R"(
#include <std/memory>
struct Holder {
  mut const int* chosen = nullptr;
  mut const int* other = nullptr;
public:
  Holder() = default;
};
struct Relay {
  mut Holder inner = Holder();
  Relay(const int* chosen, const int* other) {
    this.inner.chosen = chosen;
    this.inner.other = other;
  }
  Holder& operator->() { return this.inner; }
public:
  Relay() = default;
};
Relay make(const int* live) { int dead = 7; return Relay(live, &dead); }
)";
  auditNegative("arrow result retains the selected expired raw field",
                arrow + R"(
int main() { int live = 9; Relay value = make(&live);
  unsafe { return *value->other; } }
)",
                "scope has ended");
  const auto live = auditAnalyze(arrow + R"(
int main() { int live = 9; Relay value = make(&live);
  unsafe { return std::assume_ref(value->chosen, live); } }
)");
  if (!live.canGenerateCode())
    show(live);
  expect(live.canGenerateCode(),
         "arrow projection does not join unrelated raw fields");
  auditNegative("temporary aggregate field projection preserves source expiry",
                arrow + R"(
Holder project(const int* live) { return make(live).inner; }
int main() { int live = 9; Holder result = project(&live);
  unsafe { return *result.other; } }
)",
                "scope has ended");
  const auto nullHandle = auditAnalyze(R"(
extern "C" { void close_native(void* handle); }
struct Receiver {
  mut void* handle = nullptr;
  void scalar(int value, int& read, mut int& write) mut { write = value + read; }
  void close() mut { unsafe { close_native(this.handle); } }
public:
  Receiver() = default;
};
int main() { mut Receiver value{}; int read = 1; mut int write = 0;
  value.scalar(1, read, write); value.close(); return write - 2; }
)");
  if (!nullHandle.canGenerateCode())
    show(nullHandle);
  expect(nullHandle.canGenerateCode(),
         "complete no-write query does not manufacture a raw origin");
  auditNegative(
      "ordinary checked reference results retain projected raw fields",
      arrow + R"(
Holder& view(Relay& value) { return value.inner; }
int main() { int live = 9; Relay value = make(&live);
  unsafe { return *view(value).other; } }
)",
      "scope has ended");
  const auto passive = auditAnalyze(R"(
#include <std/utility>
struct Passive { mut const int* pointer = nullptr;  public: Passive() = default; };
int main() {
  mut Passive first{};
  mut Passive second{};
  { int dead = 7; first.pointer = &dead; second.pointer = &dead; }
  std::swap(first, second);
  Passive previous = std::exchange(first, second);
  return previous.pointer == first.pointer ? 0 : 1;
})");
  if (!passive.canGenerateCode())
    show(passive);
  expect(passive.canGenerateCode(),
         "passive restoring transfer is inert address transport");
  auditNegative("source cleanup prevents an inert restoring-transfer proof", R"(
#include <std/utility>
struct Dropping {
  mut const int* pointer = nullptr;
  ~Dropping() { unsafe { int observed = *this.pointer; } }
public:
  Dropping() = default;
};
int main() {
  mut Dropping first{};
  mut Dropping second{};
  { int dead = 7; first.pointer = &dead; second.pointer = &dead; }
  std::swap(first, second);
  return 0;
})",
                "scope has ended");
}
} // namespace

int main() {
  auditOutputParity();
  auditDependencySeverity();
  canonicalConstructorFields();
  rawInvocationBoundaries();
  nestedQuerySymbols();
  sourceRawWrites();
  staticFieldRawWrites();
  sourceRawWriteControlFlow();
  taggedRawOrigins();
  repeatedSemanticRequest();
  producedResults();
  const auto publicBridge = analyze(R"(
struct Witness { mut int value = 42;  public: Witness() = default; };
[[borrowed_from(witness)]]
unsafe int& observe(const int* address, Witness& witness) {
  unsafe { return *address; }
}
[[borrowed_from(witness)]]
unsafe mut int& edit(int* address, mut Witness& witness) {
  unsafe { return *address; }
}
int main() {
  mut Witness owner{};
  int* address = &owner.value;
  unsafe {
    { mut int& value = edit(address, owner); value = 7; }
    int& value = observe(address, owner);
    return value - 7;
  }
}
)");
  if (!publicBridge.canGenerateCode())
    show(publicBridge);
  expect(publicBridge.canGenerateCode(),
         "application-defined raw bridge needs no private declaration");
  const auto receiverBridge = analyze(R"(
struct Reading {
  mut int value = 42;
  mut const int* address = nullptr;
  [[borrowed_from(this)]] int& read() { unsafe { return *this.address; } }
public:
  Reading() = default;
};
int main() {
  mut Reading owner{};
  owner.address = &owner.value;
  return owner.read() - 42;
}
)");
  if (!receiverBridge.canGenerateCode())
    show(receiverBridge);
  expect(receiverBridge.canGenerateCode(),
         "receiver contract transports the known owner of a raw field");
  const auto retainedOwner = analyze(R"(
#include <std/memory>
struct Reading { int value = 42;  public: Reading() = default; };
class View {
  const int* address = nullptr;
  Reading& owner;
public:
  View(Reading& source) : address(&source.value), owner(source) {}
  int& get() { unsafe { return std::assume_ref(this.address, this.owner); } }
};
int main() { Reading owner{}; View view{owner}; return view.get() - 42; }
)");
  if (!retainedOwner.canGenerateCode())
    show(retainedOwner);
  expect(retainedOwner.canGenerateCode(),
         "raw-backed view resolves its retained reference field to the "
         "external owner");
  auditNegative(
      "receiver raw assertion cannot conceal an unrelated field origin",
      R"(
struct Reading {
  mut const int* address = nullptr;
  [[borrowed_from(this)]] int& read() { unsafe { return *this.address; } }
public:
  Reading() = default;
};
int main() {
  int unrelated = 42;
  mut Reading owner{};
  owner.address = &unrelated;
  return owner.read();
}
)",
      "known origin");
  auditNegative("raw assertion wrapper preserves aggregate field provenance",
                R"(
struct Address { const int* value = nullptr;  public: Address() = default; };
[[borrowed_from(owner)]]
int& read(Address address, int& owner) { unsafe { return *address.value; } }
int main() {
  int owner = 42;
  int other = 7;
  Address address{.value = &other};
  return read(address, owner);
}
)",
                "known origin");
  auditNegative(
      "raw assertion obligations cannot disappear through virtual dispatch", R"(
class Provider {
public:
  [[borrowed_from(owner)]]
  virtual int& read(const int* address, int& owner) {
    unsafe { return *address; }
  }
public:
  Provider() = default;
};
int main() { return 0; }
)",
      "audit is incomplete across virtual or interface dispatch");
  const auto selectedOrigin = analyze(R"(
[[borrowed_from(left)]]
int& select(int& left, int& right) { return left; }
int main() {
  int first = 42;
  int second = 7;
  return select(first, second) - 42;
}
)");
  if (!selectedOrigin.canGenerateCode())
    show(selectedOrigin);
  expect(selectedOrigin.canGenerateCode(),
         "explicit source contract selects one of multiple reference inputs");
  negative("safe origin annotation cannot relabel another parameter", R"(
[[borrowed_from(left)]]
int& select(int& left, int& right) { return right; }
int main() { int first = 42; int second = 7; return select(first, second); }
)",
           "reference");
  negative("origin name must resolve to a declaration parameter", R"(
[[borrowed_from(missing)]]
int& select(int& value) { return value; }
int main() { int value = 42; return select(value); }
)",
           "selected borrow origin");
  bridge();
  assertionReset();
  ownerViewBoundaries();
  const auto loop = analyze(R"(
#include <std/memory>
int main() {
  int owner = 42;
  mut const int* address = &owner;
  for (mut int i = 0; i < 2; ++i) {
    address = &owner;
    unsafe { int& value = std::assume_ref(address, owner); if (value != 42) return 1; }
  }
  return 0;
}
)");
  if (!loop.canGenerateCode())
    show(loop);
  expect(loop.canGenerateCode(),
         "repaired loop address can create a checked borrow");
  auditNegative("backedge cannot change a borrow to an unrelated live owner",
                R"(
#include <std/memory>
int main() {
  int owner = 42;
  int other = 9;
  mut const int* address = &owner;
  for (mut int i = 0; i < 2; ++i) {
    unsafe { int& value = std::assume_ref(address, owner); if (value == 0) return 1; }
    address = &other;
  }
  return 0;
}
)",
                "may originate outside its checked owner witness");
  auditNegative("backedge cannot turn expired storage into a checked borrow",
                R"(
#include <std/memory>
int main() {
  int owner = 42;
  mut const int* address = &owner;
  for (mut int i = 0; i < 2; ++i) {
    unsafe { int& value = std::assume_ref(address, owner); if (value == 0) return 1; }
    { int dead = 9; address = &dead; }
  }
  return 0;
}
)",
                "scope has ended");
  negative("bridge still requires unsafe", R"(
#include <std/memory>
int main() {
  int owner = 42;
  const int* address = nullptr;
  int& value = std::assume_ref(address, owner);
  return value;
}
)",
           "unsafe");
  auditNegative("known unrelated witness is rejected", R"(
#include <std/memory>
int main() {
  int owner = 42;
  int unrelated = 7;
  unsafe {
    const int* address = &owner;
    int& value = std::assume_ref(address, unrelated);
    return value;
  }
}
)",
                "not contained");
  auditNegative("expired pointer cannot hide behind aliased wrapper", R"(
#include <std/memory>
unsafe int& relay(const int* address, int& owner) {
  const int* alias = address;
  unsafe { return std::assume_ref(alias, owner); }
}
int main() {
  mut const int* address = nullptr;
  int witness = 9;
  unsafe {
    { int dead = 42; address = &dead; }
    int& value = relay(address, witness);
    return value;
  }
}
)",
                "scope has ended");
  negative("mutable witness cannot upgrade const address", R"(
#include <std/memory>
int main() {
  mut int owner = 42;
  unsafe {
    const int* address = &owner;
    mut int& value = std::assume_mut(address, owner);
    return value;
  }
}
)",
           "parameter requires");
  return failures ? 1 : 0;
}
