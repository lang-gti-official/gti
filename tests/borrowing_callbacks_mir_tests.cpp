#include "../src/compiler/mir_verification_queries.h"
#include "compiler_test_fixture.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <iostream>
#include <string_view>

namespace {
void expireLoanBefore(lang::MirBody &body, lang::MirLoanId loan,
                      lang::MirInstructionId consumer) {
  lang::MirInstructionId last = 0;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      last = std::max(last, instruction.id);
  for (auto &block : body.blocks) {
    const auto position = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const auto &instruction) { return instruction.id == consumer; });
    if (position != block.instructions.end()) {
      block.instructions.insert(position,
                                {.id = last + 1,
                                 .kind = lang::MirInstructionKind::EndBorrow,
                                 .loan = loan});
      return;
    }
  }
}

bool callResultAliasProofTests() {
  using namespace lang;
  MirBody original;
  original.entry = 1;
  SemanticType reference = SemanticType::Reference;
  reference.arguments = {SemanticType::Int32};
  original.places.push_back({.id = 1,
                             .root = MirPlaceRootKind::Binding,
                             .binding = 1,
                             .type = reference});
  original.loans.push_back({.id = 1,
                            .kind = MirLoanKind::CallResult,
                            .source = 1,
                            .producedBy = 1,
                            .carriers = {1}});
  MirInstruction producer;
  producer.id = 1;
  producer.kind = MirInstructionKind::Call;
  producer.hirValue = 1;
  producer.loan = 1;
  producer.result = 1;
  producer.info.type = SemanticType::Int32;
  producer.info.category = ValueCategory::Place;
  MirInstruction initialize;
  initialize.id = 2;
  initialize.kind = MirInstructionKind::Initialize;
  initialize.destination = 1;
  initialize.operands = {
      {.kind = MirOperandKind::Loan, .loan = 1, .type = SemanticType::Int32}};
  MirBlock block;
  block.id = 1;
  block.reachable = true;
  block.instructions = {producer, initialize};
  block.terminator.kind = MirTerminatorKind::Return;
  original.blocks.push_back(block);
  const auto proof = [](const MirBody &body, std::size_t position) {
    return mir_detail::callResultReferenceAliases(
        body, body.loans.front(), body.blocks.front(), position);
  };
  if (!proof(original, 2)) {
    std::cerr << "exact call-result reference alias proof failed\n";
    return false;
  }
  auto runtimeAlias = original;
  runtimeAlias.places.push_back({.id = 2,
                                 .root = MirPlaceRootKind::Loan,
                                 .loan = 1,
                                 .type = SemanticType::Int32});
  runtimeAlias.blocks.front().instructions[1].operands.front().place = 2;
  if (!proof(runtimeAlias, 2)) {
    std::cerr << "exact runtime call-result alias proof failed\n";
    return false;
  }
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto changed = runtimeAlias;
    auto &referent = changed.places.back();
    if (mutation == 0)
      referent.loan = 2;
    else if (mutation == 1)
      referent.type = SemanticType::Bool;
    else if (mutation == 2)
      referent.projections.push_back({.kind = MirProjectionKind::Dereference});
    else
      referent.access = AccessMode::Mutable;
    if (proof(changed, 2)) {
      std::cerr << "accepted forged runtime call-result alias " << mutation
                << '\n';
      return false;
    }
  }
  for (int mutation = 0; mutation != 17; ++mutation) {
    auto changed = original;
    auto &instructions = changed.blocks.front().instructions;
    std::size_t consumer = 2;
    switch (mutation) {
    case 0:
      consumer = 1;
      break; // Consumer precedes alias initialization.
    case 1:
      std::swap(instructions[0], instructions[1]);
      break;
    case 2: // Root and operand agree, but not with the actual producer type.
      changed.places.front().type.arguments.front() = SemanticType::Bool;
      instructions[1].operands.front().type = SemanticType::Bool;
      break;
    case 3:
      changed.places.front().type.referenceAccess = AccessMode::Mutable;
      break;
    case 4:
      instructions[1].operands.front().loan = 2;
      break;
    case 5:
      instructions.push_back(initialize);
      consumer = 3;
      break;
    case 6:
      changed.blocks.front().terminator.targetArguments = {
          {.kind = MirOperandKind::Copy, .place = 1, .type = reference}};
      break;
    case 7:
      instructions.push_back({.id = 3,
                              .operands = {{.kind = MirOperandKind::Address,
                                            .place = 1,
                                            .type = reference}}});
      consumer = 3;
      break;
    case 8:
      instructions[0].hirValue = 2;
      break;
    case 9:
      instructions[0].info.access = AccessMode::Mutable;
      break;
    case 10:
      instructions.push_back(producer);
      consumer = 3;
      break;
    case 11:
      changed.loans.front().carriers.push_back(1);
      break;
    case 12:
      changed.loans.front().carriers.clear();
      break;
    case 13:
      consumer = 3;
      break;
    case 14:
      changed.blocks.front().terminator.switchTargets = {
          {.target = 1,
           .arguments = {
               {.kind = MirOperandKind::Copy, .place = 1, .type = reference}}}};
      break;
    case 15:
      instructions.push_back(initialize);
      instructions.back().kind = MirInstructionKind::Assign;
      consumer = 3;
      break;
    case 16:
      instructions.push_back(
          {.id = 3,
           .kind = MirInstructionKind::Move,
           .operands = {
               {.kind = MirOperandKind::Move, .place = 1, .type = reference}}});
      consumer = 3;
      break;
    }
    if (proof(changed, consumer)) {
      std::cerr << "accepted forged call-result alias proof " << mutation
                << '\n';
      return false;
    }
  }
  auto acrossBlocks = original;
  acrossBlocks.blocks.front().terminator.kind = MirTerminatorKind::Goto;
  acrossBlocks.blocks.front().terminator.target = 2;
  MirBlock consumer;
  consumer.id = 2;
  consumer.reachable = true;
  consumer.terminator.kind = MirTerminatorKind::Return;
  acrossBlocks.blocks.push_back(consumer);
  if (!mir_detail::callResultReferenceAliases(acrossBlocks,
                                              acrossBlocks.loans.front(),
                                              acrossBlocks.blocks.back(), 0)) {
    std::cerr << "dominating call-result reference alias proof failed\n";
    return false;
  }
  // The initializer on only one branch must not authorize the join.
  acrossBlocks.blocks.front().instructions.pop_back();
  acrossBlocks.blocks.front().terminator.kind = MirTerminatorKind::Branch;
  acrossBlocks.blocks.front().terminator.elseTarget = 3;
  acrossBlocks.blocks[1].instructions = {initialize};
  acrossBlocks.blocks[1].terminator.kind = MirTerminatorKind::Goto;
  acrossBlocks.blocks[1].terminator.target = 3;
  consumer.id = 3;
  acrossBlocks.blocks.push_back(consumer);
  if (mir_detail::callResultReferenceAliases(acrossBlocks,
                                             acrossBlocks.loans.front(),
                                             acrossBlocks.blocks.back(), 0)) {
    std::cerr << "accepted nondominating call-result alias initialization\n";
    return false;
  }
  return true;
}

