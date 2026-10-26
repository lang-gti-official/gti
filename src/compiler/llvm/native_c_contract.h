#pragma once

#include "gti/lowered_program.h"

namespace lang::native_llvm {

enum class NativeCValueRole { Argument, Result, Storage };

[[nodiscard]] bool nativeCScalarValueContract(const TargetInfo &target,
                                              const SemanticType &type);

// Representation eligibility for an already verified program, not a source
// type checker or a register classifier. The physical planner consumes the
// same bounded family and separately verifies LLVM's target/layout contract.
[[nodiscard]] bool nativeCValueContract(const MirProgram &program,
                                        const TargetInfo &target,
                                        const SemanticType &type,
                                        NativeCValueRole role);
[[nodiscard]] bool
nativeCDeclarationContract(const MirProgram &program, const TargetInfo &target,
                           const MirFunctionInstance &function);
// First public integration profile: no record/coercion, counted-text pair,
// returned native signature or callback composition is implied by this gate.
[[nodiscard]] bool
nativeCScalarImportContract(const MirProgram &program, const TargetInfo &target,
                            const MirFunctionInstance &function);
[[nodiscard]] bool
nativeCCountedInputContract(const MirProgram &program, const TargetInfo &target,
                            const MirFunctionInstance &function);
[[nodiscard]] bool nativeCScalarFunctionValueContract(const MirProgram &program,
                                                      const TargetInfo &target,
                                                      const SemanticType &type);
[[nodiscard]] bool
nativeCScalarFunctionImportContract(const MirProgram &program,
                                    const TargetInfo &target,
                                    const MirFunctionInstance &function);
[[nodiscard]] bool
nativeCScalarIndirectCallContract(const MirProgram &program,
                                  const TargetInfo &target, const MirBody &body,
                                  const MirInstruction &instruction);
[[nodiscard]] bool nativeCScalarReceiverInputContract(
    const MirProgram &program, const TargetInfo &target, const MirBody &body,
    const MirInstruction &instruction);
[[nodiscard]] bool nativeCDirectCallContract(const MirProgram &program,
                                             const TargetInfo &target,
                                             const MirBody &body,
                                             const MirInstruction &instruction);
[[nodiscard]] bool
nativeCIndirectCallContract(const MirProgram &program, const TargetInfo &target,
                            const MirBody &body,
                            const MirInstruction &instruction);

[[nodiscard]] bool nativeCStorageContract(const MirProgram &program,
                                          const TargetInfo &target,
                                          const ForeignStorageInfo &storage);

} // namespace lang::native_llvm
