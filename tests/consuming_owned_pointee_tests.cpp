#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
void checkNamedBorrowRejection() {
  const std::string text = R"gti(
class Ticket {
  int value = 42;
public:
  Ticket() = default;
  int& view() { return this.value; }
};
class Envelope {
  mut Ticket ticket{};
public:
  Envelope() = default;
  int finish() && { return std::move(this.ticket).view(); }
};
int main() { Envelope value{}; return std::move(value).finish(); }
)gti";
  const auto frontend =
      analyzeWithStandardLibrary("owned-named-borrow.gti", text);
  const auto diagnostic =
      std::find_if(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                   [](const auto &item) { return item.code == "GTI-S2022"; });
  expect(!frontend.canGenerateCode() &&
             diagnostic != frontend.diagnostics.end(),
         "named retained invocation rejects even immediately copied borrowed "
         "results");
  if (diagnostic == frontend.diagnostics.end())
    return;
  const auto callOffset = text.find(".view()") + 6;
  const auto declarationOffset = text.find("view()");
  expect(diagnostic->primary.start == callOffset &&
             diagnostic->primary.end == callOffset + 1 &&
             diagnostic->related.size() == 1 &&
             diagnostic->related.front().span.source ==
                 diagnostic->primary.source &&
             diagnostic->related.front().span.start == declarationOffset &&
             diagnostic->related.front().span.end == declarationOffset + 4 &&
             diagnostic->message.find("cannot return a reference") !=
                 std::string::npos &&
             !diagnostic->hints.empty() && diagnostic->fixes.empty(),
         "named borrowed-result rejection retains source spans, declaration, "
         "and ownership advice");
}

void checkInlineProjection() {
  const auto frontend = analyzeWithStandardLibrary("owned-inline.gti", R"gti(
import std.polymorphic;
interface Command { int operator()(int value) &&; };
class Increment : public Command {
public:
  Increment() = default;
  int operator()(int value) && override { return value + 1; }
};
int main() {
  std::inline_polymorphic<Command, 32, 8> value{Increment()};
  return std::move(value).invoke_once<int>(41);
}
)gti");
  expect(frontend.canGenerateCode(),
         "inline owning projection passes frontend");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.ownedPointeeProjection ||
        function.ownedPointeeProjection->borrowIntrinsic !=
            lang::IntrinsicKind::PolymorphicStorageReadMut)
      continue;
    ++checked;
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto forged = frontend.mir;
      auto *accessor = const_cast<lang::MirFunctionInstance *>(
          forged.findFunctionInstance(function.id));
      if (mutation == 0)
        accessor->ownedPointeeProjection->borrowIntrinsic =
            lang::IntrinsicKind::UniqueOwnerBorrowMut;
      else if (mutation == 1)
        accessor->ownedPointeeProjection->fields.clear();
      else if (mutation == 2)
        accessor->receiverMutability = lang::ReceiverMutability::ReadOnly;
      else
        accessor->body.blocks.front().terminator.returnLoan.reset();
      expect(
          !lang::verifyMirProgram(forged).valid(),
          "inline projection must prove storage identity, access, and return");
    }
  }
  expect(checked == 1, "one inline owning accessor is concretely verified");
  std::size_t roots = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &input : block.instructions) {
        const auto *owner =
            input.retainedOwnerPlace
                ? function.body.findPlace(*input.retainedOwnerPlace)
                : nullptr;
        if (owner == nullptr || !owner->projections.empty())
          continue;
        ++roots;
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto forged = frontend.mir;
          auto *caller = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          if (mutation == 0)
            caller->receiverMutability = lang::ReceiverMutability::ReadOnly;
          else
            const_cast<lang::MirPlace *>(caller->body.findPlace(owner->id))
                ->key->receiver = false;
          expect(!lang::verifyMirProgram(forged).valid(),
                 "root consumption requires exact consuming-this authority");
        }
      }
  expect(roots == 1, "inline invocation consumes exactly one root owner");
}

const std::string source = R"gti(
import std.memory;
interface Operation { int operator()(int value) &&; };
class Increment : public Operation {
public:
  int operator()(int value) && override { return value + 1; }
public:
  Increment() = default;
};
void observe(Operation& value) {}
class Owner {
  mut std::unique_ptr<Operation> operation;
  mut std::unique_ptr<Operation> spare{nullptr};
public:
  Owner(std::unique_ptr<Operation> operation)
      : operation(std::move(operation)) {}
  int operator()(int value) && {
    return std::move(*this.operation)(value);
  }
};
int main() {
  auto concrete = std::make_unique<Increment>();
  auto erased = std::upcast_unique<Operation, Increment>(std::move(concrete));
  Owner owner{std::move(erased)};
  return std::move(owner)(4) - 5;
}
)gti";

