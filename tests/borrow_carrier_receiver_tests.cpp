#include "compiler_test_fixture.h"
#include "gti/mir_printer.h"

namespace {

// Analyzes one checked-in fixture against the standard library, optionally
// under the concurrent execution profile its synchronization APIs require.
lang::FrontendResult analyzeFixture(const std::string &name, bool concurrent) {
  const auto fixture =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  lang::FrontendOptions options;
  if (concurrent) {
    options.target.executionProfile = lang::ExecutionProfile::Concurrent;
    options.target.runtimeCapabilities.threads = true;
  }
  auto result = lang::Frontend(options).analyze(
      fixture, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << name << ':' << diagnostic.primary.line << ": "
                << diagnostic.code << ' ' << diagnostic.message << '\n';
    const auto log = std::filesystem::temp_directory_path() /
                     ("gti-" + fixture.stem().string() + ".mir");
    std::ofstream output(log);
    lang::MirPrinter().print(result.mir, output);
    std::cerr << name << " MIR: " << log << '\n';
  }
  return result;
}

const lang::MirInstruction *definition(const lang::MirBody &body,
                                       lang::MirValueId id) {
  const auto *value = body.findValue(id);
  const auto *block = value ? body.findBlock(value->definitionBlock) : nullptr;
  if (block == nullptr)
    return nullptr;
  for (const auto &instruction : block->instructions)
    if (instruction.id == value->definition)
      return &instruction;
  return nullptr;
}

// The call-input stage that prepares this call's receiver.
const lang::MirInstruction *receiverStage(const lang::MirBody &body,
                                          const lang::MirInstruction &call) {
  if (!call.receiver || call.receiver->kind != lang::MirOperandKind::Value)
    return nullptr;
  const auto *stage = definition(body, call.receiver->value);
  return stage != nullptr &&
                 stage->kind == lang::MirInstructionKind::CallInput &&
                 stage->callInputRole == lang::MirCallInputRole::Receiver &&
                 stage->operands.size() == 1
             ? stage
             : nullptr;
}

// A place that reaches its storage through a stored reference field of its
// root carrier: some field projection is immediately dereferenced.
bool behindStoredReference(const lang::MirPlace &place) {
  for (std::size_t index = 1; index < place.projections.size(); ++index)
    if (place.projections[index].kind ==
            lang::MirProjectionKind::Dereference &&
        place.projections[index - 1].kind == lang::MirProjectionKind::Field)
      return true;
  return false;
}

// The entry loans a formal carrier binding holds: its stored entry, which
// describes the referents of its stored references, and its own alias.
struct FormalLoans {
  const lang::MirLoan *stored = nullptr;
  const lang::MirLoan *alias = nullptr;
};

FormalLoans formalLoans(const lang::MirBody &body, lang::HirBindingId binding) {
  FormalLoans result;
  for (const auto &loan : body.loans) {
    const auto *source = body.findPlace(loan.source);
    if (!loan.entry || source == nullptr ||
        source->root != lang::MirPlaceRootKind::Binding ||
        source->binding != binding || !source->projections.empty())
      continue;
    if (loan.kind == lang::MirLoanKind::Stored)
      result.stored = &loan;
    else if (loan.kind == lang::MirLoanKind::Parameter)
      result.alias = &loan;
  }
  return result;
}

lang::MirFunctionInstance &mutableFunction(lang::MirProgram &program,
                                           lang::HirFunctionInstanceId id) {
  return *const_cast<lang::MirFunctionInstance *>(
      program.findFunctionInstance(id));
}

// A call whose result borrows a receiver behind a stored reference field
// borrows that exact receiver place, under every carrier kind: a reference
// formal, a by-value formal, a local reference alias and a lifecycle-bound
// user carrier. Flattening it onto the binding that holds the reference
// names the wrong storage and is rejected.
void storedReferenceReceivers() {
  const auto frontend =
      analyzeFixture("stored_reference_receivers.gti", false);
  expect(frontend.canGenerateCode(),
         "results borrowed through stored reference fields construct MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t selected = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.loan ||
            call.borrowOrigin != lang::BorrowOriginKind::Receiver)
          continue;
        const auto *stage = receiverStage(body, call);
        const auto *place =
            stage ? body.findPlace(stage->operands.front().place) : nullptr;
        if (place == nullptr ||
            place->root != lang::MirPlaceRootKind::Binding ||
            !behindStoredReference(*place))
          continue;
        ++selected;
        const auto &loan = body.loans[*call.loan - 1];
        expect(loan.source == place->id,
               "a result borrowed behind a stored reference keeps the exact "
               "selected receiver place");
        const auto root = std::find_if(
            body.places.begin(), body.places.end(), [&](const auto &other) {
              return other.root == lang::MirPlaceRootKind::Binding &&
                     other.binding == place->binding &&
                     other.projections.empty();
            });
        expect(root != body.places.end(), "the carrier binding has a place");
        if (root == body.places.end())
          continue;
        auto flattened = frontend.mir;
        mutableFunction(flattened, function.id)
            .body.loans[*call.loan - 1]
            .source = root->id;
        expect(!lang::verifyMirProgram(flattened).valid(),
               "a result borrowed behind a stored reference cannot be "
               "flattened onto the carrier that holds the reference");
      }
    }
  }
  expect(selected == 9,
         "every receiver behind a stored reference field is checked (" +
             std::to_string(selected) + ")");
}

