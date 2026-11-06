#include "../src/compiler/mir_lifetimes.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

using namespace lang;

namespace {
int failures = 0;
std::filesystem::path evidenceDirectory;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << message << '\n';
  }
}

void retain(std::string_view name, const FrontendResult &result) {
  if (evidenceDirectory.empty())
    return;
  std::filesystem::create_directories(evidenceDirectory);
  const auto path = evidenceDirectory / (std::string(name) + ".mir.txt");
  std::ofstream output(path);
  MirPrinter{}.print(result.mir, output);
  for (const auto &function : result.hir.functionInstances()) {
    output << "HIR function=" << function.id << " declaration=" << function.declaration << '\n';
    for (const auto &loan : function.body.loans)
      output << "HIR loan=" << loan.semanticLoan << " parent=" << loan.parent
             << " stored=" << loan.storedDependency << " entry=" << loan.entry
             << " fields=" << loan.designatedFields.size()
             << " suppliers=" << loan.additionalDependencies.size() << '\n';
  }
  std::cerr << "  evidence: " << path << '\n';
}

FrontendResult positive(std::string_view name, std::string_view source) {
  auto result = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      std::string(name) + ".gti", std::string(source));
  if (!result.canGenerateCode()) {
    expect(false, std::string(name) + ": expected verified MIR");
    std::size_t count = 0;
    for (const auto &diagnostic : result.diagnostics) {
      if (count++ == 16)
        break;
      std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    retain(name, result);
  } else {
    expect(verifyMirProgram(result.mir).valid(),
           std::string(name) + ": independent verification must accept the source program");
  }
  return result;
}

void negative(std::string_view name, std::string_view source) {
  const auto result = Frontend({.stopAfter = FrontendPhase::Semantics}).analyze(
      std::string(name) + ".gti", std::string(source));
  expect(result.syntaxValid && !result.semanticValid &&
             std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                          [](const auto &diagnostic) { return diagnostic.code == "GTI-B0001"; }),
         std::string(name) + ": expected a source semantic rejection");
}

MirInstruction *payloadConstruct(MirBody &body) {
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (!instruction.tombstone && instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::PayloadConstruct)
        return &instruction;
  return nullptr;
}

void checkSelectedReferenceRegionMutations(const MirProgram &program) {
  std::optional<MirBodyAddress> selectedBody;
  MirLoanId selectedLoan = 0;
  Lifetime bindingRegion;
  for (const auto address : enumerateMirBodyAddresses(program))
    if (const auto *body = findMirBody(program, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != MirInstructionKind::Initialize ||
              !instruction.destination || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != MirOperandKind::Loan)
            continue;
          const auto *place = body->findPlace(*instruction.destination);
          const auto *loan = body->findLoan(instruction.operands.front().loan);
          if (!place || place->type.kind != SemanticType::Reference ||
              place->type.referenceLifetime.kind != Lifetime::Kind::Inference ||
              !loan || !loan->referenceSelection || loan->referenceSelection->kind !=
                  ReferenceSelectionKind::StoredExternalReference)
            continue;
          selectedBody = address;
          selectedLoan = loan->id;
          bindingRegion = place->type.referenceLifetime;
        }
  expect(selectedBody && selectedLoan && bindingRegion.specified(),
         "stored reference pattern retains an inferred region and exact selection");
  if (!selectedBody)
    return;
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto invalid = program;
    auto *body = findMirBody(invalid, *selectedBody);
    auto &loan = body->loans[selectedLoan - 1];
    if (mutation == 0) {
      const auto removed = std::erase_if(body->lifetimeBody.obligations,
          [&](const auto &edge) { return edge.shorter == bindingRegion; });
      expect(removed != 0, "selected reference binding records its external-region relationship");
    } else if (mutation == 1) {
      loan.referenceSelection->reference.referenceLifetime = Lifetime::staticLifetime();
    } else if (mutation == 2) {
      loan.referenceSelection->conditions.clear();
    } else {
      loan.referenceSelection.reset();
    }
    expect(!verifyMirProgram(invalid).valid(),
           "stored reference regions require exact extraction, type, guard and binding proof");
  }
}