lang::MirInstruction *instructionWithId(lang::MirBody &body,
                                        lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.id == id)
        return &instruction;
  return nullptr;
}

void checkMutations() {
  const auto frontend =
      analyzeWithStandardLibrary("consuming-owned-pointee.gti", source);
  expect(frontend.canGenerateCode(),
         "owned-pointee immediate consuming invocation passes frontend");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "unmodified consuming-owned-pointee MIR is independently valid");
  std::size_t count = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &input : block.instructions) {
        if (!input.retainedOwnerPlace)
          continue;
        ++count;
        const auto *referent =
            function.body.findPlace(input.operands.front().place);
        const auto *anchor = function.body.findPlace(*input.retainedOwnerPlace);
        expect(input.kind == lang::MirInstructionKind::CallInput &&
                   input.callInputKind ==
                       lang::HirCallInputKind::RetainedReceiver &&
                   referent != nullptr &&
                   referent->root == lang::MirPlaceRootKind::Loan &&
                   referent->projections.empty() && anchor != nullptr &&
                   anchor->root == lang::MirPlaceRootKind::This &&
                   input.ownership && input.ownership->place == anchor->key &&
                   input.lifecycle.empty() && !input.destination &&
                   !input.preparedParameterDrop,
               "retained pointee carries executable loan and separate owner "
               "anchor without transfer");
        if (referent == nullptr || anchor == nullptr)
          continue;
        for (int mutation = 0; mutation != 18; ++mutation) {
          auto malformed = frontend.mir;
          auto *caller = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *stage = instructionWithId(caller->body, input.id);
          auto *ownerPlace = const_cast<lang::MirPlace *>(
              caller->body.findPlace(*stage->retainedOwnerPlace));
          auto *loanPlace = const_cast<lang::MirPlace *>(
              caller->body.findPlace(stage->operands.front().place));
          auto *loan = const_cast<lang::MirLoan *>(
              caller->body.findLoan(loanPlace->loan));
          lang::MirInstruction *accessorCall = nullptr;
          lang::MirInstruction *invocation = nullptr;
          for (auto &candidateBlock : caller->body.blocks)
            for (auto &candidate : candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Call &&
                  candidate.loan == loan->id)
                accessorCall = &candidate;
              if (candidate.receiver &&
                  candidate.receiver->kind == lang::MirOperandKind::Value &&
                  candidate.receiver->value == stage->result)
                invocation = &candidate;
            }
          expect(accessorCall != nullptr && invocation != nullptr,
                 "mutation fixture retains accessor and invocation");
          if (accessorCall == nullptr || invocation == nullptr)
            continue;
          auto *accessor = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(*accessorCall->functionTarget));
          switch (mutation) {
          case 0:
            stage->retainedOwnerPlace.reset();
            break;
          case 1:
            stage->retainedOwnerPlace = loanPlace->id;
            break;
          case 2:
            ownerPlace->access = lang::AccessMode::ReadOnly;
            break;
          case 3:
            stage->ownership->place.projections.clear();
            break;
          case 4:
            stage->ownership->after = lang::OwnershipStateSet::Available;
            break;
          case 5:
            loan->escapes = true;
            break;
          case 6:
            loan->parent = loan->id;
            break;
          case 7:
            loan->access = lang::AccessMode::ReadOnly;
            break;
          case 8:
            loanPlace->projections.push_back(ownerPlace->projections.front());
            break;
          case 9:
            accessor->ownedPointeeProjection.reset();
            break;
          case 10:
            accessor->ownedPointeeProjection->borrowIntrinsic =
                lang::IntrinsicKind::UniqueOwnerBorrow;
            break;
          case 11:
            accessorCall->borrowOrigin = lang::BorrowOriginKind::Argument;
            break;
          case 12:
            caller->receiverMutability = lang::ReceiverMutability::ReadOnly;
            break;
          case 13:
            stage->info.traits.containsBorrowedState = true;
            break;
          case 14:
            invocation->retainedOwnerPlace = ownerPlace->id;
            break;
          case 15: {
            auto *owner = const_cast<lang::MirClassInstance *>(
                malformed.findClassInstance(*caller->owner));
            for (auto &field : owner->declaredFields)
              if (field.symbol == ownerPlace->projections.front().field)
                field.access = lang::AccessMode::ReadOnly;
            break;
          }
          case 16: {
            auto *owner = const_cast<lang::MirClassInstance *>(
                malformed.findClassInstance(*caller->owner));
            const auto other = std::find_if(
                owner->declaredFields.begin(), owner->declaredFields.end(),
                [&](const auto &field) {
                  return field.symbol !=
                             ownerPlace->projections.front().field &&
                         field.type == ownerPlace->type;
                });
            expect(other != owner->declaredFields.end(),
                   "same-type sibling owner exists");
            if (other != owner->declaredFields.end()) {
              auto impostor = *ownerPlace;
              impostor.id = caller->body.places.size() + 1;
              impostor.projections.front().field = other->symbol;
              impostor.key->projections.front().field = other->symbol;
              stage->retainedOwnerPlace = impostor.id;
              stage->ownership->place = *impostor.key;
              caller->body.places.push_back(std::move(impostor));
            }
            break;
          }
          case 17:
            invocation->operands.push_back(stage->operands.front());
            invocation->parameterTypes.push_back(stage->info.type);
            break;
          }
          (void)lang::rebuildMirValueUses(caller->body);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "forged owned-pointee retention must fail independently");
        }
      }
    }
  }
  expect(count == 1, "fixture has one owned-pointee retained stage");
}

