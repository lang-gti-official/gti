#include "compiler_test_fixture.h"

namespace {
void checkTextOwnerPublication() {
  const auto frontend = lang::Frontend().analyze(
      "text-owner-publication.gti", R"gti(
import std.memory;
class ByteOwner {
  uint8_t& byte;
public:
  ByteOwner(uint8_t& byte) : byte(byte) {}
  uint8_t& get() { return this.byte; }
};
std::size_t view_size(ByteOwner& input) {
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&input.get()));
    std::string_view text = std::assume_string_view(first, std::size_t(1), input);
    return text.size();
  }
}
int main() {
  uint8_t byte = uint8_t(104);
  ByteOwner owner{byte};
  return int(view_size(owner)) - 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "new text retains a reference carrier without pretending to copy it");
  for (const auto &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.message << '\n';
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &loan : function.body.loans) {
      const auto *parent = function.body.findLoan(loan.parent);
      const auto *dependency = function.body.findLoan(loan.storedDependency);
      if (loan.entry || loan.kind != lang::MirLoanKind::Stored ||
          loan.carriers.empty() || !parent || !parent->entry ||
          parent->kind != lang::MirLoanKind::Parameter || !dependency ||
          !dependency->entry || dependency->kind != lang::MirLoanKind::Stored)
        continue;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        if (mutation == 0)
          body->loans[loan.id - 1].storedDependency = 0;
        if (mutation == 1)
          body->loans[loan.id - 1].parent = 0;
        if (mutation == 2)
          body->loans[dependency->id - 1].carriers.push_back(
              loan.carriers.front());
        expect(!lang::verifyMirProgram(malformed).valid(),
               "text publication rejects lost owner/shell ancestry or a forged "
               "copy of the input carrier " +
                   std::to_string(mutation));
      }
      checked++;
    }
  expect(checked == 1,
         "one local text publication has distinct physical ancestry");
  const auto escaping = lang::Frontend().analyze("text-owner-escape.gti", R"gti(
import std.memory;
class ByteOwner {
  uint8_t& byte;
public:
  ByteOwner(uint8_t& byte) : byte(byte) {}
  uint8_t& get() { return this.byte; }
};
std::string_view escape() {
  uint8_t byte = uint8_t(104);
  ByteOwner owner{byte};
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    return std::assume_string_view(first, std::size_t(1), owner);
  }
}
int main() { return int(escape().size()); }
)gti",
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(!escaping.semanticValid &&
             hasDiagnosticCode(escaping.diagnostics, "GTI-S2045") &&
             !hasDiagnosticCode(escaping.diagnostics, "GTI-B0001"),
         "a checked text view cannot escape its local reference carrier owner");
}

