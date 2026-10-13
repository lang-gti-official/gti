#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_lowering.h"

namespace {
void checkStaticReturnAlternatives() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrow_static_return_alternative.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "static text and null alternatives compose with borrowed returns");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "mixed static and dependent return MIR verifies");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "mixed return alternatives reach the production backend");
  std::size_t literals = 0;
  std::size_t dependent = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.returnBorrowOrigin == lang::BorrowOriginKind::None)
      continue;
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value ||
          block.terminator.value->kind != lang::MirOperandKind::Value)
        continue;
      const auto *value =
          function.body.findValue(block.terminator.value->value);
      if (!value)
        continue;
      if (block.terminator.returnLoan) {
        ++dependent;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &candidate : changed->body.blocks)
          if (candidate.id == block.id)
            candidate.terminator.returnLoan.reset();
        for (auto &candidate : changed->body.values)
          if (candidate.id == value->id)
            candidate.info.traits.containsBorrowedState = false;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "clearing borrowed metadata cannot replace an escaping loan");
      } else {
        ++literals;
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions)
              if (candidate.id == value->definition) {
                if (mutation == 0)
                  candidate.literal.reset();
                if (mutation == 1)
                  candidate.literal = std::uint64_t(0);
                if (mutation == 2)
                  candidate.info.traits.containsBorrowedState = true;
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "ownerless returns require an exact static literal witness");
        }
      }
    }
  }
  expect(literals >= 5 && dependent >= 5,
         "receiver, parameter and unsafe raw relay alternatives are exercised");
  const auto invalid =
      analyzeWithStandardLibrary("invalid-static-return-alternative.gti", R"gti(
#include <std/string>
std::string_view Dangling(bool empty) {
  if (empty) { return ""; }
  std::string local{"expired"};
  return local.view();
}
int main() { return 0; }
)gti");
  expect(!invalid.semanticValid && !invalid.canGenerateCode(),
         "a static alternative does not permit returning a local borrow");
  expect(std::none_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      }),
         "invalid mixed return fails in semantics, not MIR/backend");
}

void checkReferents() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  for (const auto &relative :
       {"fixtures/borrow_return_exact_referent.gti",
        "fixtures/borrow_projected_parameter_return.gti",
        "fixtures/borrow_direct_projected_return.gti",
        "fixtures/fallible_borrow.gti",
        "coverage/reproducers/a25-expected-ref-relay-wrong-referent.gti",
        "coverage/reproducers/a25-ref-return-from-expected-value.gti"}) {
    const auto path = root / relative;
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    expect(input.good() || input.eof(), "referent fixture is readable");
    auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "exact borrowed referent is publishable: " + std::string(relative));
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "exact referent MIR independently verifies");
    try {
      expect(!gti_test::emitCpp(frontend).contents.empty(),
             "exact referent reaches production representation");
    } catch (const std::logic_error &error) {
      expect(false, std::string("exact referent emission: ") + error.what());
    }
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Borrow ||
              !instruction.loan || instruction.operands.size() != 1 ||
              instruction.operands.front().place == 0 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan)
            continue;
          const auto *published = function.body.findLoan(*instruction.loan);
          const auto *retained =
              function.body.findLoan(instruction.operands.front().loan);
          if (!published || published->kind != lang::MirLoanKind::Return ||
              !retained || retained->kind != lang::MirLoanKind::Parameter)
            continue;
          ++checked;
          for (unsigned mutation = 0; mutation < 5; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.operands.front().place = 0;
                if (mutation == 1)
                  candidate.operands.front().place = retained->source;
                if (mutation == 2)
                  candidate.operands.front().loan = published->id;
                if (mutation == 3)
                  changed->body.loans[published->id - 1].access =
                      lang::AccessMode::Mutable;
                if (mutation == 4)
                  changed->body.loans[published->id - 1].source =
                      candidate.operands.front().place;
              }
            expect(
                !lang::verifyMirProgram(malformed).valid(),
                "borrow return rejects forged address, authority, or access");
            expect(
                !lang::CppMirBodyEmitter(malformed, rows)
                     .supportsFailureBodyText(
                         {lang::MirBodyKind::Function, function.id}),
                "representation admission rejects forged borrow publication");
          }
        }
    if (std::string(relative) != "fixtures/fallible_borrow.gti")
      expect(checked != 0,
             "fixture exercises exact local reference publication");
  }
}

