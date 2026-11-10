// Frozen C++ backend: C++ MIR body emitter admission of borrow publication
// (forged origins, addresses and carrier selections must not reach C++
// text) and the C++ backend's documented limits. The backend-neutral MIR
// contracts for the same programs live in borrow_origin_tests.cpp.
#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_lowering.h"

namespace {
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
    // A reference selected through an accessor call relays that call's loan
    // without an address operand; only a direct local publication has an
    // address to forge. The runtime test checks these referents.
    const std::string_view name = relative;
    if (name != "fixtures/fallible_borrow.gti" &&
        !name.starts_with("coverage/reproducers/a25-"))
      expect(checked != 0,
             "fixture exercises exact local reference publication");
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
        // A binding place's symbol names its source binding, which is formal
        // root evidence only where a returned reference publishes the
        // address; a carrier's dependency is proved by binding identity.
        if (mutation == 2 &&
            function.returnType.kind != lang::SemanticType::Reference)
          continue;
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

void checkNestedReadonlyReferences() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::size_t publications = 0;
  // The reproducers only read through accessor pairs, which then select their
  // read-only forms; the fixture's `mut`-only operator[] keeps a mutable
  // parent loan under its read-only child.
  for (const auto &[relative, mutableParent] :
       {std::pair{"coverage/reproducers/"
                  "a59-readonly-ref-into-mut-nested-container.gti",
                  false},
        std::pair{"coverage/reproducers/a59-map-at-then-element-ref.gti",
                  false},
        std::pair{"fixtures/borrow_nested_readonly.gti", true}}) {
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
    expect(mutableParent ? checked != 0 : checked == 0,
           mutableParent
               ? "fixture exercises suspended parent provenance publication"
               : "a read-only use of an accessor pair keeps no mutable parent "
                 "loan");
  }
  std::cout << "nested references: " << publications << " publications, "
            << publications * 7
            << " mutations checked by MIR and C++ admission\n";
}

// A reference selected from a payload enum held in a local carrier sources a
// place rooted at the carrier's field-less Stored dependency. That place is
// provenance: the address comes from the exact PayloadExtract. Admission must
// reject it once the selection loses its extract or the place is evaluated.
void checkLocalCarrierSelections() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrowed_enum_local_selection.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "references select from local payload carriers");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "local carrier selection MIR verifies");
  try {
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "local carrier selections reach the production backend");
  } catch (const std::logic_error &error) {
    expect(false,
           std::string("local carrier selection emission: ") + error.what());
  }
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t owners = 0;
  std::size_t evaluated = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &place : function.body.places) {
      const auto *stored = place.root == lang::MirPlaceRootKind::Loan
                               ? function.body.findLoan(place.loan)
                               : nullptr;
      const auto selection =
          std::find_if(function.body.loans.begin(), function.body.loans.end(),
                       [&](const lang::MirLoan &loan) {
                         return loan.source == place.id &&
                                loan.referenceSelection &&
                                loan.carriers.size() == 1;
                       });
      if (stored == nullptr || stored->kind != lang::MirLoanKind::Stored ||
          stored->storedField != 0 || selection == function.body.loans.end())
        continue;
      ++owners;
      expect(supportsSelectedBodyText(frontend.mir, rows, function.id),
             "a provenance-only carrier owner is representable");
      // The selected binding's referent read, redirected to the owner place.
      const auto read = [&](const lang::MirInstruction &instruction) {
        const auto *operand =
            instruction.operands.size() == 1
                ? function.body.findPlace(instruction.operands.front().place)
                : nullptr;
        return instruction.kind == lang::MirInstructionKind::Load &&
               operand != nullptr &&
               operand->root == lang::MirPlaceRootKind::Binding &&
               operand->binding == selection->carriers.front() &&
               operand->projections.size() == 1 &&
               operand->projections.front().kind ==
                   lang::MirProjectionKind::Dereference &&
               operand->type == place.type;
      };
      for (unsigned mutation = 0; mutation < 2; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = &const_cast<lang::MirFunctionInstance *>(
                          malformed.findFunctionInstance(function.id))
                          ->body;
        bool changed = mutation == 0;
        if (mutation == 0)
          body->loans[selection->id - 1].referenceSelection.reset();
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions)
            if (mutation == 1 && !changed && read(instruction)) {
              instruction.operands.front().place = place.id;
              changed = true;
            }
        if (!changed)
          continue;
        evaluated += mutation;
        expect(!supportsSelectedBodyText(malformed, rows, function.id),
               "a carrier owner place cannot become a runtime address " +
                   std::to_string(mutation));
      }
    }
  expect(owners >= 10 && evaluated >= 10,
         "fixture exercises call-result, constructed, parameter, generic, "
         "class-field and mutable local carriers");
  std::cout << "local carrier selections: " << owners << " owners, "
            << owners + evaluated << " mutations checked by C++ admission\n";
}

// A switch over a payload enum reached through a call's reference result
// reads that result's place after the subject's full expression. Its
// call-result loans stay live across the Switch and end inside each arm;
// ending one before the Switch is a use after the borrow ends.
void checkReferenceCallSwitchSubjects() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrowed_enum_reference_subjects.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "switches observe payload enums through reference results");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "reference-call switch subject MIR verifies");
  try {
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "reference-call switch subjects reach the production backend");
  } catch (const lang::BackendUnsupportedError &error) {
    // The frozen C++ backend cannot represent a text call result that
    // carries a borrow dependency and says so (GTI-B0003); LLVM executes it.
    expect(std::string(error.what())
                   .find("borrow-dependent text call results require the LLVM "
                         "backend") != std::string::npos,
           std::string(
               "reference-call switch emission: unexpected C++ limitation: ") +
               error.what());
  } catch (const std::logic_error &error) {
    expect(false,
           std::string("reference-call switch emission: ") + error.what());
  }
}
} // namespace

int main() {
  checkReferents();
  checkProjectedParameterOrigins();
  checkDirectProjectedReturnOrigins();
  checkOwnedFieldPlaces();
  checkNestedReadonlyReferences();
  checkLocalCarrierSelections();
  checkReferenceCallSwitchSubjects();
  return failures == 0 ? 0 : 1;
}