void checkOwningTransferMutations(const MirProgram &program) {
  struct Stage { MirBodyAddress body; MirInstructionId instruction; };
  std::vector<Stage> stages;
  bool valueMove = false;
  bool borrowedMove = false;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &input : block.instructions) {
        if (input.kind != MirInstructionKind::CallInput ||
            (input.callInputKind != HirCallInputKind::MoveValue &&
             input.callInputKind != HirCallInputKind::MoveBorrowedTemporary) ||
            !input.preparedParameterDrop || input.operands.size() != 1 ||
            input.operands.front().type == input.info.type ||
            !input.info.traits.containsBorrowedState)
          continue;
        expect(input.lifecycle.size() == 1 &&
                   input.lifecycle.front().kind == MirLifecycleEventKind::Reparent,
               "owning lifetime conversion reparents the exact object into prepared storage");
        if (input.lifecycle.size() != 1)
          continue;
        valueMove |= input.callInputKind == HirCallInputKind::MoveValue;
        borrowedMove |= input.callInputKind == HirCallInputKind::MoveBorrowedTemporary;
        stages.push_back({address, input.id});
      }
  }
  expect(valueMove && borrowedMove,
         "owning fixture must exercise fresh construction and named borrowed-result transfers");
  for (const auto selected : stages)
    for (int mutation = 0; mutation != 8; ++mutation) {
      auto broken = program;
      auto *body = findMirBody(broken, selected.body);
      MirInstruction *input = nullptr;
      for (auto &block : body->blocks)
        for (auto &candidate : block.instructions)
          if (candidate.id == selected.instruction)
            input = &candidate;
      MirDropObligation *source = nullptr;
      MirDropObligation *target = nullptr;
      for (auto &drop : body->dropObligations) {
        if (drop.id == input->lifecycle.front().source)
          source = &drop;
        if (drop.id == *input->preparedParameterDrop)
          target = &drop;
      }
      expect(source && target, "owning input names both exact cleanup obligations");
      if (!source || !target)
        continue;
      switch (mutation) {
      case 0: input->lifecycle.clear(); break;
      case 1: input->lifecycle.push_back(input->lifecycle.front()); break;
      case 2: input->lifecycle.front().source = target->id; break;
      case 3: input->lifecycle.front().target = source->id; break;
      case 4: target->dropType.destructor.reset(); break;
      case 5: source->initiallyActive = true; break;
      case 6: source->place = target->place; break;
      case 7: {
        input->info.type.lifetimeArguments = {Lifetime::staticLifetime()};
        input->operands.front().type = input->info.type;
        for (auto &value : body->values)
          if (value.id == *input->result)
            value.info.type = input->info.type;
        target->dropType.type = input->info.type;
        for (auto &place : body->places)
          if (place.id == target->place)
            place.type = input->info.type;
        expect(!verifyMirLifetimeApplications(broken, *body, selected.body.owner).valid(),
               "owning stage cannot strengthen the original source behind matching type tags");
        break;
      }
      }
      expect(!verifyMirProgram(broken).valid(),
             "owning conversion cannot forge transfer, source activation, storage or cleanup");
    }
}

void checkOwningPayloadMutations(const MirProgram &program) {
  std::optional<MirBodyAddress> selected;
  MirInstructionId producerId = 0;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        const auto *loan = instruction.loan ? body->findLoan(*instruction.loan) : nullptr;
        if (instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::PayloadConstruct && loan &&
            loan->lifecycleBound) {
          selected = address;
          producerId = instruction.id;
        }
      }
  }
  expect(selected.has_value(), "owning payload has an exact cleanup-bound producer");
  if (!selected)
    return;
  for (int mutation = 0; mutation != 7; ++mutation) {
    auto broken = program;
    auto *body = findMirBody(broken, *selected);
    MirInstruction *producer = nullptr;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == producerId)
          producer = &instruction;
    auto *loan = &body->loans[*producer->loan - 1];
    const auto activation = std::find_if(producer->lifecycle.begin(), producer->lifecycle.end(),
        [](const auto &event) { return event.kind == MirLifecycleEventKind::Initialize; });
    expect(activation != producer->lifecycle.end(), "payload activates its exact aggregate obligation");
    if (activation == producer->lifecycle.end())
      continue;
    auto *drop = &body->dropObligations[activation->target - 1];
    switch (mutation) {
    case 0: loan->lifecycleBound = false; break;
    case 1: loan->producedBy = 0; break;
    case 2:
      std::erase_if(producer->lifecycle, [](const auto &event) {
        return event.kind == MirLifecycleEventKind::TransferOut;
      });
      break;
    case 3: producer->lifecycle.erase(activation); break;
    case 4:
      expect(!drop->dropType.aggregate.empty(), "owning enum cleanup names its selected payload child");
      if (!drop->dropType.aggregate.empty())
        drop->dropType.aggregate.front().payloadField = 999;
      break;
    case 5: drop->initiallyActive = true; break;
    case 6: {
      const auto loanId = *producer->loan;
      std::optional<MirInstruction> endpoint;
      for (auto &block : body->blocks) {
        const auto end = std::find_if(block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == MirInstructionKind::EndBorrow && instruction.loan == loanId;
            });
        if (end != block.instructions.end()) {
          endpoint = *end;
          block.instructions.erase(end);
          break;
        }
      }
      bool relocated = false;
      if (endpoint)
        for (auto &block : body->blocks) {
          const auto produced = std::find_if(block.instructions.begin(), block.instructions.end(),
              [&](const auto &instruction) { return instruction.id == producerId; });
          if (produced != block.instructions.end()) {
            block.instructions.insert(produced + 1, *endpoint);
            relocated = true;
            break;
          }
        }
      expect(relocated, "owning enum has an endpoint to move before its cleanup control-flow graph");
      break;
    }
    }
    expect(!verifyMirProgram(broken).valid(),
           "owning payload cannot forge carrier classification, handoff, child cleanup or loan endpoint");
  }
}

