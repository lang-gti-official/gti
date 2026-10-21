#include "gti/ast_printer.h"
#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir_effects.h"
#include "gti/support.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>

namespace {

int failures = 0;

lang::Frontend auditFrontend() {
  lang::FrontendOptions options;
  options.rawPointerAudit = lang::RawPointerAuditMode::Strict;
  return lang::Frontend(options);
}

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  return static_cast<std::size_t>(
      std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

bool hasMessage(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

bool hasRelated(const lang::FrontendResult &result, std::string_view text) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    if (std::any_of(diagnostic.related.begin(), diagnostic.related.end(),
                    [&](const lang::RelatedDiagnostic &related) {
                      return related.message.find(text) != std::string::npos;
                    })) {
      return true;
    }
  }
  return false;
}

bool hasHint(const lang::FrontendResult &result, std::string_view text) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    if (std::any_of(diagnostic.hints.begin(), diagnostic.hints.end(),
                    [&](const std::string &hint) {
                      return hint.find(text) != std::string::npos;
                    })) {
      return true;
    }
  }
  return false;
}

const lang::HirFunctionInstance *
findHirFunction(const lang::FrontendResult &result, std::string_view name) {
  const auto found =
      std::find_if(result.hir.functionInstances().begin(),
                   result.hir.functionInstances().end(),
                   [&](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == name;
                   });
  return found == result.hir.functionInstances().end() ? nullptr : &*found;
}

const lang::MirBody *findMirFunction(const lang::FrontendResult &result,
                                     std::string_view name) {
  const lang::HirFunctionInstance *hir = findHirFunction(result, name);
  const lang::MirFunctionInstance *mir =
      hir == nullptr ? nullptr : result.mir.findFunctionInstance(hir->id);
  return mir == nullptr ? nullptr : &mir->body;
}

const lang::FunctionDecl *findTopLevelFunction(const lang::Program &program,
                                               std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

const lang::ClassDecl *findTopLevelClass(const lang::Program &program,
                                         std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *owner =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (owner != nullptr && owner->name().lexeme == name) {
      return owner;
    }
  }
  return nullptr;
}

void testRawPointerAstPrinting() {
  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("[]() -> const native::Cell* { return nullptr; }"));
  const lang::ExprPtr expression = parser.parseExpression();
  const std::string printed = expression == nullptr
                                  ? std::string("<null>")
                                  : lang::AstPrinter().print(*expression);
  if (expression == nullptr || parser.hadError() ||
      printed != "(lambda [] -> const native::Cell*)") {
    std::cerr << "raw pointer AST printer produced: " << printed << '\n';
  }
  expect(expression != nullptr && !parser.hadError() &&
             printed == "(lambda [] -> const native::Cell*)",
         "AST printing should keep pointee const before the first qualified "
         "type segment");

  for (const std::string type :
       {"native::Cell***", "const native::Cell***", "native::Cell* const*",
        "const native::Cell* const** const*"}) {
    lang::Parser nested(lexer.scan("[]() -> " + type + " { return nullptr; }"));
    const lang::ExprPtr parsed = nested.parseExpression();
    expect(parsed != nullptr && !nested.hadError() &&
               lang::AstPrinter().print(*parsed) ==
                   "(lambda [] -> " + type + ")",
           "AST printing should preserve every pointer layer and its own "
           "pointee qualifier: " +
               type);
    const auto *lambda =
        parsed ? lang::exprAs<lang::Lambda>(parsed.get()) : nullptr;
    if (lambda) {
      const lang::TypeRef &result = lambda->returnType();
      expect(result.pointers.size() == static_cast<std::size_t>(std::count(
                                           type.begin(), type.end(), '*')),
             "each written pointer layer should retain its own star token");
      for (const auto &layer : result.pointers) {
        expect(layer.star.kind == lang::TokenKind::STAR &&
                   layer.star.lexeme == "*",
               "pointer syntax layers should preserve exact source tokens");
      }
    }
  }

  lang::Parser topLevelConst(
      lexer.scan("[]() -> native::Cell* const { return nullptr; }"));
  (void)topLevelConst.parseExpression();
  expect(topLevelConst.hadError(),
         "trailing const should not introduce C++ binding constness");
}

void testNestedPointerTypes() {
  const auto result = lang::Frontend().analyze("nested-pointer-types.gti", R"(
using Pointer = int*;
using Slot = Pointer*;
[[c_abi]] struct NativeSlots { mut Slot slot; };
struct Holder { mut Slot slot = nullptr; };
extern "C" {
  void native_write(Slot slot, Pointer value);
  Slot native_return();
  void native_read(int* const* readonly_slot, const int** readonly_value);
}
Slot forward(Slot slot) { return slot; }
int*** deeper(int*** slots) { return slots; }
int main() {
  mut int owner = 7;
  mut Pointer pointer = &owner;
  mut Slot slot = &pointer;
  Holder carrier{.slot = forward(slot)};
  int* const* shared = slot;
  unsafe {
    *slot = &owner;
    native_write(*deeper(&slot), &owner);
    if (*shared != pointer || *carrier.slot != pointer) { return 1; }
  }
  return 0;
}
)");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(),
         "nested pointers should compose with aliases, fields, C parameters, "
         "unannotated returns and GTI helper functions");
  if (result.canGenerateCode()) {
    const lang::MirBody *body = findMirFunction(result, "main");
    expect(body && body->loans.empty(),
           "raw pointer-slot addresses must not create checked loans");
    const auto artifact = gti_test::emitCpp(result, result.mir, result.mir,
                                            lang::OptimizationResult{});
    expect(artifact.contents.find("__gti_native_out_") == std::string::npos &&
               artifact.contents.find("union raw_address") != std::string::npos,
           "native calls should use actual pointer slots without contextual "
           "temporary writeback staging");
  }

  for (const auto &[name, source, code] :
       {std::tuple{"deep-const-addition",
                   "int main() { mut int* p = nullptr; int** s = &p; "
                   "const int** invalid = s; return 0; }",
                   "GTI-S2003"},
        std::tuple{"readonly-slot-write",
                   "int main() { mut int* p = nullptr; int* const* s = &p; "
                   "unsafe { *s = nullptr; } return 0; }",
                   "GTI-S2002"},
        std::tuple{"immutable-slot-address",
                   "int main() { int* p = nullptr; int** s = &p; return 0; }",
                   "GTI-S2003"},
        std::tuple{"unsafe-slot-read",
                   "int main() { mut int* p = nullptr; int** s = &p; "
                   "int* read = *s; return 0; }",
                   "GTI-S2055"}}) {
    const auto invalid = lang::Frontend().analyze(name, source);
    expect(!invalid.canGenerateCode() && hasCode(invalid, code),
           std::string(name) + " should retain mandatory pointer checking");
  }
}

