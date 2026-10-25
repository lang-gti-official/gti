#pragma once

#include "abi.h"

#include <functional>
#include <map>

namespace lang::native_llvm {

struct CallableSignature final {
  SemanticType result = SemanticType::Unknown;
  std::vector<SemanticType> parameters;
};

// Callable representation consumes exact MIR targets and signatures. It does
// not inspect source names, overload sets or public wrapper declarations.
class Callables final {
public:
  using FindBody = std::function<llvm::Function *(MirBodyAddress)>;
  using FindAbi = std::function<FunctionAbi(MirBodyAddress)>;

  Callables(Session &session, FindBody findBody, FindAbi findAbi);

  [[nodiscard]] const MirLambdaInstance &lambda(HirLambdaId id) const;
  [[nodiscard]] const MirLambdaInstance &
  lambda(const SemanticType &type) const;
  [[nodiscard]] CallableSignature
  pointerSignature(const SemanticType &type) const;
  [[nodiscard]] FunctionAbi pointerAbi(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *
  pointerAdapter(const MirInstruction &formation);

private:
  Session &session;
  FindBody findBody;
  FindAbi findAbi;
  std::map<std::pair<MirBodyKind, std::size_t>, llvm::Function *>
      pointerAdapters;
};

} // namespace lang::native_llvm