void checkGuardBindingViews(const FrontendResult &frontend) {
  struct Site { MirBodyAddress body; MirLoanId loan; MirPlaceId binding; MirInstructionId initializer; };
  std::optional<Site> site;
  for (const auto address : enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = findMirBody(frontend.mir, address);
    if (!body) continue;
    for (const auto &loan : body->loans) {
      if (loan.entry || loan.kind != MirLoanKind::Stored || loan.conditions.size() != 1 ||
          !loan.conditions.front().subobject.empty())
        continue;
      for (const auto binding : loan.carriers)
        for (const auto &place : body->places)
          if (place.root == MirPlaceRootKind::Binding && place.binding == binding &&
              place.projections.empty() && place.type.kind == SemanticType::Enum &&
              place.type != loan.conditions.front().enumeration)
            for (const auto &block : body->blocks)
              for (const auto &instruction : block.instructions)
                if (instruction.kind == MirInstructionKind::Initialize && instruction.destination == place.id)
                  site = Site{address, loan.id, place.id, instruction.id};
    }
  }
  expect(site.has_value(), "shortened enum binding retains the immutable producer guard domain");
  if (!site) return;
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto broken = frontend.mir;
    auto *body = findMirBody(broken, site->body);
    auto &loan = body->loans[site->loan - 1];
    switch (mutation) {
    case 0: loan.conditions.front().enumeration.lifetimeArguments = {Lifetime::staticLifetime()}; break;
    case 1: {
      const auto &target = body->places[site->binding - 1].type;
      const auto &source = loan.conditions.front().enumeration;
      const LifetimeBound edge{source.lifetimeArguments.front(), target.lifetimeArguments.front()};
      const auto size = body->lifetimeBody.obligations.size();
      std::erase(body->lifetimeBody.obligations, edge);
      expect(body->lifetimeBody.obligations.size() < size, "binding conversion has its exact recorded edge");
      expect(!verifyMirLifetimeApplications(broken, *body, site->body.owner).valid(),
             "conditional binding view cannot invent its source-to-binding lifetime proof");
      break;
    }
    case 2: loan.parent = loan.id; break;
    case 3: loan.source = site->binding; break;
    case 4:
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == site->initializer)
            instruction.tombstone = true;
      break;
    }
    expect(!verifyMirProgram(broken).valid(),
           "conditional binding views reject stale guards, missing conversion/init and forged source authority");
  }
  auto staleHir = frontend.hir;
  bool changed = false;
  for (const auto &immutable : staleHir.functionInstances()) {
    auto &function = const_cast<HirFunctionInstance &>(immutable);
    for (auto &loan : function.body.loans)
      if (!loan.entry && !loan.carriers.empty() && !loan.conditions.empty()) {
        loan.conditions.front().enumeration.lifetimeArguments = {Lifetime::staticLifetime()};
        changed = true;
      }
  }
  expect(changed && !MirLowerer{}.lower(staleHir, frontend.failureMetadata).valid(),
         "lowering cannot reinterpret a stale semantic binding guard as a checked typed view");
}