void checkTextReturnPublication() {
  const auto frontend = lang::Frontend().analyze(
      "text-return-publication.gti", R"gti(
import std.memory;
class ByteOwner {
  uint8_t& byte;
public:
  ByteOwner(uint8_t& byte) : byte(byte) {}
  uint8_t& get() { return this.byte; }
  std::string_view receiver_text(std::string_view unrelated) {
    std::string_view copy = unrelated;
    std::size_t observed = copy.size();
    unsafe {
      const char* first = std::typed_address<char>(std::erase_address(&this.byte));
      auto text = std::assume_string_view(first, std::size_t(1), this);
      return text;
    }
  }
  c_string receiver_c_text(c_string unrelated) {
    bool observed = unrelated == nullptr;
    unsafe {
      const char* first = std::typed_address<char>(std::erase_address(&this.byte));
      auto text = std::assume_c_string(first, this);
      return text;
    }
  }
};
std::string_view direct_text(ByteOwner& owner, std::string_view unrelated) {
  std::string_view copy = unrelated;
  std::size_t observed = copy.size();
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    return std::assume_string_view(first, std::size_t(1), owner);
  }
}
std::string_view copied_text(ByteOwner& owner, std::string_view unrelated) {
  std::string_view copy = unrelated;
  std::size_t observed = copy.size();
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    auto text = std::assume_string_view(first, std::size_t(1), owner);
    return text;
  }
}
c_string direct_c_text(ByteOwner& owner, c_string unrelated) {
  bool observed = unrelated == nullptr;
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    return std::assume_c_string(first, owner);
  }
}
c_string copied_c_text(ByteOwner& owner, c_string unrelated) {
  bool observed = unrelated == nullptr;
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    auto text = std::assume_c_string(first, owner);
    return text;
  }
}
int main() {
  uint8_t bytes[2] = {uint8_t(104), uint8_t(0)};
  uint8_t& byte = bytes[0];
  ByteOwner owner{byte};
  if (direct_c_text(owner, nullptr) == nullptr || copied_c_text(owner, nullptr) == nullptr) { return 1; }
  if (owner.receiver_text("other").size() != std::size_t(1) || owner.receiver_c_text(nullptr) == nullptr) { return 2; }
  return int(direct_text(owner, "other").size() + copied_text(owner, "other").size()) - 2;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "returned text preserves its value separately from its carrier owner");
  for (const auto &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.message << '\n';
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "direct_text" &&
         hir->source->name().lexeme != "copied_text" &&
         hir->source->name().lexeme != "direct_c_text" &&
         hir->source->name().lexeme != "copied_c_text" &&
         hir->source->name().lexeme != "receiver_text" &&
         hir->source->name().lexeme != "receiver_c_text"))
      continue;
    for (const auto &block : function.body.blocks) {
      if (!block.terminator.returnLoan)
        continue;
      for (const auto &transfer : block.instructions) {
        if (transfer.kind != lang::MirInstructionKind::Borrow ||
            transfer.loan != block.terminator.returnLoan)
          continue;
        expect(transfer.operands.size() == 1 &&
                   transfer.operands.front().value != 0,
               "text return has an explicit evaluated value witness");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        for (auto &candidate : body->blocks)
          for (auto &instruction : candidate.instructions)
            if (instruction.id == transfer.id)
              instruction.operands.front().value = 0;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "text return cannot substitute its owner representation for its "
               "value");
        const auto *retained =
            function.body.findLoan(transfer.operands.front().loan);
        lang::MirValueId unrelatedValue = 0;
        for (const auto &sourceBlock : function.body.blocks)
          for (const auto &producer : sourceBlock.instructions) {
            if (producer.kind != lang::MirInstructionKind::Load ||
                !producer.result || producer.info.type != transfer.info.type ||
                producer.operands.size() != 1)
              continue;
            const auto *place =
                function.body.findPlace(producer.operands.front().place);
            if (place && place->root == lang::MirPlaceRootKind::Binding &&
                retained &&
                std::find(retained->carriers.begin(), retained->carriers.end(),
                          place->binding) == retained->carriers.end())
              unrelatedValue = *producer.result;
          }
        expect(unrelatedValue != 0,
               "text forgery control has a different same-type value");
        if (unrelatedValue != 0) {
          auto substituted = frontend.mir;
          auto *substitutedBody = lang::findMirBody(
              substituted, {lang::MirBodyKind::Function, function.id});
          for (auto &candidate : substitutedBody->blocks) {
            for (auto &instruction : candidate.instructions)
              if (instruction.id == transfer.id)
                instruction.operands.front().value = unrelatedValue;
            if (candidate.terminator.returnLoan == transfer.loan)
              candidate.terminator.value->value = unrelatedValue;
          }
          expect(!lang::verifyMirProgram(substituted).valid(),
                 "text return rejects a same-type value unrelated to the "
                 "retained carrier");
        }
        checked++;
      }
    }
  }
  expect(checked == 6,
         "direct, local-copy and receiver string_view and c_string returns "
         "carry exact values");
}