void checkInvalidEscapes() {
  const std::string declarations = R"gti(
enum class errc { missing };
class Item {
  mut int32_t value_;
public:
  Item(int32_t value) : value_(value) {}
  int32_t read() { return this.value_; }
  void set(int32_t value) mut { this.value_ = value; }
};
expected<Item&, errc> relay(Item& item) {
  Item& alias = item;
  return alias;
}
)gti";
  for (const auto &body :
       {"expected<Item&, errc> bad() { Item local{1}; return local; } "
        "int main() { return 0; }",
        "int main() { mut Item owner{1}; Item& alias = relay(owner).value(); "
        "owner.set(2); return alias.read(); }",
        "int main() { Item owner{1}; Item& alias = relay(owner).value(); "
        "alias.set(2); return alias.read(); }"}) {
    const auto frontend = analyzeWithStandardLibrary(
        "invalid-borrow-publication.gti", declarations + body);
    expect(
        frontend.syntaxValid && !frontend.semanticValid &&
            !frontend.canGenerateCode(),
        "local escape and writable use of a shared reference remain invalid");
    expect(std::none_of(frontend.diagnostics.begin(),
                        frontend.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           "invalid source is rejected before MIR/backend generation");
  }
}

void checkProjectedParameterOrigins() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrow_projected_parameter_return.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "projected parameter and captured-reference origins publish exactly");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t publications = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *child = instruction.loan
                                ? function.body.findLoan(*instruction.loan)
                                : nullptr;
        const auto *parameter =
            child ? function.body.findLoan(child->parent) : nullptr;
        const auto *dependency =
            child ? function.body.findPlace(child->source) : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.receiver || !child || !parameter || !dependency ||
            child->kind != lang::MirLoanKind::CallResult ||
            parameter->kind != lang::MirLoanKind::Parameter ||
            child->source == parameter->source)
          continue;
        ++publications;
        for (unsigned mutation = 0; mutation < 6; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = &const_cast<lang::MirFunctionInstance *>(
                            malformed.findFunctionInstance(function.id))
                            ->body;
          auto &changedLoan = body->loans[child->id - 1];
          auto &changedPlace = body->places[dependency->id - 1];
          if (mutation == 0)
            changedLoan.source = parameter->source;
          if (mutation == 1)
            changedPlace.binding = parameter->carriers.back();
          if (mutation == 2)
            changedPlace.symbol = 0;
          if (mutation == 3)
            changedLoan.parent = 0;
          if (mutation == 4)
            changedLoan.producedBy = 0;
          if (mutation == 5)
            changedPlace.projections.back().kind =
                lang::MirProjectionKind::RawDereference;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "projected parameter rejects forged physical origin " +
                     std::to_string(mutation));
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "representation rejects forged projected origin " +
                     std::to_string(mutation));
        }
      }
  expect(publications == 2,
         "plain and fallible projected-parameter returns have origin controls");
  std::cout << "projected parameter origins: " << publications * 6
            << " mutations checked by MIR and C++ admission\n";
}

