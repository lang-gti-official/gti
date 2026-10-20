#include "../src/compiler/mir_verification_queries.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace lang {
struct MirProgramTestAccess {
  static auto &functions(MirProgram &program) { return program.functions; }
  static auto &payloads(MirProgram &program) { return program.payloadEnums_; }
  static auto &constructors(MirProgram &program) { return program.constructors; }
};
} // namespace lang

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  {
    using Availability = lang::BorrowSupplierAvailability;
    using Status = lang::BorrowResultDomainStatus;
    const lang::BorrowDependencyCondition outer{
        .enumeration = lang::SemanticType::enumType(1), .variant = 0};
    auto error = outer;
    error.variant = 1;
    const lang::BorrowDependencyCondition inner{
        .subobject = {10},
        .enumeration = lang::SemanticType::enumType(2),
        .variant = 0};
    auto otherInner = inner;
    otherInner.variant = 1;
    const auto restrict = [&](const auto &contract,
                              std::initializer_list<Availability> states) {
      return lang::restrictCompleteBorrowResultDomain(
          contract, std::span(states.begin(), states.size()));
    };
    lang::FunctionBorrowDependencySummary contract{
        .dependencies = {{.resultFields = {20},
                          .origin = lang::BorrowOriginKind::Argument,
                          .parameter = 0,
                          .resultConditions = {outer}},
                         {.resultFields = {20},
                          .origin = lang::BorrowOriginKind::Argument,
                          .parameter = 1,
                          .resultConditions = {outer}}},
        .possibleAlternatives = {{{outer}}, {{error}}}};
    auto domain =
        restrict(contract, {Availability::Impossible, Availability::Available});
    expect(domain.status == Status::Known &&
               domain.possibleAlternatives == contract.possibleAlternatives &&
               domain.dependencies == std::vector<std::size_t>{1},
           "OR suppliers preserve a case when another supplier remains");
    domain =
        restrict(contract, {Availability::Available, Availability::Available});
    expect(domain.status == Status::Known &&
               domain.dependencies == std::vector<std::size_t>({0, 1}),
           "every available supplier survives result-domain restriction");
    domain =
        restrict(contract, {Availability::Unknown, Availability::Available});
    expect(domain.status == Status::Unknown,
           "one known OR supplier cannot erase another unresolved owner");
    domain = restrict(contract,
                      {Availability::Impossible, Availability::Impossible});
    expect(domain.status == Status::Known && domain.dependencies.empty() &&
               domain.possibleAlternatives ==
                   std::vector<lang::BorrowAlternativePath>{{{error}}},
           "all impossible suppliers retain a real dependency-free error case");
    contract.possibleAlternatives.pop_back();
    expect(
        restrict(contract, {Availability::Impossible, Availability::Impossible})
                .status == Status::NoNormalResult,
        "pruning every case is not complete empty result evidence");
    auto reversed = contract;
    std::reverse(reversed.dependencies.begin(), reversed.dependencies.end());
    const auto forwardDomain =
        restrict(contract, {Availability::Impossible, Availability::Available});
    const auto reverseDomain =
        restrict(reversed, {Availability::Available, Availability::Impossible});
    expect(forwardDomain.possibleAlternatives ==
                   reverseDomain.possibleAlternatives &&
               forwardDomain.dependencies == std::vector<std::size_t>{1} &&
               reverseDomain.dependencies == std::vector<std::size_t>{0},
           "supplier declaration order does not change the retained domain");
    lang::FunctionBorrowDependencySummary nested{
        .dependencies = {{.resultFields = {10, 30},
                          .origin = lang::BorrowOriginKind::Argument,
                          .parameter = 0,
                          .resultConditions = {outer, inner}},
                         {.resultFields = {10, 31},
                          .origin = lang::BorrowOriginKind::Argument,
                          .parameter = 1,
                          .resultConditions = {outer, otherInner}}},
        .possibleAlternatives = {
            {{outer}}, {{error}}, {{outer, inner}}, {{outer, otherInner}}}};
    domain =
        restrict(nested, {Availability::Impossible, Availability::Available});
    expect(domain.status == Status::Known &&
               domain.possibleAlternatives ==
                   std::vector<lang::BorrowAlternativePath>(
                       {{{outer}}, {{error}}, {{outer, otherInner}}}) &&
               domain.dependencies == std::vector<std::size_t>{1},
           "nested pruning keeps the other exact typed alternative");
    domain =
        restrict(nested, {Availability::Impossible, Availability::Impossible});
    expect(domain.status == Status::Known && domain.dependencies.empty() &&
               domain.possibleAlternatives ==
                   std::vector<lang::BorrowAlternativePath>{{{error}}},
           "empty nested required domain prunes its ancestor case");
    nested.possibleAlternatives.erase(nested.possibleAlternatives.begin() + 1);
    expect(
        restrict(nested, {Availability::Impossible, Availability::Impossible})
                .status == Status::NoNormalResult,
        "nested pruning cannot fabricate a vacuous normal return");
    lang::FunctionBorrowDependencySummary scalar{
        .dependencies = {{.origin = lang::BorrowOriginKind::Argument}}};
    expect(
        restrict(scalar, {Availability::Impossible}).status ==
            Status::NoNormalResult &&restrict(scalar, {Availability::Unknown})
                .status ==
            Status::Unknown &&restrict(scalar, {Availability::Available})
                .status == Status::Known,
        "unconditional scalar owner has the same three-state boundary");
    expect(restrict(scalar, {}).status == Status::Unknown,
           "missing supplier status remains unknown");
  }
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    auto result = lang::Frontend().analyze(
        root / "tests" / "complete-borrow-summary.gti", source,
        {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(result.canGenerateCode(), "result-summary fixture must lower");
    return result;
  };
  const auto functionId = [](const lang::FrontendResult &result,
                             const std::string &name) {
    for (const auto &function : result.hir.functionInstances())
      if (function.source && function.source->name().lexeme == name)
        return function.id;
    return lang::HirFunctionInstanceId{0};
  };
  {
    std::ifstream input(root /
                        "tests/fixtures/borrowed_enum_scalar_sibling.gti");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto result = analyze(source);
    expect(result.canGenerateCode(),
           "disjoint scalar field initialization retains the exact conditional "
           "borrowed field, including its known-empty alternative");
    for (const auto &constructor : result.mir.constructorInstances()) {
      if (!constructor.completeBorrow)
        continue;
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto malformed = result.mir;
        auto &changed = lang::MirProgramTestAccess::constructors(
            malformed)[constructor.id - 1];
        auto borrowed =
            std::find_if(changed.body.places.begin(), changed.body.places.end(),
                         [](const auto &place) {
                           return place.root == lang::MirPlaceRootKind::This &&
                                  place.projections.size() == 1 &&
                                  place.type.kind == lang::SemanticType::Enum;
                         });
        bool altered = false;
        if (borrowed != changed.body.places.end())
          for (auto &block : changed.body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Initialize &&
                  instruction.destination &&
                  changed.body.findPlace(*instruction.destination)->type ==
                      lang::SemanticType::Int32) {
                if (mutation == 0)
                  instruction.destination = borrowed->id;
                else
                  borrowed->projections.front().field = 0;
                altered = true;
              }
        expect(altered && !lang::verifyMirProgram(malformed).valid(),
               "disjoint sibling proof rejects overlapping writes and forged "
               "field identities");
      }
    }
  }
  {
    std::ifstream input(root /
                        "tests/fixtures/contextual_borrowed_construction.gti");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto result = analyze(source);
    if (!result.canGenerateCode()) {
      const auto dump = std::filesystem::temp_directory_path() /
                        "gti-contextual-borrow-proof.mir";
      std::ofstream(dump) << lang::MirPrinter().print(result.mir);
      std::cerr << "Contextual borrow proof MIR: " << dump << '\n';
    }
    bool contextual = false;
    for (const auto &function : result.hir.functionInstances())
      for (const auto &entry : function.body.contextualConstructions) {
        const auto *value = function.body.findValue(entry.value);
        if (!value || !value->info.traits.containsBorrowedState)
          continue;
        contextual = true;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = result.hir;
          auto *changed = const_cast<lang::HirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *constructed = const_cast<lang::HirValue *>(
              changed->body.findValue(entry.value));
          auto *argument = const_cast<lang::HirValue *>(
              changed->body.findValue(constructed->operands.front()));
          if (mutation == 0)
            constructed->borrowDependencies.reset();
          else if (mutation == 1)
            constructed->borrowDependencies =
                lang::SemanticBorrowDependencySet{};
          else if (mutation == 2)
            constructed->borrowOrigin = lang::BorrowOriginKind::Receiver;
          else
            constructed->operands.front() = constructed->id;
          expect(argument &&
                     !lang::verifyHirProgramPlans(result.semantics, malformed)
                          .valid(),
                 "contextual borrowed result preserves its separately sealed "
                 "input, complete dependencies and constructor selection");
        }
      }
    expect(contextual,
           "unrelated source wrappers use contextual borrowed construction");
    for (const auto &rejected :
         {"View<int> dangling() { int local = 3; return local; }",
          "int conflict() { mut int owner = 3; View<int> view = owner; owner = "
          "4; return view.value(); }"}) {
      const auto invalid = lang::Frontend().analyze(
          root / "tests/contextual-borrow-rejection.gti", source + rejected,
          {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
      expect(!invalid.canGenerateCode() &&
                 std::none_of(invalid.diagnostics.begin(),
                              invalid.diagnostics.end(),
                              [](const auto &diagnostic) {
                                return diagnostic.code == "GTI-B0001";
                              }),
             "contextual borrowing rejects dangling owners and live owner "
             "writes in semantics");
    }
  }
  {
    std::ifstream input(root /
                        "tests/fixtures/borrowed_enum_no_normal_result.gti");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto result = analyze(source);
    bool found = false;
    for (const auto &function : result.hir.functionInstances())
      for (const auto &value : function.body.values)
        if (value.noNormalBorrowResultProof) {
          for (int mutation = 0; mutation < 6; ++mutation) {
            auto malformed = result.hir;
            auto *changed = const_cast<lang::HirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &call = *std::find_if(changed->body.values.begin(),
                                       changed->body.values.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == value.id;
                                       });
            if (mutation == 0)
              call.noNormalBorrowResultProof.reset();
            if (mutation == 1)
              call.source = nullptr;
            if (mutation == 2)
              call.neverReturns = false;
            if (mutation == 3)
              call.borrowDependencies = lang::SemanticBorrowDependencySet{};
            if (mutation == 4)
              const_cast<lang::HirFunctionInstance *>(
                  malformed.findFunctionInstance(*call.functionTarget))
                  ->typeArguments.push_back(lang::SemanticType::Int32);
            if (mutation == 5)
              const_cast<lang::HirFunctionInstance *>(
                  malformed.findFunctionInstance(*call.functionTarget))
                  ->parameterTypes.push_back(lang::SemanticType::Int32);
            expect(!lang::verifyHirProgramPlans(result.semantics, malformed)
                        .valid(),
                   "excluded normal result requires exact semantic proof " +
                       std::to_string(mutation));
          }
        }
    for (const auto &function : result.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.noNormalBorrowResult) {
            found = true;
            expect(instruction.neverReturns && !instruction.result &&
                       !instruction.loan,
                   "excluded reference-result domain publishes no result");
            for (int mutation = 0; mutation < 5; ++mutation) {
              auto malformed = result.mir;
              auto &changed = lang::MirProgramTestAccess::functions(
                  malformed)[function.id - 1];
              auto &instructions =
                  changed.body.blocks[block.id - 1].instructions;
              auto &call =
                  *std::find_if(instructions.begin(), instructions.end(),
                                [&](const auto &candidate) {
                                  return candidate.id == instruction.id;
                                });
              if (mutation == 0)
                call.noNormalBorrowResult = false;
              if (mutation == 1)
                call.neverReturns = false;
              if (mutation == 2)
                call.result = changed.body.values.front().id;
              if (mutation == 3)
                call.borrowFields.clear();
              if (mutation == 4)
                for (auto &continuation : changed.body.blocks)
                  if (continuation.terminator.kind ==
                      lang::MirTerminatorKind::Unreachable)
                    continuation.terminator = {
                        .kind = lang::MirTerminatorKind::Return};
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "excluded normal result rejects absent proof, publication "
                     "or continuation " +
                         std::to_string(mutation));
            }
          }
    expect(found, "known missing alternative has a call-local terminal proof");
    const auto unknownId = functionId(result, "inspect_unknown");
    const auto presentId = functionId(result, "inspect_present");
    const auto *unknown = result.mir.findFunctionInstance(unknownId);
    const auto *present = result.mir.findFunctionInstance(presentId);
    expect(unknown && present,
           "terminal proof includes unknown-input and normal-result controls");
    if (unknown && !unknown->parameterBindings.empty()) {
      auto malformed = result.mir;
      auto &changed =
          lang::MirProgramTestAccess::functions(malformed)[unknownId - 1];
      const auto incoming = std::find_if(
          changed.body.places.begin(), changed.body.places.end(),
          [&](const auto &place) {
            return place.root == lang::MirPlaceRootKind::Binding &&
                   place.binding == changed.parameterBindings.front() &&
                   place.projections.empty();
          });
      bool substituted = false;
      if (incoming != changed.body.places.end())
        for (auto &block : changed.body.blocks)
          for (auto &instruction : block.instructions) {
            if (instruction.kind != lang::MirInstructionKind::CallInput ||
                instruction.callInputRole != lang::MirCallInputRole::Receiver ||
                instruction.operands.size() != 1 || !instruction.result)
              continue;
            // Keep the exact receiver type and valid caller-owned storage,
            // but replace the known-empty input by a parameter whose active
            // alternative is unknown. The optional witness is omitted so
            // exclusion must fail on independently reconstructed flow.
            instruction.operands.front() = {
                .kind = lang::MirOperandKind::BorrowRead,
                .place = incoming->id,
                .type = incoming->type};
            const_cast<lang::MirValue *>(
                changed.body.findValue(*instruction.result))
                ->borrowDependencies.reset();
            substituted = true;
          }
      expect(substituted && !lang::verifyMirProgram(malformed).valid(),
             "unknown actual input cannot retain an excluded-result claim");
    }
    if (present) {
      bool normal = false;
      for (const auto &block : present->body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.functionTarget) {
            const auto *target =
                result.mir.findFunctionInstance(*instruction.functionTarget);
            if (target &&
                target->returnType.kind == lang::SemanticType::Reference)
              normal = !instruction.noNormalBorrowResult &&
                       !instruction.neverReturns && !target->neverReturns &&
                       instruction.loan != 0;
          }
      expect(normal,
             "the same accessor retains an ordinary checked normal result");
    }
  }
  {
    const auto result = analyze(R"gti(
#include <std/panic>
enum Selected { empty, value(int& owner) };
[[borrowed_from(input)]] int& selected(mut Selected input, int& other) {
  int ignored = other;
  switch (std::move(input)) {
  case Selected::value(owner): return owner;
  case Selected::empty: std::panic("empty");
  }
}
int main() { return 0; }
)gti");
    const auto id = functionId(result, "selected");
    const auto *function = result.mir.findFunctionInstance(id);
    expect(function && function->completeReturnBorrow &&
               function->completeReturnBorrow->dependencies.size() == 1 &&
               function->completeReturnBorrow->possibleAlternatives.empty() &&
               function->completeReturnBorrow->dependencies.front()
                   .resultFields.empty() &&
               function->completeReturnBorrow->dependencies.front()
                       .sourceConditions.size() == 1,
           "selected reference return retains its guarded external source");
    if (function && function->completeReturnBorrow) {
      for (int mutation = 0; mutation < 4; ++mutation) {
        auto malformed = result.mir;
        auto &changed =
            lang::MirProgramTestAccess::functions(malformed)[id - 1];
        auto &mapping = changed.completeReturnBorrow->dependencies.front();
        if (mutation == 0)
          mapping.sourceConditions.clear();
        else if (mutation == 1)
          mapping.sourceFields.clear();
        else if (mutation == 2)
          mapping.access = lang::AccessMode::Mutable;
        else
          changed.completeReturnBorrow->dependencies.clear();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "reference contract rejects erased guard/path/owner and "
               "strengthened authority");
      }
      for (int mutation = 0; mutation < 5; ++mutation) {
        auto malformed = result.mir;
        auto &changed =
            lang::MirProgramTestAccess::functions(malformed)[id - 1];
        auto &body = changed.body;
        auto published = std::find_if(
            body.loans.begin(), body.loans.end(), [](const auto &loan) {
              return loan.kind == lang::MirLoanKind::Return;
            });
        auto selected = std::find_if(
            body.loans.begin(), body.loans.end(), [](const auto &loan) {
              return loan.referenceSelection.has_value();
            });
        expect(published != body.loans.end() && selected != body.loans.end(),
               "selected scalar return has separate activation/publication");
        if (published == body.loans.end() || selected == body.loans.end())
          continue;
        if (mutation == 0) {
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
                  instruction.loan == selected->id)
                instruction.loan = published->id;
        } else if (mutation == 1) {
          published->conditions = selected->referenceSelection->conditions;
        } else if (mutation == 2) {
          ++selected->producedBy;
        } else if (mutation == 3) {
          selected->access = lang::AccessMode::Mutable;
          published->access = lang::AccessMode::Mutable;
        } else {
          const auto other = std::find_if(
              body.places.begin(), body.places.end(), [](const auto &place) {
                return place.type == lang::SemanticType::Int32 &&
                       place.root == lang::MirPlaceRootKind::Binding &&
                       place.projections.size() == 1 &&
                       place.projections.front().kind ==
                           lang::MirProjectionKind::Dereference;
              });
          expect(other != body.places.end(),
                 "unrelated formal referent exists");
          if (other == body.places.end())
            continue;
          published->source = other->id;
          selected->source = other->id;
          auto &mapping = changed.completeReturnBorrow->dependencies.front();
          mapping.parameter = 1;
          mapping.sourceFields.clear();
          mapping.sourceConditions.clear();
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "selected return rejects missing retirement, forged activation, "
               "access strengthening and mutually substituted origin claims");
      }
    }
  }
  {
    const auto result = analyze(R"gti(
enum Exclusive { empty, value(mut int& owner) };
void hold(mut int& owner) { auto carrier = Exclusive::value(owner); }
int main() { return 0; }
)gti");
    if (result.canGenerateCode()) {
      auto malformed = result.mir;
      bool changed = false;
      for (auto &payload : lang::MirProgramTestAccess::payloads(malformed))
        for (const auto &variant : payload.variants)
          for (const auto &field : variant.payloadTypes)
            if (field.kind == lang::SemanticType::Reference &&
                field.referenceAccess == lang::AccessMode::Mutable) {
              expect(!payload.traits.copyable,
                     "exclusive stored capability is move-only");
              payload.traits.copyable = true;
              changed = true;
            }
      expect(changed && !lang::verifyMirProgram(malformed).valid(),
             "MIR rejects copyable exclusive-reference enum metadata");
    }
  }
  {
    const std::string declaration =
        "enum Reading { error, value(int& owner) };\n";
    const std::string inspect = R"gti(
[[borrowed_from(owner)]] Reading inspect(int& owner, bool fail) {
  if (fail) return Reading::error;
  return Reading::value(owner);
}
)gti";
    const std::string unrelated = "int unrelated() { return 7; }\n";
    for (const bool reversed : {false, true}) {
      const auto result =
          lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
              .analyze(
                  root / "tests" / "complete-borrow-producer.gti",
                  declaration +
                      (reversed ? unrelated + inspect : inspect + unrelated) +
                      "int main() { return 0; }\n",
                  {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
      expect(result.syntaxValid && result.semanticValid,
             "conditional constraints admit complete borrowed producers");
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(result.diagnostics.empty(),
             "conditional constraint has no unrelated semantic failure");
      bool inspected = false;
      for (const auto &statement : result.program.declarations()) {
        const auto *syntax = lang::stmtAs<lang::FunctionDecl>(statement.get());
        if (!syntax || syntax->name().lexeme != "inspect")
          continue;
        inspected = true;
        const auto *function = result.semantics.findFunction(*syntax);
        expect(function && function->explicitReturnBorrowOrigin &&
                   function->returnBorrowOrigin ==
                       lang::BorrowOriginKind::Argument &&
                   function->returnBorrowParameter == 0,
               "conditional result retains its explicit formal constraint");
        expect(
            function && function->completeReturnBorrow &&
                function->completeReturnBorrow->dependencies.size() == 1 &&
                function->completeReturnBorrow->possibleAlternatives.size() ==
                    2,
            "valid returns publish both the borrowed and dependency-free "
            "domains");
        const auto *owner =
            result.semantics.findBinding(syntax->parameters().front());
        bool foundProducer = false;
        for (const auto &bodyStatement : syntax->body()->statements()) {
          const auto *returned =
              lang::stmtAs<lang::ReturnStmt>(bodyStatement.get());
          if (!returned || !returned->value())
            continue;
          foundProducer = true;
          const auto *fact =
              result.semantics.findBorrowDependencySet(*returned->value());
          expect(fact && fact->dependencies.size() == 1 &&
                     fact->possibleAlternatives.size() == 1,
                 "direct producer retains its exact active owner fact");
          if (!fact || fact->dependencies.size() != 1)
            continue;
          const auto &dependency = fact->dependencies.front();
          expect(owner && dependency.place.root == owner->symbol &&
                     dependency.place.projections.empty() &&
                     dependency.designatedFields.size() == 1 &&
                     dependency.conditions.size() == 1 &&
                     dependency.access == lang::AccessMode::ReadOnly &&
                     fact->possibleAlternatives.front().conditions ==
                         dependency.conditions,
                 "producer keeps the exact source, field, access and guard");
        }
        expect(foundProducer, "conditional producer return was inspected");
      }
      expect(inspected, "conditional constraint declaration was inspected");
    }
    // Bad explicit constraints require their own owning-layer diagnostic;
    // unrelated admission failures are not evidence that they were checked.
    for (const auto &[source, expectedCode] :
         std::vector<std::pair<std::string, std::string>>{
             {declaration +
                  "[[borrowed_from(typo)]] Reading wrong(int& owner) { "
                  "return Reading::value(owner); }\n",
              "GTI-S2099"},
             {declaration +
                  "[[borrowed_from(owner)]] Reading wrong(int& owner, "
                  "int& other) { return Reading::value(other); }\n",
              "GTI-S2045"},
             {"[[borrowed_from(owner)]] int wrong(int& owner) { "
              "return owner; }\n",
              "GTI-S2099"}}) {
      const auto result =
          lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
              .analyze(root / "tests" / "complete-borrow-constraint.gti",
                       source + "int main() { return 0; }\n",
                       {root / "stdlib" / "prelude.gti"}, {},
                       {root / "stdlib"});
      expect(!result.semanticValid && std::any_of(result.diagnostics.begin(),
                                                  result.diagnostics.end(),
                                                  [&](const auto &item) {
                                                    return item.code ==
                                                           expectedCode;
                                                  }),
             "bad explicit constraints retain their own diagnostic");
    }
  }
  const auto explicitConstraint = analyze(R"gti(
[[borrowed_from(value)]] int& selected(int& value, int& other) {
  return value;
}
int& inferred(int& value) { return value; }
[[borrowed_from(value)]] T& selected_generic<T>(T& value) { return value; }
[[borrowed_from(value)]] T copied_generic<T>(T value) { return value; }
int main() {
  int first = 1;
  int second = 2;
  int& chosen = selected(first, second);
  int& automatic = inferred(first);
  int& concrete = selected_generic<int>(first);
  int copied = copied_generic<int>(first);
  return chosen + automatic + concrete + copied - 4;
}
)gti");
  if (explicitConstraint.canGenerateCode()) {
    for (const auto &name : {"selected", "inferred", "selected_generic"}) {
      const auto id = functionId(explicitConstraint, name);
      const auto *hir = explicitConstraint.hir.findFunctionInstance(id);
      const auto *mir = explicitConstraint.mir.findFunctionInstance(id);
      const bool explicitOrigin = std::string(name) != "inferred";
      expect(hir && mir && hir->explicitReturnBorrowOrigin == explicitOrigin &&
                 mir->explicitReturnBorrowOrigin == explicitOrigin &&
                 mir->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
                 mir->returnBorrowParameter == 0,
             "source and concrete lifetime constraints survive HIR and MIR");
    }
    auto missingOrigin = explicitConstraint.mir;
    const auto *copied = explicitConstraint.mir.findFunctionInstance(
        functionId(explicitConstraint, "copied_generic"));
    expect(copied && copied->explicitReturnBorrowOrigin &&
               copied->returnBorrowOrigin == lang::BorrowOriginKind::None,
           "dependency-free concrete result retains vacuous explicit intent");
    const auto selectedId = functionId(explicitConstraint, "selected");
    lang::MirProgramTestAccess::functions(missingOrigin)[selectedId - 1]
        .returnBorrowOrigin = lang::BorrowOriginKind::None;
    expect(!lang::verifyMirProgram(missingOrigin).valid(),
           "explicit constraint cannot lose its resolved origin");
    auto substitutedOrigin = explicitConstraint.mir;
    lang::MirProgramTestAccess::functions(substitutedOrigin)[selectedId - 1]
        .returnBorrowParameter = 1;
    expect(!lang::verifyMirProgram(substitutedOrigin).valid(),
           "explicit formal substitution must disagree with actual return");
    expect(lang::MirPrinter()
                   .print(explicitConstraint.mir)
                   .find("explicit-return-borrow=1") != std::string::npos,
           "deterministic program serialization preserves explicit intent");
  }
  {
    const auto result = analyze(R"gti(
enum Forwarded { empty, value(int& owner) };
[[borrowed_from(owner)]] Forwarded make(int& owner, bool empty) {
  if (empty) return Forwarded::empty;
  return Forwarded::value(owner);
}
[[borrowed_from(owner)]] Forwarded relay(int& owner) {
  return make(owner, false);
}
int inspect_forwarded(int& owner) {
  auto held = make(owner, false);
  return 0;
}
int main() { return 0; }
)gti");
    const auto id = functionId(result, "inspect_forwarded");
    const auto *function = result.mir.findFunctionInstance(id);
    if (result.canGenerateCode() && function) {
      expect(
          std::none_of(function->body.loans.begin(), function->body.loans.end(),
                       [](const auto &loan) { return loan.entry; }),
          "forwarding-only formal fixture uses exact places, not entry loans");
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = result.mir;
        auto &changed =
            lang::MirProgramTestAccess::functions(malformed)[id - 1];
        const auto binding = changed.parameterBindings.front();
        for (auto &place : changed.body.places) {
          if (place.root != lang::MirPlaceRootKind::Binding ||
              place.binding != binding)
            continue;
          if (mutation == 0)
            place.initiallyAvailable = false;
          if (mutation == 1)
            place.foreignStorage = true;
          if (mutation == 2 && !place.projections.empty())
            ++place.symbol;
          if (mutation == 3 && !place.projections.empty())
            place.key.reset();
          if (mutation == 4 && !place.projections.empty())
            place.access = lang::AccessMode::Mutable;
        }
        if (mutation == 5)
          changed.parameterTypes.front() = lang::SemanticType::Int32;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "forwarded formal reference requires exact typed, available, "
               "same-identity referent with no strengthened access " +
                   std::to_string(mutation));
      }
    }
  }
  {
    const auto result = analyze(R"gti(
enum Either { left(int& owner), right(int& owner) };
[[borrowed_from(input)]] int& select_either(mut Either input) {
  switch (std::move(input)) {
  case Either::left(owner): return owner;
  case Either::right(owner): return owner;
  }
}
int main() { return 0; }
)gti");
    const auto id = functionId(result, "select_either");
    const auto *function = result.mir.findFunctionInstance(id);
    expect(function && function->completeReturnBorrow &&
               function->completeReturnBorrow->dependencies.size() == 2,
           "scalar reference summary preserves owners from every normal arm");
    if (result.canGenerateCode() && function &&
        function->completeReturnBorrow) {
      for (int mutation = 0; mutation < 3; ++mutation) {
        auto malformed = result.mir;
        auto &changed =
            lang::MirProgramTestAccess::functions(malformed)[id - 1];
        auto &dependencies = changed.completeReturnBorrow->dependencies;
        if (mutation == 0)
          dependencies.pop_back();
        else if (mutation == 1)
          dependencies.back() = dependencies.front();
        else
          dependencies.back().sourceConditions =
              dependencies.front().sourceConditions;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "scalar summary rejects lost, substituted and wrongly guarded "
               "alternative owners");
      }
    }
  }
  {
    // Static storage is its own origin: the contract names the global, not an
    // argument, and the verifier rejects a lost, substituted or strengthened
    // global from the provisional program onward.
    const auto result = analyze(R"gti(
enum Slot { item(int& value), empty };
int global_value = 19;
Slot borrow_global(bool present) {
  if (!present) { return Slot::empty; }
  return Slot::item(global_value);
}
bool occupied(Slot value) {
  switch (value) {
  case Slot::item: return true;
  case Slot::empty: return false;
  }
}
int main() { auto value = borrow_global(true); return occupied(value) ? 0 : 1; }
)gti");
    const auto id = functionId(result, "borrow_global");
    const auto *function = result.mir.findFunctionInstance(id);
    const bool globalContract =
        function && function->completeReturnBorrow &&
        function->completeReturnBorrow->dependencies.size() == 1 &&
        function->completeReturnBorrow->dependencies.front().origin ==
            lang::BorrowOriginKind::Global &&
        function->completeReturnBorrow->dependencies.front().globalRoot != 0;
    expect(result.canGenerateCode() && globalContract,
           "global-borrowing carrier names its static storage origin");
    if (result.canGenerateCode() && globalContract) {
      for (int mutation = 0; mutation < 4; ++mutation) {
        auto malformed = result.mir;
        auto &dependency =
            lang::MirProgramTestAccess::functions(malformed)[id - 1]
                .completeReturnBorrow->dependencies.front();
        if (mutation == 0)
          dependency.globalRoot = 0;
        else if (mutation == 1)
          dependency.globalRoot += 1000;
        else if (mutation == 2) {
          dependency.origin = lang::BorrowOriginKind::Argument;
          dependency.globalRoot = 0;
        } else
          dependency.access = lang::AccessMode::Mutable;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "global summary rejects a lost, substituted or strengthened "
               "static origin " +
                   std::to_string(mutation));
      }
      for (const auto level :
           {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
        const auto optimized = lang::OptimizationPipeline().run(
            {.hir = result.hir, .mir = result.mir, .level = level});
        expect(optimized.valid(),
               "optimizer preserves the global result contract");
      }
    }
  }
  const std::string token = "enum Token { empty, item(int value) };\n";
  const std::string leaf = "Token leaf() { return Token::empty; }\n";
  const std::string forwarding = "Token forward() { return leaf(); }\n";
  {
    const auto stagedSource = analyze(
        token +
        "Token staged() { return Token::item(7); } int main() { return 0; }");
    if (const auto *function = stagedSource.mir.findFunctionInstance(
            functionId(stagedSource, "staged"))) {
      auto staged = function->body;
      lang::MirInstructionId fresh = 1;
      for (const auto &block : staged.blocks)
        for (const auto &instruction : block.instructions)
          fresh = std::max(fresh, instruction.id + 1);
      lang::MirInstructionId constructor = 0;
      for (auto &block : staged.blocks) {
        for (std::size_t index = 0; index < block.instructions.size();
             ++index) {
          auto &producer = block.instructions[index];
          if (producer.operation != lang::MirOperation::PayloadConstruct ||
              producer.operands.size() != 1)
            continue;
          const auto source = producer.operands.front();
          const auto *sourceValue = staged.findValue(source.value);
          if (!sourceValue)
            continue;
          constructor = producer.id;
          auto info = sourceValue->info;
          info.category = lang::ValueCategory::Value;
          const auto prepared =
              static_cast<lang::MirValueId>(staged.values.size() + 1);
          const auto sourceHir = sourceValue->sourceValue;
          staged.values.push_back({.id = prepared,
                                   .sourceValue = sourceHir,
                                   .info = info,
                                   .definitionBlock = block.id,
                                   .definition = fresh});
          producer.callSite = producer.hirValue;
          producer.parameterTypes = {source.type};
          producer.operands = {{.kind = lang::MirOperandKind::Value,
                                .value = prepared,
                                .type = source.type}};
          lang::MirInstruction input{
              .id = fresh,
              .kind = lang::MirInstructionKind::CallInput,
              .hirValue = sourceHir,
              .callSite = producer.callSite,
              .callInputRole = lang::MirCallInputRole::Argument,
              .callInputKind = lang::HirCallInputKind::Value,
              .result = prepared,
              .operands = {source},
              .info = info};
          block.instructions.insert(block.instructions.begin() + index, input);
          ++index;
        }
      }
      expect(constructor != 0 && lang::rebuildMirValueUses(staged) &&
                 lang::verifyMirBody(staged).valid(),
             "typed payload construction reuses ordered argument staging");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto invalid = staged;
        for (auto &block : invalid.blocks)
          for (auto &instruction : block.instructions) {
            if (instruction.id == fresh && mutation == 0)
              instruction.callInputIndex = 1;
            if (instruction.id == constructor && mutation == 1)
              instruction.callSite = 0;
            if (instruction.id == constructor && mutation == 2)
              instruction.parameterTypes = {lang::SemanticType::Bool};
            if (instruction.id == constructor && mutation == 3)
              instruction.receiver = instruction.operands.front();
          }
        expect(!lang::verifyMirBody(invalid).valid(),
               "payload staging rejects wrong index/site/type/receiver");
      }
    }
  }
  {
    // This is a shape-query fixture, not a well-typed admitted program. The
    // separate program and live-flow proofs cannot be replaced by this query.
    lang::MirBody body;
    body.entry = 1;
    body.blocks.resize(2);
    body.blocks[0].id = 1;
    body.blocks[1].id = 2;
    body.places.resize(3);
    body.places[0].id = 1;
    body.places[0].root = lang::MirPlaceRootKind::Binding;
    body.places[0].binding = 1;
    body.places[0].type = lang::SemanticType::enumType(1);
    body.places[1].id = 2;
    body.places[1].root = lang::MirPlaceRootKind::Value;
    body.places[1].value = 1;
    body.places[2].id = 3;
    body.places[2].root = lang::MirPlaceRootKind::Binding;
    body.places[2].binding = 2;
    body.values.push_back({.id = 1, .definitionBlock = 2, .definition = 1});
    body.loans.resize(2);
    body.loans[0].id = 1;
    body.loans[0].kind = lang::MirLoanKind::Stored;
    body.loans[0].carriers = {1};
    body.loans[1].id = 2;
    body.loans[1].producedBy = 1;
    body.loans[1].carriers = {2};
    body.loans[1].referenceSelection = lang::MirReferenceSelection{
        .value = 1, .carrier = 1, .transferredFrom = 1};
    auto &edge = body.blocks[0].terminator;
    edge.kind = lang::MirTerminatorKind::Switch;
    edge.value =
        lang::MirOperand{.kind = lang::MirOperandKind::BorrowRead, .place = 1};
    lang::SwitchCaseValue tag;
    tag.enumOwner = 1;
    edge.switchTargets.push_back({.value = tag, .target = 2});
    auto &instructions = body.blocks[1].instructions;
    instructions.resize(4);
    auto &extract = instructions[0];
    extract.id = 1;
    extract.hirValue = 1;
    extract.result = 1;
    extract.operation = lang::MirOperation::PayloadExtract;
    extract.enumOwner = 1;
    extract.enumVariant = 0;
    extract.payloadIndex = 0;
    extract.payloadExtractionKind =
        lang::PayloadExtractionKind::TransferReference;
    extract.operands = {{.kind = lang::MirOperandKind::Move,
                         .place = 1,
                         .type = body.places[0].type}};
    instructions[1].id = 2;
    instructions[1].kind = lang::MirInstructionKind::EndBorrow;
    instructions[1].loan = 1;
    instructions[2].id = 3;
    instructions[2].kind = lang::MirInstructionKind::Borrow;
    instructions[2].loan = 2;
    instructions[2].operands = {
        {.kind = lang::MirOperandKind::BorrowRead, .place = 2}};
    instructions[3].id = 4;
    instructions[3].kind = lang::MirInstructionKind::Initialize;
    instructions[3].destination = 3;
    instructions[3].operands = {
        {.kind = lang::MirOperandKind::Loan, .loan = 2}};
    const auto proof =
        lang::mir_detail::selectedReferenceHandoff(body, body.loans[1]);
    expect(proof && proof->retirement == 2 && proof->endPosition == 4 &&
               proof->successors == std::vector<lang::MirLoanId>{2},
           "closed handoff shape caches before retirement and publishes before "
           "use");
    for (int mutation = 0; mutation != 10; ++mutation) {
      auto malformed = body;
      auto &prefix = malformed.blocks[1].instructions;
      if (mutation == 0)
        std::swap(prefix[0], prefix[1]);
      if (mutation == 1)
        prefix[1].kind = lang::MirInstructionKind::Call;
      if (mutation == 2)
        prefix[1].loan = 2;
      if (mutation == 3)
        prefix[2].operands[0].place = 3;
      if (mutation == 4)
        prefix.pop_back();
      if (mutation == 5)
        prefix[0].payloadIndex = 1;
      if (mutation == 6)
        malformed.blocks[0].terminator.switchTargets[0].value->value.magnitude =
            1;
      if (mutation == 7)
        malformed.loans[0].carriers.clear();
      if (mutation == 8)
        malformed.places[2].binding = 3;
      if (mutation == 9)
        prefix.insert(prefix.begin() + 1, prefix.front());
      expect(!lang::mir_detail::selectedReferenceHandoff(malformed,
                                                         malformed.loans[1]),
             "handoff rejects reordered/missing/substituted/duplicated setup "
             "identities");
    }
    // A body-local fixture supplies an already established external authority.
    // It does not claim the sealed enum/formal inventory of a complete program.
    // This checks the permission transition separately from the shape query.
    auto live = body;
    live.kind = lang::MirBodyKind::Function;
    live.blocks.resize(3);
    live.blocks[2].id = 3;
    live.blocks[0].terminator.target = 3;
    live.blocks[0].terminator.value->type = live.places[0].type;
    live.blocks[0].terminator.switchTargets[0].value->kind =
        lang::SwitchCaseKind::Enumerator;
    live.blocks[0].terminator.switchTargets[0].value->type =
        live.places[0].type;
    live.blocks[1].terminator.kind = lang::MirTerminatorKind::Return;
    live.blocks[2].terminator.kind = lang::MirTerminatorKind::Return;
    live.places[0].initiallyAvailable = true;
    live.places[1].type = lang::SemanticType::Int32;
    const auto reference =
        lang::SemanticType::referenceTo(lang::SemanticType::Int32);
    live.places[2].type = reference;
    live.places.push_back({.id = 4,
                           .root = lang::MirPlaceRootKind::Binding,
                           .binding = 3,
                           .type = lang::SemanticType::Int32,
                           .initiallyAvailable = true});
    live.loans[0].source = 4;
    live.loans[0].entry = true;
    live.loans[0].carriers.push_back(3);
    live.loans[1].source = 4;
    live.loans[1].referenceSelection->fields = {1};
    live.loans[1].referenceSelection->conditions = {
        {.enumeration = live.places[0].type, .variant = 0}};
    live.loans[1].referenceSelection->reference = reference;
    auto &setup = live.blocks[1].instructions;
    setup[0].info.type = lang::SemanticType::Int32;
    setup[0].info.category = lang::ValueCategory::Place;
    live.values[0].info = setup[0].info;
    live.values[0].sourceValue = setup[0].hirValue;
    setup[2].operands[0].type = reference;
    setup[3].operands[0].type = reference;
    setup[3].operands[0].place = 2;
    setup.push_back(
        {.id = 5, .kind = lang::MirInstructionKind::EndBorrow, .loan = 2});
    live.blocks[2].instructions.push_back(
        {.id = 6, .kind = lang::MirInstructionKind::EndBorrow, .loan = 1});
    lang::rebuildMirReachability(live);
    expect(lang::rebuildMirValueUses(live), "handoff fixture value uses index");
    const auto liveResult = lang::verifyMirBody(live);
    for (const auto &error : liveResult.errors)
      std::cerr << "live handoff fixture block " << error.block
                << " instruction " << error.instruction << ": " << error.message
                << '\n';
    expect(liveResult.valid(),
           "exact consuming prefix transfers an active external capability");
    auto expired = live;
    expired.blocks[0].instructions.push_back(
        {.id = 7, .kind = lang::MirInstructionKind::EndBorrow, .loan = 1});
    expect(lang::rebuildMirValueUses(expired) &&
               !lang::verifyMirBody(expired).valid(),
           "an earlier retirement cannot authorize a later handoff");
    auto invented = live;
    invented.loans[1].producedBy = 0;
    expect(!lang::verifyMirBody(invented).valid(),
           "a handoff producer cannot pretend to be a parentless later alias");
    auto substituted = live;
    substituted.places.push_back({.id = 5,
                                  .root = lang::MirPlaceRootKind::Binding,
                                  .binding = 4,
                                  .type = lang::SemanticType::Int32,
                                  .initiallyAvailable = true});
    substituted.loans[1].source = 5;
    expect(!lang::verifyMirBody(substituted).valid(),
           "handoff cannot substitute a same-typed external owner");
    auto stronger = live;
    stronger.loans[1].access = lang::AccessMode::Mutable;
    expect(!lang::verifyMirBody(stronger).valid(),
           "handoff cannot strengthen its former group's shared access");
  }
  for (const auto &declarations : {leaf + forwarding, forwarding + leaf}) {
    const auto result =
        analyze(token + declarations +
                "int main() { auto value = forward(); return 0; }");
    if (!result.canGenerateCode())
      continue;
    const auto *forward =
        result.mir.findFunctionInstance(functionId(result, "forward"));
    expect(forward && forward->completeReturnBorrow &&
               forward->completeReturnBorrow->possibleAlternatives.size() == 1,
           "forward result evidence must be independent of declaration order");
    bool provedCall = false;
    if (forward)
      for (const auto &block : forward->body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.result)
            provedCall |= forward->body.findValue(*instruction.result)
                              ->borrowDependencies.has_value();
    expect(provedCall,
           "exact calls transport independently verified complete facts");
    auto missing = result.mir;
    auto &functions = lang::MirProgramTestAccess::functions(missing);
    functions[functionId(result, "leaf") - 1].completeReturnBorrow.reset();
    expect(!lang::verifyMirProgram(missing).valid(),
           "caller cannot rely on a missing callee result contract");
  }

  const auto joined = analyze(token + R"gti(
Token choose(bool first) {
  if (first) { return Token::empty; }
  return Token::item(7);
}
Token narrowed() {
  if (true) { return Token::empty; }
  return Token::item(8);
}
int main() { auto value = choose(true); return 0; }
)gti");
  if (joined.canGenerateCode()) {
    const auto id = functionId(joined, "choose");
    const auto *function = joined.mir.findFunctionInstance(id);
    expect(function && function->completeReturnBorrow &&
               function->completeReturnBorrow->possibleAlternatives.size() == 2,
           "all reachable return alternatives contribute to the contract");
    if (function && function->completeReturnBorrow) {
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = joined.mir;
        auto &summary =
            *lang::MirProgramTestAccess::functions(malformed)[id - 1]
                 .completeReturnBorrow;
        if (mutation == 0)
          summary.possibleAlternatives.pop_back();
        if (mutation == 1)
          summary.possibleAlternatives.back() =
              summary.possibleAlternatives.front();
        if (mutation == 2)
          summary.possibleAlternatives.front().conditions.front().enumeration =
              lang::SemanticType::Int32;
        if (mutation == 3)
          summary.possibleAlternatives.front().conditions.front().variant =
              1000;
        if (mutation == 4)
          summary.possibleAlternatives.front().conditions.front().subobject = {
              1};
        if (mutation == 5)
          summary.dependencies.push_back({});
        expect(!lang::verifyMirProgram(malformed).valid(),
               "forged, missing or substituted result contract must fail: " +
                   std::to_string(mutation));
      }
    }
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = joined.hir, .mir = joined.mir, .level = level});
      expect(
          optimized.valid(),
          "optimizer narrowing preserves conservative declaration contracts");
    }
  }

  const auto nested = analyze(token + R"gti(
enum Envelope { wrap(Token item), empty };
Token unwrap(Envelope input) {
  switch (input) {
  case Envelope::wrap(value): return value;
  case Envelope::empty: return Token::empty;
  }
}
int main() { auto value = unwrap(Envelope::wrap(Token::item(7))); return 0; }
)gti");
  if (nested.canGenerateCode()) {
    const auto id = functionId(nested, "unwrap");
    auto projected = nested.mir;
    auto &body = lang::MirProgramTestAccess::functions(projected)[id - 1].body;
    lang::MirValueId selected = 0;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.operation == lang::MirOperation::PayloadExtract &&
            instruction.result &&
            instruction.info.type.kind == lang::SemanticType::Enum) {
          selected = *instruction.result;
          lang::MirBorrowDependencySet complete;
          const auto *enumeration =
              projected.findPayloadEnum(instruction.info.type);
          if (enumeration)
            for (std::size_t variant = 0;
                 variant < enumeration->variants.size(); ++variant)
              complete.possibleAlternatives.push_back(
                  {.conditions = {{.enumeration = instruction.info.type,
                                   .variant = variant}}});
          body.values[selected - 1].borrowDependencies = std::move(complete);
        }
    expect(selected != 0, "nested payload fixture must select a child enum");
    expect(lang::verifyMirProgram(projected).valid(),
           "selected nested payload facts follow exact carrier flow");
    if (selected != 0)
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = projected;
        auto &set = *lang::MirProgramTestAccess::functions(malformed)[id - 1]
                         .body.values[selected - 1]
                         .borrowDependencies;
        if (mutation == 0)
          set.possibleAlternatives.pop_back();
        if (mutation == 1)
          set.possibleAlternatives.front().conditions.front().enumeration =
              lang::SemanticType::Int32;
        if (mutation == 2)
          set.possibleAlternatives.front().conditions.front().variant = 1000;
        if (mutation == 3)
          set.possibleAlternatives.front().conditions.front().subobject = {1};
        if (mutation == 4)
          set.possibleAlternatives.back() = set.possibleAlternatives.front();
        if (mutation == 5)
          set.dependencies.push_back({});
        expect(!lang::verifyMirProgram(malformed).valid(),
               "selected payload proof rejects missing/substituted facts: " +
                   std::to_string(mutation));
      }
  }

  const auto selectedShape = analyze(token + R"gti(
int read(mut Token& input, mut int& other) {
  auto& otherRef = other;
  mut int result = 0;
  switch (input) {
  case Token::item(auto& item): result = otherRef; result = item; break;
  case Token::empty: result = otherRef; break;
  }
  return result + other;
}
int main() { mut Token value = Token::item(7); mut int other = 1; return read(value, other); }
)gti");
  if (selectedShape.canGenerateCode()) {
    auto shape = selectedShape.mir;
    const auto id = functionId(selectedShape, "read");
    auto &body = lang::MirProgramTestAccess::functions(shape)[id - 1].body;
    lang::MirLoanId selectedLoan = 0;
    lang::MirInstructionId producer = 0;
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.operation == lang::MirOperation::PayloadExtract &&
            instruction.result)
          for (auto &loan : body.loans)
            if (loan.producedBy == instruction.hirValue && loan.parent != 0) {
              const auto &type = instruction.operands.front().type;
              const auto *enumeration = shape.findPayloadEnum(type);
              const auto field = enumeration->variants[*instruction.enumVariant]
                                     .payloadSymbols[*instruction.payloadIndex];
              instruction.payloadExtractionKind =
                  lang::PayloadExtractionKind::ReborrowReference;
              loan.referenceSelection = lang::MirReferenceSelection{
                  .value = *instruction.result,
                  .carrier = instruction.operands.front().place,
                  .fields = {field},
                  .conditions = {{.enumeration = type,
                                  .variant = *instruction.enumVariant}},
                  .reference = lang::SemanticType::referenceTo(
                      instruction.info.type, instruction.info.access)};
              selectedLoan = loan.id;
              producer = instruction.id;
            }
    // This deliberately exercises body-local identity shape only. Its sealed
    // program still declares an owned int field, so the stronger program
    // proof must reject it; metadata alone never grants reference-payload
    // admission.
    const auto selectedResult = lang::verifyMirBody(body);
    if (!selectedResult.valid())
      for (const auto &error : selectedResult.errors)
        std::cerr << "selected reborrow: " << error.message << '\n';
    expect(selectedLoan != 0 && selectedResult.valid(),
           "selected reborrow has an exact producer and active parent");
    // Exact descriptor IDs alone cannot invent a consuming arm's whole-group
    // handoff. The sealed program also still declares an owned int field;
    // neither proof layer may grant reference-payload admission from metadata.
    const auto handoffShape = [&] {
      auto consuming = body;
      if (selectedLoan != 0)
        consuming.loans[selectedLoan - 1].referenceSelection->transferredFrom =
            consuming.loans[selectedLoan - 1].parent;
      for (auto &block : consuming.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == producer)
            instruction.payloadExtractionKind =
                lang::PayloadExtractionKind::TransferReference;
      return lang::verifyMirBody(consuming);
    }();
    expect(selectedLoan != 0 && !handoffShape.valid() &&
               std::any_of(
                   handoffShape.errors.begin(), handoffShape.errors.end(),
                   [](const auto &error) {
                     return error.message.find("whole-group arm handoff") !=
                            std::string::npos;
                   }),
           "handoff identity also requires a verified whole-group arm handoff");
    expect(!lang::verifyMirProgram(shape).valid(),
           "body-local handoff identity cannot override the sealed field type");
    if (selectedLoan != 0) {
      auto transfer = body;
      transfer.loans[selectedLoan - 1].referenceSelection->transferredFrom =
          transfer.loans[selectedLoan - 1].parent;
      for (auto &block : transfer.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == producer)
            instruction.payloadExtractionKind =
                lang::PayloadExtractionKind::TransferReference;
      const auto transferResult = lang::verifyMirBody(transfer);
      expect(std::any_of(
                 transferResult.errors.begin(), transferResult.errors.end(),
                 [](const auto &error) {
                   return error.message.find("whole-group arm handoff") !=
                          std::string::npos;
                 }),
             "a plausible transfer identity does not prove its group handoff");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = transfer;
        auto &selection = *malformed.loans[selectedLoan - 1].referenceSelection;
        if (mutation == 0)
          selection.transferredFrom = 0;
        if (mutation == 1)
          selection.transferredFrom = selectedLoan;
        if (mutation == 2)
          selection.transferredFrom = malformed.loans.size() + 1;
        if (mutation == 3)
          for (auto &block : malformed.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == producer)
                instruction.payloadExtractionKind =
                    lang::PayloadExtractionKind::CopyReference;
        const auto malformedResult = lang::verifyMirBody(malformed);
        expect(std::any_of(malformedResult.errors.begin(),
                           malformedResult.errors.end(),
                           [](const auto &error) {
                             return error.message.find("selection producer") !=
                                    std::string::npos;
                           }),
               "handoff identity rejects missing/self/foreign/copy-group IDs");
      }
      lang::MirLoanId otherLoan = 0;
      const auto otherBinding =
          shape.findFunctionInstance(id)->parameterBindings[1];
      for (const auto &loan : body.loans)
        if (loan.entry && body.findPlace(loan.source)->binding == otherBinding)
          otherLoan = loan.id;
      expect(otherLoan != 0,
             "independent reference parameter has entry authority");
      if (otherLoan != 0) {
        auto alternatives = body;
        alternatives.loans[selectedLoan - 1].additionalDependencies.push_back(
            {.source = alternatives.loans[otherLoan - 1].source,
             .parent = otherLoan,
             .access = lang::AccessMode::ReadOnly});
        const auto result = lang::verifyMirBody(alternatives);
        if (!result.valid())
          for (const auto &error : result.errors)
            std::cerr << "selected alternative: " << error.message << '\n';
        expect(result.valid(), "one selected scalar access checks "
                               "independently borrowed alternatives");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = alternatives;
          auto &dependency =
              malformed.loans[selectedLoan - 1].additionalDependencies.front();
          if (mutation == 0)
            malformed.loans[otherLoan - 1].entry = false;
          if (mutation == 1)
            dependency.access = lang::AccessMode::Mutable;
          if (mutation == 2)
            dependency.parent = malformed.loans[selectedLoan - 1].parent;
          expect(!lang::verifyMirBody(malformed).valid(),
                 "selected alternatives reject missing, stronger or "
                 "substituted parents");
        }
        auto expired = alternatives;
        lang::MirLoanId otherChild = 0;
        lang::MirInstructionId nextInstruction = 0;
        for (const auto &loan : expired.loans)
          if (loan.parent == otherLoan && !loan.referenceSelection)
            otherChild = loan.id;
        for (const auto &block : expired.blocks)
          for (const auto &instruction : block.instructions)
            nextInstruction = std::max(nextInstruction, instruction.id);
        expect(otherChild != 0, "fixture retains a normal shared sibling");
        if (otherChild != 0) {
          for (auto &block : expired.blocks) {
            const auto activation = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind == lang::MirInstructionKind::Borrow &&
                         instruction.loan == selectedLoan;
                });
            if (activation == block.instructions.end())
              continue;
            lang::MirInstruction endChild;
            endChild.id = ++nextInstruction;
            endChild.kind = lang::MirInstructionKind::EndBorrow;
            endChild.loan = otherChild;
            lang::MirInstruction endParent = endChild;
            endParent.id = ++nextInstruction;
            endParent.loan = otherLoan;
            block.instructions.insert(activation, {endChild, endParent});
          }
          const auto expiredResult = lang::verifyMirBody(expired);
          expect(std::any_of(expiredResult.errors.begin(),
                             expiredResult.errors.end(),
                             [](const auto &error) {
                               return error.message.find(
                                          "lacks a live input authority") !=
                                      std::string::npos;
                             }),
                 "selected activation rejects an actually retired additional "
                 "parent");
        }
      }
    }
  }

  const auto single = analyze(token + leaf + "int main() { return 0; }");
  if (single.canGenerateCode()) {
    auto conservative = single.mir;
    auto &function = lang::MirProgramTestAccess::functions(
        conservative)[functionId(single, "leaf") - 1];
    if (function.completeReturnBorrow) {
      auto alternative =
          function.completeReturnBorrow->possibleAlternatives.front();
      alternative.conditions.front().variant = 1;
      function.completeReturnBorrow->possibleAlternatives.push_back(
          alternative);
      expect(lang::verifyMirProgram(conservative).valid(),
             "extra well-typed dependency-free alternative is conservative");
    }
  }

  {
    const auto result = analyze(token + R"gti(
Token identity(Token input) { return input; }
int main() { auto value = identity(Token::empty); return 0; }
)gti");
    const auto id = functionId(result, "identity");
    const auto *function = result.mir.findFunctionInstance(id);
    expect(function && function->completeReturnBorrow &&
               function->completeReturnBorrow->dependencies.empty() &&
               function->completeReturnBorrow->possibleAlternatives.size() == 2,
           "a dependency-free enum parameter retains every possible tag");
    if (function && function->completeReturnBorrow)
      for (int mutation = 0; mutation < 4; ++mutation) {
        auto malformed = result.mir;
        auto &changed =
            lang::MirProgramTestAccess::functions(malformed)[id - 1];
        if (mutation == 0)
          changed.parameterTypes.front() = lang::SemanticType::Int32;
        else if (mutation == 1)
          changed.parameterBindings.front() = 0;
        else if (mutation == 2)
          changed.completeReturnBorrow->possibleAlternatives.pop_back();
        else
          for (auto &place : changed.body.places)
            if (place.root == lang::MirPlaceRootKind::Binding &&
                place.binding == changed.parameterBindings.front())
              place.initiallyAvailable = false;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "entry proof rejects substituted type/binding, omitted tag or "
               "unavailable storage");
      }
  }

  for (const std::string source :
       {"Token unknown() { return unknown(); }",
        "Token other() { return unknown(); } Token unknown() { return other(); "
        "}",
        "Token unknown() { while (true) {} }",
        "void replace(mut Token& value) { value = Token::item(9); } Token "
        "unknown() { mut auto value = Token::empty; replace(value); return "
        "value; }"}) {
    const auto result = analyze(token + source + " int main() { return 0; }");
    if (!result.canGenerateCode())
      continue;
    const auto id = functionId(result, "unknown");
    const auto *function = result.mir.findFunctionInstance(id);
    expect(function && !function->completeReturnBorrow,
           "unknown/recursive/nonreturning/mutated result remains unproved: " +
               source);
    auto malformed = result.mir;
    for (auto &candidate : lang::MirProgramTestAccess::functions(malformed))
      if (candidate.returnType == function->returnType)
        candidate.completeReturnBorrow = lang::FunctionBorrowDependencySummary{
            .possibleAlternatives = {
                {.conditions = {
                     {.enumeration = candidate.returnType, .variant = 0}}}}};
    expect(!lang::verifyMirProgram(malformed).valid(),
           "forged cycle/no-return/unknown evidence cannot justify itself");
  }

  const auto generic = analyze(R"gti(
enum Box<T> { empty, item(T value) };
Box<T> factory<T>(T value) { return Box<T>::item(value); }
Box<int> integer() { return factory(4); }
Box<bool> boolean() { return factory(true); }
int main() { auto first = integer(); auto second = boolean(); return 0; }
)gti");
  if (generic.canGenerateCode()) {
    const auto integer = functionId(generic, "integer");
    const auto boolean = functionId(generic, "boolean");
    const auto *left = generic.mir.findFunctionInstance(integer);
    const auto *right = generic.mir.findFunctionInstance(boolean);
    expect(left && right && left->completeReturnBorrow &&
               right->completeReturnBorrow,
           "concrete generic calls compute exact result summaries");
    if (left && right && left->completeReturnBorrow &&
        right->completeReturnBorrow) {
      auto malformed = generic.mir;
      lang::MirProgramTestAccess::functions(malformed)[integer - 1]
          .completeReturnBorrow = right->completeReturnBorrow;
      expect(
          !lang::verifyMirProgram(malformed).valid(),
          "one concrete instance cannot substitute another's typed contract");
    }
  }
  // Retention is an optimization, not another language acceptance limit.
  // More distinct queries than the cache retains must still permit a later
  // query for the first concrete instance to rebuild the same exact summary.
  std::string evicted = token + R"gti(
