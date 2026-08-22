#include "gti/ast_printer.h"
#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/optimization/effects.h"
#include "gti/support.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

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
                         return value.unsafeOperation ==
                                lang::UnsafeOperationKind::AddressOf;
                       }),
           "HIR should mark raw address formation");
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
            instruction.receiver && instruction.receiver->place != 0) {
          const lang::MirPlace *receiver =
              body->findPlace(instruction.receiver->place);
          rawMethodReceiver |=
              receiver != nullptr &&
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
             emittedMain.find(".get().pointer = __gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find(").pointer = __gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find("[static_cast<std::size_t>(0)] = ") !=
                 std::string_view::npos,
         "raw-pointer compound field and fixed-array element targets should "
         "use ordered native pointer arithmetic after any required bounds "
         "check");
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
         "raw address and dereference should require unsafe");

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

  const lang::FrontendResult aliasDepth =
      lang::Frontend().analyze("nested-raw-pointer-alias.gti", R"(
using Pointer = int32_t*;
int main() { Pointer* nested = nullptr; return 0; }
)");
  expect(!aliasDepth.canGenerateCode() && hasCode(aliasDepth, "GTI-S2056"),
         "aliases should not bypass the one-level pointer rule");

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

  const lang::FrontendResult referencePointer =
      lang::Frontend().analyze("raw-pointer-reference.gti", R"(
int main() {
  int32_t* pointer = nullptr;
  int32_t*& alias = pointer;
  return 0;
}
)");
  expect(!referencePointer.canGenerateCode() &&
             hasMessage(referencePointer, "raw pointers"),
         "checked references to raw pointers should remain out of scope");

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
         "array-to-pointer mismatches should explain the explicit unsafe "
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

} // namespace

int main() {
  lang::installCrashHandlers("gti_raw_pointer_tests");
  testRawPointerAstPrinting();
  testRawPointerPipeline();
  testUnsafeAndPointerDiagnostics();
  testRawPointerConstructorRanking();
  testPointerBearingCAbi();
  if (failures != 0) {
    std::cerr << failures << " raw pointer test(s) failed\n";
    return 1;
  }
  std::cout << "raw pointer tests passed\n";
  return 0;
}
