#include "gti/lowered_program.h"
#include "lowered_program_closure.h"

#include <algorithm>

namespace lang {
const LoweredBody *LoweredProgram::findBody(MirBodyAddress address) const {
  return lowered_detail::findBody(bodies_, address);
}

const LoweredDeclaration *
LoweredProgram::findDeclaration(LoweredDeclarationId id) const {
  return id == 0 || id > declarations_.size() ? nullptr
                                              : &declarations_[id - 1];
}

const LoweredClassDeclaration *
LoweredProgram::findClassDeclaration(ClassId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredClassDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredEnumDeclaration *
LoweredProgram::findEnumDeclaration(EnumId id) const {
  return findEnumDeclaration(SemanticType::enumType(id));
}

const LoweredEnumDeclaration *
LoweredProgram::findEnumDeclaration(const SemanticType &type) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredEnumDeclaration>(&declaration.payload);
        payload != nullptr && payload->type == type) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredFunctionDeclaration *
LoweredProgram::findFunctionDeclaration(FunctionId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredConstructorDeclaration *
LoweredProgram::findConstructorDeclaration(ConstructorId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredConstructorDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredStorageDeclaration *
LoweredProgram::findStorageDeclaration(SymbolId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredStorageDeclaration>(&declaration.payload);
        payload != nullptr && payload->symbol == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredGenericParameter *
LoweredProgram::findGenericParameter(GenericParameterId id) const {
  const auto find = [id](const std::vector<LoweredGenericParameter> &items) {
    const auto found = std::find_if(
        items.begin(), items.end(),
        [id](const LoweredGenericParameter &item) { return item.id == id; });
    return found == items.end() ? nullptr : &*found;
  };
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      if (const LoweredGenericParameter *parameter =
              find(payload->genericParameters)) {
        return parameter;
      }
    } else if (const auto *payload = std::get_if<LoweredFunctionDeclaration>(
                   &declaration.payload)) {
      if (const LoweredGenericParameter *parameter =
              find(payload->genericParameters)) {
        return parameter;
      }
    } else if (const auto *payload = std::get_if<LoweredConstructorDeclaration>(
                   &declaration.payload)) {
      if (const LoweredGenericParameter *parameter =
              find(payload->genericParameters)) {
        return parameter;
      }
    }
  }
  return nullptr;
}

const LoweredSymbol *LoweredProgram::findSymbol(SymbolId id) const {
  const auto found = std::find_if(
      symbols_.begin(), symbols_.end(),
      [id](const LoweredSymbol &symbol) { return symbol.id == id; });
  return found == symbols_.end() ? nullptr : &*found;
}

const LoweredClassInstance *
LoweredProgram::findClassInstance(LoweredClassInstanceId id) const {
  return id == 0 || id > classInstances_.size() ? nullptr
                                                : &classInstances_[id - 1];
}

const LoweredFunctionInstance *
LoweredProgram::findFunctionInstance(LoweredFunctionInstanceId id) const {
  return id == 0 || id > functionInstances_.size()
             ? nullptr
             : &functionInstances_[id - 1];
}

const LoweredConstructorInstance *
LoweredProgram::findConstructorInstance(LoweredConstructorInstanceId id) const {
  return id == 0 || id > constructorInstances_.size()
             ? nullptr
             : &constructorInstances_[id - 1];
}

const LoweredDestructorInstance *
LoweredProgram::findDestructorInstance(LoweredDestructorInstanceId id) const {
  return id == 0 || id > destructorInstances_.size()
             ? nullptr
             : &destructorInstances_[id - 1];
}

const LoweredLambdaInstance *
LoweredProgram::findLambdaInstance(LoweredLambdaInstanceId id) const {
  return id == 0 || id > lambdaInstances_.size() ? nullptr
                                                 : &lambdaInstances_[id - 1];
}

const LoweredGeneratedItem *LoweredProgram::findGeneratedItem(
    const LoweredGeneratedItemIdentity &identity) const {
  return lowered_detail::findGenerated(generatedItems_, identity);
}

} // namespace lang