void testRawPointerPipeline() {
  const std::string source = R"(
struct Cell {
  mut int32_t value = 0;

  void set(int32_t next) mut {
    this.value = next;
  }
};

struct PointerHolder {
  mut int32_t* pointer = nullptr;
};

class Box<T> {
  T stored;

public:
  Box(T value) : stored(value) {}

  T get() {
    return this.stored;
  }
};

int32_t identify(int32_t* value) { return 1; }
int32_t identify(const int32_t* value) { return 2; }

int main() {
  mut int32_t values[3] = {10, 20, 30};
  mut int32_t* pointer = nullptr;
  mut Cell cell{};
  mut Cell* cell_pointer = nullptr;
  mut Box<int32_t> box{9};
  mut Box<int32_t>* box_pointer = nullptr;
  mut PointerHolder holder{};
  mut PointerHolder* holder_pointer = nullptr;
  mut int32_t* pointers[2] = {};
  unsafe {
    pointer = &values[0];
    holder.pointer = pointer;
    pointers[0] = pointer;
    holder.pointer += 1;
    pointers[0] += 2;
    holder_pointer = &holder;
    holder_pointer->pointer -= 1;
    pointer[1] = 25;
    *(pointer + 2) = 35;
    int64_t distance = (pointer + 2) - pointer;
    pointer++;
    pointer--;
    cell_pointer = &cell;
    cell_pointer->set(pointer[1]);
    box_pointer = &box;
    int32_t exact = identify(pointer);
    const int32_t* read_only = pointer;
    const int32_t* selected = true ? read_only : nullptr;
    if (distance == 2 and exact == 1 and selected[1] == 25 and
        cell_pointer->value == 25 and box_pointer->get() == 9 and
        *holder.pointer == 10 and *pointers[0] == 35) {
      return 0;
    }
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("raw-pointer-pipeline.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "well-typed raw pointer operations in unsafe should lower");
  expect(frontend.mirValid && frontend.mir.valid(),
         "raw pointer operations should produce verified MIR");

  const lang::HirFunctionInstance *main = findHirFunction(frontend, "main");
  expect(main != nullptr, "raw pointer HIR should retain main");
  if (main != nullptr) {
    expect(std::any_of(main->body.statements.begin(),
                       main->body.statements.end(),
                       [](const lang::HirStatement &statement) {
                         return statement.unsafeBlock;
                       }),
           "HIR should preserve the lexical unsafe scope");
    expect(std::any_of(main->body.values.begin(), main->body.values.end(),
                       [](const lang::HirValue &value) {
                         return value.kind == lang::HirValueKind::Unary &&
                                value.operation == lang::TokenKind::AMPERSAND &&
                                value.unsafeOperation ==
                                    lang::UnsafeOperationKind::None;
                       }),
           "HIR should retain safe raw address formation without unsafe "
           "permission");
    expect(
        std::any_of(main->body.values.begin(), main->body.values.end(),
                    [](const lang::HirValue &value) {
                      return value.kind == lang::HirValueKind::MemberSet &&
                             value.unsafeOperation ==
                                 lang::UnsafeOperationKind::PointerArithmetic;
                    }),
        "HIR should mark compound arithmetic on a raw-pointer field");
    expect(
        std::any_of(main->body.values.begin(), main->body.values.end(),
                    [](const lang::HirValue &value) {
                      return value.kind == lang::HirValueKind::IndexSet &&
                             value.unsafeOperation ==
                                 lang::UnsafeOperationKind::PointerArithmetic;
                    }),
        "HIR should mark compound arithmetic on a raw-pointer array "
        "element");
    expect(std::any_of(main->body.values.begin(), main->body.values.end(),
                       [](const lang::HirValue &value) {
                         return value.kind == lang::HirValueKind::MemberSet &&
                                value.unsafeOperation ==
                                    lang::UnsafeOperationKind::RawMember;
                       }),
           "raw-arrow compound assignment should preserve raw-member "
           "metadata");
  }

  const lang::MirBody *body = findMirFunction(frontend, "main");
  expect(body != nullptr, "raw pointer HIR should have a matching MIR body");
  if (body != nullptr) {
    bool address = false;
    bool add = false;
    bool subtract = false;
    bool pointerDifferenceFailureFree = true;
    bool rawLoadsFailureFree = true;
    bool rawMethodReceiver = false;
    bool rawRead = false;
    bool rawWrite = false;
    bool fusedPointerModify = false;
    bool fusedPointerCompoundAssignment = false;
    std::size_t orderedPointerModifications = 0;
    for (const lang::MirBlock &block : body->blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        address |= instruction.operation == lang::MirOperation::AddressOf;
        add |= instruction.operation == lang::MirOperation::PointerAdd;
        fusedPointerModify |=
            instruction.kind == lang::MirInstructionKind::Modify &&
            instruction.info.type.kind == lang::SemanticType::RawPointer;
        fusedPointerCompoundAssignment |=
            instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.info.type.kind == lang::SemanticType::RawPointer &&
            (instruction.operation == lang::MirOperation::AddAssign ||
             instruction.operation == lang::MirOperation::SubtractAssign);
        if (instruction.operation == lang::MirOperation::PointerDifference) {
          subtract = true;
          pointerDifferenceFailureFree &= instruction.definedFailure.empty() &&
                                          instruction.localFailureSites.empty();
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.receiver &&
            instruction.receiver->kind == lang::MirOperandKind::Value) {
          const lang::MirInstruction *stage = nullptr;
          for (const auto &stageBlock : body->blocks)
            for (const auto &candidate : stageBlock.instructions)
              if (candidate.result == instruction.receiver->value &&
                  candidate.kind == lang::MirInstructionKind::CallInput &&
                  candidate.callSite == instruction.callSite &&
                  candidate.callInputRole == lang::MirCallInputRole::Receiver &&
                  candidate.operands.size() == 1)
                stage = &candidate;
          const lang::MirPlace *receiver =
              stage == nullptr ? nullptr
                               : body->findPlace(stage->operands.front().place);
          rawMethodReceiver |=
              receiver != nullptr &&
              (stage->operands.front().kind ==
                   lang::MirOperandKind::BorrowRead ||
               stage->operands.front().kind ==
                   lang::MirOperandKind::BorrowWrite) &&
              instruction.unsafeOperation ==
                  lang::UnsafeOperationKind::RawMember &&
              instruction.rawMemoryAccess &&
              instruction.receiver->type.kind == lang::SemanticType::Class &&
              std::any_of(receiver->projections.begin(),
                          receiver->projections.end(),
                          [](const lang::MirPlaceProjection &projection) {
                            return projection.kind ==
                                   lang::MirProjectionKind::RawDereference;
                          });
        }
        if (instruction.rawMemoryAccess) {
          const lang::MirEffectTraits effects = lang::effects(instruction);
          rawRead |= effects.readsUnknownMemory;
          rawWrite |= effects.writesUnknownMemory;
          if (instruction.kind == lang::MirInstructionKind::Load) {
            rawLoadsFailureFree &= instruction.definedFailure.empty() &&
                                   instruction.localFailureSites.empty();
          }
        }
      }
    }
    for (const lang::MirBlock &block : body->blocks) {
      for (const lang::MirInstruction &compute : block.instructions) {
        const bool pointerArithmetic =
            compute.kind == lang::MirInstructionKind::Compute &&
            (compute.operation == lang::MirOperation::PointerAdd ||
             compute.operation == lang::MirOperation::PointerSubtract);
        if (!pointerArithmetic ||
            compute.unsafeOperation == lang::UnsafeOperationKind::None ||
            !compute.result || compute.operands.size() != 2 ||
            compute.operands.front().kind != lang::MirOperandKind::Value ||
            (compute.operands.back().kind != lang::MirOperandKind::Constant &&
             compute.operands.back().kind != lang::MirOperandKind::Value)) {
          continue;
        }
        const lang::MirInstruction *read = nullptr;
        const lang::MirInstruction *commit = nullptr;
        for (const lang::MirBlock &candidateBlock : body->blocks) {
          for (const lang::MirInstruction &candidate :
               candidateBlock.instructions) {
            if (candidate.hirValue != compute.hirValue) {
              continue;
            }
            if (candidate.kind == lang::MirInstructionKind::Load &&
                candidate.result && candidate.operands.size() == 1 &&
                candidate.operands.front().place != 0 &&
                *candidate.result == compute.operands.front().value) {
              read = &candidate;
            }
            if (candidate.kind == lang::MirInstructionKind::Assign &&
                candidate.operation == lang::MirOperation::Assign &&
                candidate.destination && candidate.operands.size() == 1 &&
                candidate.operands.front().value == *compute.result) {
              commit = &candidate;
            }
          }
        }
        if (read != nullptr && commit != nullptr && commit->destination &&
            *commit->destination == read->operands.front().place) {
          ++orderedPointerModifications;
        }
      }
    }
    expect(address && add && subtract,
           "MIR should distinguish address, pointer offset, and difference");
    expect(!fusedPointerModify && !fusedPointerCompoundAssignment &&
               orderedPointerModifications == 5,
           "raw pointer increments, decrements, and compound assignments "
           "should lower to five ordered read/offset/commit schedules "
           "without fused update nodes");
    expect(pointerDifferenceFailureFree,
           "unsafe pointer difference should not acquire a checked-integer "
           "overflow edge from its int64_t result type");
    expect(rawLoadsFailureFree,
           "unsafe raw-pointer loads should not acquire checked arithmetic "
           "failure edges from their loaded integer type");
    expect(rawMethodReceiver,
           "raw pointer method calls should borrow the unchecked pointee "
           "place and retain unsafe memory metadata");
    expect(rawRead && rawWrite,
           "raw pointer loads and stores should conservatively affect unknown "
           "memory");
    expect(body->loans.empty(),
           "raw pointers should not manufacture checked-reference loans");
    expect(
        std::any_of(body->places.begin(), body->places.end(),
                    [](const lang::MirPlace &place) {
                      return std::any_of(
                          place.projections.begin(), place.projections.end(),
                          [](const lang::MirPlaceProjection &projection) {
                            return projection.kind ==
                                       lang::MirProjectionKind::RawIndex ||
                                   projection.kind ==
                                       lang::MirProjectionKind::RawDereference;
                          });
                    }),
        "MIR places should identify unchecked raw memory projections");
  }

  const lang::OptimizationResult optimizations;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("owner_access(pointer)") == std::string::npos,
         "raw dereference must not use checked-owner lowering");
  const std::size_t mainMarker =
      artifact.contents.find("scalar-cfg-failure-v1 function-instance");
  const std::string_view emittedMain =
      mainMarker == std::string::npos
          ? std::string_view{}
          : std::string_view(artifact.contents).substr(mainMarker);
  expect(emittedMain.find("(*__gti_mir_v_") != std::string_view::npos &&
             emittedMain.find(").value") != std::string_view::npos &&
             emittedMain.find("[__gti_mir_v_") != std::string_view::npos &&
             emittedMain.find("(&__gti_mir_p_") != std::string_view::npos &&
             emittedMain.find(".get());") != std::string_view::npos,
         "raw member and index operations should remain native C++ syntax");
  expect(emittedMain.find("mir_checked_array_read_v1(") !=
                 std::string_view::npos &&
             emittedMain.find(".get().pointer, __gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find("raw_pointer_slot_copy((*__gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find(").pointer, __gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find("[static_cast<std::size_t>(0)] = ") !=
                 std::string_view::npos,
         "raw-pointer compound field and fixed-array element targets should "
         "use ordered pointer arithmetic and byte-preserving slot writes "
         "after any required bounds check");
}

void testUnsafeAndPointerDiagnostics() {
  const lang::FrontendResult outside =
      lang::Frontend().analyze("raw-pointer-outside-unsafe.gti", R"(
int main() {
  mut int32_t value = 1;
  int32_t* pointer = nullptr;
  pointer = &value;
  return *pointer;
}
)");
  expect(!outside.canGenerateCode() && hasCode(outside, "GTI-S2055"),
         "raw dereference should require unsafe even when address formation "
         "does not");

  const lang::FrontendResult invalidDomain =
      lang::Frontend().analyze("invalid-raw-pointer-domain.gti", R"(
int main() {
  void* handle = nullptr;
  unsafe { *handle; }
  return 0;
}
)");
  expect(!invalidDomain.canGenerateCode() &&
             hasMessage(invalidDomain, "void* cannot be dereferenced") &&
             !hasCode(invalidDomain, "GTI-S2055"),
         "an invalid pointer domain should not also suggest unsafe");

  const lang::FrontendResult constWrite =
      lang::Frontend().analyze("const-raw-pointer-write.gti", R"(
int main() {
  const int32_t* pointer = nullptr;
  unsafe { *pointer = 1; }
  return 0;
}
)");
  expect(!constWrite.canGenerateCode() && hasCode(constWrite, "GTI-S2002"),
         "const raw pointer memory should be read-only");

  const lang::FrontendResult uninitialized =
      lang::Frontend().analyze("uninitialized-raw-pointer.gti", R"(
struct Holder { mut int32_t* value; };
int main() { int32_t* local; return 0; }
)");
  expect(!uninitialized.canGenerateCode() &&
             hasMessage(uninitialized, "explicit initializer"),
         "raw pointer locals and fields should never be indeterminate");

  // The explicit-initializer rule applies to a field declared with a pointer
  // type. A generic field that becomes a pointer in an instance is, like
  // every field, initialized by each constructor.
  const lang::FrontendResult genericField =
      lang::Frontend().analyze("generic-raw-pointer-field.gti", R"(
class Slot<T> {
  mut T stored;
public:
  Slot(T stored) : stored(stored) {}
};
int main() {
  mut int32_t value = 1;
  Slot<int32_t*> slot = Slot<int32_t*>(&value);
  return 0;
}
)");
  expect(genericField.canGenerateCode(),
         "a generic field instantiated as a raw pointer needs no declaration "
         "initializer");
  const lang::FrontendResult writtenGenericField =
      lang::Frontend().analyze("generic-written-raw-pointer-field.gti", R"(
class Slot<T> {
  mut T* stored;
public:
  Slot(T* stored) : stored(stored) {}
};
int main() { return 0; }
)");
  expect(!writtenGenericField.canGenerateCode() &&
             hasMessage(writtenGenericField, "explicit initializer"),
         "a generic field declared with a pointer type keeps the rule");

  const lang::FrontendResult aliasDepth =
      lang::Frontend().analyze("nested-raw-pointer-alias.gti", R"(
using Pointer = int32_t*;
int main() { Pointer* nested = nullptr; return 0; }
)");
  expect(aliasDepth.canGenerateCode(),
         "a transparent alias should compose with further pointer layers");

  const lang::FrontendResult aliasPointerToArray =
      lang::Frontend().analyze("raw-pointer-to-array-alias.gti", R"(
using Row = int32_t[2];
int main() { Row* pointer = nullptr; return 0; }
)");
  expect(!aliasPointerToArray.canGenerateCode() &&
             hasCode(aliasPointerToArray, "GTI-S2056") &&
             hasMessage(aliasPointerToArray,
                        "Pointer-to-array types are not supported"),
         "aliases should not expose pointer-to-array types");

  const lang::FrontendResult arrayOfPointers =
      lang::Frontend().analyze("array-of-raw-pointers.gti", R"(
int main() { int32_t* pointers[2] = {}; return 0; }
)");
  expect(arrayOfPointers.canGenerateCode(),
         "fixed arrays of one-level raw pointers should remain supported");

  // A checked reference names the pointer slot: reading or reseating through
  // it is safe, while the pointee stays behind lexical unsafe.
  const lang::FrontendResult referencePointer =
      lang::Frontend().analyze("raw-pointer-reference.gti", R"(
void point(mut const int32_t*& slot, int32_t& target) { slot = &target; }
int main() {
  int32_t value = 7;
  mut const int32_t* pointer = nullptr;
  point(pointer, value);
  const int32_t*& alias = pointer;
  unsafe { return *alias - 7; }
}
)");
  expect(referencePointer.canGenerateCode(),
         "checked references may read and reseat a raw-pointer slot");
  for (const auto &[source, code, message] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"int main() { mut int32_t* pointer = nullptr; int32_t*& alias = "
            "pointer; alias = nullptr; return 0; }",
            "GTI-S2002", "immutable binding"},
           {"int main() { mut int32_t value = 1; mut int32_t* pointer = "
            "&value; int32_t*& alias = pointer; return *alias; }",
            "GTI-S2055", "unsafe"},
           {"int main() { int32_t* pointer = nullptr; mut int32_t*& slot = "
            "pointer; return 0; }",
            "GTI-S2017", "mutable initializer"},
           // Fixed pointer arrays and native records hold native pointer
           // objects, not GTI pointer slots.
           {"int main() { mut int32_t a = 1; int32_t* pointers[1] = {&a}; "
            "int32_t*& first = pointers[std::size_t(0)]; return 0; }",
            "GTI-S2056", "native layout"},
           {"int main() { mut int32_t a = 1; int32_t* pointers[1] = {&a}; "
            "for (auto& p : pointers) { unsafe { return *p; } } return 0; }",
            "GTI-S2056", "native layout"},
           {"void reset(mut int32_t*& slot) { slot = nullptr; } int main() { "
            "mut int32_t a = 1; mut int32_t* pointers[1] = {&a}; "
            "reset(pointers[std::size_t(0)]); return 0; }",
            "GTI-S2056", "native layout"},
           {"[[c_abi]] struct Pair { mut int32_t* left; }; extern \"C\" { "
            "Pair make_pair(); } int main() { mut Pair pair = unsafe { "
            "make_pair() }; int32_t*& left = pair.left; return 0; }",
            "GTI-S2056", "native layout"},
           {"[[c_abi]] struct Pair { mut int32_t* left; }; extern \"C\" { "
            "Pair make_pair(); } int main() { mut Pair pair = unsafe { "
            "make_pair() }; mut Pair* p = &pair; unsafe { int32_t*& left = "
            "p->left; } return 0; }",
            "GTI-S2056", "native layout"}}) {
    const lang::FrontendResult rejected =
        lang::Frontend().analyze("raw-pointer-reference-negative.gti", source);
    expect(!rejected.canGenerateCode() && hasCode(rejected, code) &&
               hasMessage(rejected, message),
           "a pointer-slot reference keeps reference access and unsafe "
           "pointee rules: " +
               source);
  }

  const lang::FrontendResult arrayDecay =
      lang::Frontend().analyze("raw-pointer-array-decay.gti", R"(
void inspect(const int32_t* values) {}
int main() {
  int32_t values[2] = {1, 2};
  inspect(values);
  return 0;
}
)");
  expect(!arrayDecay.canGenerateCode() && hasCode(arrayDecay, "GTI-S2003") &&
             hasHint(arrayDecay, "do not decay") &&
             !hasHint(arrayDecay, "int32_t*(value)"),
         "array-to-pointer mismatches should explain the explicit "
         "element-address path instead of suggesting an unavailable cast");

  const lang::FrontendResult leakedUnsafe =
      lang::Frontend().analyze("unsafe-lambda-boundary.gti", R"(
int main() {
  mut int32_t value = 1;
  mut int32_t* pointer = nullptr;
  unsafe {
    pointer = &value;
    auto read = [pointer]() -> int32_t { return *pointer; };
    return read();
  }
}
)");
  expect(!leakedUnsafe.canGenerateCode() && hasCode(leakedUnsafe, "GTI-S2055"),
         "an unsafe scope should not leak into a nested callable");

  const lang::FrontendResult fieldCompoundOutside =
      lang::Frontend().analyze("raw-pointer-field-compound-outside.gti", R"(
struct Holder { mut int32_t* pointer = nullptr; };
int main() {
  mut Holder holder{};
  holder.pointer += 1;
  return 0;
}
)");
  expect(!fieldCompoundOutside.canGenerateCode() &&
             hasCode(fieldCompoundOutside, "GTI-S2055"),
         "compound arithmetic on a raw-pointer field should require unsafe");

  const lang::FrontendResult arrayCompoundOutside =
      lang::Frontend().analyze("raw-pointer-array-compound-outside.gti", R"(
int main() {
  mut int32_t* pointers[1] = {};
  pointers[0] -= 1;
  return 0;
}
)");
  expect(!arrayCompoundOutside.canGenerateCode() &&
             hasCode(arrayCompoundOutside, "GTI-S2055"),
         "compound arithmetic on a raw-pointer array element should require "
         "unsafe");

  const lang::FrontendResult immutableCompound =
      lang::Frontend().analyze("immutable-raw-pointer-compound.gti", R"(
struct Holder { mut int32_t* pointer = nullptr; };
int main() {
  Holder holder{};
  int32_t* pointers[1] = {};
  holder.pointer += 1;
  pointers[0] -= 1;
  return 0;
}
)");
  expect(!immutableCompound.canGenerateCode() &&
             hasCode(immutableCompound, "GTI-S2002") &&
             !hasCode(immutableCompound, "GTI-S2055"),
         "immutable compound targets should fail mutability checks without "
         "an unsafe suggestion");
}

void testRawPointerConstructorRanking() {
  const lang::FrontendResult exact =
      lang::Frontend().analyze("raw-pointer-constructor-ranking.gti", R"(
class PointerChoice {
public:
  PointerChoice(int32_t* value) {}
  PointerChoice(const int32_t* value) {}
};

class PointerBase {
public:
  PointerBase(int32_t* value) {}
  PointerBase(const int32_t* value) {}
};

class PointerDerived : public PointerBase {
public:
  PointerDerived(int32_t* value) : PointerBase(value) {}
};

int main() {
  mut int32_t value = 1;
  mut int32_t* pointer = nullptr;
  unsafe { pointer = &value; }
  PointerChoice direct{pointer};
  PointerDerived derived{pointer};
  return 0;
}
)");
  if (!exact.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : exact.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(exact.canGenerateCode(),
         "constructor overloads should prefer an exact raw pointer over "
         "adding pointee const");

  const auto isMutableIntPointer = [](const lang::SemanticType &type) {
    return type.kind == lang::SemanticType::RawPointer &&
           type.pointerAccess == lang::AccessMode::Mutable &&
           type.arguments.size() == 1 &&
           type.arguments.front() == lang::SemanticType::Int32;
  };
  const lang::FunctionDecl *main = findTopLevelFunction(exact.program, "main");
  const auto *directDeclaration =
      main == nullptr || main->body()->statements().size() <= 3
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                main->body()->statements()[3].get());
  const auto *directInitializer =
      directDeclaration == nullptr
          ? nullptr
          : dynamic_cast<const lang::DirectInitializer *>(
                directDeclaration->initializer().get());
  const lang::ResolvedConstructionInfo *directConstruction =
      directInitializer == nullptr
          ? nullptr
          : exact.semantics.findConstruction(*directInitializer);
  expect(directConstruction != nullptr &&
             directConstruction->parameterTypes.size() == 1 &&
             isMutableIntPointer(directConstruction->parameterTypes.front()),
         "direct construction should record the exact T* constructor");

  const lang::ClassDecl *derived =
      findTopLevelClass(exact.program, "PointerDerived");
  const lang::ConstructorDecl *derivedConstructor = nullptr;
  if (derived != nullptr) {
    for (const lang::StmtPtr &member : derived->members()) {
      if (const auto *constructor =
              dynamic_cast<const lang::ConstructorDecl *>(member.get())) {
        derivedConstructor = constructor;
        break;
      }
    }
  }
  const lang::ConstructorInitializer *baseInitializer =
      derivedConstructor == nullptr ||
              derivedConstructor->initializers().empty()
          ? nullptr
          : &derivedConstructor->initializers().front();
  const lang::ResolvedConstructorInitializerInfo *baseConstruction =
      baseInitializer == nullptr
          ? nullptr
          : exact.semantics.findConstructorInitializer(*baseInitializer);
  expect(baseConstruction != nullptr &&
             baseConstruction->kind ==
                 lang::ConstructorInitializerTargetKind::Base &&
             baseConstruction->parameterTypes.size() == 1 &&
             isMutableIntPointer(baseConstruction->parameterTypes.front()),
         "base construction should record the exact T* constructor");

  const lang::FrontendResult nullPointer =
      lang::Frontend().analyze("raw-pointer-null-constructor-ranking.gti", R"(
class PointerBase {
public:
  PointerBase(int32_t* value) {}
  PointerBase(const int32_t* value) {}
};

class PointerDerived : public PointerBase {
public:
  PointerDerived() : PointerBase(nullptr) {}
};

int main() {
  PointerBase direct{nullptr};
  return 0;
}
)");
  expect(!nullPointer.canGenerateCode() &&
             countCode(nullPointer, "GTI-S2013") == 2 &&
             hasMessage(nullPointer,
                        "Construction of 'PointerBase' is ambiguous") &&
             hasMessage(nullPointer,
                        "Base construction of 'PointerBase' is ambiguous"),
         "nullptr should remain equally ranked for T* and const T* in direct "
         "and base construction");

  const lang::FrontendResult crossRanked =
      lang::Frontend().analyze("raw-pointer-cross-ranked-overloads.gti", R"(
int32_t choose(int32_t* first, const int32_t* second,
               const int32_t* third) { return 1; }
int32_t choose(const int32_t* first, int32_t* second,
               int32_t* third) { return 2; }

class Selector {
public:
  int32_t select(int32_t* first, const int32_t* second,
                 const int32_t* third) { return 1; }
  int32_t select(const int32_t* first, int32_t* second,
                 int32_t* third) { return 2; }
};

class PointerChoice {
public:
  PointerChoice(int32_t* first, const int32_t* second,
                const int32_t* third) {}
  PointerChoice(const int32_t* first, int32_t* second,
                int32_t* third) {}
};

class PointerBase {
public:
  PointerBase(int32_t* first, const int32_t* second,
              const int32_t* third) {}
  PointerBase(const int32_t* first, int32_t* second,
              int32_t* third) {}
};

class PointerDerived : public PointerBase {
public:
  PointerDerived(int32_t* value) : PointerBase(value, value, value) {}
};

int main() {
  mut int32_t value = 1;
  mut int32_t* pointer = nullptr;
  unsafe { pointer = &value; }
  [[discard]] choose(pointer, pointer, pointer);
  Selector selector{};
  [[discard]] selector.select(pointer, pointer, pointer);
  PointerChoice direct{pointer, pointer, pointer};
  PointerDerived derived{pointer};
  return 0;
}
)");
  expect(!crossRanked.canGenerateCode() &&
             countCode(crossRanked, "GTI-S2013") == 4 &&
             hasMessage(crossRanked, "Call to 'choose' is ambiguous") &&
             hasMessage(crossRanked,
                        "Construction of 'PointerChoice' is ambiguous") &&
             hasMessage(crossRanked,
                        "Base construction of 'PointerBase' is ambiguous"),
         "raw pointer ranking should require one candidate to dominate every "
         "argument across functions, methods, direct construction, and base "
         "construction");
}

void testPointerBearingCAbi() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("raw-pointer-c-abi.gti", R"(
namespace native {
extern "C" {
  int32_t scalar();
  void* open(int32_t value);
  int32_t inspect(const void* handle);
  void close(void* handle);
}
}

int main() {
  mut int32_t value = native::scalar();
  mut void* handle = nullptr;
  unsafe {
    handle = native::open(value);
    value = native::inspect(handle);
    native::close(handle);
  }
  return value;
}
)");
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "opaque void pointer C APIs should work inside unsafe");
  const lang::OptimizationResult optimizations;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
  expect(artifact.contents.find("extern \"C\"") != std::string::npos &&
             artifact.contents.find("const void*") != std::string::npos,
         "C prototypes should preserve raw pointer and pointee const types");

  const lang::FrontendResult outside =
      lang::Frontend().analyze("raw-pointer-c-call-outside.gti", R"(
extern "C" { void* open(); int32_t scalar(); }
int main() { int32_t value = scalar(); void* handle = open(); return value; }
)");
  expect(!outside.canGenerateCode() && hasCode(outside, "GTI-S2055") &&
             hasRelated(outside, "C declaration"),
         "pointer-bearing C calls should require unsafe and identify their "
         "declaration");

  const lang::FrontendResult invalidLeaf =
      lang::Frontend().analyze("invalid-raw-pointer-c-abi.gti", R"(
struct NativeRecord { int32_t value = 0; };
extern "C" { NativeRecord* acquire(); }
int main() { return 0; }
)");
  expect(!invalidLeaf.canGenerateCode() && hasCode(invalidLeaf, "GTI-S2054"),
         "C ABI raw pointers should retain the fixed-layout leaf allowlist");
}

void testOptionalRawAuditOff() {
  lang::FrontendWorkMetrics metrics;
  lang::FrontendOptions options;
  options.workMetrics = &metrics;
  const auto result = lang::Frontend(options).analyze("raw-audit-off.gti", R"(
unsafe int read(const int* pointer) { unsafe { return *pointer; } }
int main() {
  mut const int* expired = nullptr;
  { int dead = 7; expired = &dead; }
  mut int live = 1;
  int* address = &live;
  int& loan = live;
  unsafe { *address = read(expired); }
  return loan;
}
)");
  expect(result.canGenerateCode(),
         "default Off does not enforce optional raw lifetime/access findings");
  expect(metrics.semanticRawAuditQueries == 0,
         "default Off does not execute raw source-query workers");
}

void testKnownRawPointerOrigins() {
  const auto expired = auditFrontend().analyze("raw-expired.gti", R"(
int main() {
  mut const int* pointer = nullptr;
  unsafe {
    { int value = 42; pointer = &value; }
    auto copied = pointer;
    return *copied;
  }
}

)");
  expect(!expired.canGenerateCode() && hasCode(expired, "GTI-S2101") &&
             hasMessage(expired, "scope has ended") &&
             hasRelated(expired, "Raw address obtained here") &&
             hasHint(expired, "Unsafe validity is a programmer obligation"),
         "copying a raw address must preserve known expired local provenance");

  const auto joined = auditFrontend().analyze("raw-joined.gti", R"(
int read(bool choose) {
  int alive = 7;
  mut const int* pointer = nullptr;
  unsafe {
    pointer = &alive;
    { int gone = 42; if (choose) { pointer = &gone; } }
    return pointer[0];
  }
}
int main() { return read(false); }
)");
  expect(!joined.canGenerateCode() && hasMessage(joined, "scope has ended"),
         "flow joins must preserve each possible raw address origin");

  const auto reseated = auditFrontend().analyze("raw-reseated.gti", R"(
int main() {
  int alive = 7;
  mut const int* pointer = nullptr;
  unsafe {
    { int gone = 42; pointer = &gone; }
    const int* unused_old_address = pointer;
    pointer = &alive;
    return *pointer;
  }
}
)");
  expect(reseated.canGenerateCode(), "raw addresses do not retain loans and "
                                     "reseating replaces old provenance");

  const auto conflicting = auditFrontend().analyze("raw-conflict.gti", R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    *pointer = 9;
    return loan;
  }
}
)");
  expect(!conflicting.canGenerateCode() &&
             hasMessage(conflicting, "live incompatible checked borrow"),
         "raw writes cannot bypass a known live checked borrow");

  const auto arrayConflict =
      auditFrontend().analyze("raw-array-conflict.gti", R"(
int main() {
  mut int values[2] = {1, 2};
  unsafe {
    auto pointer = &values[0];
    int& loan = values[1];
    *(pointer + 1) = 9;
    return loan;
  }
}
)");
  expect(!arrayConflict.canGenerateCode() &&
             hasMessage(arrayConflict, "live incompatible checked borrow"),
         "derived raw array addresses must not retain a false disjoint index");

  const auto methodConflict =
      auditFrontend().analyze("raw-method-conflict.gti", R"(
struct Value {
  mut int count = 0;
  void change() mut { this.count = 1; }
};
int main() {
  mut Value value{};
  unsafe {
    auto pointer = &value;
    Value& loan = value;
    pointer->change();
    return loan.count;
  }
}
)");
  expect(!methodConflict.canGenerateCode() &&
             hasMessage(methodConflict, "live incompatible checked borrow"),
         "selected mutable raw receiver calls must respect checked borrows");

  const auto endedLoan = auditFrontend().analyze("raw-ended-loan.gti", R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    int before = loan;
    *pointer = before + 2;
    return *pointer;
  }
}
)");
  expect(endedLoan.canGenerateCode(),
         "raw provenance must reuse nonlexical loan endpoints");

  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto moved = auditFrontend().analyze("raw-moved.gti", R"(
struct Owner {
  int value = 1;
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
};
int main() {
  Owner original{};
  unsafe {
    auto pointer = &original;
    Owner next = std::move(original);
    return pointer->value;
  }
}
)",
                                             {root / "stdlib/prelude.gti"}, {},
                                             {root / "stdlib"});
  expect(!moved.canGenerateCode() && hasMessage(moved, "owner that was moved"),
         "raw pointer copies must not erase known moved-owner state");
  if (!hasMessage(moved, "owner that was moved"))
    for (const auto &diagnostic : moved.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}

