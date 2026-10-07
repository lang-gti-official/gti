#include "cpp_mir_body_emitter_test_fixture.h"

void testPassiveArrayParameterStages() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/passive_fixed_array_parameter.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "ordinary and library array parameter fixtures have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t stages = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const auto &stage = block.instructions[index];
        const auto &consumer = block.instructions[index + 1];
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.result ||
            stage.info.type.kind != lang::SemanticType::Array ||
            stage.callInputKind != lang::HirCallInputKind::Value ||
            stage.operands.size() != 1 ||
            stage.operands.front().kind != lang::MirOperandKind::Value ||
            consumer.kind != lang::MirInstructionKind::Construct)
          continue;
        const auto *value =
            function.body.findValue(stage.operands.front().value);
        if (value == nullptr)
          continue;
        const auto drop = std::find_if(
            function.body.dropObligations.begin(),
            function.body.dropObligations.end(), [&](const auto &candidate) {
              const auto *place = function.body.findPlace(candidate.place);
              return place && place->root == lang::MirPlaceRootKind::Value &&
                     place->value == value->id && place->projections.empty();
            });
        expect(
            drop != function.body.dropObligations.end(),
            "copied parameter source retains its full-expression obligation");
        if (drop == function.body.dropObligations.end())
          continue;
        ++stages;
        expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                   .supportsFailureBodyText(
                       {lang::MirBodyKind::Function, function.id}),
               "passive aggregate CallInput uses exact source storage without "
               "defaults");
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *changedBlock =
              const_cast<lang::MirBlock *>(changed->body.findBlock(block.id));
          auto &changedStage = changedBlock->instructions[index];
          auto &changedConsumer = changedBlock->instructions[index + 1];
          if (mutation == 0)
            changedStage.callSite += 1;
          if (mutation == 1)
            changedStage.callInputKind = lang::HirCallInputKind::CopyValue;
          if (mutation == 2)
            changedStage.operands.front().value = 0;
          if (mutation == 3) {
            for (const auto &owner : malformed.classInstances())
              if (owner.type == stage.info.type.arguments.front())
                const_cast<lang::MirClassInstance &>(owner)
                    .requiresActiveCleanup = true;
          }
          if (mutation == 4) {
            auto *changedValue = const_cast<lang::MirValue *>(
                changed->body.findValue(value->id));
            changedValue->info.traits.containsBorrowedState = true;
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &instruction : candidateBlock.instructions)
                if (instruction.id == changedValue->definition)
                  instruction.info = changedValue->info;
          }
          if (mutation == 5)
            changedConsumer.lifecycle.push_back(
                {.kind = lang::MirLifecycleEventKind::TransferOut,
                 .source = drop->id});
          if (mutation == 6)
            std::swap(changedStage, changedConsumer);
          if (mutation == 7)
            changedConsumer.parameterTypes[stage.callInputIndex].arrayLength +=
                1;
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "passive parameter alias rejects stale identities, "
                 "owning/borrowed "
                 "elements, transfers, and nonadjacent consumption");
        }
      }
    }
  }
  expect(
      stages == 2,
      "both unrelated Table and std::array use passive fixed-array parameters");
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(
        cpp.find("std::as_const(__gti_mir_p_") != std::string::npos &&
            cpp.find(
                "passive array input copies its retained aggregate slot") !=
                std::string::npos,
        "passive parameter copies retain source storage rather than taking it");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
}

int main() {
  testPassiveArrayParameterStages();
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/passive_fixed_array_values.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "typed passive arrays have verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t passiveUnions = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    if (!owner.unionLayout)
      continue;
    ++passiveUnions;
    expect(lang::mirTypeMoveIsDefinedFailureFree(frontend.mir, owner.type),
           "verified passive union movement cannot raise defined failure");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(owner.id));
      if (mutation == 0)
        changed->moveConstructor = lang::SpecialMemberStatus::Deleted;
      if (mutation == 1)
        changed->requiresActiveDropState = true;
      if (mutation == 2)
        changed->destructorStatus = lang::SpecialMemberStatus::Declared;
      if (mutation == 3)
        changed->declaredFields.front().type = lang::SemanticType::Unknown;
      expect(!lang::mirTypeMoveIsDefinedFailureFree(malformed, owner.type),
             "passive union movement retains special-member and field proofs");
    }
  }
  expect(passiveUnions == 1, "fixture contains one independent passive union");
  std::size_t unionFieldSchedules = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            instruction.constructorTarget)
          continue;
        const auto found = std::find_if(
            frontend.mir.classInstances().begin(),
            frontend.mir.classInstances().end(), [&](const auto &owner) {
              return owner.unionLayout && owner.type == instruction.info.type;
            });
        if (found == frontend.mir.classInstances().end())
          continue;
        ++unionFieldSchedules;
        expect(
            lang::CppMirBodyEmitter(frontend.mir, rows)
                .supportsFailureBodyText(
                    {lang::MirBodyKind::Constructor, constructor.id}),
            "generated constructor preserves passive union field publication");
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto malformed = frontend.mir;
          auto *owner = const_cast<lang::MirClassInstance *>(
              malformed.findClassInstance(found->id));
          if (mutation == 0)
            owner->defaultConstructor = lang::SpecialMemberStatus::Deleted;
          if (mutation == 1)
            owner->moveAssignment = lang::SpecialMemberStatus::Deleted;
          if (mutation == 2)
            owner->requiresActiveCleanup = true;
          if (mutation == 3)
            owner->unionLayout->fields.front().type =
                lang::SemanticType::UInt32;
          if (mutation == 4)
            owner->unionLayout->fields.front().field = 0;
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Constructor, constructor.id}),
                 "passive union default publication retains lifecycle/layout "
                 "proofs");
        }
      }
    }
  }
  expect(unionFieldSchedules == 1,
         "fixture exercises one generated passive-union field schedule");
  std::size_t arrays = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Aggregate ||
            instruction.info.type.kind != lang::SemanticType::Array)
          continue;
        ++arrays;
        if (instruction.info.type.arguments.front().kind !=
            lang::SemanticType::Class)
          continue;
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions) {
              if (candidate.id != instruction.id)
                continue;
              if (mutation == 0)
                candidate.info.type.arrayLength += 1;
              else
                candidate.operands.front().type = lang::SemanticType::Int32;
            }
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "passive aggregate rejects forged element count/type");
        }
      }
    }
  }
  expect(arrays == 7,
         "fixture reaches both integer and all passive array shapes");
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(cpp.find(".construction_address())) std::array<") !=
               std::string::npos,
           "passive arrays construct final storage without default staging");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  if (failures)
    return 1;
  std::cout << "passive fixed-array MIR and emitter proofs passed\n";
}
