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
    output << "mir-v1 valid=" << program.valid() << '\n';
    output << "module\n";
    body(program.module(), 0);

    for (const MirClassInstance &instance : program.classInstances()) {
      output << "class @" << instance.id << " type=";
      type(instance.type);
      output << " kind=" << number(instance.kind)
             << " abstract=" << instance.abstract
             << " polymorphic=" << instance.polymorphic << " bases=[";
      for (std::size_t index = 0; index < instance.bases.size(); ++index) {
        separator(index);
        const HirBaseInstance &base = instance.bases[index];
        output << "{instance=" << base.instance << ",type=";
        type(base.type);
        output << ",interface=" << base.interface << '}';
      }
      output << "] drops=[";
      for (std::size_t index = 0; index < instance.fieldDropOrder.size();
           ++index) {
        separator(index);
        const MirFieldDrop &drop = instance.fieldDropOrder[index];
        output << "{field=" << drop.field << ",symbol=" << drop.symbol
               << ",type=";
        type(drop.type);
        output << '}';
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
      output << "lambda @" << instance.id << '\n';
      body(instance.body, instance.id);
    }
    return output.str();
  }

  [[nodiscard]] std::string print(const MirBody &value) {
    output << "mir-body-v1\n";
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
            output << "f32:0x" << std::hex << std::setw(8) << std::setfill('0')
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
           << value.containsBorrowedState << ')';
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

  void projection(const MirPlaceProjection &value) {
    output << "projection(" << number(value.kind) << ";field=" << value.field
           << ";index=" << value.index << ')';
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
    output << " source=v" << value.sourceValue << '\n';
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
           << " entry=bb" << value.entry << " return=";
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
