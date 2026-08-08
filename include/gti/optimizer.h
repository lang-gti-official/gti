#pragma once

#include "gti/hir.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace lang {

enum class OptimizationLevel {
  O0,
  O1,
  O2,
  O3,
};

struct IntegerConstant {
  bool negative = false;
  std::uint64_t magnitude = 0;
  SemanticType type = SemanticType::Unknown;

  friend bool operator==(const IntegerConstant &,
                         const IntegerConstant &) = default;
};

struct NullConstant {
  friend bool operator==(NullConstant, NullConstant) = default;
};

using ConstantValue = std::variant<IntegerConstant, double, CharacterLiteral,
                                   std::string, bool, NullConstant>;

class OptimizationResult {
public:
  [[nodiscard]] const ConstantValue *replacement(HirValueId value) const {
    const auto found = constants.find(value);
    return found == constants.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ConstantValue *replacement(const HirProgram &program,
                                                 const Expr &expression) const {
    const ConstantValue *replacement = nullptr;
    for (const HirValueId id : program.valueIdsForSource(expression)) {
      const ConstantValue *candidate = this->replacement(id);
      if (candidate == nullptr) {
        return nullptr;
      }
      if (replacement != nullptr && *replacement != *candidate) {
        return nullptr;
      }
      replacement = candidate;
    }
    return replacement;
  }

  [[nodiscard]] std::size_t foldedExpressionCount() const {
    return constants.size();
  }

  void setReplacement(HirValueId value, ConstantValue replacement) {
    if (value != 0) {
      constants.insert_or_assign(value, std::move(replacement));
    }
  }

private:
  std::unordered_map<HirValueId, ConstantValue> constants;
};

struct OptimizationContext {
  const HirProgram &program;
  OptimizationLevel level;
  TargetInfo target;
};

class OptimizationPass {
public:
  OptimizationPass() = default;
  OptimizationPass(const OptimizationPass &) = delete;
  OptimizationPass &operator=(const OptimizationPass &) = delete;
  virtual ~OptimizationPass() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  virtual void run(const OptimizationContext &context,
                   OptimizationResult &result) = 0;
};

class ConstantFoldingPass final : public OptimizationPass {
public:
  [[nodiscard]] std::string_view name() const override {
    return "constant-folding";
  }

  void run(const OptimizationContext &context,
           OptimizationResult &output) override {
    constants.clear();
    result = &output;
    analyze(context.program.module());
    for (const HirClassInstance &instance : context.program.classInstances()) {
      analyze(instance.fieldInitializers);
    }
    for (const HirFunctionInstance &instance :
         context.program.functionInstances()) {
      analyze(instance.body);
    }
    for (const HirConstructorInstance &instance :
         context.program.constructorInstances()) {
      analyze(instance.body);
    }
    for (const HirDestructorInstance &instance :
         context.program.destructorInstances()) {
      analyze(instance.body);
    }
    for (const HirLambda &lambda : context.program.lambdaInstances()) {
      analyze(lambda.body);
    }
  }

private:
  template <typename Value>
  [[nodiscard]] static const Value *
  constant(const std::optional<ConstantValue> &value) {
    return value ? std::get_if<Value>(&*value) : nullptr;
  }

  [[nodiscard]] std::optional<ConstantValue> operand(const HirValue &value,
                                                     std::size_t index) const {
    if (index >= value.operands.size()) {
      return std::nullopt;
    }
    const auto found = constants.find(value.operands[index]);
    return found == constants.end()
               ? std::nullopt
               : std::optional<ConstantValue>{found->second};
  }

  void analyze(const HirBody &body) {
    for (const HirValue &value : body.values) {
      const std::optional<ConstantValue> folded = evaluate(value);
      if (!folded) {
        continue;
      }
      constants.insert_or_assign(value.id, *folded);
      if (value.kind != HirValueKind::Literal) {
        result->setReplacement(value.id, *folded);
      }
    }
  }