void checkGuardMutations(const MirProgram &program) {
  std::optional<MirBodyAddress> selected;
  MirValueId valueId = 0;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    if (!body)
      continue;
    for (const auto &value : body->values)
      if (value.info.type.kind == SemanticType::Enum && value.borrowDependencies &&
          !value.borrowDependencies->dependencies.empty() &&
          !value.borrowDependencies->dependencies.front().conditions.empty() &&
          !value.borrowDependencies->possibleAlternatives.empty()) {
        selected = address;
        valueId = value.id;
      }
  }
  expect(selected.has_value(), "guard fixture retains complete typed enum producer evidence");
  if (!selected)
    return;
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto broken = program;
    auto *body = findMirBody(broken, *selected);
    auto &fact = *body->values[valueId - 1].borrowDependencies;
    auto &guard = fact.dependencies.front().conditions.front();
    switch (mutation) {
    case 0: guard.enumeration.lifetimeArguments = {Lifetime::staticLifetime()}; break;
    case 1: guard.variant = 99; break;
    case 2: guard.subobject.push_back(9999); break;
    case 3: fact.possibleAlternatives.clear(); break;
    case 4: fact.dependencies.front().parent = body->loans.size() + 1; break;
    }
    expect(!verifyMirProgram(broken).valid(),
           "MIR rejects stale guard types, forged paths/variants, omitted alternatives and foreign parents");
  }
}

void checkConstructorFieldDomains(const MirProgram &program) {
  for (const auto &constructor : program.constructorInstances()) {
    if (!constructor.completeBorrow)
      continue;
    std::vector<SymbolId> fields;
    for (const auto &dependency : constructor.completeBorrow->dependencies)
      if (!dependency.resultFields.empty() &&
          std::find(fields.begin(), fields.end(), dependency.resultFields.front()) == fields.end())
        fields.push_back(dependency.resultFields.front());
    if (fields.size() < 2)
      continue;
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto broken = program;
      auto *changed = const_cast<MirConstructorInstance *>(broken.findConstructorInstance(constructor.id));
      bool mutated = false;
      if (mutation == 0) {
        std::erase_if(changed->completeBorrow->dependencies,
            [&](const auto &item) { return !item.resultFields.empty() && item.resultFields.front() == fields.back(); });
        std::erase_if(changed->completeBorrow->possibleAlternatives,
            [&](const auto &path) { return !path.conditions.empty() &&
                !path.conditions.front().subobject.empty() && path.conditions.front().subobject.front() == fields.back(); });
        mutated = true;
      } else if (mutation == 2) {
        for (auto &block : changed->body.blocks)
          for (auto &instruction : block.instructions) {
            const auto *target = instruction.destination ? changed->body.findPlace(*instruction.destination) : nullptr;
            if (instruction.kind == MirInstructionKind::Initialize && target &&
                target->root == MirPlaceRootKind::This && target->projections.size() == 1 &&
                target->projections.front().field == fields.back()) {
              instruction.tombstone = true;
              mutated = true;
            }
          }
      } else if (mutation == 4) {
        if (!changed->ownerType.lifetimeArguments.empty()) {
          changed->ownerType.lifetimeArguments.front() = Lifetime::staticLifetime();
          mutated = true;
        }
      } else {
        for (auto &loan : changed->body.loans)
          if (loan.escapes && loan.storedField == fields.back()) {
            if (mutation == 1) {
              loan.storedField = fields.front();
              mutated = true;
            } else if (!loan.conditions.empty()) {
              loan.conditions.front().enumeration.lifetimeArguments = {Lifetime::staticLifetime()};
              mutated = true;
            }
          }
      }
      expect(mutated && !verifyMirProgram(broken).valid(),
             "constructor publication requires every exact field domain, initializer, group and owner view");
    }
    return;
  }
  expect(false, "constructor domain fixture must publish two independent borrowed fields");
}