void testProjectedRawPointerOrigins() {
  const auto recursiveReference =
      auditFrontend().analyze("raw-recursive-reference-fields.gti", R"(
class Node {
public:
  Node& next;
  const int* pointer = nullptr;
  Node(Node& source, int tag) : next(source) {}
  const int* address() { return this.pointer; }
};
const int* address(Node& node) { return node.pointer; }
int main() { return 0; }
)");
  expect(recursiveReference.canGenerateCode(),
         "raw-field collection must not recurse through stored checked "
         "references");
  const auto recursiveCarrier =
      auditFrontend().analyze("raw-recursive-reference-carrier.gti", R"(
class Node {
public:
  Node& next;
  const int* pointer = nullptr;
  Node(Node& source, int tag) : next(source) {}
};
class NodeCarrier {
public:
  Node& referent;
  NodeCarrier(Node& source) : referent(source) {}
};
Node& escape(NodeCarrier carrier) { return carrier.referent.next; }
)");
  expect(!recursiveCarrier.canGenerateCode() &&
             hasCode(recursiveCarrier, "GTI-S2085"),
         "recursive checked-reference carriers must retain their ordinary "
         "escape diagnostic");
  const std::string prefix = R"(
struct Address { mut const int* pointer = nullptr; };
struct Pair { mut Address first{}; mut Address second{}; };
)";
  const std::vector<std::pair<std::string, std::string>> expiredPrograms = {
      {"field-copy", R"(
int main() {
  mut Address original{};
  unsafe { { int value = 42; original.pointer = &value; }
    Address copied = original; return *copied.pointer; }
})"},
      {"raw-field-projection", R"(
int main() {
  mut Address address{};
  { int gone = 5; address.pointer = &gone; }
  const Address* outer = &address;
  unsafe { return *outer->pointer; }
})"},
      {"raw-alias-field-write", R"(
int main() {
  mut Address address{};
  Address* alias = &address;
  unsafe {
    { int gone = 5; alias->pointer = &gone; }
    return *address.pointer;
  }
})"},
      {"raw-alias-nested-field-write", R"(
int main() {
  mut Pair pair{};
  Pair* alias = &pair;
  unsafe {
    { int gone = 5; alias->first.pointer = &gone; }
    return *pair.first.pointer;
  }
})"},
      {"raw-alias-chain-write", R"(
struct Link { mut Address* address = nullptr; };
int main() {
  mut Address address{};
  mut Link link = {.address = &address};
  Link* alias = &link;
  unsafe {
    { int gone = 5; alias->address->pointer = &gone; }
    return *address.pointer;
  }
})"},
      {"raw-alias-projected-address-write", R"(
int main() {
  mut Pair pair{};
  Pair* outer = &pair;
  unsafe {
    Address* inner = &outer->first;
    { int gone = 5; inner->pointer = &gone; }
    return *pair.first.pointer;
  }
})"},
      {"raw-alias-chain-read", R"(
struct Link { mut Address* address = nullptr; };
int main() {
  mut Address address{};
  mut Link link = {.address = &address};
  Link* alias = &link;
  unsafe {
    { int gone = 5; alias->address->pointer = &gone; }
    return *alias->address->pointer;
  }
})"},
      {"raw-alias-aggregate-write", R"(
int main() {
  mut Address address{};
  Address* alias = &address;
  unsafe {
    { int gone = 5; Address incoming = {.pointer = &gone}; *alias = incoming; }
    return *address.pointer;
  }
})"},
      {"raw-alias-index-write", R"(
int main() {
  mut Address addresses[2] = {Address(), Address()};
  Address* alias = &addresses[0];
  unsafe {
    { int gone = 5; Address incoming = {.pointer = &gone}; alias[1] = incoming; }
    return *addresses[1].pointer;
  }
})"},
      {"raw-alias-index-field-write", R"(
int main() {
  mut Address addresses[2] = {Address(), Address()};
  Address* alias = &addresses[0];
  unsafe {
    { int gone = 5; alias[1].pointer = &gone; }
    return *addresses[1].pointer;
  }
})"},
      {"raw-multiple-destinations-write", R"(
int read(bool choose) {
  int live = 9;
  mut Address first{};
  mut Address second{};
  { int gone = 5; first.pointer = &gone; }
  Address* alias = choose ? &first : &second;
  unsafe { alias->pointer = &live; return *first.pointer; }
}
int main() { return read(false); }
)"},
      {"raw-dynamic-destination-write", R"(
int read(int index) {
  int live = 9;
  mut Address addresses[2] = {Address(), Address()};
  unsafe {
    { int gone = 5; addresses[index].pointer = &gone; }
    addresses[index].pointer = &live;
    return *addresses[0].pointer;
  }
}
int main() { return read(1); }
)"},
      {"raw-dynamic-array-replacement", R"(
int read(int index) {
  int live = 9;
  mut const int* pointers[2] = {nullptr, nullptr};
  unsafe {
    { int gone = 5; pointers[index] = &gone; }
    pointers[index] = &live;
    return *pointers[0];
  }
}
int main() { return read(1); }
)"},
      {"nested-designated", R"(
int main() {
  mut Pair pair{};
  unsafe { { int value = 42;
    pair.first = {.pointer = &value}; }
    Pair copied = pair; return *copied.first.pointer; }
})"},
      {"field-branch", R"(
int read(bool choose) {
  int live = 9;
  mut Address address{};
  unsafe { address.pointer = &live; { int gone = 7;
    if (choose) { address.pointer = &gone; } }
    return address.pointer[0]; }
}

int main() { return read(false); }
)"},
      {"array-copy", R"(
int main() {
  mut const int* pointers[2] = {nullptr, nullptr};
  unsafe { { int gone = 7; pointers[1] = &gone; }
    auto copied = pointers; return *copied[1]; }
})"},
      {"dynamic-array-index", R"(
int read(int index) {
  int live = 9;
  mut const int* pointers[2] = {nullptr, nullptr};
  unsafe { pointers[0] = &live; { int gone = 7; pointers[1] = &gone; }
    return *pointers[index]; }
}
int main() { return read(1); }
)"}};
  for (const auto &[name, program] : expiredPrograms) {
    const auto result =
        auditFrontend().analyze("raw-" + name + ".gti", prefix + program);
    expect(!result.canGenerateCode() && hasMessage(result, "scope has ended"),
           "raw projection transport must preserve an expired origin: " + name);
    if (!hasMessage(result, "scope has ended"))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << name << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
  }
  const auto disjoint =
      auditFrontend().analyze("raw-disjoint-reseat.gti", prefix + R"(
int main() {
  int live = 9;
  mut Pair pair{};
  unsafe {
    pair.second.pointer = &live;
    { int gone = 7; pair.first.pointer = &gone; }
    pair.first.pointer = &live;
    return *pair.first.pointer + *pair.second.pointer;
  }
})");
  expect(disjoint.canGenerateCode(),
         "reseating a raw field must replace only its own facts");
  const auto aliasReseated =
      auditFrontend().analyze("raw-alias-reseat.gti", prefix + R"(
int main() {
  int live = 9;
  mut Pair pair{};
  Pair* alias = &pair;
  unsafe {
    pair.second.pointer = &live;
    { int gone = 5; alias->first.pointer = &gone; }
    alias->first.pointer = &live;
    return *pair.first.pointer + *alias->second.pointer;
  }
})");
  expect(aliasReseated.canGenerateCode(),
         "an exact raw alias write must replace only its known destination");
  const auto unknownDestination =
      auditFrontend().analyze("raw-unknown-destination.gti", prefix + R"(
unsafe void set(Address* destination, const int* value) {
  unsafe { destination->pointer = value; }
}
int main() { return 0; }
)");
  expect(unknownDestination.canGenerateCode(),
         "unknown raw destinations retain the caller's unsafe proof boundary");
  const auto conflict = auditFrontend().analyze("raw-field-conflict.gti", R"(
struct Address { mut int* pointer = nullptr; };
int main() {
  mut int value = 7;
  unsafe {
    Address address = {.pointer = &value};
    int& checked = value;
    *address.pointer = 9;
    return checked;
  }
})");
  expect(!conflict.canGenerateCode() &&
             hasMessage(conflict, "live incompatible checked borrow"),
         "raw pointer fields must preserve conflicts with checked loans");
  const auto aliasConflict =
      auditFrontend().analyze("raw-alias-field-conflict.gti", R"(
struct Address { mut int* pointer = nullptr; };
int main() {
  mut int value = 7;
  mut Address address{};
  Address* alias = &address;
  unsafe {
    alias->pointer = &value;
    int& checked = value;
    *address.pointer = 9;
    return checked;
  }
})");
  expect(!aliasConflict.canGenerateCode() &&
             hasMessage(aliasConflict, "live incompatible checked borrow"),
         "writes through raw aliases must retain checked-loan conflicts");
}