Token factory<uint64_t N>() { return Token::item(int(N)); }
Token before() { return factory<0>(); }
)gti";
  for (unsigned index = 1; index != 72; ++index)
    evicted += "Token item" + std::to_string(index) + "() { return factory<" +
               std::to_string(index) + ">(); }\n";
  evicted +=
      "Token after() { return factory<0>(); }\n"
      "int main() { auto first = before(); auto last = after(); return 0; }";
  const auto cache = analyze(evicted);
  if (cache.canGenerateCode()) {
    const auto *before =
        cache.mir.findFunctionInstance(functionId(cache, "before"));
    const auto *after =
        cache.mir.findFunctionInstance(functionId(cache, "after"));
    expect(before && after && before->completeReturnBorrow &&
               before->completeReturnBorrow == after->completeReturnBorrow,
           "eviction and requery preserve exact result contracts");
  }
  const auto chain = [&](unsigned depth) {
    return token + R"gti(
Token bounded<uint64_t N>() {
  if constexpr (N == 0) { return Token::empty; }
  else { return bounded<N - 1>(); }
}
int main() { auto value = bounded<)gti" +
           std::to_string(depth) + ">(); return 0; }";
  };
  const auto finite = analyze(chain(40));
  expect(finite.canGenerateCode(),
         "finite result-summary chain beyond the old private depth32 succeeds");
  const auto exhausted = lang::Frontend().analyze(
      root / "tests" / "complete-borrow-resource.gti", chain(66),
      {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  expect(!exhausted.canGenerateCode() &&
             std::any_of(
                 exhausted.diagnostics.begin(), exhausted.diagnostics.end(),
                 [](const auto &diagnostic) {
                   return diagnostic.code == "GTI-S2105" &&
                          diagnostic.primary.end > diagnostic.primary.start &&
                          !diagnostic.primary.source.empty() &&
                          !diagnostic.related.empty() &&
                          !diagnostic.hints.empty() && diagnostic.fixes.empty();
                 }),
         "resource exhaustion reports the established located diagnostic");
  // Exercise the independent formal-entry proof before source admission. This
  // deliberately artificial program must remain rejected by the full verifier;
  // only its exact guarded entry contract is being established here.
  const auto entryShape = analyze(R"gti(
enum EntryShape { empty, both(int first, int second) };
int inspect(EntryShape input) {
  switch (input) {
  case EntryShape::empty: return 0;
  case EntryShape::both(first, second): return first + second;
  }
}
int main() { return 0; }
)gti");
  if (entryShape.canGenerateCode()) {
    auto shape = entryShape.mir;
    const auto id = functionId(entryShape, "inspect");
    auto &function = lang::MirProgramTestAccess::functions(shape)[id - 1];
    auto &body = function.body;
    auto &payloads = lang::MirProgramTestAccess::payloads(shape);
    const auto payload =
        std::find_if(payloads.begin(), payloads.end(), [&](const auto &item) {
          return item.type == function.parameterTypes.front();
        });
    const auto carrier = std::find_if(
        body.places.begin(), body.places.end(), [&](const auto &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == function.parameterBindings.front() &&
                 place.projections.empty();
        });
    expect(payload != payloads.end() && carrier != body.places.end() &&
               payload->variants.size() == 2 && body.loans.empty(),
           "guarded entry fixture has an exact unborrowed formal");
    if (payload != payloads.end() && carrier != body.places.end() &&
        payload->variants.size() == 2 && body.loans.empty()) {
      const auto base = *carrier;
      const auto first = payload->variants[1].payloadSymbols[0];
      const auto second = payload->variants[1].payloadSymbols[1];
      const lang::BorrowDependencyCondition guard{.enumeration = payload->type,
                                                  .variant = 1};
      for (std::size_t index = 0; index != 2; ++index) {
        const auto access =
            index == 0 ? lang::AccessMode::ReadOnly : lang::AccessMode::Mutable;
        const auto field = index == 0 ? first : second;
        auto &fieldType = payload->variants[1].payloadTypes[index];
        fieldType = lang::SemanticType::referenceTo(fieldType, access);
        auto external = base;
        external.id = body.places.size() + 1;
        external.type = fieldType.arguments.front();
        external.access = access;
        external.projections = {
            {.kind = lang::MirProjectionKind::Field, .field = field},
            {.kind = lang::MirProjectionKind::Dereference}};
        body.places.push_back(std::move(external));
      }
      body.loans.push_back(
          {.id = 1,
           .kind = lang::MirLoanKind::Stored,
           .source = base.id,
           .additionalDependencies = {{.designatedFields = {second},
                                       .access = lang::AccessMode::Mutable,
                                       .conditions = {guard}}},
           .access = lang::AccessMode::ReadOnly,
           .carriers = {base.binding},
           .designatedFields = {first},
           .conditions = {guard},
           .entry = true});
      const auto entryFailure = [](const auto &verified) {
        return std::any_of(verified.errors.begin(), verified.errors.end(),
                           [](const auto &error) {
                             return error.message.find(
                                        "conditional stored entry lacks") !=
                                    std::string::npos;
                           });
      };
      const auto verified = lang::verifyMirProgram(shape);
      expect(!verified.valid() && !entryFailure(verified),
             "exact guarded entry proof does not waive closed source/producer "
             "admission");
      for (int mutation = 0; mutation != 11; ++mutation) {
        auto malformed = shape;
        auto &changed =
            lang::MirProgramTestAccess::functions(malformed)[id - 1];
        auto &loan = changed.body.loans.front();
        if (mutation == 0)
          loan.conditions.front().variant = 0;
        if (mutation == 1)
          loan.designatedFields = {second};
        if (mutation == 2)
          loan.additionalDependencies.clear();
        if (mutation == 3)
          loan.access = lang::AccessMode::Mutable;
        if (mutation == 4)
          loan.parent = 1;
        if (mutation == 5)
          changed.body.places.back().projections.pop_back();
        if (mutation == 6)
          changed.parameterTypes.front() = lang::SemanticType::Int32;
        if (mutation == 7)
          changed.body.places[base.id - 1].initiallyAvailable = false;
        if (mutation == 8)
          changed.body.places[base.id - 1].foreignStorage = true;
        if (mutation == 9)
          loan.additionalDependencies.front().conditions.clear();
        if (mutation == 10)
          loan.additionalDependencies.front().parent = 1;
        expect(entryFailure(lang::verifyMirProgram(malformed)),
               "guarded entry rejects missing/substituted formal, path, "
               "guard, access, parent or availability " +
                   std::to_string(mutation));
      }
    }
  }
  {
    std::ifstream fixture(root /
                          "tests/fixtures/borrowed_enum_generic_fields.gti");
    const std::string source((std::istreambuf_iterator<char>(fixture)), {});
    expect(!source.empty(), "generic borrowed class fixture exists");
    const auto result = analyze(source);
    expect(result.canGenerateCode(), "concrete generic borrowed classes prove "
                                     "shared copy and owned-error extraction");
  }
  {
    std::ifstream fixture(root / "tests/fixtures/borrowed_enum_fields.gti");
    const std::string source((std::istreambuf_iterator<char>(fixture)), {});
    expect(!source.empty(), "borrowed class fixture exists");
    const auto result = analyze(source);
    if (!result.canGenerateCode()) {
      const auto log = std::filesystem::temp_directory_path() /
                       "gti-borrowed-class-proof.mir";
      std::ofstream output(log);
      lang::MirPrinter().print(result.mir, output);
      std::cerr << "Borrowed class proof MIR: " << log << '\n';
    }
    if (result.semanticValid) {
      std::size_t tested = 0;
      const auto ctorProofFailed = [](const lang::MirVerificationResult &checked) {
        return std::any_of(checked.errors.begin(), checked.errors.end(), [](const auto &error) {
          return error.message.find("complete constructor dependency summary lacks") != std::string::npos;
        });
      };
      expect(!ctorProofFailed(lang::verifyMirProgram(result.mir)),
             "constructor complete proof independently verifies exact initialized fields");
      for (const auto &constructor : result.mir.constructorInstances()) {
        if (!constructor.completeBorrow ||
            constructor.completeBorrow->dependencies.empty())
          continue;
        ++tested;
        for (int mutation = 0; mutation < 10; ++mutation) {
          auto malformed = result.mir;
          auto &changed = lang::MirProgramTestAccess::constructors(malformed)
                              [constructor.id - 1];
          if (mutation == 0)
            changed.completeBorrow->dependencies.clear();
          if (mutation == 1)
            changed.completeBorrow->dependencies.front().parameter = 999;
          if (mutation == 2)
            changed.completeBorrow->dependencies.front().resultConditions.clear();
          if (mutation == 3)
            changed.completeBorrow->dependencies.front().access = lang::AccessMode::Mutable;
          if (mutation == 4)
            for (auto &block : changed.body.blocks)
              for (auto &instruction : block.instructions)
                if (instruction.kind == lang::MirInstructionKind::Initialize)
                  instruction.operands.clear();
          if (mutation >= 5)
            for (auto &loan : changed.body.loans) {
              if (!loan.escapes || !loan.storedField)
                continue;
              if (mutation == 6) loan.source = 0;
              if (mutation == 7) loan.conditions.clear();
              if (mutation == 8) loan.storedField = 0;
              for (auto &block : changed.body.blocks)
                for (auto &instruction : block.instructions)
                  if (instruction.kind == lang::MirInstructionKind::Borrow &&
                      instruction.loan == loan.id) {
                    if (mutation == 5) instruction.operands.clear();
                    if (mutation == 9) instruction.operands.front().value = 0;
                  }
            }
          expect(ctorProofFailed(lang::verifyMirProgram(malformed)),
                 "borrowed constructor rejects absent/substituted origin, guard, "
                 "access, field initialization or exact publication " + std::to_string(mutation));
        }
      }
      expect(tested != 0, "borrowed constructor has an independently checked summary");
    }
    if (result.canGenerateCode()) {
      std::size_t freshConsumingProofs = 0;
      for (const auto &function : result.hir.functionInstances()) {
        if (!function.source || function.source->name().lexeme != "relay")
          continue;
        const auto *original = result.mir.findFunctionInstance(function.id);
        if (!original)
          continue;
        for (const auto &block : original->body.blocks)
          for (const auto &input : block.instructions) {
            if (input.kind != lang::MirInstructionKind::CallInput ||
                input.callInputRole != lang::MirCallInputRole::Receiver ||
                input.callInputKind !=
                    lang::HirCallInputKind::MutableTemporaryBorrow ||
                !input.info.traits.containsBorrowedState || !input.loan)
              continue;
            ++freshConsumingProofs;
            for (int mutation = 0; mutation < 8; ++mutation) {
              auto malformed = result.mir;
              auto &body = lang::MirProgramTestAccess::functions(
                               malformed)[function.id - 1]
                               .body;
              auto &shell = body.loans[*input.loan - 1];
              const auto group = shell.storedDependency;
              if (mutation == 0)
                shell.storedDependency = 0;
              if (mutation == 1)
                shell.storedDependency = shell.id;
              if (mutation == 2)
                shell.source = 0;
              if (mutation == 3)
                shell.access = lang::AccessMode::ReadOnly;
              if (mutation == 4)
                body.loans[group - 1].designatedFields.clear();
              if (mutation == 5)
                body.loans[group - 1].conditions.clear();
              if (mutation == 6)
                body.loans[group - 1].parent = shell.id;
              if (mutation == 7)
                body.loans[group - 1].access = lang::AccessMode::Mutable;
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "fresh consuming borrowed receiver preserves exact shell "
                     "and stored dependency authority " +
                         std::to_string(mutation));
            }
          }
      }
      expect(freshConsumingProofs == 1,
             "ordinary reference relay consumes one fresh borrowed wrapper");
      lang::HirFunctionInstanceId takeId = 0;
      for (const auto &function : result.hir.functionInstances()) {
        const auto *candidate = result.mir.findFunctionInstance(function.id);
        if (function.source && function.source->name().lexeme == "take" &&
            candidate &&
            candidate->receiverMutability ==
                lang::ReceiverMutability::Consuming &&
            candidate->completeReturnBorrow)
          takeId = function.id;
      }
      const auto *take = result.mir.findFunctionInstance(takeId);
      expect(take && take->completeReturnBorrow,
             "consuming borrowed wrapper has an external receiver contract");
      if (take && take->completeReturnBorrow) {
        for (int mutation = 0; mutation < 7; ++mutation) {
          auto malformed = result.mir;
          auto &changed =
              lang::MirProgramTestAccess::functions(malformed)[takeId - 1];
          for (auto &place : changed.body.places) {
            if (place.root != lang::MirPlaceRootKind::This ||
                !place.projections.empty())
              continue;
            if (mutation == 0)
              place.key.reset();
            if (mutation == 1 && place.key)
              place.key->receiver = false;
            if (mutation == 2)
              place.foreignStorage = true;
            if (mutation == 3)
              place.access = lang::AccessMode::ReadOnly;
          }
          for (auto &loan : changed.body.loans) {
            if (!loan.entry || loan.kind != lang::MirLoanKind::Stored)
              continue;
            if (mutation == 4)
              loan.conditions.clear();
            if (mutation == 5)
              loan.designatedFields.clear();
            if (mutation == 6)
              loan.access = lang::AccessMode::Mutable;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "consuming receiver requires exact domain, access and "
                 "complete guarded entry " +
                     std::to_string(mutation));
        }
      }
      for (const auto &constructor : result.mir.constructorInstances()) {
        if (!constructor.completeBorrow ||
            constructor.completeBorrow->dependencies.empty())
          continue;
        auto malformed = result.mir;
        auto &changed = lang::MirProgramTestAccess::constructors(
            malformed)[constructor.id - 1];
        const auto published = std::find_if(
            changed.body.loans.begin(), changed.body.loans.end(),
            [](const auto &loan) { return loan.escapes && loan.storedField; });
        expect(published != changed.body.loans.end(),
               "constructor publishes its field group");
        if (published == changed.body.loans.end())
          continue;
        for (auto &block : changed.body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::EndBorrow)
              instruction.loan = published->id;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "constructor cannot retire the published field group before "
               "normal exit");
      }
    }
    for (const std::string extra :
         {"int& dangling_relay() { int local = 3; return relay(local); }",
          "int alias_relay() { mut int owner = 3; int& view = relay(owner); "
          "owner = 4; return view; }"}) {
      const auto rejected = lang::Frontend().analyze(
          root / "tests/reference-relay-rejection.gti", source + "\n" + extra,
          {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
      expect(!rejected.canGenerateCode() &&
                 std::none_of(
                     rejected.diagnostics.begin(), rejected.diagnostics.end(),
                     [](const auto &d) { return d.code == "GTI-B0001"; }),
             "reference relay rejects expired owner or mutable overlap in "
             "semantics");
    }
  }
  return failures ? 1 : 0;
}
