#include "text.h"

#include <set>

namespace lang::native_llvm {

const std::string *wholeCStringLiteral(const MirBody &body,
                                       const MirOperand &operand) {
  const auto *input = &operand;
  std::set<MirValueId> visited;
  while (input->kind == MirOperandKind::Value &&
         visited.insert(input->value).second) {
    const auto *value = body.findValue(input->value);
    const auto *block =
        value ? body.findBlock(value->definitionBlock) : nullptr;
    if (!block || value->info.type != SemanticType::StringView)
      return nullptr;
    const auto stage =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const auto &candidate) {
                       return candidate.id == value->definition;
                     });
    if (stage == block->instructions.end() || stage->tombstone ||
        stage->result != value->id)
      return nullptr;
    if (stage->kind == MirInstructionKind::Compute &&
        stage->operation == MirOperation::Literal &&
        stage->literalProvenance.kind == MirLiteralProvenanceKind::Source &&
        stage->literal) {
      const auto *text = std::get_if<std::string>(&*stage->literal);
      return text && text->find('\0') == std::string::npos ? text : nullptr;
    }
    // Immutable local aliases retain their exact initializer's literal storage.
    // Parameters, mutable/view computations, projections and multiple stores
    // do not establish this representation proof.
    if (stage->kind != MirInstructionKind::Load || stage->operands.size() != 1)
      return nullptr;
    const auto *place = body.findPlace(stage->operands[0].place);
    if (!place || place->root != MirPlaceRootKind::Binding ||
        !place->projections.empty() || place->access != AccessMode::ReadOnly ||
        place->initiallyAvailable)
      return nullptr;
    const MirInstruction *initializer = nullptr;
    for (const auto &candidateBlock : body.blocks)
      for (const auto &candidate : candidateBlock.instructions)
        if (!candidate.tombstone && candidate.destination == place->id) {
          if (initializer || candidate.kind != MirInstructionKind::Initialize ||
              candidate.operands.size() != 1)
            return nullptr;
          initializer = &candidate;
        }
    if (!initializer)
      return nullptr;
    input = &initializer->operands[0];
  }
  return nullptr;
}

llvm::Value *equalText(Session &session, llvm::IRBuilder<> &ir,
                       llvm::Value *left, llvm::Value *right) {
  auto *function = ir.GetInsertBlock()->getParent();
  auto *start = ir.GetInsertBlock();
  auto *header =
      llvm::BasicBlock::Create(session.context(), "text.equal.loop", function);
  auto *bytes =
      llvm::BasicBlock::Create(session.context(), "text.equal.bytes", function);
  auto *advance =
      llvm::BasicBlock::Create(session.context(), "text.equal.next", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "text.equal.done", function);
  auto *length = ir.CreateExtractValue(left, 1);
  ir.CreateCondBr(ir.CreateICmpEQ(length, ir.CreateExtractValue(right, 1)),
                  header, done);
  ir.SetInsertPoint(header);
  auto *index = ir.CreatePHI(ir.getInt64Ty(), 2);
  index->addIncoming(ir.getInt64(0), start);
  ir.CreateCondBr(ir.CreateICmpEQ(index, length), done, bytes);
  ir.SetInsertPoint(bytes);
  auto *a = ir.CreateGEP(ir.getInt8Ty(), ir.CreateExtractValue(left, 0), index);
  auto *b =
      ir.CreateGEP(ir.getInt8Ty(), ir.CreateExtractValue(right, 0), index);
  ir.CreateCondBr(ir.CreateICmpEQ(ir.CreateLoad(ir.getInt8Ty(), a),
                                  ir.CreateLoad(ir.getInt8Ty(), b)),
                  advance, done);
  ir.SetInsertPoint(advance);
  index->addIncoming(ir.CreateAdd(index, ir.getInt64(1)), advance);
  ir.CreateBr(header);
  ir.SetInsertPoint(done);
  auto *result = ir.CreatePHI(ir.getInt1Ty(), 3);
  result->addIncoming(ir.getFalse(), start);
  result->addIncoming(ir.getTrue(), header);
  result->addIncoming(ir.getFalse(), bytes);
  return result;
}

} // namespace lang::native_llvm