void testRawPointerLoopOrigins() {
  const auto stale = auditFrontend().analyze("raw-loop-stale.gti", R"(
int read(int count) {
  int initial = 1;
  unsafe {
    mut const int* pointer = &initial;
    mut int result = 0;
    for (mut int index = 0; index < count; index++) {
      result += *pointer;
      int per_iteration = index;
      pointer = &per_iteration;
    }
    return result;
  }
}
int main() { return read(2); }
)");
  expect(!stale.canGenerateCode() && hasHint(stale, "later loop iteration"),
         "loop-carried raw origins must be checked at accesses on later "
         "iterations");
  const auto repaired = auditFrontend().analyze("raw-loop-repaired.gti", R"(
int read(int count) {
  int initial = 1;
  unsafe {
    mut const int* pointer = &initial;
    mut int result = 0;
    for (mut int index = 0; index < count; index++) {
      pointer = &initial;
      result += *pointer;
      int per_iteration = index;
      pointer = &per_iteration;
    }
    auto inert = pointer;
    return result;
  }
}
int main() { return read(2); }
)");
  expect(
      repaired.canGenerateCode(),
      "reseating before each access and inert expired copies remain allowed");
  const auto transitive = auditFrontend().analyze("raw-loop-transitive.gti", R"(
int read(int count) {
  int initial = 1;
  unsafe {
    mut const int* first = &initial;
    mut const int* second = &initial;
    mut int result = 0;
    for (mut int index = 0; index < count; index++) {
      auto alias = first;
      result += *alias;
      first = second;
      int per_iteration = index;
      second = &per_iteration;
    }
    return result;
  }
}
int main() { return read(3); }
)");
  expect(!transitive.canGenerateCode() &&
             hasHint(transitive, "later loop iteration"),
         "loop transfer must reach a fixed point through copies and aliases");
}