// A guard locked through a stored reference to a mutex is published from a
// shared input. Its lifetime authority is the stored entry for that
// reference: a formal's own alias reaches the mutex only as far as the
// stored reference allows, and selecting the referent through that alias
// does not make the alias a competing sibling.
void synchronizedReferenceFields() {
  const auto frontend =
      analyzeFixture("synchronized_reference_fields.gti", true);
  expect(frontend.canGenerateCode(),
         "guards locked through stored mutex references construct MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t guards = 0;
  std::size_t throughThis = 0;
  std::size_t readOnlyReferences = 0;
  std::size_t mutableAliases = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.loan ||
            call.borrowOrigin != lang::BorrowOriginKind::Receiver)
          continue;
        const auto &guard = body.loans[*call.loan - 1];
        const auto *stage = receiverStage(body, call);
        const auto *place =
            stage ? body.findPlace(stage->operands.front().place) : nullptr;
        if (guard.kind != lang::MirLoanKind::Stored || !guard.lifecycleBound ||
            guard.access != lang::AccessMode::Mutable || place == nullptr ||
            stage->callInputKind != lang::HirCallInputKind::ReadBorrow ||
            !behindStoredReference(*place))
          continue;
        ++guards;
        expect(guard.source == place->id,
               "a published guard keeps its exact shared mutex input");
        expect(guard.parent != 0 && body.loans[guard.parent - 1].entry,
               "a guard behind a stored reference depends on an entry loan");
        if (place->root == lang::MirPlaceRootKind::This) {
          expect(body.loans[guard.parent - 1].kind ==
                     lang::MirLoanKind::Stored,
                 "a guard behind a `this` reference depends on its stored "
                 "entry");
          ++throughThis;
          // A mutable stored reference and the guard's own provenance field
          // do not change how the guard is derived: without the callee's
          // exclusive publication, a mutable child of a shared input is not
          // derived from its parent.
          auto unpublished = frontend.mir;
          for (auto &changed :
               mutableFunction(unpublished, function.id).body.blocks)
            for (auto &instruction : changed.instructions)
              if (instruction.id == call.id)
                instruction.borrowAccess = lang::AccessMode::ReadOnly;
          expect(!lang::verifyMirProgram(unpublished).valid(),
                 "only a callee-published carrier takes exclusion from a "
                 "shared input");
          continue;
        }
        if (place->root != lang::MirPlaceRootKind::Binding)
          continue;
        const auto formal = formalLoans(body, place->binding);
        if (formal.stored == nullptr || formal.alias == nullptr)
          continue;
        // The alias parents the guard while its access reaches through
        // every stored reference on the way; a read-only reference leaves
        // only that reference's stored entry.
        if (formal.stored->access != lang::AccessMode::ReadOnly) {
          expect(guard.parent ==
                     (formal.alias->access == lang::AccessMode::Mutable
                          ? formal.alias->id
                          : formal.stored->id),
                 "a formal's mutable alias parents a guard behind its mutable "
                 "stored reference; a read-only alias leaves the stored entry");
          continue;
        }
        expect(guard.parent == formal.stored->id,
               "a formal's guard behind a read-only stored reference depends "
               "on that reference's stored entry, not on the alias");
        ++readOnlyReferences;
        if (formal.alias->access != lang::AccessMode::Mutable)
          continue;
        ++mutableAliases;
        // The alias's mutable access never reaches through a read-only
        // stored reference, so it cannot parent the guard; selecting the
        // mutex through that alias does not make the alias a competing
        // sibling of the guard either.
        auto throughAlias = frontend.mir;
        auto &aliased =
            mutableFunction(throughAlias, function.id).body.loans[*call.loan - 1];
        aliased.parent = formal.alias->id;
        aliased.storedDependency = formal.alias->storedDependency;
        expect(!lang::verifyMirProgram(throughAlias).valid(),
               "a mutable alias cannot parent a guard behind a read-only "
               "stored reference");
      }
    }
  }
  expect(guards == 9 && throughThis == 2 && readOnlyReferences == 3 &&
             mutableAliases == 2,
         "every guard behind a stored mutex reference is checked (" +
             std::to_string(guards) + ", " + std::to_string(throughThis) +
             ", " + std::to_string(readOnlyReferences) + ", " +
             std::to_string(mutableAliases) + ")");
}

