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
// Applies only to the immediate referent of this exact pointer. It does not
// admit an opaque object value, its layout, or a dereference.
[[nodiscard]] bool
nativeCOpaquePointerReferentContract(const MirProgram &program,
                                     const TargetInfo &target,
                                     const SemanticType &pointer);
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
[[nodiscard]] bool
nativeCAggregateImportContract(const MirProgram &program,
                               const TargetInfo &target,
                               const MirFunctionInstance &function);
[[nodiscard]] bool nativeCFunctionValueContract(const MirProgram &program,
                                                const TargetInfo &target,
                                                const SemanticType &type);
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
[[nodiscard]] bool
nativeCReceiverInputContract(const MirProgram &program,
                             const TargetInfo &target, const MirBody &body,
                             const MirInstruction &instruction);
// A GTI reference to an exact native function value is an address of its
// storage, never a C reference parameter or permission to invoke the value.
[[nodiscard]] bool
nativeCFunctionBorrowInputContract(const MirProgram &program,
                                   const TargetInfo &target, const MirBody &body,
                                   const MirInstruction &instruction);
[[nodiscard]] bool
nativeCResultInputContract(const MirProgram &program, const TargetInfo &target,
                           const MirBody &body,
                           const MirInstruction &instruction);
[[nodiscard]] bool nativeCDirectCallContract(const MirProgram &program,
                                             const TargetInfo &target,
                                             const MirBody &body,
                                             const MirInstruction &instruction);
[[nodiscard]] bool
nativeCIndirectCallContract(const MirProgram &program, const TargetInfo &target,
                            const MirBody &body,
                            const MirInstruction &instruction);

[[nodiscard]] bool
nativeCCallbackContract(const MirProgram &program, const TargetInfo &target,
                        const MirNativeCallbackAdapter &adapter);
[[nodiscard]] bool
nativeCCallbackStageContract(const MirProgram &program,
                             const TargetInfo &target, const MirBody &body,
                             const MirInstruction &instruction);

[[nodiscard]] bool nativeCStorageContract(const MirProgram &program,
                                          const TargetInfo &target,
                                          const ForeignStorageInfo &storage);

// Public storage admission bounds the general representation helper. Place and
// access witnesses consume exact same-body rows and never grant raw-pointee
// access, capture policy or a different unsafe effect.
[[nodiscard]] bool
nativeCStorageImportContract(const MirProgram &program,
                             const TargetInfo &target,
                             const ForeignStorageInfo &storage);
[[nodiscard]] bool nativeCStoragePlaceContract(const MirProgram &program,
                                               const TargetInfo &target,
                                               const MirBody &body,
                                               const MirPlace &place);
[[nodiscard]] bool
nativeCStorageAccessContract(const MirProgram &program,
                             const TargetInfo &target, const MirBody &body,
                             const MirInstruction &instruction);
[[nodiscard]] bool
nativeCStorageUnionAccessContract(const MirProgram &program,
                                  const TargetInfo &target, const MirBody &body,
                                  const MirInstruction &instruction);

} // namespace lang::native_llvm