void checkDirectProjectedReturnOrigins() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrow_direct_projected_return.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "direct field aliases return their exact caller-owned referent");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t changedAddresses = 0;
  std::size_t changedSuffixes = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *published = instruction.loan
                                    ? function.body.findLoan(*instruction.loan)
                                    : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Borrow ||
            !published || published->kind != lang::MirLoanKind::Return ||
            instruction.operands.size() != 1)
          continue;
        const auto &operand = instruction.operands.front();
        const auto *retained = function.body.findLoan(operand.loan);
        const auto *runtime = function.body.findPlace(operand.place);
        if (!retained || retained->kind != lang::MirLoanKind::Parameter ||
            !runtime || runtime->root != lang::MirPlaceRootKind::Binding)
          continue;
        for (const auto &other : function.body.places) {
          if (other.root != lang::MirPlaceRootKind::Binding ||
              other.binding == runtime->binding ||
              other.type != runtime->type || other.projections.size() != 1 ||
              other.projections.front().kind !=
                  lang::MirProjectionKind::Dereference ||
              std::find(retained->carriers.begin(), retained->carriers.end(),
                        other.binding) == retained->carriers.end())
            continue;
          auto malformed = frontend.mir;
          auto &body = const_cast<lang::MirFunctionInstance *>(
                           malformed.findFunctionInstance(function.id))
                           ->body;
          for (auto &candidateBlock : body.blocks) {
            for (auto &candidate : candidateBlock.instructions)
              if (candidate.id == instruction.id)
                candidate.operands.front().place = other.id;
            if (candidateBlock.terminator.returnLoan == published->id &&
                candidateBlock.terminator.value)
              candidateBlock.terminator.value->place = other.id;
          }
          (void)lang::rebuildMirValueUses(body);
          ++changedAddresses;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "return cannot substitute another same-type alias of the "
                 "same owner for the selected source expression");
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "representation rejects substituted return aliases");
        }
      }
    for (const auto &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          place.projections.size() < 2 ||
          place.projections.front().kind !=
              lang::MirProjectionKind::Dereference ||
          place.projections.back().kind != lang::MirProjectionKind::Field)
        continue;
      const bool executedAddress = std::any_of(
          function.body.blocks.begin(), function.body.blocks.end(),
          [&](const auto &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind ==
                             lang::MirInstructionKind::Initialize &&
                         std::any_of(instruction.operands.begin(),
                                     instruction.operands.end(),
                                     [&](const auto &operand) {
                                       return operand.kind ==
                                                  lang::MirOperandKind::Loan &&
                                              operand.place == place.id;
                                     });
                });
          });
      if (!executedAddress)
        continue;
      for (unsigned mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         malformed.findFunctionInstance(function.id))
                         ->body;
        auto &changed = body.places[place.id - 1];
        if (mutation == 0)
          changed.projections.back().field = 0;
        if (mutation == 1)
          changed.projections.back().kind =
              lang::MirProjectionKind::RawDereference;
        if (mutation == 2)
          changed.symbol = 0;
        (void)lang::rebuildMirValueUses(body);
        ++changedSuffixes;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "projected return rejects forged field paths or formal roots " +
                   std::to_string(mutation) + " at " +
                   std::to_string(function.id) + ":" +
                   std::to_string(place.id));
        expect(!lang::CppMirBodyEmitter(malformed, rows)
                    .supportsFailureBodyText(
                        {lang::MirBodyKind::Function, function.id}),
               "representation rejects forged executed field paths");
      }
    }
  }
  expect(changedAddresses != 0 && changedSuffixes != 0,
         "direct projection regressions exercise alias and field mutations");
  const auto declarations = source.substr(0, source.find("int main()"));
  for (const auto &body :
       {"int32_t& bad() { Table local{}; return plain(local); } "
        "int main() { return 0; }",
        "int main() { mut Table table{}; int32_t& held = plain(table); "
        "table.record.selected = 7; return held; }",
        "int main() { Table table{}; mut int32_t& held = plain(table); "
        "held = 7; return held; }",
        "mut int32_t& bad(mut Table& table) { "
        "mut Record& record = table.record; "
        "mut int32_t& selected = record.selected; return selected; } "
        "int main() { return 0; }"}) {
    const auto invalid = analyzeWithStandardLibrary(
        "invalid-direct-projected-return.gti", declarations + body);
    expect(invalid.syntaxValid && !invalid.semanticValid &&
               !invalid.canGenerateCode(),
           "direct projected aliases retain escape and readonly restrictions");
    expect(std::none_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           "invalid projected reference uses fail before MIR/backend");
  }
  std::cout << "Direct projected returns: " << changedAddresses
            << " alias mutations, " << changedSuffixes << " suffix mutations\n";
}

