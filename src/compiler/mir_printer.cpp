#include "gti/mir_printer.h"

#include "gti/optimization/effects.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

namespace lang {
namespace {

template <typename Enum> [[nodiscard]] constexpr auto number(Enum value) {
  return static_cast<std::underlying_type_t<Enum>>(value);
}

class Printer {
public:
  [[nodiscard]] std::string print(const MirProgram &program) {
    output << "mir-v4 valid=" << program.valid() << '\n';
    output << "module\n";
    body(program.module(), 0);

    for (const MirClassInstance &instance : program.classInstances()) {
      output << "class @" << instance.id
             << " declaration=" << instance.declaration << " type=";
      type(instance.type);
      output << " kind=" << number(instance.kind)
             << " abstract=" << instance.abstract
             << " polymorphic=" << instance.polymorphic
             << " c-abi=" << instance.cAbiRecord;
      if (instance.cAbiLayout) {
        output << " layout={size=" << instance.cAbiLayout->sizeBytes
               << ",align=" << instance.cAbiLayout->abiAlignmentBytes
               << ",fields=[";
        for (std::size_t index = 0; index < instance.cAbiLayout->fields.size();
             ++index) {
          separator(index);
          const CAbiRecordFieldLayout &field =
              instance.cAbiLayout->fields[index];
          output << "{name="
                 << (field.declaration == nullptr
                         ? std::string_view{"?"}
                         : std::string_view{field.declaration->name().lexeme})
                 << ",type=";
          type(field.type);
          output << ",offset=" << field.offsetBytes
                 << ",size=" << field.sizeBytes
                 << ",align=" << field.abiAlignmentBytes << '}';
        }
        output << "]}";
      }
      output << " destructor=";
      optional(instance.destructor);
      output << " active-drop=" << instance.requiresActiveDropState
             << " active-cleanup=" << instance.requiresActiveCleanup
             << " bases=[";
      for (std::size_t index = 0; index < instance.bases.size(); ++index) {
        separator(index);
        const HirBaseInstance &base = instance.bases[index];
        output << "{instance=" << base.instance << ",type=";
        type(base.type);
        output << ",interface=" << base.interface << '}';
      }
      output << "] structural-bases=[";
      for (std::size_t index = 0; index < instance.structuralBases.size();
           ++index) {
        separator(index);
        const HirBaseInstance &base = instance.structuralBases[index];
        output << "{instance=" << base.instance << ",type=";
        type(base.type);
        output << ",interface=" << base.interface << '}';
      }
      output << "] lifecycle-fields=[";
      for (std::size_t index = 0; index < instance.fields.size(); ++index) {
        separator(index);
        const MirClassFieldLifecycle &field = instance.fields[index];
        output << "{field=" << field.field << ",symbol=" << field.symbol
               << ",type=";
        type(field.type);
        output << ",drop=" << number(field.dropKind)
               << ",active-cleanup=" << field.requiresActiveCleanup << '}';
      }
      output << "] drops=[";
      for (std::size_t index = 0; index < instance.fieldDropOrder.size();
           ++index) {
        separator(index);
        const MirFieldDrop &drop = instance.fieldDropOrder[index];
        output << "{field=" << drop.field << ",symbol=" << drop.symbol
               << ",type=";
        type(drop.type);
        output << ",active-cleanup=" << drop.requiresActiveCleanup << '}';
      }
      output << "]\nclass-fields\n";
      body(instance.fieldInitializers, instance.id);
      output << "class-static-fields\n";
      body(instance.staticFieldInitializers, instance.id);
    }

    for (const MirFunctionInstance &instance : program.functionInstances()) {
      output << "function @" << instance.id << " owner=";
      optional(instance.owner);
      output << " static=" << instance.staticMember
             << " entry=" << number(instance.entryKind) << ':'
             << (instance.entryArgumentAppendTarget
                     ? *instance.entryArgumentAppendTarget
                     : 0)
             << " return=";
      type(instance.returnType);
      output << " parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        output << "{binding="
               << (index < instance.parameterBindings.size()
                       ? instance.parameterBindings[index]
                       : 0)
               << ",type=";
        type(instance.parameterTypes[index]);
        output << '}';
      }
      output << "] return-borrow=" << number(instance.returnBorrowOrigin) << ':'
             << instance.returnBorrowParameter << ':'
             << number(instance.returnBorrowAccess) << " linkage="
             << (instance.linkage == LanguageLinkage::C ? "c" : "gti")
             << " symbol=" << instance.externalSymbol
             << " virtual=" << instance.virtualMethod
             << " pure=" << instance.pureVirtual
             << " override=" << instance.overrideMethod << " roots=[";
      list(instance.virtualRoots);
      output << "] callables=[";
      for (std::size_t index = 0; index < instance.callableParameters.size();
           ++index) {
        separator(index);
        callableParameter(instance.callableParameters[index]);
      }
      output << "]\n";
      body(instance.body, instance.id);
    }

    for (const MirConstructorInstance &instance :
         program.constructorInstances()) {
      output << "constructor @" << instance.id << " owner=" << instance.owner
             << " parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        output << "{binding="
               << (index < instance.parameterBindings.size()
                       ? instance.parameterBindings[index]
                       : 0)
               << ",type=";
        type(instance.parameterTypes[index]);
        output << '}';
      }
      output << "] borrow=" << number(instance.borrowOrigin) << ':'
             << instance.borrowParameter << ':' << number(instance.borrowAccess)
             << " initializers=[";
      for (std::size_t index = 0; index < instance.initializers.size();
           ++index) {
        separator(index);
        constructorInitializer(instance.initializers[index]);
      }
      output << "]\n";
      body(instance.body, instance.id);
    }

    for (const MirDestructorInstance &instance :
         program.destructorInstances()) {
      output << "destructor @" << instance.id << " owner=" << instance.owner
             << '\n';
      body(instance.body, instance.id);
    }

    for (const MirLambdaInstance &instance : program.lambdaInstances()) {
      output << "lambda @" << instance.id << " parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        type(instance.parameterTypes[index]);
      }
      output << "] captures=[";
      for (std::size_t index = 0; index < instance.captureTypes.size();
           ++index) {
        separator(index);
        output << "{type=";
        type(instance.captureTypes[index]);
        output << ",active-cleanup="
               << (index < instance.captureRequiresActiveCleanup.size()
                       ? instance.captureRequiresActiveCleanup[index]
                       : false)
               << '}';
      }
      output << "]\n";
      body(instance.body, instance.id);
    }
    return output.str();
  }

