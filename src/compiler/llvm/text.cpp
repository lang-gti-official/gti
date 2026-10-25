#include "text.h"

namespace lang::native_llvm {

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