void checkOwnedFieldPlaces() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  for (const auto &relative :
       {"coverage/reproducers/a58-ref-to-owner-field-pointee-via-local-ref.gti",
        "coverage/reproducers/a58-map-unique-ptr-value-deref.gti",
        "fixtures/borrow_owner_alias_field.gti"}) {
    const auto path = root / relative;
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "owning field pointee through a reference");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "owned field borrow verifies");
    try {
      expect(!gti_test::emitCpp(frontend).contents.empty(),
             "owned field borrow emits");
    } catch (const std::logic_error &error) {
      expect(false, error.what());
    }
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              instruction.operands.size() != 1)
            continue;
          const auto &operand = instruction.operands.front();
          const auto *address = function.body.findPlace(operand.place);
          const auto *owner = function.body.findLoan(operand.loan);
          if (operand.kind != lang::MirOperandKind::Loan || !address ||
              !owner || address->root != lang::MirPlaceRootKind::Loan ||
              address->loan == owner->id)
            continue;
          ++checked;
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &candidate : changedBlock.instructions)
              if (candidate.id == instruction.id)
                candidate.operands.front().place = owner->source;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "borrow initializer cannot substitute owner for referent");
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "representation independently rejects the wrong address");
        }
    expect(checked != 0,
           "owned field fixture retains a distinct accessor address");
  }
}

void checkLoweringFailureContext() {
  const auto frontend = analyze("lowering-context.gti", R"gti(
struct Item { mut int value = 0; };
int main() {
  Item item{};
  Item& alias = item;
  return alias.value;
}
)gti");
  expect(frontend.canGenerateCode(), "lowering context mutation starts valid");
  const auto *entry =
      frontend.sourceGraph.findUnit(frontend.sourceGraph.entryUnit());
  auto malformed = frontend.hir;
  lang::HirFunctionInstanceId owner = 0;
  for (const auto &function : malformed.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    owner = function.id;
    auto *changed = const_cast<lang::HirFunctionInstance *>(
        malformed.findFunctionInstance(function.id));
    for (auto &loan : changed->body.loans)
      loan.parent = loan.semanticLoan;
  }
  const auto rejected =
      lang::MirLowerer().lower(malformed, frontend.failureMetadata);
  expect(!rejected.valid() && !rejected.errors.empty(),
         "lowering retains its rejection before hosted startup");
  for (const auto &error : rejected.errors) {
    expect(error.body.kind == lang::MirBodyKind::Function &&
               error.body.owner == owner,
           "lowering error retains its exact concrete body identity");
    expect(
        entry && error.source && error.source->source == entry->path.string() &&
            error.source->end > error.source->start && error.source->line > 1,
        "lowering rejection retains an enclosing source statement");
    expect(error.message.find("hosted-startup") == std::string::npos,
           "lowering context reports its cause, not absent startup");
  }
}