void checkConditionalConstructorSources(const MirProgram &program) {
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    if (!body)
      continue;
    const auto definition = [&](MirValueId value) -> const MirInstruction * {
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.result == value)
            return &instruction;
      return nullptr;
    };
    for (const auto &block : body->blocks)
      for (const auto &producer : block.instructions) {
        if (producer.kind != MirInstructionKind::Construct ||
            producer.operands.size() != 2 || !producer.loan || !producer.result)
          continue;
        const auto *result = body->findValue(*producer.result);
        const auto *group = body->findLoan(*producer.loan);
        const auto *first = producer.operands.front().kind == MirOperandKind::Value
                                ? body->findValue(producer.operands.front().value) : nullptr;
        if (!result || !result->borrowDependencies ||
            result->borrowDependencies->dependencies.size() != 1 || !group ||
            group->conditions.empty() || !first || !first->borrowDependencies ||
            !first->borrowDependencies->dependencies.empty())
          continue;
        const auto *stage = definition(first->id);
        const auto *load = stage && stage->operands.size() == 1 &&
                                  stage->operands.front().kind == MirOperandKind::Value
                               ? definition(stage->operands.front().value) : nullptr;
        const MirPlaceId emptySource = load && load->kind == MirInstructionKind::Load &&
                                                load->operands.size() == 1
                                            ? load->operands.front().place : 0;
        expect(emptySource && emptySource != group->source,
               "conditional constructor fixture excludes its empty first input supplier");
        if (!emptySource)
          return;
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto broken = program;
          auto *changed = findMirBody(broken, address);
          auto &claim = changed->values[result->id - 1].borrowDependencies;
          auto &loan = changed->loans[group->id - 1];
          if (mutation == 0) {
            claim->dependencies.front().source = emptySource;
            loan.source = emptySource;
          } else if (mutation == 1) {
            claim->dependencies.front().conditions.front().variant = 0;
            loan.conditions.front().variant = 0;
          } else if (mutation == 2) {
            claim->dependencies.clear();
          } else if (mutation == 3) {
            claim.reset();
          } else {
            const_cast<MirConstructorInstance *>(
                broken.findConstructorInstance(*producer.constructorTarget))->completeBorrow.reset();
          }
          expect(!verifyMirProgram(broken).valid(),
                 "conditional constructor sources require replayed input domains, exact groups and callee proof");
        }
        return;
      }
  }
  expect(false, "conditional constructor fixture must have an empty first input and guarded second supplier");
}

void checkRetainedPayloadCopy(const MirProgram &program) {
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &producer : block.instructions) {
        if (producer.operation != MirOperation::PayloadConstruct || !producer.loan ||
            !producer.result || producer.operands.size() != 1)
          continue;
        const auto *group = body->findLoan(*producer.loan);
        const auto *retained = group ? body->findLoan(group->parent) : nullptr;
        if (!retained || retained->entry || retained->kind != MirLoanKind::Stored)
          continue;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto broken = program;
          auto *changedBody = findMirBody(broken, address);
          auto *changed = payloadConstruct(*changedBody);
          auto *loan = const_cast<MirLoan *>(changedBody->findLoan(*changed->loan));
          MirInstruction *stage = nullptr;
          for (auto &candidate : changedBody->blocks)
            for (auto &input : candidate.instructions)
              if (input.result == changed->operands.front().value)
                stage = &input;
          expect(stage && stage->callInputKind == HirCallInputKind::CopyValue &&
                     stage->operands.size() == 1 && stage->operands.front().kind == MirOperandKind::Copy,
                 "payload parent fixture uses an exact copied carrier stage");
          if (!stage || stage->operands.size() != 1)
            return;
          bool mutated = true;
          if (mutation == 0) {
            loan->parent = 0;
            auto &fact = changedBody->values[*changed->result - 1].borrowDependencies;
            expect(fact.has_value(), "payload copied carrier publishes complete origin evidence");
            if (fact)
              for (auto &dependency : fact->dependencies)
                dependency.parent = 0;
          } else if (mutation == 1) {
            auto *parent = const_cast<MirLoan *>(changedBody->findLoan(loan->parent));
            parent->source = stage->operands.front().place;
          } else if (mutation == 2) {
            stage->operands.front().kind = MirOperandKind::Move;
          } else {
            const auto *copied = changedBody->findPlace(stage->operands.front().place);
            mutated = false;
            for (auto &candidate : changedBody->blocks)
              for (auto &initialize : candidate.instructions) {
                const auto *destination = initialize.destination
                    ? changedBody->findPlace(*initialize.destination) : nullptr;
                if (initialize.kind == MirInstructionKind::Initialize && copied && destination &&
                    destination->root == MirPlaceRootKind::Binding &&
                    destination->binding == copied->binding && destination->projections.empty()) {
                  initialize.tombstone = true;
                  mutated = true;
                }
              }
          }
          expect(mutated && !verifyMirProgram(broken).valid(),
                 "payload copied parent requires exact retained contents, source, copy and initialization");
        }
        return;
      }
  }
  expect(false, "payload copy fixture must retain a separately proved Stored group parent");
}