  [[nodiscard]] std::optional<ConstantValue>
  evaluate(const HirValue &value) const {
    switch (value.kind) {
    case HirValueKind::Literal:
      return literal(value);
    case HirValueKind::Grouping:
      return operand(value, 0);
    case HirValueKind::Binary:
      return value.operation
                 ? foldComparison(*value.operation, operand(value, 0),
                                  operand(value, 1))
                 : std::nullopt;
    case HirValueKind::Logical:
      return logical(value);
    case HirValueKind::Unary:
      return unary(value);
    case HirValueKind::Assignment:
    case HirValueKind::ArrayInitializer:
    case HirValueKind::Call:
    case HirValueKind::Move:
    case HirValueKind::Conversion:
    case HirValueKind::DirectInitializer:
    case HirValueKind::DereferenceSet:
    case HirValueKind::MemberAccess:
    case HirValueKind::Index:
    case HirValueKind::IndexSet:
    case HirValueKind::Lambda:
    case HirValueKind::PackExpansion:
    case HirValueKind::Postfix:
    case HirValueKind::QualifiedName:
    case HirValueKind::This:
    case HirValueKind::MemberSet:
    case HirValueKind::Unexpected:
    case HirValueKind::Variable:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] static std::optional<ConstantValue>
  literal(const HirValue &value) {
    if (!value.literal) {
      return std::nullopt;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&*value.literal)) {
      SemanticType type = value.info.type;
      if (type == SemanticType::Unknown) {
        type = *integer <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int32_t>::max())
                   ? SemanticType::Int32
               : *integer <= static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max())
                   ? SemanticType::Int64
                   : SemanticType::UInt64;
      }
      return IntegerConstant{.magnitude = *integer, .type = type};
    }
    if (const auto *floating = std::get_if<double>(&*value.literal)) {
      return *floating;
    }
    if (const auto *character =
            std::get_if<CharacterLiteral>(&*value.literal)) {
      return *character;
    }
    if (const auto *string = std::get_if<std::string>(&*value.literal)) {
      return *string;
    }
    if (const auto *boolean = std::get_if<bool>(&*value.literal)) {
      return *boolean;
    }
    if (std::holds_alternative<std::nullptr_t>(*value.literal)) {
      return NullConstant{};
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ConstantValue>
  logical(const HirValue &value) const {
    if (!value.operation) {
      return std::nullopt;
    }
    const std::optional<ConstantValue> left = operand(value, 0);
    const bool *leftBoolean = constant<bool>(left);
    if (leftBoolean != nullptr && *value.operation == TokenKind::AND &&
        !*leftBoolean) {
      return false;
    }
    if (leftBoolean != nullptr && *value.operation == TokenKind::OR &&
        *leftBoolean) {
      return true;
    }

    const std::optional<ConstantValue> right = operand(value, 1);
    const bool *rightBoolean = constant<bool>(right);
    if (leftBoolean == nullptr || rightBoolean == nullptr) {
      return std::nullopt;
    }
    return *value.operation == TokenKind::AND
               ? ConstantValue{*leftBoolean && *rightBoolean}
               : ConstantValue{*leftBoolean || *rightBoolean};
  }

  [[nodiscard]] std::optional<ConstantValue>
  unary(const HirValue &value) const {
    if (!value.operation) {
      return std::nullopt;
    }
    const std::optional<ConstantValue> right = operand(value, 0);
    if (!right) {
      return std::nullopt;
    }
    if (*value.operation == TokenKind::BANG) {
      if (const bool *boolean = constant<bool>(right)) {
        return !*boolean;
      }
      return std::nullopt;
    }
    if (*value.operation == TokenKind::PLUS) {
      if (const auto *integer = constant<IntegerConstant>(right)) {
        IntegerConstant folded = *integer;
        folded.type = value.info.type;
        return folded;
      }
      if (const auto *floating = constant<double>(right)) {
        return *floating;
      }
      return std::nullopt;
    }
    if (*value.operation == TokenKind::MINUS) {
      if (const auto *integer = constant<IntegerConstant>(right)) {
        IntegerConstant folded = *integer;
        folded.negative = folded.magnitude != 0 && !folded.negative;
        folded.type = value.info.type;
        return folded;
      }
      if (const auto *floating = constant<double>(right)) {
        return -*floating;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static int compare(const IntegerConstant &left,
                                   const IntegerConstant &right) {
    if (left.negative != right.negative) {
      return left.negative ? -1 : 1;
    }
    if (left.magnitude == right.magnitude) {
      return 0;
    }
    if (left.negative) {
      return left.magnitude > right.magnitude ? -1 : 1;
    }
    return left.magnitude < right.magnitude ? -1 : 1;
  }

  [[nodiscard]] static std::optional<ConstantValue>
  foldComparison(TokenKind operation, const std::optional<ConstantValue> &left,
                 const std::optional<ConstantValue> &right) {
    if (!left || !right) {
      return std::nullopt;
    }

    std::optional<int> ordering;
    if (const auto *leftInteger = constant<IntegerConstant>(left)) {
      if (const auto *rightInteger = constant<IntegerConstant>(right)) {
        ordering = compare(*leftInteger, *rightInteger);
      }
    } else if (const auto *leftFloat = constant<double>(left)) {
      if (const auto *rightFloat = constant<double>(right)) {
        if (std::isnan(*leftFloat) || std::isnan(*rightFloat)) {
          switch (operation) {
          case TokenKind::EQUAL_EQUAL:
            return false;
          case TokenKind::BANG_EQUAL:
            return true;
          case TokenKind::LESS:
          case TokenKind::LESS_EQUAL:
          case TokenKind::GREATER:
          case TokenKind::GREATER_EQUAL:
            return false;
          default:
            return std::nullopt;
          }
        }
        ordering = *leftFloat < *rightFloat   ? -1
                   : *leftFloat > *rightFloat ? 1
                                              : 0;
      }
    } else if (const auto *leftString = constant<std::string>(left)) {
      if (const auto *rightString = constant<std::string>(right)) {
        ordering = leftString->compare(*rightString);
      }
    } else if (const auto *leftCharacter = constant<CharacterLiteral>(left)) {
      if (const auto *rightCharacter = constant<CharacterLiteral>(right)) {
        ordering = leftCharacter->value < rightCharacter->value   ? -1
                   : leftCharacter->value > rightCharacter->value ? 1
                                                                  : 0;
      }
    } else if (const auto *leftBoolean = constant<bool>(left)) {
      if (const auto *rightBoolean = constant<bool>(right)) {
        ordering = *leftBoolean == *rightBoolean ? 0 : (*leftBoolean ? 1 : -1);
      }
    } else if (constant<NullConstant>(left) != nullptr &&
               constant<NullConstant>(right) != nullptr) {
      ordering = 0;
    }

    if (!ordering) {
      return std::nullopt;
    }
    switch (operation) {
    case TokenKind::EQUAL_EQUAL:
      return *ordering == 0;
    case TokenKind::BANG_EQUAL:
      return *ordering != 0;
    case TokenKind::LESS:
      return *ordering < 0;
    case TokenKind::LESS_EQUAL:
      return *ordering <= 0;
    case TokenKind::GREATER:
      return *ordering > 0;
    case TokenKind::GREATER_EQUAL:
      return *ordering >= 0;
    default:
      return std::nullopt;
    }
  }

  OptimizationResult *result = nullptr;
  std::unordered_map<HirValueId, ConstantValue> constants;
};

class OptimizationPipeline {
public:
  [[nodiscard]] OptimizationResult
  run(const HirProgram &program, OptimizationLevel level,
      TargetInfo target = TargetInfo::host()) const {
    OptimizationResult result;
    if (level == OptimizationLevel::O0) {
      return result;
    }

    const OptimizationContext context{
        .program = program, .level = level, .target = std::move(target)};
    ConstantFoldingPass().run(context, result);
    return result;
  }
};

} // namespace lang