void checkFieldReborrow() {
  const std::string declarations = R"gti(
class Read {
  int& value;
public:
  Read(int& value) : value(value) {}
  int get() { return this.value; }
};
class Write {
  mut int& value;
public:
  Write(mut int& value) : value(value) {}
  Read read() { return Read(this.value); }
  void bump() mut { this.value++; }
};
class Holder {
  mut Write value;
public:
  Holder(Write value) : value(std::move(value)) {}
  int get() { auto view = this.value.read(); return view.get(); }
};
int main() {
  mut int value = 4;
  Write writer{value};
  Holder holder{std::move(writer)};
  return holder.get() - 4;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "field-reborrow.gti", declarations, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(
      frontend.canGenerateCode(),
      "a read-only child of a nested exclusive field retains entry authority");
  for (const auto &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.message << '\n';
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &loan : function.body.loans) {
      const auto *parent = function.body.findLoan(loan.parent);
      if (loan.entry || loan.access != lang::AccessMode::ReadOnly ||
          loan.kind != lang::MirLoanKind::Stored || !parent || !parent->entry ||
          parent->kind != lang::MirLoanKind::Stored ||
          parent->access != lang::AccessMode::Mutable || loan.carriers.empty())
        continue;
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(
          malformed, {lang::MirBodyKind::Function, function.id});
      body->loans[loan.id - 1].access = lang::AccessMode::Mutable;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "a shared field reborrow cannot forge exclusive result authority");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = frontend.mir;
        auto *forgedBody = lang::findMirBody(
            forged, {lang::MirBodyKind::Function, function.id});
        if (mutation == 0)
          forgedBody->loans[loan.id - 1].parent = 0;
        if (mutation == 1)
          forgedBody->loans[loan.id - 1].source = parent->source;
        if (mutation == 2)
          for (auto &block : forgedBody->blocks)
            for (auto &instruction : block.instructions)
              if (instruction.loan == loan.id)
                instruction.borrowFields.clear();
        expect(!lang::verifyMirProgram(forged).valid(),
               "a mapped field child requires its exact parent, referent and "
               "callee field map " +
                   std::to_string(mutation));
      }
      checked++;
    }
  expect(checked == 1, "the nested field child has one exact entry parent");
  const std::string original =
      "int get() { auto view = this.value.read(); return view.get(); }";
  auto chained = declarations;
  const std::string reader = "int get() { return this.value; }";
  chained.replace(chained.find(reader), reader.size(),
                  reader + " Read subview() { return Read(this.value); }");
  chained.replace(chained.find(original), original.size(),
                  "int get() { auto view = this.value.read(); "
                  "auto child = view.subview(); return child.get(); }");
  const std::string formalChain =
      "int through_formal(mut Write& input, mut Write& unrelated) { "
      "auto view = input.read(); auto child = view.subview(); "
      "return child.get(); } ";
  chained.insert(chained.find("int main()"), formalChain);
  const auto sharedChain = lang::Frontend().analyze(
      "field-reborrow-shared-chain.gti", chained, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(
      sharedChain.canGenerateCode(),
      "a readonly subview preserves an existing exclusive-owner child chain");
  std::size_t checkedChain = 0;
  std::size_t checkedFormals = 0;
  if (sharedChain.canGenerateCode())
    for (const auto &function : sharedChain.mir.functionInstances())
      for (const auto &loan : function.body.loans) {
        const auto *parent = function.body.findLoan(loan.parent);
        if (loan.kind != lang::MirLoanKind::Stored || !parent ||
            parent->entry || parent->kind != lang::MirLoanKind::Stored ||
            parent->parent == 0)
          continue;
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = sharedChain.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          if (mutation == 0)
            body->loans[loan.id - 1].parent = 0;
          else
            body->loans[loan.id - 1].source =
                function.body.findLoan(parent->parent)->source;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "a shared subview cannot discard its parent or substitute its "
                 "owner shell");
        }
        for (const auto &formal : function.body.loans) {
          if (!formal.entry || formal.kind != lang::MirLoanKind::Parameter ||
              formal.storedDependency != parent->parent)
            continue;
          const auto unrelated = std::find_if(
              function.body.loans.begin(), function.body.loans.end(),
              [&](const auto &other) {
                return other.entry &&
                       other.kind == lang::MirLoanKind::Parameter &&
                       other.id != formal.id && other.storedDependency != 0;
              });
          expect(unrelated != function.body.loans.end(),
                 "formal-shell mutation has an independent owner control");
          if (unrelated == function.body.loans.end())
            continue;
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = sharedChain.mir;
            auto *body = lang::findMirBody(
                malformed, {lang::MirBodyKind::Function, function.id});
            if (mutation == 0)
              body->loans[formal.id - 1].source = unrelated->source;
            if (mutation == 1)
              body->loans[formal.id - 1].storedDependency =
                  unrelated->storedDependency;
            if (mutation == 2)
              body->loans[loan.id - 1].access = lang::AccessMode::Mutable;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "shared descendants require the exact formal shell, owner "
                   "dependency and readonly permission");
          }
          checkedFormals++;
        }
        checkedChain++;
      }
  expect(checkedChain != 0,
         "shared subview exercises an exact intermediate parent");
  expect(checkedFormals != 0,
         "shared subview exercises exact formal-shell authority mutations");
  auto formalConflict = chained;
  const auto formalPosition = formalConflict.find(formalChain);
  formalConflict.replace(
      formalPosition, formalChain.size(),
      "int through_formal(mut Write& input, mut Write& unrelated) { "
      "auto view = input.read(); auto child = view.subview(); "
      "input.bump(); return child.get(); } ");
  const auto conflicting = lang::Frontend().analyze(
      "field-reborrow-formal-conflict.gti", formalConflict,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!conflicting.semanticValid &&
             std::any_of(conflicting.diagnostics.begin(),
                         conflicting.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2017";
                         }),
         "a live shared descendant still excludes mutable formal access");
  auto disjoint = declarations;
  disjoint.replace(disjoint.find("mut Write value;"),
                   std::string("mut Write value;").size(),
                   "mut Write value; mut int count = 0;");
  disjoint.replace(disjoint.find(original), original.size(),
                   "int get() mut { auto view = this.value.read(); "
                   "this.count++; return view.get(); }");
  disjoint.replace(disjoint.find("Holder holder"),
                   std::string("Holder holder").size(), "mut Holder holder");
  const auto independent = lang::Frontend().analyze(
      "field-reborrow-independent.gti", disjoint, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(independent.canGenerateCode(),
         "a shared external child does not freeze unrelated owned receiver "
         "fields");
  const auto plain = lang::Frontend().analyze(
      "plain-shared-reference.gti",
      "int read(mut int& owner) { int& shared = owner; int first = owner; "
      "return first + shared; } int main() { mut int value=4; return "
      "read(value)-8; }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(plain.canGenerateCode(),
         "a plain exclusive reference can still read alongside a shared child");
  auto readonlyParent = declarations;
  readonlyParent.replace(readonlyParent.find("void bump()"), 0,
                         "int bookkeeping() { return 0; } ");
  readonlyParent.replace(
      readonlyParent.find(original), original.size(),
      "int get() { auto view = this.value.read(); "
      "int count = this.value.bookkeeping(); return view.get() + count; }");
  const auto suspended = lang::Frontend().analyze(
      "nested-readonly-parent.gti", readonlyParent, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(!suspended.semanticValid &&
             hasDiagnosticCode(suspended.diagnostics, "GTI-S2017") &&
             !hasDiagnosticCode(suspended.diagnostics, "GTI-B0001"),
         "nested carrier suspension matches the existing named-carrier "
         "readonly rule");
  auto invalid = declarations;
  invalid.replace(invalid.find(original), original.size(),
                  "int get() mut { auto view = this.value.read(); "
                  "this.value.bump(); return view.get(); }");
  invalid.replace(invalid.find("Holder holder"),
                  std::string("Holder holder").size(), "mut Holder holder");
  const auto rejected = lang::Frontend().analyze(
      "field-reborrow-conflict.gti", invalid, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(
      !rejected.semanticValid &&
          hasDiagnosticCode(rejected.diagnostics, "GTI-S2017") &&
          !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
      "an exclusive parent remains suspended while its shared child is live");

  // Issue #217: a shared local reborrow of an exclusive formal carrier (a
  // reference binding, or the range binding of `for (x : formal)`) selects
  // the formal's stored dependency for a shared mapped result. The suspended
  // formal shell is that alias's ancestor, not a competing sibling.
  auto aliased = declarations;
  aliased.replace(aliased.find("int main()"), 0,
                  "int through_alias(mut Write& input) { Write& alias = input; "
                  "auto view = alias.read(); return view.get(); } ");
  aliased.replace(aliased.find("return holder.get() - 4;"),
                  std::string("return holder.get() - 4;").size(),
                  "mut int other = 4; mut Write direct{other}; "
                  "return holder.get() + through_alias(direct) - 8;");
  const auto aliasRead = lang::Frontend().analyze(
      "formal-alias-shared-read.gti", aliased, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  for (const auto &diagnostic : aliasRead.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.message << '\n';
  expect(aliasRead.canGenerateCode() &&
             !hasDiagnosticCode(aliasRead.diagnostics, "GTI-B0001"),
         "a shared alias of an exclusive formal carrier can read its stored "
         "referent");
  std::size_t checkedAliases = 0;
  if (aliasRead.canGenerateCode())
    for (const auto &function : aliasRead.mir.functionInstances())
      for (const auto &loan : function.body.loans) {
        const auto *parent = function.body.findLoan(loan.parent);
        if (loan.entry || loan.kind != lang::MirLoanKind::Stored ||
            loan.access != lang::AccessMode::ReadOnly || !parent ||
            !parent->entry || parent->kind != lang::MirLoanKind::Stored ||
            parent->access != lang::AccessMode::Mutable)
          continue;
        const auto alias = std::find_if(
            function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &candidate) {
              return !candidate.entry &&
                     candidate.kind == lang::MirLoanKind::Local &&
                     candidate.access == lang::AccessMode::ReadOnly &&
                     candidate.storedDependency == parent->id;
            });
        if (alias == function.body.loans.end())
          continue;
        // The alias is shared: it cannot select the exclusive dependency for
        // an exclusive result, and the result must keep its exact parent.
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto forged = aliasRead.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          if (mutation == 0)
            body->loans[loan.id - 1].access = lang::AccessMode::Mutable;
          else
            body->loans[loan.id - 1].parent = 0;
          expect(!lang::verifyMirProgram(forged).valid(),
                 "a shared formal alias cannot forge exclusive or parentless "
                 "referent authority " +
                     std::to_string(mutation));
        }
        checkedAliases++;
      }
  expect(checkedAliases == 1,
         "the formal alias read has one exact shared stored child");
  auto aliasConflict = aliased;
  const std::string aliasRelay = "auto view = alias.read(); return view.get();";
  aliasConflict.replace(aliasConflict.find(aliasRelay), aliasRelay.size(),
                        "auto view = alias.read(); input.bump(); "
                        "return view.get();");
  const auto aliasRejected = lang::Frontend().analyze(
      "formal-alias-conflict.gti", aliasConflict, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(!aliasRejected.semanticValid &&
             hasDiagnosticCode(aliasRejected.diagnostics, "GTI-S2017") &&
             !hasDiagnosticCode(aliasRejected.diagnostics, "GTI-B0001"),
         "a formal stays suspended while its alias's shared child is live");
}

void checkReceiverFields() {
  const std::string declarations = R"gti(
struct Entry {
  int& key;
  mut int& value;
  Entry(int& k, mut int& v) : key(k), value(v) {}
};
class Owner {
public:
  mut int key = 1;
  mut int value = 2;
  Owner() {}
  Entry entry() mut { return Entry(this.key, this.value); }
};
)gti";
  const auto frontend = lang::Frontend().analyze(
      "receiver-field-publication.gti", declarations + R"gti(
int main() {
  mut Owner owner{};
  { mut auto entry = owner.entry(); entry.value = 7; }
  return owner.value == 7 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "a mutable receiver publishes each exact field permission");
  for (const auto &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.message << '\n';
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.receiver ||
            call.receiver->kind != lang::MirOperandKind::Value ||
            call.borrowOrigin != lang::BorrowOriginKind::Receiver ||
            call.borrowFields.size() != 2)
          continue;
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        bool changed = false;
        for (auto &candidateBlock : body->blocks)
          for (auto &stage : candidateBlock.instructions)
            if (stage.result == call.receiver->value &&
                stage.kind == lang::MirInstructionKind::CallInput &&
                stage.operands.size() == 1 &&
                stage.operands.front().kind ==
                    lang::MirOperandKind::BorrowWrite) {
              stage.operands.front().kind = lang::MirOperandKind::BorrowRead;
              changed = true;
            }
        expect(changed,
               "returned receiver field test selects its writable input");
        expect(!lang::verifyMirProgram(malformed).valid(),
               "mutable place storage alone cannot upgrade a read-only input");
        checked++;
      }
    }
  }
  expect(checked == 1,
         "one field-mapped receiver call checks input permission");
  std::string readonly = declarations;
  readonly.replace(readonly.find("entry() mut"),
                   std::string("entry() mut").size(), "entry()");
  const auto rejected = lang::Frontend().analyze(
      "readonly-field-publication.gti", readonly + "int main() { return 0; }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!rejected.semanticValid &&
             !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
         "read-only receiver cannot publish a mutable field reference");
}

