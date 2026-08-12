#pragma once

#include "gti/constant_evaluator.h"
#include "gti/hir.h"
#include "gti/mir.h"
#include "gti/optimization/report.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lang {

class OptimizationResult;

enum class OptimizationLevel {
  O0,
  O1,
  O2,
  O3,
};

struct OptimizationOptions {
  bool verifyMir = true;
};

struct OptimizationRequest {
  const HirProgram &hir;
  MirProgram mir;
  OptimizationLevel level = OptimizationLevel::O0;
  TargetInfo target = TargetInfo::host();
  OptimizationOptions options;
  const OptimizationResult *compatibility = nullptr;
};

struct OptimizedProgram {
  MirProgram mir;
  OptimizationReport report;

  [[nodiscard]] bool valid() const { return report.valid(); }
};

using IntegerConstant = ConstantInteger;

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
    if (value.constant) {
      return value.constant;
    }
    switch (value.kind) {
    case HirValueKind::Literal:
      return literal(value);
    case HirValueKind::Grouping:
      return operand(value, 0);
    case HirValueKind::Binary:
      return foldBinary(value);
    case HirValueKind::Logical:
      return logical(value);
    case HirValueKind::Unary:
      return unary(value);
    case HirValueKind::Conditional:
      return conditional(value);
    case HirValueKind::Conversion:
      return conversion(value);
    case HirValueKind::Assignment:
    case HirValueKind::ArrayInitializer:
    case HirValueKind::Call:
    case HirValueKind::Move:
    case HirValueKind::DirectInitializer:
    case HirValueKind::DereferenceSet:
    case HirValueKind::MemberAccess:
    case HirValueKind::Index:
    case HirValueKind::IndexSet:
    case HirValueKind::Lambda:
    case HirValueKind::LayoutQuery:
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
    return evaluateConstantLiteral(*value.literal,
                                   constantIntegerDomain(value.info.type))
        .value;
  }

  [[nodiscard]] std::optional<ConstantValue>
  logical(const HirValue &value) const {
    if (!value.operation) {
      return std::nullopt;
    }
    const std::optional<ConstantValue> left = operand(value, 0);
    const bool *leftBoolean = left ? std::get_if<bool>(&*left) : nullptr;
    if (leftBoolean != nullptr && *value.operation == TokenKind::AND &&
        !*leftBoolean) {
      return false;
    }
    if (leftBoolean != nullptr && *value.operation == TokenKind::OR &&
        *leftBoolean) {
      return true;
    }

    const std::optional<ConstantValue> right = operand(value, 1);
    const bool *rightBoolean = right ? std::get_if<bool>(&*right) : nullptr;
    if (leftBoolean == nullptr || rightBoolean == nullptr) {
      return std::nullopt;
    }
    return evaluateConstantLogical(*value.operation, *left, *right).value;
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
    return evaluateConstantUnary(*value.operation, *right,
                                 constantIntegerDomain(value.info.type))
        .value;
  }

  [[nodiscard]] std::optional<ConstantValue>
  foldBinary(const HirValue &value) const {
    if (!value.operation) {
      return std::nullopt;
    }
    const std::optional<ConstantValue> left = operand(value, 0);
    const std::optional<ConstantValue> right = operand(value, 1);
    if (!left || !right) {
      return std::nullopt;
    }
    return evaluateConstantBinary(*value.operation, *left, *right,
                                  constantIntegerDomain(value.info.type))
        .value;
  }

  [[nodiscard]] std::optional<ConstantValue>
  conditional(const HirValue &value) const {
    const std::optional<ConstantValue> condition = operand(value, 0);
    const bool *selected = condition ? std::get_if<bool>(&*condition) : nullptr;
    if (selected == nullptr) {
      return std::nullopt;
    }
    return operand(value, *selected ? 1 : 2);
  }

  [[nodiscard]] std::optional<ConstantValue>
  conversion(const HirValue &value) const {
    const std::optional<ConstantValue> source = operand(value, 0);
    if (!source) {
      return std::nullopt;
    }
    if (value.info.type == SemanticType::Float) {
      return convertConstantFloat(*source).value;
    }
    const std::optional<CheckedIntegerDomain> target =
        constantIntegerDomain(value.info.type);
    if (!target) {
      return std::nullopt;
    }
    return convertConstantInteger(*source, *target).value;
  }

  OptimizationResult *result = nullptr;
  std::unordered_map<HirValueId, ConstantValue> constants;
};

class OptimizationPipeline {
public:
  [[nodiscard]] OptimizedProgram run(OptimizationRequest request) const;

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