void testRawPointerCallTransport() {
  const std::vector<std::string> programs = {
      R"(
struct Address {
  const int* pointer = nullptr;
  Address(const int* input) : pointer(input) {}
};
int main() {
  mut const int* pointer = nullptr;
  { int gone = 5; pointer = &gone; }
  Address address{pointer};
  unsafe { return *address.pointer; }
})",
      R"(
int main() {
  mut const int* pointer = nullptr;
  { int gone = 5; pointer = &gone; }
  auto output = identity(pointer);
  unsafe { return *output; }
}
const int* identity(const int* input) { return input; }
)",
      R"(
struct Address {
  mut const int* pointer = nullptr;
  void set(const int* input) mut { this.pointer = input; }
};
int main() {
  mut Address address{};
  { int gone = 5; address.set(&gone); }
  unsafe { return *address.pointer; }
})",
      R"(
struct Address { mut const int* pointer = nullptr; };
void set(mut Address& address, const int* input) { address.pointer = input; }
int main() {
  mut Address address{};
  { int gone = 5; set(address, &gone); }
  unsafe { return *address.pointer; }
})"};
  for (const auto &program : programs) {
    const auto result =
        auditFrontend().analyze("raw-call-transport.gti", program);
    expect(!result.canGenerateCode() && hasMessage(result, "scope has ended"),
           "unsummarized source calls must preserve possibly forwarded known "
           "origins");
    if (!hasMessage(result, "scope has ended"))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto replaced = auditFrontend().analyze("raw-replaced.gti", R"(
struct Owner { int value = 1; };
int main() {
  mut Owner original{};
  const int* pointer = &original.value;
  Owner moved = std::move(original);
  original = Owner();
  unsafe { return *pointer; }
})",
                                                {root / "stdlib/prelude.gti"},
                                                {}, {root / "stdlib"});
  expect(
      !replaced.canGenerateCode() &&
          hasMessage(replaced, "lifetime was replaced"),
      "reinitialization must not revive raw addresses into a consumed object");
}