void checkRejectedSource() {
  const std::string original = "return std::move(*this.operation)(value);";
  const std::vector<std::string> replacements{
      "mut Operation& alias = *this.operation; int result = "
      "std::move(*this.operation)(value); observe(alias); return result;",
      "auto saved = std::move(*this.operation); return "
      "std::move(saved)(value);",
      "int first = std::move(*this.operation)(value); return "
      "std::move(*this.operation)(first);",
      "int first = std::move(*this.operation)(value); auto saved = "
      "std::move(this.operation); return first;",
      "mut Operation& reference = *this.operation; return "
      "std::move(reference)(value);",
      "auto local = std::move(this.operation); return "
      "std::move(*local)(value);",
  };
  for (const auto &replacement : replacements) {
    auto negative = source;
    negative.replace(negative.find(original), original.size(), replacement);
    const auto result =
        analyzeWithStandardLibrary("rejected-owned-pointee.gti", negative);
    expect(result.syntaxValid && !result.semanticValid,
           "aliases, extraction, second invocation, and owner reuse remain "
           "semantic errors");
  }
  const auto raw =
      analyzeWithStandardLibrary("raw-pointee-consumption.gti", R"gti(
interface Operation { int operator()(int value) &&; };
int invoke(Operation* pointer, int value) {
  unsafe { return std::move(*pointer)(value); }
}
int main() { return 0; }
)gti");
  expect(raw.syntaxValid && !raw.semanticValid,
         "raw pointer dereference does not confer owned-pointee consumption");
}

void checkPassiveFreshClosureStaging() {
  const auto frontend =
      analyzeWithStandardLibrary("passive-fresh-wrapper.gti", R"gti(
import std.functional;
import std.string;
int main() {
  std::move_only_function<void() &&> empty{[]() -> void {}};
  std::move(empty)();
  int identity = 9;
  std::move_only_function<void() &&> captured{
      [identity]() -> void { std::println(identity); }};
  std::move(captured)();
  std::function<int() &&> scalar{[]() -> int { return 4; }};
  return std::move(scalar)() - 4;
}
)gti");
  expect(frontend.canGenerateCode(),
         "fresh passive callback constructors pass frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  std::size_t staged = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.info.type.kind != lang::SemanticType::Lambda ||
            input.info.traits.drop != lang::DropKind::Trivial ||
            !input.preparedParameterDrop || !input.destination ||
            input.operands.size() != 1 ||
            input.operands.front().kind != lang::MirOperandKind::Value)
          continue;
        const auto *value =
            function.body.findValue(input.operands.front().value);
        if (value == nullptr)
          continue;
        const lang::MirInstruction *producer = nullptr;
        for (const auto &candidateBlock : function.body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.id == value->definition)
              producer = &candidate;
        if (producer == nullptr ||
            producer->operation != lang::MirOperation::Closure)
          continue;
        ++staged;
        const auto *drop =
            function.body.findDropObligation(*input.preparedParameterDrop);
        expect(producer->lifecycle.empty() && input.lifecycle.size() == 1 &&
                   input.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize &&
                   input.lifecycle.front().source == 0 &&
                   input.lifecycle.front().target ==
                       input.preparedParameterDrop &&
                   drop != nullptr && !drop->dropType.requiresActiveCleanup,
               "passive fresh closure initializes prepared storage without a "
               "source lifetime");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *stage = instructionWithId(changed->body, input.id);
          if (mutation == 0)
            stage->lifecycle.front().kind =
                lang::MirLifecycleEventKind::Reparent;
          else if (mutation == 1)
            stage->lifecycle.front().target = 0;
          else
            stage->preparedParameterDrop.reset();
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "passive prepared publication cannot forge ownership or skip "
                 "its slot proof");
        }
      }
    }
  }
  expect(staged == 3,
         "fixture has three directly prepared cleanup-free closures");
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(
        cpp.find("fresh closure already occupies its prepared argument slot") !=
            std::string::npos,
        "fresh closure remains staged before constructor invocation");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
}