void checkIndependentAccessorInputs() {
  const auto frontend = lang::Frontend().analyze(
      "independent-accessor-inputs.gti", R"gti(
struct Entry {
  int& first;
  mut int& second;
  Entry(int& first, mut int& second) : first(first), second(second) {}
};
class Slot {
  mut int value;
public:
  Slot(int value) : value(value) {}
  mut int& get() mut { return this.value; }
};
class Owner {
  mut Slot first;
  mut Slot second;
public:
  Owner() : first(Slot(1)), second(Slot(2)) {}
  Entry entry() mut { return Entry(this.first.get(), this.second.get()); }
};
int main() {
  mut Owner owner{};
  { mut auto entry = owner.entry(); entry.second = entry.first + 6; }
  { auto entry = owner.entry(); return entry.second == 7 ? 0 : 1; }
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "independent accessor inputs retain their own external authorities");
  for (const auto &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.message << '\n';
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || loan.entry ||
          loan.escapes || loan.parent == 0 ||
          loan.additionalDependencies.size() != 1 ||
          loan.additionalDependencies.front().parent == 0 ||
          loan.additionalDependencies.front().parent == loan.parent)
        continue;
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        body->loans[loan.id - 1].additionalDependencies.front().parent =
            mutation == 0 ? 0 : loan.parent;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "an additional receiver field cannot lose or conflate its "
               "accessor parent " +
                   std::to_string(mutation));
      }
      checked++;
    }
  expect(checked == 1,
         "one returned carrier protects two independent accessors");
}
} // namespace

int main() {
  checkTextOwnerPublication();
  checkTextReturnPublication();
  checkReceiverFields();
  checkFieldReborrow();
  checkIndependentAccessorInputs();
  return failures == 0 ? 0 : 1;
}