bool referenceInitializerBodyProofTests() {
  using namespace lang;
  const auto analyzed =
      Frontend().analyze("reference-initializer-body-proof.gti", R"gti(
int global_value = 3;
int& independent() { return global_value; }
int& relay(int& value) { return value; }
class View {
  int& value;
public:
  View(int& value) : value(relay(value)) {}
  int read() { return this.value; }
};
int main() {
  int owner = 7;
  View view{owner};
  return view.read() + independent() - 10;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  const MirFunctionInstance *global = nullptr;
  for (const auto &function : analyzed.mir.functionInstances())
    if (function.returnBorrowOrigin == BorrowOriginKind::Global &&
        function.parameterTypes.empty())
      global = &function;
  if (!global)
    return false;
  bool checked = false;
  for (const auto &constructor : analyzed.mir.constructorInstances())
    for (const auto &initializer : constructor.initializers) {
      const auto *call = mir_detail::referenceInitializerCall(
          analyzed.mir, constructor, initializer);
      if (!call)
        continue;
      const auto *original =
          analyzed.mir.findFunctionInstance(*call->functionTarget);
      if (!original || original->returnType != global->returnType ||
          original->parameterBindings.size() != 1)
        return false;
      const auto formal = std::find_if(
          original->body.places.begin(), original->body.places.end(),
          [&](const auto &place) {
            return place.root == MirPlaceRootKind::Binding &&
                   place.projections.empty() &&
                   place.binding == original->parameterBindings.front();
          });
      if (formal == original->body.places.end())
        return false;
      auto changed = analyzed.mir;
      auto *helper = const_cast<MirFunctionInstance *>(
          changed.findFunctionInstance(original->id));
      helper->body = global->body;
      helper->body.placeDomain = original->body.placeDomain;
      for (auto &place : helper->body.places)
        if (place.key)
          place.key->domain = helper->body.placeDomain;
      for (auto &block : helper->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.ownership)
            instruction.ownership->place.domain = helper->body.placeDomain;
      auto unusedFormal = *formal;
      unusedFormal.id = helper->body.places.size() + 1;
      helper->body.places.push_back(unusedFormal);
      // A genuinely valid global-returning body, not a dangling/unknown place.
      // The incoming formal is unused; signature and claimed parameter-origin
      // contract are deliberately unchanged from the real relay.
      const auto bodyProof = verifyMirBody(helper->body, helper->id);
      if (!bodyProof.valid()) {
        for (const auto &error : bodyProof.errors)
          std::cerr << "non-relay mutation invalid locally: " << error.message
                    << '\n';
        return false;
      }
      const auto *unchangedConstructor =
          changed.findConstructorInstance(constructor.id);
      if (!mir_detail::referenceInitializerCall(changed, *unchangedConstructor,
                                                initializer)) {
        std::cerr
            << "non-relay mutation changed the declared initializer shape\n";
        return false;
      }
      const auto programProof = verifyMirProgram(changed);
      if (std::none_of(
              programProof.errors.begin(), programProof.errors.end(),
              [&](const auto &error) {
                return error.bodyKind == MirBodyKind::Function &&
                       error.owner == helper->id &&
                       error.message.find(
                           "argument-dependent return does not originate") !=
                           std::string::npos;
              })) {
        std::cerr << "non-relay helper was not rejected by its actual "
                     "return-origin contract\n";
        for (const auto &error : programProof.errors)
          std::cerr << error.message << '\n';
        return false;
      }
      checked = true;
    }
  return checked;
}

bool namedRestorationReturnTests() {
  using namespace lang;
  const auto application = [](const std::string &name,
                              const std::string &source) {
    const auto entry = std::filesystem::temp_directory_path() / name;
    return Frontend().analyze(
        entry, source, {standardLibraryPrelude()},
        {{std::filesystem::weakly_canonical(entry).string(), source}},
        {standardLibraryRoot()});
  };
  const auto analyzed = application("named-restoration-return-proof.gti", R"gti(
#include <std/memory>
struct Reader {
  int& value;
  Reader(int& source) : value(source) {}
  int read() { return this.value; }
};
struct Writer {
  mut int& value;
  Writer(mut int& source) : value(source) {}
  void increment() mut { this.value += 1; }
};
unsafe Reader& restore_reader(std::erased_object& object) {
  unsafe { Reader& restored = std::restore_ref<Reader>(object); return restored; }
}
unsafe mut Writer& restore_writer(mut std::erased_object& object) {
  unsafe { mut Writer& restored = std::restore_mut<Writer>(object); return restored; }
}
int main() {
  int value = 9;
  Reader reader{value};
  std::erased_object& object = std::erase_ref(reader);
  unsafe {
    Reader& restored = restore_reader(object);
    if (restored.read() != 9) return 1;
  }
  mut int count = 4;
  mut Writer writer{count};
  mut std::erased_object& mutable_object = std::erase_mut(writer);
  unsafe {
    mut Writer& restored = restore_writer(mutable_object);
    restored.increment();
  }
  return count - 5;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &function : analyzed.mir.functionInstances()) {
      if (verifyMirBody(function.body, function.id).valid())
        continue;
      std::cerr << "restore body " << function.id
                << " summary=" << function.restoresReferenceResult << '\n';
      for (const auto &loan : function.body.loans)
        std::cerr << " loan " << loan.id << " kind=" << int(loan.kind)
                  << " source=" << loan.source << " parent=" << loan.parent
                  << " stored=" << loan.storedDependency
                  << " entry=" << loan.entry << " producer=" << loan.producedBy
                  << " carriers=" << loan.carriers.size() << '\n';
      for (const auto &place : function.body.places)
        std::cerr << " place " << place.id << " root=" << int(place.root)
                  << " binding=" << place.binding
                  << " type=" << int(place.type.kind)
                  << " projections=" << place.projections.size()
                  << " available=" << place.initiallyAvailable << '\n';
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.loan || instruction.destination) {
            std::cerr << " instruction " << instruction.id
                      << " kind=" << int(instruction.kind)
                      << " loan=" << instruction.loan.value_or(0)
                      << " hir=" << instruction.hirValue
                      << " restore=" << instruction.restoresReferenceResult
                      << " fields=" << instruction.restoredBorrowFields.size()
                      << " dest=" << instruction.destination.value_or(0)
                      << '\n';
            for (const auto &operand : instruction.operands)
              std::cerr << "  operand kind=" << int(operand.kind)
                        << " loan=" << operand.loan
                        << " place=" << operand.place
                        << " value=" << operand.value
                        << " type=" << int(operand.type.kind) << '\n';
          }
    }
    return false;
  }
  std::size_t checked = 0;
  for (const auto &function : analyzed.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *output = instruction.loan
                                 ? function.body.findLoan(*instruction.loan)
                                 : nullptr;
        const auto *input =
            instruction.operands.size() == 1 &&
                    instruction.operands.front().kind == MirOperandKind::Loan
                ? function.body.findLoan(instruction.operands.front().loan)
                : nullptr;
        if (instruction.kind != MirInstructionKind::Borrow || !output ||
            !input || output->kind != MirLoanKind::Return ||
            input->kind != MirLoanKind::CallResult || input->carriers.empty())
          continue;
        ++checked;
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto changed = analyzed.mir;
          auto *target = const_cast<MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &retained = target->body.loans[input->id - 1];
          auto &published = target->body.loans[output->id - 1];
          if (mutation == 0)
            published.storedDependency = 0;
          else if (mutation == 1)
            retained.parent = 0;
          else if (mutation == 2) {
            auto &witness = target->body.loans[retained.storedDependency - 1];
            witness.access = witness.access == AccessMode::ReadOnly
                                 ? AccessMode::Mutable
                                 : AccessMode::ReadOnly;
          } else if (mutation == 3)
            retained.lifecycleBound = true;
          else if (mutation == 4)
            retained.additionalDependencies.push_back(
                {.source = retained.source});
          else {
            MirInstructionId last = 0;
            for (const auto &candidateBlock : target->body.blocks)
              for (const auto &candidate : candidateBlock.instructions)
                last = std::max(last, candidate.id);
            auto &instructions = target->body.blocks[block.id - 1].instructions;
            const auto position =
                std::find_if(instructions.begin(), instructions.end(),
                             [&](const auto &candidate) {
                               return candidate.id == instruction.id;
                             });
            instructions.insert(position,
                                {.id = last + 1,
                                 .kind = MirInstructionKind::EndBorrow,
                                 .hirValue = instruction.hirValue,
                                 .loan = input->id});
          }
          if (verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged named-restoration return " << mutation
                      << '\n';
            return false;
          }
        }
      }
  if (checked != 2) {
    std::cerr << "named restoration did not exercise both read and mutable "
                 "alias transfers: "
              << checked << '\n';
    return false;
  }
  for (const auto &[name, source] :
       std::vector<std::pair<std::string, std::string>>{
           {"restoration-local-owner", R"gti(
#include <std/memory>
struct Reader { int& value; Reader(int& source) : value(source) {} };
Reader& bad(int& source) {
  Reader local{source};
  std::erased_object& object = std::erase_ref(local);
  unsafe { Reader& restored = std::restore_ref<Reader>(object); return restored; }
}
int main() { return 0; }
)gti"},
           {"restoration-access-upgrade", R"gti(
#include <std/memory>
struct Reader { int& value; Reader(int& source) : value(source) {} };
unsafe mut Reader& bad(std::erased_object& object) {
  unsafe { Reader& restored = std::restore_ref<Reader>(object); return restored; }
}
int main() { return 0; }
)gti"}}) {
    const auto rejected = application(name + ".gti", source);
    if (rejected.canGenerateCode() ||
        std::none_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2017";
                     })) {
      std::cerr << "missing source lifetime rejection for " << name << '\n';
      return false;
    }
  }
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (!callResultAliasProofTests())
    return 10;
  if (argc == 2 && std::string_view(argv[1]) == "--alias-proof-only") {
    std::cout
        << "PASS exact alias producer, order, access and storage proofs\n";
    return 0;
  }
  if (!referenceInitializerBodyProofTests())
    return 16;
  if (argc == 2 && std::string_view(argv[1]) == "--relay-proof-only") {
    std::cout << "PASS initializer helper body-origin proof\n";
    return 0;
  }
  if (!namedRestorationReturnTests())
    return 17;
  if (argc == 2 && std::string_view(argv[1]) == "--restoration-proof-only") {
    std::cout << "PASS named restoration return lifetime proofs\n";
    return 0;
  }
  if (argc != 1)
    return 15;
  const auto application = [](const std::string &name,
                              const std::string &source) {
    const auto entry = std::filesystem::temp_directory_path() / name;
    return lang::Frontend().analyze(
        entry, source, {standardLibraryPrelude()},
        {{std::filesystem::weakly_canonical(entry).string(), source}},
        {standardLibraryRoot()});
  };
  const auto erased = application("erased-user-wrapper-mir.gti", R"gti(
#include <std/memory>
#include <std/functional>
class Inspection {
  std::erased_object& object;
public:
  Inspection(int& object) : object(std::erase_ref(object)) {}
  int read() {
    unsafe { int& value = std::restore_ref<int>(this.object); return value; }
  }
};
class Reader {
  int& value;
public:
  Reader(int& value) : value(value) {}
  int read() { return this.value; }
};
class HiddenReader {
  std::erased_object& object;
public:
  HiddenReader(Reader& original, Reader& unrelated) : object(std::erase_ref(original)) {}
};
Reader& restore_reader(std::erased_object& object) {
  unsafe { Reader& reader = std::restore_ref<Reader>(object); return reader; }
}
int read_erased(std::erased_object& object) {
  unsafe { Reader& reader = std::restore_ref<Reader>(object); return reader.read(); }
}
int main() {
  int owner = 7; Inspection view{owner};
  Reader original{owner};
  int other_owner = 19;
  Reader unrelated{other_owner};
  HiddenReader correlated{original, unrelated};
  std::erased_object& hidden = std::erase_ref(original);
  std::function_ptr<int(std::erased_object&)> opaque_pack{read_erased};
  if (opaque_pack(hidden) != 7) return 3;
  Reader& restored = restore_reader(hidden);
  if (restored.read() != 7) return 1;
  auto reader = [&owner](int value) -> int { return owner + value; };
  std::function_ref<int(int)> callback{reader};
  mut int count = 0;
  mut auto writer = [mut &count](int value) -> int { count += value; return count; };
  mut std::function_ref<int(int) mut> update{writer};
  return view.read() + callback(1) + update(2) - 17;
}
)gti");
  if (!erased.canGenerateCode()) {
    for (const auto &diagnostic : erased.diagnostics)
      std::cerr << diagnostic.message << '\n';
    const auto verification = lang::verifyMirProgram(erased.mir);
    for (const auto &error : verification.errors)
      std::cerr << "verify " << int(error.bodyKind) << ':' << error.owner
                << " b" << error.block << " i" << error.instruction << ' '
                << error.message << '\n';
    const auto dump = [&](const auto &instance) {
      const auto &body = instance.body;
      if (std::none_of(verification.errors.begin(), verification.errors.end(),
                       [&](const auto &error) {
                         return error.bodyKind == body.kind &&
                                error.owner == instance.id;
                       }))
        return;
      std::cerr << "body " << int(body.kind) << ':' << instance.id << '\n';
      for (const auto &loan : body.loans)
        std::cerr << "  loan " << loan.id << " kind=" << int(loan.kind)
                  << " source=" << loan.source << " parent=" << loan.parent
                  << " access=" << int(loan.access) << " entry=" << loan.entry
                  << " escapes=" << loan.escapes
                  << " producer=" << loan.producedBy << '\n';
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions) {
          std::cerr << "  i" << instruction.id << " k=" << int(instruction.kind)
                    << " intrinsic=" << int(instruction.intrinsic)
                    << " op=" << int(instruction.operation)
                    << " result=" << instruction.result.value_or(0)
                    << " loan=" << instruction.loan.value_or(0)
                    << " type=" << int(instruction.info.type.kind)
                    << " restore=" << instruction.restoresReferenceResult
                    << " fields=" << instruction.restoredBorrowFields.size()
                    << " target=" << instruction.functionTarget.value_or(0)
                    << " origin=" << int(instruction.borrowOrigin) << ':'
                    << instruction.borrowArgument << " operands=";
          for (const auto &operand : instruction.operands)
            std::cerr << int(operand.kind) << ':' << int(operand.type.kind)
                      << ":v" << operand.value << ":p" << operand.place << ":l"
                      << operand.loan << ',';
          std::cerr << '\n';
        }
    };
    for (const auto &instance : erased.mir.functionInstances())
      dump(instance);
    for (const auto &instance : erased.mir.constructorInstances())
      dump(instance);
    for (const auto &instance : erased.mir.lambdaInstances())
      dump(instance);
    return 7;
  }
  bool erasedChecked = false;
  bool restoredFieldsChecked = false;
  for (const auto &function : erased.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::RestoreReference)
          continue;
        erasedChecked = true;
        const bool hasFields = !instruction.restoredBorrowFields.empty();
        restoredFieldsChecked = restoredFieldsChecked || hasFields;
        for (int mutation = 0; mutation != (hasFields ? 8 : 3); ++mutation) {
          auto changed = erased.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &instructions = target->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(instructions.begin(), instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
          if (mutation == 0)
            selected->unsafeOperation = lang::UnsafeOperationKind::None;
          else if (mutation == 1)
            selected->parameterTypes.front().referenceAccess =
                lang::AccessMode::Mutable;
          else if (mutation == 2)
            selected->borrowOrigin = lang::BorrowOriginKind::None;
          else if (mutation == 3)
            selected->restoredBorrowFields.clear();
          else if (mutation == 4)
            selected->restoredBorrowFields.front().fields.front() = 0;
          else if (mutation == 5)
            selected->restoredBorrowFields.front()
                .referenceType.referenceAccess =
                selected->restoredBorrowFields.front()
                            .referenceType.referenceAccess ==
                        lang::AccessMode::Mutable
                    ? lang::AccessMode::ReadOnly
                    : lang::AccessMode::Mutable;
          else if (mutation == 6)
            target->body.loans[*selected->loan - 1].storedDependency = 0;
          else
            target->body.loans[*selected->loan - 1].parent = 0;
          if (lang::verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged erased reference " << mutation
                      << '\n';
            return 8;
          }
        }
      }
  if (!erasedChecked || !restoredFieldsChecked)
    return 9;
  bool aliasReturnChecked = false;
  bool opaquePackChecked = false;
  bool transformedDependencyChecked = false;
  bool unrelatedDependencyChecked = false;
  for (const auto &function : erased.mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *output = instruction.loan
                                 ? function.body.findLoan(*instruction.loan)
                                 : nullptr;
        const auto *input =
            instruction.operands.size() == 1 &&
                    instruction.operands.front().kind ==
                        lang::MirOperandKind::Loan
                ? function.body.findLoan(instruction.operands.front().loan)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Borrow || !output ||
            !input || output->kind != lang::MirLoanKind::Return ||
            input->kind != lang::MirLoanKind::CallResult ||
            input->carriers.empty())
          continue;
        aliasReturnChecked = true;
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        lang::MirInstructionId last = 0;
        for (const auto &candidateBlock : target->body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            last = std::max(last, candidate.id);
        auto &instructions = target->body.blocks[block.id - 1].instructions;
        const auto position =
            std::find_if(instructions.begin(), instructions.end(),
                         [&](const auto &candidate) {
                           return candidate.id == instruction.id;
                         });
        instructions.insert(position,
                            {.id = last + 1,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .hirValue = instruction.hirValue,
                             .loan = input->id});
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted expired call-result alias return\n";
          return 11;
        }
      }
    for (const auto &loan : function.body.loans) {
      const auto *source = function.body.findPlace(loan.source);
      if (!loan.entry || loan.kind != lang::MirLoanKind::Parameter ||
          loan.access != lang::AccessMode::ReadOnly || !source ||
          source->projections.size() != 1 ||
          source->projections.front().kind !=
              lang::MirProjectionKind::PackElement)
        continue;
      opaquePackChecked = true;
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        auto &entry = target->body.loans[loan.id - 1];
        auto &place = target->body.places[source->id - 1];
        if (mutation == 0)
          place.projections.front().constantIndex = 999;
        else if (mutation == 1)
          place.type = lang::SemanticType::Bool;
        else if (mutation == 2)
          entry.access = lang::AccessMode::Mutable;
        else if (mutation == 3)
          entry.source = 0;
        else if (mutation == 4)
          entry.parent = entry.id;
        else {
          const auto *entryBlock = target->body.findBlock(target->body.entry);
          if (!entryBlock || entryBlock->instructions.empty()) {
            std::cerr << "opaque pack mutation has no entry consumer\n";
            return 12;
          }
          expireLoanBefore(target->body, entry.id,
                           entryBlock->instructions.front().id);
        }
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted forged opaque pack entry " << mutation << '\n';
          return 12;
        }
      }
    }
  }
  for (const auto &constructor : erased.mir.constructorInstances())
    for (const auto &initializer : constructor.initializers) {
      const auto *call = lang::mir_detail::referenceInitializerCall(
          erased.mir, constructor, initializer);
      const auto *result =
          call && call->loan ? constructor.body.findLoan(*call->loan) : nullptr;
      if (!result || !result->storedDependency)
        continue;
      transformedDependencyChecked = true;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirConstructorInstance *>(
            changed.findConstructorInstance(constructor.id));
        // Matching call/publication metadata cannot invent or erase the
        // independent original owner authority. A separate scalar fixture
        // checks a valid-body, signature-identical non-relay replacement.
        if (mutation == 2) {
          expireLoanBefore(target->body, result->storedDependency, call->id);
        } else {
          for (auto &loan : target->body.loans)
            if (loan.storedDependency == result->storedDependency)
              loan.storedDependency = mutation == 0 ? 0 : result->id;
        }
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted forged transformed constructor authority "
                    << mutation << '\n';
          return 13;
        }
      }
      const auto unrelated =
          std::find_if(constructor.body.loans.begin(),
                       constructor.body.loans.end(), [&](const auto &loan) {
                         return loan.entry &&
                                loan.kind == lang::MirLoanKind::Stored &&
                                loan.id != result->storedDependency;
                       });
      if (unrelated != constructor.body.loans.end()) {
        unrelatedDependencyChecked = true;
        auto changed = erased.mir;
        auto *target = const_cast<lang::MirConstructorInstance *>(
            changed.findConstructorInstance(constructor.id));
        for (auto &loan : target->body.loans)
          if (loan.storedDependency == result->storedDependency)
            loan.storedDependency = unrelated->id;
        if (lang::verifyMirProgram(changed).valid()) {
          std::cerr << "accepted a different valid caller-owned constructor "
                       "dependency\n";
          return 13;
        }
      }
    }
  if (!aliasReturnChecked || !opaquePackChecked ||
      !transformedDependencyChecked || !unrelatedDependencyChecked) {
    std::cerr << "missing erased borrow boundary mutation coverage "
              << aliasReturnChecked << opaquePackChecked
              << transformedDependencyChecked << unrelatedDependencyChecked
              << '\n';
    return 14;
  }
  const auto exclusive =
      lang::Frontend().analyze("exclusive-borrowed-callback-mir.gti", R"gti(
int main() {
  mut int value = 4;
  mut auto change = [mut &value]() -> void { value += 1; };
  change();
  return value - 5;
}
)gti");
  if (!exclusive.canGenerateCode()) {
    for (const auto &diagnostic : exclusive.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 6;
  }
  const auto analyzed =
      lang::Frontend().analyze("borrowed-callback-mir.gti", R"gti(
int main() {
  int first = 4;
  int second = 9;
  auto difference = [&first, &second]() -> int { return second - first; };
  return difference() - 5;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  if (!lang::verifyMirProgram(analyzed.mir).valid())
    return 2;
  const auto &lambda = analyzed.mir.lambdaInstances().front();
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto changed = analyzed.mir;
    auto *target =
        const_cast<lang::MirLambdaInstance *>(changed.findLambda(lambda.id));
    if (mutation == 0)
      target->captureModes.front() = lang::LambdaCaptureMode::Copy;
    else if (mutation == 1)
      target->capability = lang::CallableInvocationCapability::Mutable;
    else
      target->captureTypes.front().referenceAccess = lang::AccessMode::Mutable;
    if (lang::verifyMirProgram(changed).valid()) {
      std::cerr << "accepted forged capture representation " << mutation
                << '\n';
      return 3;
    }
  }
  bool checked = false;
  for (const auto &function : analyzed.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Closure ||
            !instruction.loan)
          continue;
        checked = true;
        for (int mutation = 0; mutation < 4; ++mutation) {
          auto changed = analyzed.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &instructions = target->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(instructions.begin(), instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
          auto &loan = target->body.loans[*instruction.loan - 1];
          if (mutation == 0)
            selected->loan.reset();
          else if (mutation == 1)
            loan.additionalDependencies.clear();
          else if (mutation == 2)
            loan.additionalDependencies.front().designatedFields =
                loan.designatedFields;
          else
            selected->operands.front().kind = lang::MirOperandKind::Copy;
          if (lang::verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged closure dependency " << mutation
                      << '\n';
            return 4;
          }
        }
      }
  return checked ? 0 : 5;
}