void checkNestedReadonlyReferences() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::size_t publications = 0;
  for (const auto &relative :
       {"coverage/reproducers/a59-readonly-ref-into-mut-nested-container.gti",
        "coverage/reproducers/a59-map-at-then-element-ref.gti",
        "fixtures/borrow_nested_readonly.gti"}) {
    const auto path = root / relative;
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "nested read-only reference captures the exact child address");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "nested read-only loan flow independently verifies");
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "nested read-only reference reaches production emission");
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              !instruction.destination || instruction.operands.size() != 1)
            continue;
          const auto &operand = instruction.operands.front();
          const auto *destination =
              function.body.findPlace(*instruction.destination);
          const auto *address = function.body.findPlace(operand.place);
          const auto *retained = function.body.findLoan(operand.loan);
          const auto *child =
              address && address->root == lang::MirPlaceRootKind::Loan
                  ? function.body.findLoan(address->loan)
                  : nullptr;
          if (operand.kind != lang::MirOperandKind::Loan || !destination ||
              destination->type.kind != lang::SemanticType::Reference ||
              !retained || retained->access != lang::AccessMode::Mutable ||
              !child || child->parent != retained->id ||
              child->access != lang::AccessMode::ReadOnly)
            continue;
          ++checked;
          ++publications;
          for (unsigned mutation = 0; mutation < 7; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.operands.front().place = retained->source;
                if (mutation == 1)
                  candidate.operands.front().loan = child->id;
                if (mutation == 2)
                  changed->body.loans[child->id - 1].access =
                      lang::AccessMode::Mutable;
                if (mutation == 3)
                  changed->body.loans[child->id - 1].parent = 0;
                if (mutation == 4)
                  changed->body.places[destination->id - 1]
                      .type.referenceAccess = lang::AccessMode::Mutable;
                if (mutation == 5)
                  candidate.kind = lang::MirInstructionKind::Load;
                if (mutation == 6)
                  changed->body.places[address->id - 1].type =
                      lang::SemanticType::Bool;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "nested reference rejects forged provenance, type, access "
                   "or parent-use operation " +
                       std::to_string(mutation));
            expect(!lang::CppMirBodyEmitter(malformed, rows)
                        .supportsFailureBodyText(
                            {lang::MirBodyKind::Function, function.id}),
                   "representation rejects forged nested reference " +
                       std::to_string(mutation));
          }
        }
    expect(checked != 0,
           "fixture exercises suspended parent provenance publication");
  }
  std::ifstream declarationsInput(root / "fixtures/borrow_nested_readonly.gti");
  const std::string fixture{std::istreambuf_iterator<char>(declarationsInput),
                            {}};
  const auto entry = fixture.find("int main()");
  expect(entry != std::string::npos, "nested negative fixture has an entry");
  if (entry != std::string::npos) {
    const std::string declarations = fixture.substr(0, entry);
    for (const auto &body :
         {"int main() { mut Table table{}; Cell& row = table[std::size_t(0)]; "
          "int32_t& value = row[std::size_t(0)]; table.cell.selected = 5; "
          "return value; }",
          "int main() { mut Table table{}; Cell& row = table[std::size_t(0)]; "
          "int32_t& value = row[std::size_t(0)]; value = 5; return value; }",
          "int main() { mut Table table{}; Cell& row = table[std::size_t(0)]; "
          "int32_t& value = row[std::size_t(0)]; mut int32_t& writable = "
          "value; writable = 5; return value; }"}) {
      const auto invalid = analyzeWithStandardLibrary(
          "invalid-nested-reference.gti", declarations + body);
      expect(invalid.syntaxValid && !invalid.semanticValid &&
                 !invalid.canGenerateCode(),
             "nested references retain owner/access restrictions");
      expect(std::none_of(invalid.diagnostics.begin(),
                          invalid.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code.starts_with("GTI-B");
                          }),
             "invalid nested access is rejected before MIR/backend");
    }
  }
  std::cout << "nested references: " << publications << " publications, "
            << publications * 7
            << " mutations checked by MIR and C++ admission\n";
}
} // namespace

int main() {
  checkReferents();
  checkStaticReturnAlternatives();
  checkInvalidEscapes();
  checkProjectedParameterOrigins();
  checkDirectProjectedReturnOrigins();
  checkOwnedFieldPlaces();
  checkLoweringFailureContext();
  checkNestedReadonlyReferences();
  return failures == 0 ? 0 : 1;
}
