#include "gti/lowered_program_printer.h"

#include "gti/lowered_program.h"
#include "gti/mir_printer.h"

#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace lang {
namespace {

[[nodiscard]] std::string_view bodyKindName(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
    return "module";
  case MirBodyKind::FieldInitializers:
    return "field-initializers";
  case MirBodyKind::StaticFieldInitializers:
    return "static-field-initializers";
  case MirBodyKind::Function:
    return "function";
  case MirBodyKind::Constructor:
    return "constructor";
  case MirBodyKind::Destructor:
    return "destructor";
  case MirBodyKind::Lambda:
    return "lambda";
  case MirBodyKind::HostedStartup:
    return "hosted-startup";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
bodyDefinitionName(LoweredBodyDefinitionKind kind) {
  switch (kind) {
  case LoweredBodyDefinitionKind::ImplicitSource:
    return "implicit-source";
  case LoweredBodyDefinitionKind::Source:
    return "source";
  case LoweredBodyDefinitionKind::CompilerGenerated:
    return "compiler-generated";
  case LoweredBodyDefinitionKind::RuntimeBinding:
    return "runtime-binding";
  case LoweredBodyDefinitionKind::Declaration:
    return "declaration";
  case LoweredBodyDefinitionKind::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] std::string_view bodyRoleName(LoweredBodyRole role) {
  switch (role) {
  case LoweredBodyRole::SourceExecutable:
    return "source-executable";
  case LoweredBodyRole::AbiDeclaration:
    return "abi-declaration";
  case LoweredBodyRole::DataOnly:
    return "data-only";
  case LoweredBodyRole::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
declarationKindName(LoweredDeclarationKind kind) {
  switch (kind) {
  case LoweredDeclarationKind::Namespace:
    return "namespace";
  case LoweredDeclarationKind::NamespaceAlias:
    return "namespace-alias";
  case LoweredDeclarationKind::TypeAlias:
    return "type-alias";
  case LoweredDeclarationKind::Class:
    return "class";
  case LoweredDeclarationKind::Enum:
    return "enum";
  case LoweredDeclarationKind::Function:
    return "function";
  case LoweredDeclarationKind::Constructor:
    return "constructor";
  case LoweredDeclarationKind::Destructor:
    return "destructor";
  case LoweredDeclarationKind::Storage:
    return "storage";
  case LoweredDeclarationKind::Access:
    return "access";
  case LoweredDeclarationKind::LanguageLinkage:
    return "language-linkage";
  case LoweredDeclarationKind::Concept:
    return "concept";
  case LoweredDeclarationKind::Empty:
    return "empty";
  case LoweredDeclarationKind::Other:
    return "other";
  case LoweredDeclarationKind::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
generatedItemKindName(LoweredGeneratedItemKind kind) {
  switch (kind) {
  case LoweredGeneratedItemKind::ProgramInitialization:
    return "program-initialization";
  case LoweredGeneratedItemKind::HostedEntry:
    return "hosted-entry";
  case LoweredGeneratedItemKind::StructuralOperatorAdapter:
    return "structural-operator-adapter";
  case LoweredGeneratedItemKind::CallableAdapter:
    return "callable-adapter";
  case LoweredGeneratedItemKind::LifecycleCleanup:
    return "lifecycle-cleanup";
  case LoweredGeneratedItemKind::NativeInteropAdapter:
    return "native-interop-adapter";
  case LoweredGeneratedItemKind::ConcreteInstanceAdapter:
    return "concrete-instance-adapter";
  case LoweredGeneratedItemKind::Count:
    return "invalid";
  }
  return "invalid";
}

void printGeneratedIdentity(std::ostream &output,
                            const LoweredGeneratedItemIdentity &identity) {
  output << generatedItemKindName(identity.kind) << '/' << identity.owner << '/'
         << identity.ordinal;
}

class DeclarationPayloadPrinter final {
public:
  explicit DeclarationPayloadPrinter(std::ostream &output) : output(output) {}

  void print(const LoweredDeclarationPayload &payload) {
    std::visit([this](const auto &value) { printValue(value); }, payload);
  }

  void semanticType(const SemanticType &value) { type(value); }

  void semanticTypes(const std::vector<SemanticType> &values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      type(values[index]);
    }
    output << ']';
  }

  void compileTimeValues(const std::vector<CompileTimeValue> &values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      compileTimeValue(values[index]);
    }
    output << ']';
  }

  void semanticTraits(const SemanticTypeTraits &value) { traits(value); }

  void sourceSpan(const SourceSpan &value) { span(value); }

  void loweredParameters(const std::vector<LoweredParameter> &values) {
    parameters(values);
  }

private:
  template <typename Enum>
  [[nodiscard]] static constexpr std::size_t number(Enum value) {
    return static_cast<std::size_t>(value);
  }

  void separator(std::size_t index) {
    if (index != 0) {
      output << ',';
    }
  }

  void strings(const std::vector<std::string> &values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      output << std::quoted(values[index]);
    }
    output << ']';
  }

  void compileTimeValue(const CompileTimeValue &value) {
    output << '{' << number(value.kind) << ':' << value.value << ':'
           << value.parameterId << '}';
  }

  void type(const SemanticType &value) {
    output << "type(" << number(value.kind) << ";class=" << value.classId
           << ";enum=" << value.enumId
           << ";parameter=" << value.genericParameterId
           << ";lambda=" << value.lambdaId
           << ";lambda-parameters=" << value.lambdaParameterCount
           << ";lambda-captures=" << value.lambdaCaptureCount
           << ";length=" << value.arrayLength
           << ";length-parameter=" << value.arrayLengthParameterId
           << ";access=" << number(value.referenceAccess)
           << ";pointer-access=" << number(value.pointerAccess)
           << ";concrete-pack=" << value.concretePack << ";args=[";
    for (std::size_t index = 0; index < value.arguments.size(); ++index) {
      separator(index);
      type(value.arguments[index]);
    }
    output << "];values=[";
    for (std::size_t index = 0; index < value.valueArguments.size(); ++index) {
      separator(index);
      compileTimeValue(value.valueArguments[index]);
    }
    output << "];lambda-class-types=[";
    for (std::size_t index = 0; index < value.lambdaEnclosingClassTypes.size();
         ++index) {
      separator(index);
      type(value.lambdaEnclosingClassTypes[index]);
    }
    output << "];lambda-function-types=[";
    for (std::size_t index = 0;
         index < value.lambdaEnclosingFunctionTypes.size(); ++index) {
      separator(index);
      type(value.lambdaEnclosingFunctionTypes[index]);
    }
    output << "];lambda-class-values=[";
    for (std::size_t index = 0; index < value.lambdaEnclosingClassValues.size();
         ++index) {
      separator(index);
      compileTimeValue(value.lambdaEnclosingClassValues[index]);
    }
    output << "];lambda-function-values=[";
    for (std::size_t index = 0;
         index < value.lambdaEnclosingFunctionValues.size(); ++index) {
      separator(index);
      compileTimeValue(value.lambdaEnclosingFunctionValues[index]);
    }
    output << "])";
  }

  void traits(const SemanticTypeTraits &value) {
    output << "traits(" << number(value.ownership) << ',' << number(value.drop)
           << ',' << value.copyable << ',' << value.movable << ','
           << value.copyAssignable << ',' << value.moveAssignable << ','
           << value.containsBorrowedState << ',' << value.transferCapable << ','
           << value.shareCapable << ')';
  }

  void bytes(const std::string &value) {
    output << value.size() << ':' << std::hex << std::setfill('0');
    for (const unsigned char byte : value) {
      output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    output << std::dec << std::setfill(' ');
  }

  void literal(const Literal &value) {
    std::visit(
        [this](const auto &literalValue) {
          using Value = std::decay_t<decltype(literalValue)>;
          if constexpr (std::is_same_v<Value, std::monostate>) {
            output << "unset";
          } else if constexpr (std::is_same_v<Value, std::nullptr_t>) {
            output << "null";
          } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
            output << "u64:" << literalValue;
          } else if constexpr (std::is_same_v<Value, BinaryFloat>) {
            const bool binary64 =
                literalValue.format == BinaryFloatFormat::Binary64;
            output << (binary64 ? "f64:0x" : "f32:0x") << std::hex
                   << std::setw(binary64 ? 16 : 8) << std::setfill('0')
                   << literalValue.bits << std::dec << std::setfill(' ');
          } else if constexpr (std::is_same_v<Value, CharacterLiteral>) {
            output << "char:" << static_cast<unsigned int>(literalValue.value);
          } else if constexpr (std::is_same_v<Value, std::string>) {
            output << "string:";
            bytes(literalValue);
          } else if constexpr (std::is_same_v<Value, bool>) {
            output << "bool:" << literalValue;
          }
        },
        value);
  }

  void constant(const ConstantValue &value) {
    std::visit(
        [this](const auto &constantValue) {
          using Value = std::decay_t<decltype(constantValue)>;
          if constexpr (std::is_same_v<Value, ConstantInteger>) {
            output << "integer:" << constantValue.negative << ':'
                   << constantValue.magnitude << ':'
                   << static_cast<unsigned int>(constantValue.domain.width)
                   << ':' << constantValue.domain.signedValue;
          } else if constexpr (std::is_same_v<Value, NullConstant>) {
            output << "null";
          } else if constexpr (std::is_same_v<Value,
                                              ConstantCheckedIntegerResult>) {
            output << "checked:"
                   << static_cast<unsigned int>(constantValue.domain.width)
                   << ':' << constantValue.domain.signedValue << ':';
            if (constantValue.value) {
              output << constantValue.value->negative << ':'
                     << constantValue.value->magnitude;
            } else {
              output << '-';
            }
          } else {
            literal(Literal{constantValue});
          }
        },
        value);
  }

  void span(const SourceSpan &value) {
    output << std::quoted(value.source) << ':' << value.start << ':'
           << value.end << ':' << value.line;
  }

  void genericParameter(const LoweredGenericParameter &value) {
    output << "generic(id=" << value.id << ";name=" << std::quoted(value.name)
           << ";pack=" << value.pack << ";value=" << value.value
           << ";constraints=" << value.constraints << ";constraint=";
    strings(value.constraintName);
    output << ')';
  }

  void genericParameters(const std::vector<LoweredGenericParameter> &values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      genericParameter(values[index]);
    }
    output << ']';
  }

  void parameter(const LoweredParameter &value) {
    output << "parameter(symbol=" << value.symbol
           << ";name=" << std::quoted(value.name) << ";type=";
    type(value.type);
    output << ";access=" << number(value.access)
           << ";mutability=" << number(value.mutability)
           << ";pack=" << value.pack << ";default=" << value.hasDefault
           << ";source=";
    span(value.source);
    output << ')';
  }

  void parameters(const std::vector<LoweredParameter> &values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      parameter(values[index]);
    }
    output << ']';
  }

  void cAbiLayout(const std::optional<MirCAbiRecordLayout> &value) {
    if (!value) {
      output << '-';
      return;
    }
    output << "layout(size=" << value->sizeBytes
           << ";align=" << value->abiAlignmentBytes << ";fields=[";
    for (std::size_t index = 0; index < value->fields.size(); ++index) {
      separator(index);
      const MirCAbiRecordFieldLayout &field = value->fields[index];
      output << "{symbol=" << field.field << ";type=";
      type(field.type);
      output << ";offset=" << field.offsetBytes << ";size=" << field.sizeBytes
             << ";align=" << field.abiAlignmentBytes << '}';
    }
    output << "])";
  }

  void unionLayout(const std::optional<MirUnionLayout> &value) {
    if (!value) {
      output << '-';
      return;
    }
    output << "layout(size=" << value->sizeBytes
           << ";align=" << value->abiAlignmentBytes << ";fields=[";
    for (std::size_t index = 0; index < value->fields.size(); ++index) {
      separator(index);
      const MirUnionFieldLayout &field = value->fields[index];
      output << "{symbol=" << field.field << ";type=";
      type(field.type);
      output << ";size=" << field.sizeBytes
             << ";align=" << field.abiAlignmentBytes << '}';
    }
    output << "])";
  }

  void printValue(const std::monostate &) { output << "none"; }

  void printValue(const LoweredNamespaceAliasDeclaration &value) {
    output << "namespace-alias(target=";
    strings(value.target);
    output << ')';
  }

  void printValue(const LoweredTypeAliasDeclaration &value) {
    output << "type-alias(unit=" << value.sourceUnit
           << ";qualified=" << std::quoted(value.qualifiedName) << ";type=";
    type(value.type);
    output << ";private=" << value.compilerPrivate << ')';
  }

  void printValue(const LoweredClassDeclaration &value) {
    output << "class(id=" << value.id << ";unit=" << value.sourceUnit
           << ";qualified=" << std::quoted(value.qualifiedName)
           << ";kind=" << number(value.kind) << ";generics=";
    genericParameters(value.genericParameters);
    output << ";bases=[";
    for (std::size_t index = 0; index < value.bases.size(); ++index) {
      separator(index);
      output << "{type=";
      type(value.bases[index].type);
      output << ";access=" << number(value.bases[index].access)
             << ";interface=" << value.bases[index].interface << '}';
    }
    output << "];";
    traits(value.traits);
    output << ";transfer=" << number(value.transferPolicy)
           << ";share=" << number(value.sharePolicy)
           << ";special=" << number(value.defaultConstructor) << ':'
           << number(value.copyConstructor) << ':'
           << number(value.moveConstructor) << ':'
           << number(value.copyAssignment) << ':'
           << number(value.moveAssignment) << ':' << number(value.destructor)
           << ";c-abi-layout=";
    cAbiLayout(value.cAbiLayout);
    output << ";union-layout=";
    unionLayout(value.unionLayout);
    output << ";capability=" << number(value.compilerCapability)
           << ";forward=" << value.forwardDeclaration
           << ";abstract=" << value.abstract
           << ";polymorphic=" << value.polymorphic
           << ";c-abi=" << value.cAbiRecord << ";opaque=" << value.cOpaqueHandle
           << ";active-drop=" << value.requiresActiveDropState
           << ";private=" << value.compilerPrivate << ')';
  }

  void printValue(const LoweredEnumDeclaration &value) {
    output << "enum(id=" << value.id << ";unit=" << value.sourceUnit
           << ";qualified=" << std::quoted(value.qualifiedName)
           << ";underlying=";
    type(value.underlyingType);
    output << ";payload=" << value.payload
           << ";private=" << value.compilerPrivate << ";values=[";
    for (std::size_t index = 0; index < value.enumerators.size(); ++index) {
      separator(index);
      const LoweredEnumerator &enumerator = value.enumerators[index];
      output << "{name=" << std::quoted(enumerator.name)
             << ";value=" << enumerator.value.negative << ':'
             << enumerator.value.magnitude
             << ";variant=" << enumerator.variantIndex << ";payload=[";
      for (std::size_t payload = 0; payload < enumerator.payloadTypes.size();
           ++payload) {
        separator(payload);
        type(enumerator.payloadTypes[payload]);
      }
      output << "];source=";
      span(enumerator.source);
      output << '}';
    }
    output << "])";
  }

  void printValue(const LoweredFunctionDeclaration &value) {
    output << "function(id=" << value.id << ";unit=" << value.sourceUnit
           << ";owner=" << value.ownerClass
           << ";qualified=" << std::quoted(value.qualifiedName) << ";return=";
    type(value.returnType);
    output << ";parameters=";
    parameters(value.parameters);
    output << ";generics=";
    genericParameters(value.genericParameters);
    output << ";requirements=[";
    for (std::size_t index = 0; index < value.requirements.size(); ++index) {
      separator(index);
      output << "{concept=" << value.requirements[index].conceptId
             << ";arguments=[";
      for (std::size_t argument = 0;
           argument < value.requirements[index].arguments.size(); ++argument) {
        separator(argument);
        type(value.requirements[index].arguments[argument]);
      }
      output << "]}";
    }
    output << "];callables=[";
    for (std::size_t index = 0; index < value.callableParameters.size();
         ++index) {
      separator(index);
      const LoweredCallableParameter &callable =
          value.callableParameters[index];
      output << "{index=" << callable.parameterIndex
             << ";generic=" << callable.genericParameter << ";type=";
      type(callable.callableType);
      output << ";access=" << number(callable.access)
             << ";boundary=" << number(callable.boundary) << ";transport=";
      if (callable.ownedTransport) {
        output << number(callable.ownedTransport->kind) << ':';
        type(callable.ownedTransport->destinationType);
        output << ':' << callable.ownedTransport->field;
      } else {
        output << '-';
      }
      output << ";signatures=[";
      for (std::size_t signature = 0; signature < callable.signatures.size();
           ++signature) {
        separator(signature);
        output << "{return=";
        type(callable.signatures[signature].returnType);
        output << ";parameters=[";
        for (std::size_t parameterIndex = 0;
             parameterIndex <
             callable.signatures[signature].parameterTypes.size();
             ++parameterIndex) {
          separator(parameterIndex);
          type(callable.signatures[signature].parameterTypes[parameterIndex]);
        }
        output << "];capability="
               << number(callable.signatures[signature].capability) << '}';
      }
      output << "];forwardings=[";
      for (std::size_t forwarding = 0; forwarding < callable.forwardings.size();
           ++forwarding) {
        separator(forwarding);
        output << callable.forwardings[forwarding].function << ':'
               << callable.forwardings[forwarding].parameterIndex;
      }
      output << "]}";
    }
    output << "];required=" << value.requiredParameterCount
           << ";entry=" << number(value.entryKind)
           << ";append=" << value.entryArgumentAppendFunction
           << ";receiver=" << number(value.receiverMutability)
           << ";return-mutability=" << number(value.returnMutability)
           << ";operator=";
    if (value.overloadedOperator) {
      output << number(*value.overloadedOperator);
    } else {
      output << '-';
    }
    output << ";definition=" << number(value.definitionKind)
           << ";linkage=" << number(value.linkage)
           << ";external=" << std::quoted(value.externalSymbol) << ";c-array=";
    if (value.cArrayCountParameter) {
      output << *value.cArrayCountParameter;
    } else {
      output << '-';
    }
    output << ";intrinsic=" << number(value.intrinsic)
           << ";borrow-origin=" << number(value.returnBorrowOrigin)
           << ";borrow-parameter=" << value.returnBorrowParameter
           << ";borrow-access=" << number(value.returnBorrowAccess)
           << ";borrow-place=";
    if (value.returnBorrowPlace) {
      output << value.returnBorrowPlace->root << '[';
      for (std::size_t index = 0;
           index < value.returnBorrowPlace->projections.size(); ++index) {
        separator(index);
        const PlaceProjection &projection =
            value.returnBorrowPlace->projections[index];
        output << number(projection.kind) << ':' << projection.field << ':'
               << projection.index << ':' << projection.selection;
      }
      output << ']';
    } else {
      output << '-';
    }
    output << ";virtual-roots=[";
    for (std::size_t index = 0; index < value.virtualRoots.size(); ++index) {
      separator(index);
      output << value.virtualRoots[index];
    }
    output << "];flags=" << value.parameterPack << ':' << value.staticMember
           << ':' << value.internalLinkage << ':' << value.constexprFunction
           << ':' << value.virtualMethod << ':' << value.pureVirtual << ':'
           << value.overrideMethod << ':' << value.hasRequiresClause << ':'
           << value.compilerPrivate << ')';
  }

  void printValue(const LoweredConstructorDeclaration &value) {
    output << "constructor(id=" << value.id << ";owner=" << value.owner
           << ";kind=" << number(value.kind)
           << ";access=" << number(value.access) << ";generics=";
    genericParameters(value.genericParameters);
    output << ";parameters=";
    parameters(value.parameters);
    output << ";required=" << value.requiredParameterCount << ";borrow=";
    if (value.borrowParameter) {
      output << *value.borrowParameter;
    } else {
      output << '-';
    }
    output << ':' << number(value.borrowAccess) << ";specifier=";
    if (value.specifier) {
      output << number(*value.specifier);
    } else {
      output << '-';
    }
    output << ";private=" << value.compilerPrivate << ')';
  }

  void printValue(const LoweredDestructorDeclaration &value) {
    output << "destructor(owner=" << value.owner
           << ";access=" << number(value.access) << ')';
  }

  void printValue(const LoweredStorageDeclaration &value) {
    output << "storage(symbol=" << value.symbol << ";owner=" << value.ownerClass
           << ";type=";
    type(value.type);
    output << ";access=" << number(value.access) << ';';
    traits(value.traits);
    output << ";constant=";
    if (value.constant) {
      constant(*value.constant);
    } else {
      output << '-';
    }
    output << ";mutability=" << number(value.mutability)
           << ";flags=" << value.staticStorage << ':' << value.internalLinkage
           << ':' << value.constexprStorage << ':' << value.hasInitializer
           << ')';
  }

  void printValue(const LoweredAccessDeclaration &value) {
    output << "access(" << number(value.access) << ')';
  }

  void printValue(const LoweredLanguageLinkageDeclaration &value) {
    output << "linkage(" << number(value.linkage) << ')';
  }

  std::ostream &output;
};

} // namespace

std::string LoweredProgramPrinter::print(const LoweredProgram &program) const {
  std::ostringstream output;
  output << "lowered-program-v1\n";
  output << "target os=" << std::quoted(program.target().os)
         << " vendor=" << std::quoted(program.target().vendor)
         << " arch=" << std::quoted(program.target().arch) << " profile="
         << executionProfileName(program.target().executionProfile)
         << " endian="
         << targetEndiannessName(program.target().dataLayout.endianness())
         << " pointer-width=" << program.target().dataLayout.pointerWidthBits()
         << '\n';
  for (std::size_t index = 0; index < targetScalarKindCount; ++index) {
    const auto kind = static_cast<TargetScalarKind>(index);
    const std::optional<TargetTypeLayout> layout =
        program.target().dataLayout.scalarLayout(kind);
    output << "target-layout " << targetScalarKindName(kind);
    if (layout) {
      output << " size=" << layout->sizeBytes
             << " abi-align=" << layout->abiAlignmentBytes
             << " preferred-align=" << layout->preferredAlignmentBytes;
    } else {
      output << " unsupported";
    }
    output << '\n';
  }

  output << "bodies " << program.bodies().size() << '\n';
  for (const LoweredBody &body : program.bodies()) {
    const LoweredBodyIdentity &identity = body.identity;
    output << "body " << bodyKindName(identity.address.kind) << '/'
           << identity.address.owner
           << " domain=" << (identity.placeDomain.snapshot == 0 ? 0 : 1) << ':'
           << identity.placeDomain.body << ':' << identity.placeDomain.revision
           << " definition=" << bodyDefinitionName(identity.definition)
           << " role=" << bodyRoleName(body.role)
           << " declaration=" << identity.declaration
           << " concrete-owner=" << identity.concreteOwner
           << " source=" << std::quoted(identity.source.source) << ':'
           << identity.source.start << ':' << identity.source.end << ':'
           << identity.source.line
           << " roots=" << body.requiredGeneratedItems.size();
    for (const LoweredGeneratedItemIdentity &required :
         body.requiredGeneratedItems) {
      output << ' ';
      printGeneratedIdentity(output, required);
    }
    output << '\n';
  }

  output << "declarations " << program.declarations().size() << '\n';
  for (const LoweredDeclaration &declaration : program.declarations()) {
    output << "declaration " << declaration.id
           << " kind=" << declarationKindName(declaration.kind)
           << " parent=" << declaration.parent
           << " semantic=" << declaration.semanticIdentity
           << " owner-class=" << declaration.ownerClass
           << " ordinal=" << declaration.ordinal
           << " generic=" << (declaration.generic ? "true" : "false")
           << " name=" << std::quoted(declaration.name) << " namespace=";
    std::string separator;
    for (const std::string &scope : declaration.namespaceScope) {
      output << separator << scope;
      separator = "::";
    }
    output << " source=" << std::quoted(declaration.source.source) << ':'
           << declaration.source.start << ':' << declaration.source.end << ':'
           << declaration.source.line
           << " roots=" << declaration.requiredGeneratedItems.size();
    for (const LoweredGeneratedItemIdentity &required :
         declaration.requiredGeneratedItems) {
      output << ' ';
      printGeneratedIdentity(output, required);
    }
    output << " payload=";
    DeclarationPayloadPrinter(output).print(declaration.payload);
    output << '\n';
  }

  DeclarationPayloadPrinter values(output);
  output << "symbols " << program.symbols().size() << '\n';
  for (const LoweredSymbol &symbol : program.symbols()) {
    output << "symbol " << symbol.id
           << " kind=" << static_cast<std::size_t>(symbol.kind)
           << " name=" << std::quoted(symbol.name)
           << " qualified=" << std::quoted(symbol.qualifiedName)
           << " unit=" << symbol.sourceUnit << " name-source=";
    values.sourceSpan(symbol.nameSource);
    output << " declaration-source=";
    values.sourceSpan(symbol.declarationSource);
    output << " definition-source=";
    if (symbol.definitionSource) {
      values.sourceSpan(*symbol.definitionSource);
    } else {
      output << '-';
    }
    output << " type=";
    values.semanticType(symbol.type);
    output << ' ';
    values.semanticTraits(symbol.traits);
    output << " access=" << static_cast<std::size_t>(symbol.access)
           << " flags=" << symbol.mutableBinding << ':' << symbol.defaultLibrary
           << ':' << symbol.staticMember << ':' << symbol.internalLinkage << ':'
           << symbol.generated << ':' << symbol.compilerPrivate << '\n';
  }

  output << "class-instances " << program.classInstances().size() << '\n';
  for (const LoweredClassInstance &instance : program.classInstances()) {
    output << "class-instance " << instance.id
           << " unit=" << instance.sourceUnit
           << " declaration=" << instance.declaration << " types=";
    values.semanticTypes(instance.typeArguments);
    output << " values=";
    values.compileTimeValues(instance.valueArguments);
    output << " source=";
    values.sourceSpan(instance.source);
    output << '\n';
  }

  output << "function-instances " << program.functionInstances().size() << '\n';
  for (const LoweredFunctionInstance &instance : program.functionInstances()) {
    output << "function-instance " << instance.id
           << " unit=" << instance.sourceUnit
           << " declaration=" << instance.declaration << " owner=";
    if (instance.owner) {
      output << *instance.owner;
    } else {
      output << '-';
    }
    output << " types=";
    values.semanticTypes(instance.typeArguments);
    output << " values=";
    values.compileTimeValues(instance.valueArguments);
    output << " instantiation=";
    if (instance.instantiationSource) {
      values.sourceSpan(*instance.instantiationSource);
    } else {
      output << '-';
    }
    output << " source=";
    values.sourceSpan(instance.source);
    output << '\n';
  }

  output << "constructor-instances " << program.constructorInstances().size()
         << '\n';
  for (const LoweredConstructorInstance &instance :
       program.constructorInstances()) {
    output << "constructor-instance " << instance.id
           << " unit=" << instance.sourceUnit
           << " declaration=" << instance.declaration
           << " owner=" << instance.owner << " types=";
    values.semanticTypes(instance.typeArguments);
    output << " values=";
    values.compileTimeValues(instance.valueArguments);
    output << " instantiation=";
    if (instance.instantiationSource) {
      values.sourceSpan(*instance.instantiationSource);
    } else {
      output << '-';
    }
    output << " source=";
    values.sourceSpan(instance.source);
    output << '\n';
  }

  output << "destructor-instances " << program.destructorInstances().size()
         << '\n';
  for (const LoweredDestructorInstance &instance :
       program.destructorInstances()) {
    output << "destructor-instance " << instance.id
           << " unit=" << instance.sourceUnit << " owner=" << instance.owner
           << " source=";
    values.sourceSpan(instance.source);
    output << '\n';
  }

  output << "lambda-instances " << program.lambdaInstances().size() << '\n';
  for (const LoweredLambdaInstance &instance : program.lambdaInstances()) {
    output << "lambda-instance " << instance.id
           << " declaration=" << instance.declaration << " type=";
    values.semanticType(instance.type);
    output << " return=";
    values.semanticType(instance.returnType);
    output << " parameters=";
    values.loweredParameters(instance.parameters);
    output << " captures=[";
    for (std::size_t index = 0; index < instance.captures.size(); ++index) {
      if (index != 0) {
        output << ',';
      }
      const LoweredLambdaCapture &capture = instance.captures[index];
      output << "{symbol=" << capture.symbol
             << ";name=" << std::quoted(capture.name) << ";type=";
      values.semanticType(capture.type);
      output << ';';
      values.semanticTraits(capture.traits);
      output << ";mode=" << static_cast<std::size_t>(capture.mode)
             << ";cleanup=" << capture.requiresActiveCleanup << '}';
    }
    output << "] ";
    values.semanticTraits(instance.traits);
    output << " source=";
    values.sourceSpan(instance.source);
    output << '\n';
  }

  output << "generated-items " << program.generatedItems().size() << '\n';
  for (const LoweredGeneratedItem &item : program.generatedItems()) {
    output << "generated-item ";
    printGeneratedIdentity(output, item.identity);
    output << " source=";
    if (item.sourceKind == LoweredGeneratedItemSourceKind::Body) {
      output << "body:" << bodyKindName(item.sourceBody.kind) << '/'
             << item.sourceBody.owner;
    } else if (item.sourceKind == LoweredGeneratedItemSourceKind::Declaration) {
      output << "declaration:" << item.sourceDeclaration;
    } else {
      output << "invalid";
    }
    output << " dependencies=" << item.dependencies.size();
    for (const LoweredGeneratedItemIdentity &dependency : item.dependencies) {
      output << ' ';
      printGeneratedIdentity(output, dependency);
    }
    if (const auto *adapter =
            std::get_if<LoweredStructuralOperatorAdapterItem>(&item.payload)) {
      output << " payload=structural-operator:" << adapter->function << ':'
             << static_cast<std::size_t>(adapter->operation);
    } else if (const auto *adapter =
                   std::get_if<LoweredCallableAdapterItem>(&item.payload)) {
      output << " payload=callable:" << adapter->function << ':'
             << static_cast<std::size_t>(adapter->capability);
    } else if (const auto *cleanup =
                   std::get_if<LoweredLifecycleCleanupItem>(&item.payload)) {
      output << " payload=lifecycle-cleanup:" << cleanup->ownerClass << ':'
             << cleanup->classInstance << ':' << cleanup->destructorInstance
             << ':' << static_cast<std::size_t>(cleanup->form) << ':'
             << (cleanup->mayRaiseDefinedFailure ? "raise" : "no-raise");
    } else if (const auto *concrete =
                   std::get_if<LoweredConcreteInstanceAdapterItem>(
                       &item.payload)) {
      output << " payload=concrete-instance:"
             << static_cast<std::size_t>(concrete->kind) << ':'
             << bodyKindName(concrete->body.kind) << '/' << concrete->body.owner
             << ':' << concrete->declaration << ':'
             << concrete->ownerClassInstance << ':'
             << (concrete->mayRaiseDefinedFailure ? "raise" : "no-raise");
    } else if (const auto *callback =
                   std::get_if<LoweredNativeCallbackItem>(&item.payload)) {
      output << " payload=native-callback:" << callback->adapter.id << ':'
             << callback->adapter.target << ':'
             << (callback->adapter.targetMayRaiseDefinedFailure ? "raise"
                                                                : "no-raise")
             << ':'
             << (callback->adapter.catchesNativeExceptions ? "catch" : "leak");
    } else {
      output << " payload=none";
    }
    output << '\n';
  }

  output << "mir\n" << MirPrinter().print(program.mir());
  return output.str();
}

} // namespace lang