void checkMutations(const MirProgram &program) {
  std::optional<MirBodyAddress> address;
  for (const auto candidate : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, candidate);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::PayloadConstruct &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arguments.front().kind == SemanticType::Class)
          address = candidate;
  }
  expect(address.has_value(), "generic payload fixture must retain a typed payload construction");
  if (!address)
    return;

  auto missingEdges = program;
  auto *body = findMirBody(missingEdges, *address);
  expect(!body->lifetimeBody.obligations.empty(),
         "the payload conversion must publish its source-to-stored-region obligation");
  body->lifetimeBody.obligations.clear();
  const auto missingProof = verifyMirLifetimeApplications(missingEdges, *body, address->owner);
  expect(!missingProof.valid() &&
             std::any_of(missingProof.errors.begin(), missingProof.errors.end(), [](const auto &error) {
               return error.message.find("payload") != std::string::npos ||
                      error.message.find("call-input stage") != std::string::npos;
             }),
         "MIR must reconstruct a payload conversion after its recorded edges are removed");

  auto strongerRegion = program;
  body = findMirBody(strongerRegion, *address);
  auto *construction = payloadConstruct(*body);
  auto &stored = construction->info.type.arguments.front();
  expect(stored.lifetimeArguments.size() == 1,
         "generic payload fixture must preserve the stored carrier's region");
  if (stored.lifetimeArguments.size() == 1) {
    stored.lifetimeArguments.front() = Lifetime::staticLifetime();
    expect(!verifyMirLifetimeApplications(strongerRegion, *body, address->owner).valid(),
           "MIR must reject strengthening a stored carrier region to static");
  }

  auto orphan = program;
  body = findMirBody(orphan, *address);
  auto *removed = payloadConstruct(*body);
  const auto stageValue = removed->operands.front().value;
  removed->tombstone = true;
  MirInstruction *stage = nullptr;
  for (auto &block : body->blocks)
    for (auto &candidate : block.instructions)
      if (candidate.result == stageValue && candidate.kind == MirInstructionKind::CallInput)
        stage = &candidate;
  expect(stage != nullptr, "payload fixture must retain an explicit source input stage");
  if (stage && stage->info.type.lifetimeArguments.size() == 1) {
    expect(verifyMirLifetimeApplications(orphan, *body, address->owner).valid(),
           "a proved orphan input stage keeps its independently checked conversion");
    stage->info.type.lifetimeArguments.front() = Lifetime::staticLifetime();
    // Matching operand and stage tags cannot replace the original source
    // place's type. There is no consuming call left to perform this check.
    stage->operands.front().type = stage->info.type;
    const auto forged = verifyMirLifetimeApplications(orphan, *body, address->owner);
    expect(!forged.valid() && std::any_of(forged.errors.begin(), forged.errors.end(),
               [](const auto &error) { return error.message.find("call-input stage") != std::string::npos; }),
           "an orphan stage cannot forge a stronger source region with matching type tags");
  } else {
    expect(false, "the copied carrier input stage must expose its stored region slot");
  }

  auto widenedConsumer = program;
  body = findMirBody(widenedConsumer, *address);
  auto *consumer = payloadConstruct(*body);
  const auto consumerStageValue = consumer->operands.front().value;
  const MirPlace *consumerOrigin = nullptr;
  for (const auto &block : body->blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.result == consumerStageValue && candidate.kind == MirInstructionKind::CallInput &&
          candidate.operands.size() == 1)
        consumerOrigin = body->findPlace(candidate.operands.front().place);
  expect(consumerOrigin != nullptr, "consumer mutation requires the original copied place");
  if (consumerOrigin) {
    consumer->info.type.arguments.front() = consumerOrigin->type;
    consumer->parameterTypes.front() = consumerOrigin->type;
    consumer->operands.front().type = consumerOrigin->type;
    const auto invalid = verifyMirLifetimeApplications(widenedConsumer, *body, address->owner);
    expect(!invalid.valid() && std::any_of(invalid.errors.begin(), invalid.errors.end(),
               [](const auto &error) { return error.message.find("payload value") != std::string::npos; }),
           "a payload consumer cannot regain an older region through a verified input stage");
  }

  auto regained = program;
  body = findMirBody(regained, *address);
  auto *replacement = payloadConstruct(*body);
  const auto firstValue = replacement->operands.front().value;
  const MirInstruction *firstStage = nullptr;
  for (const auto &block : body->blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.result == firstValue && candidate.kind == MirInstructionKind::CallInput)
        firstStage = &candidate;
  const auto *original = firstStage && firstStage->operands.size() == 1
      ? body->findPlace(firstStage->operands.front().place) : nullptr;
  expect(original != nullptr, "copied payload stage must retain its original source place");
  if (original) {
    const auto id = replacement->id;
    const auto result = replacement->result;
    *replacement = *firstStage;
    replacement->id = id;
    replacement->result = result;
    replacement->callInputKind = HirCallInputKind::Value;
    replacement->info.type = original->type;
    replacement->operands = {{.kind = MirOperandKind::Value, .value = firstValue,
                              .type = firstStage->info.type}};
    const auto invalid = verifyMirLifetimeApplications(regained, *body, address->owner);
    expect(!invalid.valid() && std::any_of(invalid.errors.begin(), invalid.errors.end(),
               [](const auto &error) { return error.message.find("call-input stage") != std::string::npos; }),
           "a second stage cannot regain the original longer region after a checked shortening");
  }

  auto wrongType = program;
  body = findMirBody(wrongType, *address);
  payloadConstruct(*body)->info.type.arguments.front() = SemanticType::Int32;
  expect(!verifyMirLifetimeApplications(wrongType, *body, address->owner).valid(),
         "MIR must preserve ordinary generic payload type identity");

  auto wrongVariant = program;
  body = findMirBody(wrongVariant, *address);
  payloadConstruct(*body)->enumVariant = 99;
  expect(!verifyMirLifetimeApplications(wrongVariant, *body, address->owner).valid(),
         "MIR must reject a variant outside the exact payload recipe");

  auto wrongOwner = program;
  body = findMirBody(wrongOwner, *address);
  payloadConstruct(*body)->enumOwner.reset();
  expect(!verifyMirLifetimeApplications(wrongOwner, *body, address->owner).valid(),
         "MIR must reject a payload construction without its exact enum owner");
}
} // namespace