  [[nodiscard]] std::string print(const MirBody &value) {
    output << "mir-body-v4\n";
    body(value, 0);
    return output.str();
  }

private:
  template <typename Value> void list(const std::vector<Value> &values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      output << values[index];
    }
  }

  void separator(std::size_t index) {
    if (index != 0) {
      output << ',';
    }
  }

  template <typename Value> void optional(const std::optional<Value> &value) {
    if (value) {
      output << *value;
    } else {
      output << '-';
    }
  }

  void bytes(const std::string &value) {
    output << value.size() << ':' << std::hex << std::setfill('0');
    for (const unsigned char byte : value) {
      output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    output << std::dec << std::setfill(' ');
  }

  void literal(const std::optional<Literal> &value) {
    if (!value) {
      output << '-';
      return;
    }
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
        *value);
  }

  void type(const SemanticType &value) {
    output << "type(" << number(value.kind) << ";class=" << value.classId
           << ";enum=" << value.enumId
           << ";parameter=" << value.genericParameterId
           << ";lambda=" << value.lambdaId << ";length=" << value.arrayLength
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
      const CompileTimeValue &argument = value.valueArguments[index];
      output << '{' << number(argument.kind) << ':' << argument.value << ':'
             << argument.parameterId << '}';
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

  void info(const ExpressionInfo &value) {
    output << "info(";
    type(value.type);
    output << ",category=" << number(value.category)
           << ",access=" << number(value.access) << ',';
    traits(value.traits);
    output << ')';
  }

  void callableSignature(const MirCallableSignature &value) {
    output << "signature(return=";
    type(value.returnType);
    output << ";parameters=[";
    for (std::size_t index = 0; index < value.parameterTypes.size(); ++index) {
      separator(index);
      type(value.parameterTypes[index]);
    }
    output << "];function=";
    optional(value.functionTarget);
    output << ";lambda=";
    optional(value.lambdaTarget);
    output << ')';
  }

  void callableForwarding(const MirCallableForwarding &value) {
    output << "forward(parameter=" << value.parameterIndex << ";function=";
    optional(value.functionTarget);
    output << ')';
  }

  void callableParameter(const MirCallableParameter &value) {
    output << "callable(parameter=" << value.parameterIndex << ";type=";
    type(value.callableType);
    output << ";access=" << number(value.access)
           << ";non-escaping=" << value.nonEscaping << ";signatures=[";
    for (std::size_t index = 0; index < value.signatures.size(); ++index) {
      separator(index);
      callableSignature(value.signatures[index]);
    }
    output << "];forwardings=[";
    for (std::size_t index = 0; index < value.forwardings.size(); ++index) {
      separator(index);
      callableForwarding(value.forwardings[index]);
    }
    output << "])";
  }

  void operand(const MirOperand &value) {
    output << "operand(" << number(value.kind) << ";value=" << value.value
           << ";place=" << value.place << ";loan=" << value.loan << ";literal=";
    literal(value.literal);
    output << ";type=";
    type(value.type);
    output << ')';
  }

  void placeDomain(const PlaceDomain &value) {
    // Snapshot identities are process-local stale-key guards. Normalize a
    // live snapshot so equivalent programs retain deterministic text dumps.
    output << (value.snapshot == 0 ? 0 : 1) << ':' << value.body << ':'
           << value.revision;
  }

  void placeProjection(const PlaceProjection &value) {
    output << "projection(" << number(value.kind) << ";field=" << value.field
           << ";index=" << value.index << ";selection=" << value.selection
           << ')';
  }

  void placeKey(const PlaceKey &value) {
    output << "key(domain=";
    placeDomain(value.domain);
    output << ";root=" << value.root << ";receiver=" << value.receiver
           << ";projections=[";
    for (std::size_t index = 0; index < value.projections.size(); ++index) {
      separator(index);
      placeProjection(value.projections[index]);
    }
    output << "])";
  }

  void ownershipEvent(const OwnershipEvent &value) {
    output << "event(kind=" << number(value.kind) << ";place=";
    placeKey(value.place);
    output << ";before=" << static_cast<unsigned int>(value.before.bits)
           << ";after=" << static_cast<unsigned int>(value.after.bits)
           << ";reachable=" << value.reachable << ')';
  }

  void lifecycleEvent(const MirLifecycleEvent &value) {
    output << "lifecycle(kind=" << number(value.kind) << ";source=drop"
           << value.source << ";target=drop" << value.target
           << ";conditional=" << value.conditional << ')';
  }

  void dropObligation(const MirDropObligation &value) {
    output << "  drop" << value.id << " hir=" << value.hirObligation
           << " construction-order=" << value.constructionOrder
           << " kind=" << number(value.kind) << " place=p" << value.place
           << " binding=" << value.binding << " value=v" << value.value
           << " hir-full-expression=" << value.hirFullExpression
           << " full-expression=" << value.fullExpression << " type=";
    type(value.dropType.type);
    output << " class=";
    optional(value.dropType.classInstance);
    output << " lambda=";
    optional(value.dropType.lambdaInstance);
    output << " destructor=";
    optional(value.dropType.destructor);
    output << " active-cleanup=" << value.dropType.requiresActiveCleanup
           << " initially-active=" << value.initiallyActive << '\n';
  }

  void fullExpression(const MirFullExpression &value) {
    output << "  full-expression" << value.id << " hir=" << value.hirExpression
           << " statement=" << value.statement
           << " constructor-initializer=" << value.constructorInitializer
           << " roots=[";
    list(value.roots);
    output << "]\n";
  }

  void projection(const MirPlaceProjection &value) {
    output << "projection(" << number(value.kind) << ";field=" << value.field
           << ";index=" << value.index << ";constant=";
    optional(value.constantIndex);
    output << ";selection=" << value.selection << ')';
  }

  void place(const MirPlace &value) {
    output << "  p" << value.id << " root=" << number(value.root)
           << " binding=" << value.binding << " symbol=" << value.symbol
           << " temporary=" << value.temporary << " value=" << value.value
           << " loan=" << value.loan << " projections=[";
    for (std::size_t index = 0; index < value.projections.size(); ++index) {
      separator(index);
      projection(value.projections[index]);
    }
    output << "] type=";
    type(value.type);
    output << " access=" << number(value.access) << ' ';
    traits(value.traits);
    output << " source=v" << value.sourceValue << " key=";
    if (value.key) {
      placeKey(*value.key);
    } else {
      output << '-';
    }
    output << " initially-available=" << value.initiallyAvailable << '\n';
  }

  void loan(const MirLoan &value) {
    output << "  loan" << value.id << " kind=" << number(value.kind)
           << " semantic=" << value.semanticLoan << " parent=loan"
           << value.parent << " source=p" << value.source
           << " access=" << number(value.access) << " produced=v"
           << value.producedBy << " carriers=[";
    for (std::size_t index = 0; index < value.carriers.size(); ++index) {
      separator(index);
      output << value.carriers[index];
    }
    output << "] field=" << value.storedField << " entry=" << value.entry
           << " escapes=" << value.escapes << '\n';
  }

  void value(const MirValue &value) {
    output << "  %" << value.id << " source=v" << value.sourceValue << ' ';
    info(value.info);
    output << " defined=bb" << value.definitionBlock << ":i" << value.definition
           << '\n';
  }

  void instruction(const MirInstruction &value) {
    output << "    i" << value.id << ' ' << name(value.kind)
           << " hir-value=" << value.hirValue
           << " hir-statement=" << value.hirStatement
           << " unsafe-operation=" << number(value.unsafeOperation)
           << " raw-memory=" << value.rawMemoryAccess << " result=";
    optional(value.result);
    output << " destination=";
    optional(value.destination);
    output << " receiver=";
    if (value.receiver) {
      operand(*value.receiver);
    } else {
      output << '-';
    }
    output << " operands=[";
    for (std::size_t index = 0; index < value.operands.size(); ++index) {
      separator(index);
      operand(value.operands[index]);
    }
    output << "] parameters=[";
    for (std::size_t index = 0; index < value.parameterTypes.size(); ++index) {
      separator(index);
      type(value.parameterTypes[index]);
    }
    output << "] closure-captures=[";
    for (std::size_t index = 0; index < value.closureCaptureTypes.size();
         ++index) {
      separator(index);
      type(value.closureCaptureTypes[index]);
    }
    output << "] loan=";
    optional(value.loan);
    output << " borrow-origin=" << number(value.borrowOrigin)
           << " borrow-argument=" << value.borrowArgument
           << " borrow-access=" << number(value.borrowAccess)
           << " operation=" << name(value.operation) << " literal=";
    literal(value.literal);
    output << " enum-owner=";
    optional(value.enumOwner);
    output << " enum-value=";
    if (value.enumValue) {
      output << value.enumValue->negative << ':' << value.enumValue->magnitude;
    } else {
      output << '-';
    }
    output << " intrinsic=" << name(value.intrinsic)
           << " dispatch=" << number(value.dispatch) << " dispatch-owner=";
    type(value.dispatchOwner);
    output << " function=";
    optional(value.functionTarget);
    output << " constructor=";
    optional(value.constructorTarget);
    output << " constructor-kind=" << number(value.constructorKind)
           << " lambda=";
    optional(value.lambdaTarget);
    output << " non-escaping-callable=" << value.nonEscapingCallable
           << " non-escaping-arguments=[";
    list(value.nonEscapingArguments);
    output << ']';
    output << " ownership=";
    if (value.ownership) {
      ownershipEvent(*value.ownership);
    } else {
      output << '-';
    }
    output << " lifecycle=[";
    for (std::size_t index = 0; index < value.lifecycle.size(); ++index) {
      separator(index);
      lifecycleEvent(value.lifecycle[index]);
    }
    output << "] full-expression-end=" << value.fullExpressionEnd
           << " cleanup-boundary-end=" << value.cleanupBoundaryEnd;
    output << ' ';
    info(value.info);
    output << '\n';
  }

  void switchCase(const SwitchCaseValue &value) {
    output << "case(" << number(value.kind) << ";type=";
    type(value.type);
    output << ";negative=" << value.value.negative
           << ";magnitude=" << value.value.magnitude
           << ";enum=" << value.enumOwner << ')';
  }

  void terminator(const MirTerminator &value) {
    output << "    term " << number(value.kind)
           << " hir-value=" << value.hirValue
           << " hir-statement=" << value.hirStatement << " value=";
    if (value.value) {
      operand(*value.value);
    } else {
      output << '-';
    }
    output << " return-loan=";
    optional(value.returnLoan);
    output << " target=bb" << value.target << " else=bb" << value.elseTarget
           << " switches=[";
    for (std::size_t index = 0; index < value.switchTargets.size(); ++index) {
      separator(index);
      const MirSwitchTarget &target = value.switchTargets[index];
      output << '{';
      if (target.value) {
        switchCase(*target.value);
      } else {
        output << "default";
      }
      output << "->bb" << target.target << '}';
    }
    output << "]\n";
  }

  void block(const MirBlock &value) {
    output << "  bb" << value.id << " reachable=" << value.reachable << '\n';
    for (const MirInstruction &item : value.instructions) {
      instruction(item);
    }
    terminator(value.terminator);
  }

  void use(const MirValueUse &value) {
    output << "use(" << number(value.kind) << ";value=" << value.value
           << ";block=" << value.block << ";instruction=" << value.instruction
           << ";place=" << value.place << ";operand=" << value.operandIndex
           << ')';
  }

  void body(const MirBody &value, std::size_t owner) {
    output << "body kind=" << number(value.kind) << " owner=" << owner
           << " domain=";
    placeDomain(value.placeDomain);
    output << " entry=bb" << value.entry << " return=";
    type(value.returnType);
    output << '\n';
    output << " places " << value.places.size() << '\n';
    for (const MirPlace &item : value.places) {
      place(item);
    }
    output << " loans " << value.loans.size() << '\n';
    for (const MirLoan &item : value.loans) {
      loan(item);
    }
    output << " full-expressions " << value.fullExpressions.size() << '\n';
    for (const MirFullExpression &item : value.fullExpressions) {
      fullExpression(item);
    }
    output << " cleanup-boundaries " << value.cleanupBoundaries.size() << '\n';
    for (const MirCleanupBoundary &item : value.cleanupBoundaries) {
      output << "  cleanup-boundary" << item.id << " obligations=[";
      list(item.obligations);
      output << "]\n";
    }
    output << " drops " << value.dropObligations.size() << '\n';
    for (const MirDropObligation &item : value.dropObligations) {
      dropObligation(item);
    }
    output << " values " << value.values.size() << '\n';
    for (const MirValue &item : value.values) {
      this->value(item);
    }
    output << " blocks " << value.blocks.size() << '\n';
    for (const MirBlock &item : value.blocks) {
      block(item);
    }
    output << " uses " << value.valueUses.size() << '\n';
    for (std::size_t index = 0; index < value.valueUses.size(); ++index) {
      output << "  %" << index + 1 << "=[";
      for (std::size_t useIndex = 0; useIndex < value.valueUses[index].size();
           ++useIndex) {
        separator(useIndex);
        use(value.valueUses[index][useIndex]);
      }
      output << "]\n";
    }
    output << "end-body\n";
  }

  void constructorInitializer(const MirConstructorInitializer &value) {
    output << "initializer(kind=" << number(value.kind) << ";type=";
    type(value.targetType);
    output << ";field=" << value.field << ";base=";
    optional(value.base);
    output << ";constructor=";
    optional(value.constructorTarget);
    output << ";arguments=[";
    list(value.arguments);
    output << "];stores-reference=" << value.storesReference
           << ";borrow-access=" << number(value.borrowAccess)
           << ";generated=" << value.generatedDefault << ')';
  }

  std::ostringstream output;
};

} // namespace

std::string MirPrinter::print(const MirProgram &program) const {
  return Printer().print(program);
}

std::string MirPrinter::print(const MirBody &body) const {
  return Printer().print(body);
}

} // namespace lang