void checkAccessorSpellings() {
  auto grouped = source;
  const std::string original = "std::move(*this.operation)(value)";
  grouped.replace(grouped.find(original), original.size(),
                  "(std::move(((*this.operation))))(value)");
  expect(
      analyzeWithStandardLibrary("grouped-owned-pointee.gti", grouped)
          .canGenerateCode(),
      "grouping does not lose the exact immediate owned accessor capability");

  const std::string named = R"gti(
import std.memory;
interface Operation { int operator()(int value) &&; };
class Transparent<T> {
  mut gti_internal::unique_owner<T> resource = nullptr;
public:
  mut T& access() mut { return gti_internal::unique_owner_borrow_mut(this.resource); }
public:
  Transparent() = default;
};
class Holder {
  mut Transparent<Operation> operation{};
public:
  int operator()(int value) && { return std::move(this.operation.access())(value); }
public:
  Holder() = default;
};
int invoke(Holder holder) { return std::move(holder)(3); }
int main() { return 0; }
)gti";
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto path = root / "named-owned-accessor.gti";
  const std::string text =
      "import \"std/detail/memory_capabilities.gti\"; " + named;
  const auto key = std::filesystem::weakly_canonical(path).string();
  const auto frontend = lang::Frontend().analyze(
      path, text, {root / "prelude.gti", path}, {{key, text}}, {root});
  expect(frontend.canGenerateCode(), "named transparent source accessor works "
                                     "without a unique_ptr name check");
  bool retained = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        retained = retained || instruction.retainedOwnerPlace.has_value();
  expect(retained,
         "named accessor fixture exercises concrete retained-owner MIR");
}

void checkRetainedMemberPackRepresentation() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/retained_owner_member_pack.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "retained owner supports exact empty/scalar/closure member packs");
  if (!frontend.canGenerateCode())
    return;
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(
        cpp.find("_accept__gti_mir_failure<std::int32_t>") !=
                std::string::npos &&
            cpp.find("_accept__gti_mir_failure<std::int32_t, std::int32_t>") !=
                std::string::npos,
        "retained owner member packs spell exact passive template arguments");
    for (const auto arity : {0, 1, 2, 3})
      expect(cpp.find("if constexpr (sizeof...(Args) == " +
                      std::to_string(arity) + ")") != std::string::npos,
             "pack selector guards indexed identities by exact arity before "
             "type formation");
    expect(cpp.find("std::tuple_element_t<2, std::tuple<Args...>>") !=
               std::string::npos,
           "mixed-arity fixture actually exercises a third-element closure "
           "selector");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
}

void checkMaterializedPackReceiver() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/lambda_pack_invocation.gti";
  std::ifstream input(path);
  const std::string text{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), text);
  expect(frontend.canGenerateCode(),
         "local closure pack invocation has valid MIR");
  if (!frontend.canGenerateCode())
    return;
  try {
    expect(
        !gti_test::emitCppText(frontend).empty(),
        "materialized pack receiver is not mistaken for a pending SSA borrow");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  int checked = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.callInputRole != lang::MirCallInputRole::Receiver ||
            stage.callInputKind != lang::HirCallInputKind::ReadBorrow ||
            stage.info.type.kind != lang::SemanticType::Lambda)
          continue;
        ++checked;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          auto *changed = instructionWithId(*body, stage.id);
          if (mutation == 0)
            changed->callSite = 0;
          else if (mutation == 1)
            changed->callInputRole = lang::MirCallInputRole::Argument;
          else
            changed->operands.front().kind = lang::MirOperandKind::Copy;
          expect(!lang::verifyMirProgram(forged).valid() &&
                     !lang::CppMirBodyEmitter(forged, rows)
                          .supportsFailureBodyText(
                              {lang::MirBodyKind::Function, function.id}),
                 "materialized receiver exemption requires exact call-site, "
                 "receiver role and borrow operation");
        }
      }
  expect(checked == 2, "empty and nonempty pack receivers exercise the proof");
}
} // namespace

int main() {
  checkNamedBorrowRejection();
  checkInlineProjection();
  checkMutations();
  checkRejectedSource();
  checkPassiveFreshClosureStaging();
  checkAccessorSpellings();
  checkRetainedMemberPackRepresentation();
  checkMaterializedPackReceiver();
  if (failures != 0)
    return 1;
  std::cout << "consuming owned-pointee proof tests passed\n";
  return 0;
}