int main(int argc, char **argv) {
  if (argc > 1)
    evidenceDirectory = argv[1];
  const auto generic = positive("generic-payload-region-conversion", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int wrap<lifetime a>(View<a> input) {
  auto box = Box<View<_>>::item(input);
  return 1;
}
int main() { int value = 7; View view{value}; return wrap(view) - 1; }
)gti");
  if (generic.canGenerateCode())
    checkMutations(generic.mir);

  const auto externalPayload = positive("generic-payload-external-field", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int read<lifetime a>(Box<View<a>>& box) {
  switch (box) { case Box<View<a>>::item(auto& payload): return payload.value; }
}
int main() {
  int number = 7;
  View view{number};
  auto box = Box<View<_>>::item(view);
  return read(box) - 7;
}
)gti");

  if (externalPayload.canGenerateCode())
    checkRetainedPayloadCopy(externalPayload.mir);

  (void)positive("direct-payload-region-conversion", R"gti(
enum Borrow<lifetime a> { item(int&<a> value) };
int wrap<lifetime a>(int&<a> input) {
  auto box = Borrow<_>::item(input);
  return 1;
}
int main() { int value = 7; return wrap(value) - 1; }
)gti");

  (void)positive("payload-common-input-region", R"gti(
enum Pair<lifetime a> { item(int&<a> left, int&<a> right) };
int wrap<lifetime a, lifetime b>(int&<a> left, int&<b> right) {
  auto pair = Pair<_>::item(left, right);
  return 1;
}
int main() { int left = 7; int right = 9; return wrap(left, right) - 1; }
)gti");

  const auto selectedReference = positive("payload-borrow-extract-and-copy", R"gti(
enum Borrow<lifetime a> { item(int&<a> value) };
int read<lifetime a>(Borrow<a>& input) {
  switch (input) { case Borrow<a>::item(auto& value): return value; }
}
int main() {
  int value = 7;
  auto original = Borrow<_>::item(value);
  auto copied = original;
  return read(copied) - 7;
}
)gti");

  if (selectedReference.canGenerateCode())
    checkSelectedReferenceRegionMutations(selectedReference.mir);

  const auto factory = positive("returned-enum-guard-instantiation", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> owner, int sequence) };
Reading<a> make<lifetime a>(int&<a> owner, bool present) {
  if (present) { return Reading<a>::value(owner, 7); }
  return Reading<a>::empty;
}
int inspect<lifetime a>(Reading<a>& reading) {
  switch (reading) {
  case Reading<a>::empty: return 0;
  case Reading<a>::value(auto& value, sequence): return value + sequence;
  }
}
int main() {
  int first = 3; int second = 5;
  Reading<_> left = make(first, true);
  Reading<_> right = make(second, true);
  return inspect(left) + inspect(right) - 22;
}
)gti");
  if (factory.canGenerateCode()) {
    checkGuardMutations(factory.mir);
    checkGuardBindingViews(factory);
  }

  const auto paired = positive("nested-enum-field-guard-conversion", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> owner) };