void testRawProducedCallOrigins() {
  const std::vector<std::pair<std::string, std::string>> expired = {
      {"later-body", R"(
int main() { auto pointer = dangling(); unsafe { return *pointer; } }
const int* dangling() { int local = 1; return &local; }
)"},
      {"earlier-body", R"(
const int* dangling() { int local = 1; return &local; }
int main() { auto pointer = dangling(); unsafe { return *pointer; } }
)"},
      {"wrapper-chain", R"(
int main() { auto pointer = relay(); unsafe { return pointer[0]; } }
const int* relay() { return middle(); }
const int* middle() { return dangling(); }
const int* dangling() { int local = 1; return &local; }
)"},
      {"aggregate-field", R"(
struct Address { const int* pointer = nullptr; };
int main() { auto address = dangling(); unsafe { return *address.pointer; } }
Address dangling() { int local = 1; return {.pointer = &local}; }
)"},
      {"nested-aggregate-field", R"(
struct Address { const int* pointer = nullptr; };
struct Pair { Address first{}; Address second{}; };
int main() { auto pair = dangling(); unsafe { return *pair.second.pointer; } }
Pair dangling() { int local = 1; return {.first = Address(), .second = {.pointer = &local}}; }
)"},
      {"generic-instance", R"(
int main() { auto pointer = dangling<int>(1); unsafe { return *pointer; } }
const T* dangling<T>(T input) { return &input; }
)"},
      {"constructor-local", R"(
class Address {
public:
  mut const int* pointer = nullptr;
  Address() { int local = 1; this.pointer = &local; }
};
int main() { Address address{}; unsafe { return *address.pointer; } }
)"},
      {"generic-constructor-local", R"(
class Address<T> {
public:
  mut const T* pointer = nullptr;
  Address(T input) { this.pointer = &input; }
};
int main() { Address<int> address{1}; unsafe { return *address.pointer; } }
)"},
      {"constructor-default-field", R"(
const int* dangling() { int local = 1; return &local; }
class Address {
public:
  const int* pointer = dangling();
  Address() {}
};
int main() { Address address{}; unsafe { return *address.pointer; } }
)"},
      {"generated-constructor-default-field", R"(
const int* dangling() { int local = 1; return &local; }
struct Address { const int* pointer = dangling(); };
int main() { Address address{}; unsafe { return *address.pointer; } }
)"},
      {"cached-global-pointer-input", R"(
int global = 1;
mut const int* address = &global;
const int* source() { return address; }
int main() {
  auto first = source();
  { int gone = 7; address = &gone; }
  auto second = source();
  unsafe { return *second; }
}
)"},
      {"default-argument-produced-pointer", R"(
const int* dangling() { int local = 1; return &local; }
const int* source(const int* pointer = dangling()) { return pointer; }
int main() { auto pointer = source(); unsafe { return *pointer; } }
)"},
      {"cached-global-field-input", R"(
int global = 1;
struct Address { mut const int* pointer = nullptr; };
mut Address address{.pointer = &global};
const int* source() { return address.pointer; }
int main() {
  auto first = source();
  { int gone = 7; address.pointer = &gone; }
  auto second = source();
  unsafe { return *second; }
}
)"},
      {"mixed-native-result", R"(
extern "C" { const int32_t* foreign(); }
int main() { auto pointer = source(false); unsafe { return *pointer; } }
const int32_t* source(bool choose) {
  int32_t local = 1;
  if (choose) { unsafe { return foreign(); } }
  return &local;
}
)"},
      {"returned-global-alias-write", R"(
struct Address { mut const int* pointer = nullptr; };
mut Address address{};
int main() {
  Address* alias = destination();
  unsafe { { int gone = 5; alias->pointer = &gone; } return *address.pointer; }
}
Address* destination() { return &address; }
)"}};
  for (const auto &[name, source] : expired) {
    const auto result =
        auditFrontend().analyze("raw-produced-" + name + ".gti", source);
    if (result.canGenerateCode() || !hasMessage(result, "scope has ended"))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << name << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    expect(!result.canGenerateCode() && hasMessage(result, "scope has ended"),
           "source result must preserve callee-produced expired origins: " +
               name);
  }
  const std::vector<std::pair<std::string, std::string>> valid = {
      {"inert-expired-copy", R"(
int main() { auto first = dangling(); auto second = first; return first == second ? 0 : 1; }
const int* dangling() { int local = 1; return &local; }
)"},
      {"expired-reseat", R"(
int main() { int live = 7; mut const int* pointer = dangling(); pointer = &live;
  unsafe { return *pointer; } }
const int* dangling() { int local = 1; return &local; }
)"},
      {"live-global", R"(
int global = 7;
int main() { auto pointer = source(); unsafe { return *pointer; } }
const int* source() { return &global; }
)"},
      {"cached-global-pointer-reseat", R"(
int global = 1;
mut const int* address = nullptr;
const int* source() { return address; }
int main() {
  { int gone = 7; address = &gone; }
  auto first = source();
  address = &global;
  auto second = source();
  unsafe { return *second; }
}
)"},
      {"global-ended-loan", R"(
mut int global = 1;
int* source() { return &global; }
int main() { auto pointer = source();
  { int& checked = global; if (checked != 1) { return 2; } }
  unsafe { *pointer = 3; } return global; }
)"},
      {"constructor-reference-input", R"(
class Address {
public:
  const int* pointer = nullptr;
  Address(int& source) : pointer(&source) {}
};
int main() { int live = 7; Address address{live}; unsafe { return *address.pointer; } }
)"},
      {"concrete-input-instances", R"(
int main() { int first = 7; double second = 2.0;
  const int* left = source<int>(first); const double* right = source<double>(second);
  unsafe { return *left + int(*right); } }
const T* source<T>(T& input) { return &input; }
)"},
      {"separate-result-fields", R"(
int global = 7;
struct Pair { const int* live = nullptr; const int* dead = nullptr; };
int main() { auto pair = source(); unsafe { return *pair.live; } }
Pair source() { int local = 1; return {.live = &global, .dead = &local}; }
)"},
      {"recursive-inert-copy", R"(
int main() { auto first = recursive(false); auto second = first; return first == second ? 0 : 1; }
const int* recursive(bool again) { if (again) { return recursive(false); } int local = 1; return &local; }
)"},
      {"lambda-inert-copy", R"(
int main() { auto source = []() -> const int* { int gone = 1; return &gone; };
  auto first = source(); auto second = first; return first == second ? 0 : 1; }
)"}};
  for (const auto &[name, source] : valid) {
    const auto result =
        auditFrontend().analyze("raw-produced-" + name + ".gti", source);
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << name << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    expect(result.canGenerateCode(),
           "valid callee-produced raw transport: " + name);
  }
  const auto recursive =
      auditFrontend().analyze("raw-produced-recursive-use.gti", R"(
int global = 7;
int main() { auto pointer = recursive(false); unsafe { return *pointer; } }
const int* recursive(bool again) { if (again) { return recursive(false); } return &global; }
)");
  expect(!recursive.canGenerateCode() &&
             hasMessage(recursive, "completed source-result"),
         "an unresolved recursive source result must not become a foreign "
         "assertion");
  for (
      const auto &source : {
          R"(int main() { auto source = []() -> const int* { int gone = 1; return &gone; };
  auto pointer = source(); unsafe { return *pointer; } })",
          R"(struct Address { const int* pointer = nullptr; };
int main() { auto source = []() -> Address { int gone = 1; return {.pointer = &gone}; };
  auto value = source(); unsafe { return *value.pointer; } })",
          R"(struct Address { mut const int* pointer = nullptr; };
class Carrier { public: Address& value; Carrier(Address& source) : value(source) {} };
const int* source(Carrier& input) { return input.value.pointer; }
int main() { mut Address address{}; { int gone = 1; address.pointer = &gone; }
  Carrier carrier{address}; auto pointer = source(carrier); unsafe { return *pointer; } })"}) {
    const auto unresolved =
        auditFrontend().analyze("raw-produced-unresolved.gti", source);
    if (unresolved.canGenerateCode() ||
        !hasMessage(unresolved, "completed source-result"))
      for (const auto &diagnostic : unresolved.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(!unresolved.canGenerateCode() &&
               hasMessage(unresolved, "completed source-result"),
           "unresolved lambda and borrowed-storage source outputs must not "
           "become foreign pointers");
  }
  const auto conflict =
      auditFrontend().analyze("raw-produced-global-loan.gti", R"(
mut int global = 7;
int main() { auto pointer = source(); int& checked = global;
  unsafe { *pointer = 9; } return checked; }
int* source() { return &global; }
)");
  expect(!conflict.canGenerateCode() &&
             hasMessage(conflict, "live incompatible checked borrow"),
         "callee-produced global origins must enter normal caller loan checks");
  const auto replaced =
      auditFrontend().analyze("raw-produced-replaced-input.gti", R"(
struct Box { mut int value = 1; };
int main() { mut Box box{}; auto pointer = source(box); unsafe { return *pointer; } }
const int* source(mut Box& box) {
  const int* pointer = &box.value;
  box = Box();
  return pointer;
}
)");
  if (replaced.canGenerateCode() ||
      !hasMessage(replaced, "lifetime was replaced"))
    for (const auto &diagnostic : replaced.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(!replaced.canGenerateCode() &&
             hasMessage(replaced, "lifetime was replaced"),
         "a returned formal origin must retain callee replacement facts");
}

void testRawArithmeticLifetime() {
  for (const auto &operation : {"auto advanced = pointer + 1;", "pointer++;",
                                "++pointer;", "pointer += 1;"}) {
    const auto result = auditFrontend().analyze(
        "raw-expired-arithmetic.gti",
        std::string("int main() { mut const int* pointer = nullptr; { int gone "
                    "= 3; pointer = &gone; } unsafe { ") +
            operation + " } return 0; }");
    expect(!result.canGenerateCode() && hasMessage(result, "scope has ended"),
           "raw arithmetic must not erase known allocation lifetime failure");
  }
  const auto liveBorrow =
      auditFrontend().analyze("raw-arithmetic-no-loan.gti", R"(
int main() {
  mut int values[2] = {1, 2};
  int* pointer = &values[0];
  mut int& exclusive = values[0];
  unsafe { auto offset = pointer + 1; }
  exclusive = 3;
  return exclusive;
})");
  expect(liveBorrow.canGenerateCode(),
         "pointer arithmetic must not become a pointee read or conflict with a "
         "checked loan");
}

void testSafeRawAddressFormation() {
  const auto valid = lang::Frontend().analyze("safe-raw-address.gti", R"(
struct Value { mut int field = 2; };
int main() {
  mut int scalar = 1;
  mut Value object{};
  mut int array[2] = {3, 4};
  int* first = &scalar;
  int* second = &object.field;
  int* third = &array[1];
  const int* qualified = first;
  mut const int* expired = nullptr;
  { int gone = 5; expired = &gone; }
  auto inert = expired;
  bool same = inert == expired;
  { int& checked = scalar; if (checked != 1) { return 1; } }
  unsafe { *first = 2; }
  return same && qualified != nullptr && second != third ? 0 : 1;
}
)");
  expect(valid.canGenerateCode(),
         "address formation and inert expired copies/comparisons must be safe "
         "and nonowning");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  const std::vector<std::string> invalid = {
      "int* projection(int* pointer) { return &*pointer; }",
      "int* projection(int* pointer) { return &pointer[0]; }",
      "struct Value { mut int field = 1; }; int* projection(Value* pointer) { "
      "return &pointer->field; }"};
  for (const auto &program : invalid) {
    const auto result =
        lang::Frontend().analyze("unsafe-address-projection.gti",
                                 program + "\nint main() { return 0; }");
    expect(!result.canGenerateCode() && hasCode(result, "GTI-S2055"),
           "safe address-of must not authorize an underlying raw projection");
  }
}
void testRawMutationCallInvalidation() {
  for (const std::string setup :
       {"mut Owner local{}; pointer = &local.value; replace(local);",
        "pointer = &owner.value; replace(owner);"}) {
    const auto global = auditFrontend().analyze("raw-global-mutation.gti", R"(
struct Owner { mut int value = 1; };
mut Owner owner{};
mut const int* pointer = nullptr;
void replace(mut Owner& value) { value = Owner(); }
int main() {
)" + setup + R"(
  unsafe { return *pointer; }
}
)");
    if (global.canGenerateCode() ||
        !hasMessage(global, "lifetime was replaced"))
      for (const auto &diagnostic : global.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(!global.canGenerateCode() &&
               hasMessage(global, "lifetime was replaced"),
           "active global pointer carriers retain mutation invalidation");
  }
  const auto expiredGlobal =
      auditFrontend().analyze("raw-global-expiry.gti", R"(
mut const int* pointer = nullptr;
int main() {
  { int local = 1; pointer = &local; }
  unsafe { return *pointer; }
}
)");
  expect(!expiredGlobal.canGenerateCode() &&
             hasMessage(expiredGlobal, "scope has ended"),
         "active global pointer carriers retain lexical expiry");
  const std::vector<std::string> mutations = {
      "replace(owner);", "owner.replace();",
      "[[discard]] owner.borrow_after_replace();"};
  for (const auto &mutation : mutations) {
    const std::string source = R"(
struct Owner {
  mut int value = 1;
  void replace() mut { this.value = 2; }
  mut int& borrow_after_replace() mut { this.value = 3; return this.value; }
};
void replace(mut Owner& owner) { owner = Owner(); }
int main() {
  mut Owner owner{};
  const int* address = &owner.value;
)" + mutation + R"(
  unsafe { return *address; }
})";
    const auto result =
        auditFrontend().analyze("raw-mutation-call.gti", source);
    if (result.canGenerateCode() ||
        !hasMessage(result, "lifetime was replaced"))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(
        !result.canGenerateCode() &&
            hasMessage(result, "lifetime was replaced"),
        "aggregate mutation calls conservatively invalidate old raw addresses");
  }
  const auto loop = auditFrontend().analyze("raw-mutation-backedge.gti", R"(
struct Owner { mut int value = 1; };
void replace(mut Owner& owner) { owner = Owner(); }
int main() {
  mut Owner owner{};
  mut const int* address = nullptr;
  for (mut int index = 0; index < 2; index++) {
    replace(owner);
    if (index > 0) { unsafe { int observed = *address; } }
    address = &owner.value;
  }
  return 0;
})");
  expect(
      !loop.canGenerateCode() && hasMessage(loop, "lifetime was replaced"),
      "loop substitution must apply mutation before use to backedge origins");

  const auto valid = auditFrontend().analyze("raw-mutation-reacquire.gti", R"(
struct Owner { mut int value = 1; };
void replace(mut Owner& owner) { owner = Owner(); }
void increment(mut int& value) { value++; }
int main() {
  mut int scalar = 1;
  const int* scalar_address = &scalar;
  increment(scalar);
  mut Owner owner{};
  const int* old = &owner.value;
  replace(owner);
  auto inert = old;
  bool equal = inert == old;
  const int* fresh = &owner.value;
  unsafe { return equal ? *fresh + *scalar_address : 0; }
})");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(
      valid.canGenerateCode(),
      "scalar mutation, fresh reacquisition and inert stale copies stay valid");

  const auto bridge = auditFrontend().analyze("raw-inert-source-bridge.gti", R"(
struct Owner { mut int value = 1; };
int main() {
  mut Owner owner{};
  int* address = &owner.value;
  unsafe {
    { mut int& value = borrow(address, owner); value = 2; }
    return *address;
  }
}
[[borrowed_from(owner)]]
unsafe mut int& borrow(int* address, mut Owner& owner) {
  unsafe { { return *((address)); } }
}
)");
  if (!bridge.canGenerateCode())
    for (const auto &diagnostic : bridge.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(bridge.canGenerateCode(),
         "direct assertion is lifetime-inert independent of declaration order");
  const auto cleanupBridge =
      auditFrontend().analyze("raw-source-bridge-cleanup.gti", R"(
struct Owner { mut int value = 1; };
struct Cleanup { ~Cleanup() {} };
[[borrowed_from(owner)]]
unsafe mut int& borrow(int* address, mut Owner& owner, Cleanup cleanup) {
  return *address;
}
int main() {
  mut Owner owner{};
  int* address = &owner.value;
  unsafe {
    { mut int& value = borrow(address, owner, Cleanup()); value = 2; }
    return *address;
  }
}
)");
  if (cleanupBridge.canGenerateCode() ||
      !hasMessage(cleanupBridge, "lifetime was replaced"))
    for (const auto &diagnostic : cleanupBridge.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(!cleanupBridge.canGenerateCode() &&
             hasMessage(cleanupBridge, "lifetime was replaced"),
         "value parameter cleanup prevents direct-assertion purity proof");

  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  for (const std::string mutation :
       {"slots.clear();", "slots.release();",
        "slots.destroy(std::size_t(0)); [[discard]] "
        "slots.emplace(std::size_t(0), 9);",
        "slots.relocate_to(other, std::size_t(1));"}) {
    const std::string source = R"(
#include <std/memory>
int main() {
  std::allocator allocator{};
  mut auto slots = allocator.try_storage<int>(std::size_t(1)).expect("allocate");
  mut auto other = allocator.try_storage<int>(std::size_t(1)).expect("allocate");
  [[discard]] slots.emplace(std::size_t(0), 7);
  const int* address = &slots.get(std::size_t(0));
)" + mutation + R"(
  unsafe { return *address; }
})";
    const auto result = auditFrontend().analyze(
        root / "raw-storage-mutation.gti", source,
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    if (result.canGenerateCode() ||
        !hasMessage(result, "lifetime was replaced"))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(!result.canGenerateCode() &&
               hasMessage(result, "lifetime was replaced"),
           "storage lifetime mutation must invalidate raw addresses across "
           "source wrappers");
  }
}
} // namespace

int main() {
  lang::installCrashHandlers("gti_raw_pointer_tests");
  testRawPointerAstPrinting();
  testNestedPointerTypes();
  testRawPointerPipeline();
  testUnsafeAndPointerDiagnostics();
  testRawPointerConstructorRanking();
  testPointerBearingCAbi();
  testOptionalRawAuditOff();
  testKnownRawPointerOrigins();
  testProjectedRawPointerOrigins();
  testRawPointerLoopOrigins();
  testRawPointerCallTransport();
  testRawProducedCallOrigins();
  testRawArithmeticLifetime();
  testSafeRawAddressFormation();
  testRawMutationCallInvalidation();
  if (failures != 0) {
    std::cerr << failures << " raw pointer test(s) failed\n";
    return 1;
  }
  std::cout << "raw pointer tests passed\n";
  return 0;
}
