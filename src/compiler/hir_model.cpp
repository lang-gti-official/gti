#include "gti/hir.h"

#include <algorithm>

namespace lang {

const HirValue *HirBody::findValue(HirValueId id) const {
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [id](const HirValue &value) { return value.id == id; });
  return found == values.end() ? nullptr : &*found;
}

const HirStatement *HirBody::findStatement(HirStatementId id) const {
  const auto found = std::find_if(
      statements.begin(), statements.end(),
      [id](const HirStatement &statement) { return statement.id == id; });
  return found == statements.end() ? nullptr : &*found;
}

const HirLoan *HirBody::findLoan(SemanticLoanId id) const {
  const auto found =
      std::find_if(loans.begin(), loans.end(), [id](const HirLoan &loan) {
        return loan.semanticLoan == id;
      });
  return found == loans.end() ? nullptr : &*found;
}

std::size_t HirProgram::valueCount() const {
  std::size_t count = moduleBody.values.size();
  for (const HirClassInstance &classInstance : classes) {
    count += classInstance.fieldInitializers.values.size();
    count += classInstance.staticFieldInitializers.values.size();
  }
  for (const HirFunctionInstance &function : functions) {
    count += function.body.values.size();
  }
  for (const HirConstructorInstance &constructor : constructors) {
    count += constructor.body.values.size();
  }
  for (const HirDestructorInstance &destructor : destructors) {
    count += destructor.body.values.size();
  }
  for (const HirLambda &lambda : lambdas) {
    count += lambda.body.values.size();
  }
  return count;
}

std::size_t HirProgram::statementCount() const {
  std::size_t count = moduleBody.statements.size();
  for (const HirClassInstance &classInstance : classes) {
    count += classInstance.fieldInitializers.statements.size();
    count += classInstance.staticFieldInitializers.statements.size();
  }
  for (const HirFunctionInstance &function : functions) {
    count += function.body.statements.size();
  }
  for (const HirConstructorInstance &constructor : constructors) {
    count += constructor.body.statements.size();
  }
  for (const HirDestructorInstance &destructor : destructors) {
    count += destructor.body.statements.size();
  }
  for (const HirLambda &lambda : lambdas) {
    count += lambda.body.statements.size();
  }
  return count;
}

const std::vector<HirValueId> &
HirProgram::valueIdsForSource(const Expr &source) const {
  static const std::vector<HirValueId> empty;
  const auto found = sourceValueIds.find(&source);
  return found == sourceValueIds.end() ? empty : found->second;
}

} // namespace lang