struct Pair<lifetime a, lifetime b> {
  Reading<a> first;
  Reading<b> second;
  Pair(Reading<a> left, Reading<b> right) : first(left), second(right) {}
};
int inspect<lifetime a>(Reading<a>& reading) {
  switch (reading) {
  case Reading<a>::empty: return 0;
  case Reading<a>::value(auto& value): return value;
  }
}
int main() {
  int number = 7;
  auto reading = Reading<_>::value(number);
  Pair<_, _> pair{reading, reading};
  return inspect(pair.first) + inspect(pair.second) - 14;
}
)gti");

  if (paired.canGenerateCode())
    checkConstructorFieldDomains(paired.mir);

  const auto emptyFields = positive("constructor-independent-empty-field-domain", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> owner) };
struct Pair<lifetime a, lifetime b> {
  Reading<a> first;
  Reading<b> second;
  Pair(Reading<a> left, Reading<b> right) : first(left), second(right) {}
};
int inspect<lifetime a>(Reading<a>& reading) {
  switch (reading) {
  case Reading<a>::empty: return 0;
  case Reading<a>::value(auto& value): return value;
  }
}
int main() {
  int number = 7;
  auto empty = Reading<static>::empty;
  auto full = Reading<_>::value(number);
  Pair<static, _> pair{empty, full};
  return inspect(pair.first) + inspect(pair.second) - 7;
}
)gti");
  if (emptyFields.canGenerateCode())
    checkConditionalConstructorSources(emptyFields.mir);

  const auto owning = positive("owning-region-transfer", R"gti(
mut int destroyed = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { destroyed += this.value; }
};
View<a> make<lifetime a>(int&<a> input) { return View<a>(input); }
int read<lifetime a>(View<a> view) { return view.value; }
int forward<lifetime a>(int&<a> input) {
  return read(View<a>(input)) + read(make(input));
}
int main() { int value = 7; return forward(value) - 14; }
)gti");
  if (owning.canGenerateCode())
    checkOwningTransferMutations(owning.mir);

  const auto owningEnum = positive("owning-enum-payload-handoff", R"gti(
mut int destroyed = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { destroyed += this.value; }
};
enum Box<T> { item(T value) };
void wrap<lifetime a>(int&<a> input) {
  auto box = Box<View<_>>::item(View<a>(input));
}
int main() { int number = 7; wrap(number); return destroyed - 7; }
)gti");
  if (owningEnum.canGenerateCode()) {
    const auto before = failures;
    checkOwningPayloadMutations(owningEnum.mir);
    if (failures != before)
      retain("owning-enum-payload-handoff", owningEnum);
  }

  (void)positive("temporary-read-applies-declared-region", R"gti(
struct Value { int n; Value(int n) : n(n) {} };
int read(Value& value) { return value.n; }
int main() { return read(Value(3)) - 3; }
)gti");

  negative("owning-enum-retains-borrow-until-drop", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { int observed = this.value; }
};
enum Box<T> { item(T value) };
int main() {
  mut int number = 7;
  auto box = Box<View<_>>::item(View<_>(number));
  number = 9;
  return 0;
}
)gti");

  negative("owning-carrier-cannot-extend-region", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() {}
};
int read(View<static> view) { return view.value; }
int forward<lifetime a>(int&<a> input) { return read(View<a>(input)); }
int main() { int value = 7; return forward(value); }
)gti");

  negative("payload-carrier-cannot-extend-region", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int wrap<lifetime a>(View<a> input) {
  auto box = Box<View<static>>::item(input);
  return 1;
}
int main() { int value = 7; View view{value}; return wrap(view) - 1; }
)gti");

  negative("payload-reference-cannot-extend-region", R"gti(
enum Borrow<lifetime a> { item(int&<a> value) };
int wrap<lifetime a>(int&<a> input) {
  auto box = Borrow<static>::item(input);
  return 1;
}
int main() { int value = 7; return wrap(value) - 1; }
)gti");

  negative("payload-reference-preserves-access", R"gti(
enum Borrow<lifetime a> { item(mut int&<a> value) };
int wrap<lifetime a>(int&<a> input) {
  auto box = Borrow<_>::item(input);
  return 1;
}
int main() { int value = 7; return wrap(value) - 1; }
)gti");
  return failures ? 1 : 0;
}