// A confined callable that its callee returns as the exact call result, as
// `std::for_each` returns its visitor, carries its capture group out under
// that call's result loan. The return must be the exact transported
// parameter.
void returnedConfinedCallables() {
  const auto frontend =
      analyzeFixture("returned_confined_callables.gti", false);
  expect(frontend.canGenerateCode(),
         "confined callables returned as exact results construct MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t returned = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.loan ||
            !call.functionTarget ||
            call.borrowOrigin != lang::BorrowOriginKind::Argument)
          continue;
        const auto *target =
            frontend.mir.findFunctionInstance(*call.functionTarget);
        const auto index = call.borrowArgument;
        const auto contract =
            target ? std::find_if(target->callableParameters.begin(),
                                  target->callableParameters.end(),
                                  [&](const auto &candidate) {
                                    return candidate.parameterIndex == index;
                                  })
                   : std::vector<lang::MirCallableParameter>::const_iterator{};
        if (target == nullptr || contract == target->callableParameters.end() ||
            !contract->ownedTransport ||
            contract->ownedTransport->kind !=
                lang::CallableOwnedTransportKind::ExactReturn)
          continue;
        ++returned;
        const auto contractIndex =
            static_cast<std::size_t>(contract - target->callableParameters.begin());
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto &callee = mutableFunction(malformed, target->id);
          if (mutation == 0)
            callee.returnBorrowParameter = index + 1;
          if (mutation == 1)
            callee.callableParameters[contractIndex].ownedTransport->kind =
                lang::CallableOwnedTransportKind::Value;
          if (mutation == 2)
            callee.callableParameters[contractIndex]
                .ownedTransport->destinationType = lang::SemanticType::Unknown;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "a returned confined callable needs its exact transported "
                 "return contract");
        }
      }
    }
  }
  expect(returned == 5, "every returned confined callable is checked (" +
                            std::to_string(returned) + ")");
}

} // namespace

int main() {
  storedReferenceReceivers();
  synchronizedReferenceFields();
  returnedConfinedCallables();
  return failures == 0 ? 0 : 1;
}
