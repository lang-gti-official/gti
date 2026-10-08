#include "gti/lowered_program.h"
#include "lowered_program_closure.h"

#include <algorithm>

namespace lang {
namespace {

// The first row whose `Payload` matches, using `rows` as a validated hint.
template <typename Payload, typename Matches>
const Payload *
findIndexedDeclaration(const std::vector<LoweredDeclaration> &declarations,
                       const std::unordered_map<std::size_t, std::size_t> &rows,
                       std::size_t id, Matches matches) {
  if (const auto row = rows.find(id);
      row != rows.end() && row->second < declarations.size()) {
    if (const auto *payload =
            std::get_if<Payload>(&declarations[row->second].payload);
        payload != nullptr && matches(*payload)) {
      return payload;
    }
  }
  for (const LoweredDeclaration &declaration : declarations) {
    if (const auto *payload = std::get_if<Payload>(&declaration.payload);
        payload != nullptr && matches(*payload)) {
      return payload;
    }
  }
  return nullptr;
}

} // namespace

void LoweredProgram::indexDeclarations() {
  classRows_.clear();
  functionRows_.clear();
  constructorRows_.clear();
  storageRows_.clear();
  symbolRows_.clear();
  enumRows_.clear();
  genericParameterRows_.clear();
  const auto parameters = [&](const std::vector<LoweredGenericParameter> &items,
                              std::size_t row) {
    for (const LoweredGenericParameter &parameter : items) {
      genericParameterRows_.try_emplace(parameter.id, row);
    }
  };
  for (std::size_t row = 0; row < declarations_.size(); ++row) {
    const auto &payload = declarations_[row].payload;
    if (const auto *item = std::get_if<LoweredClassDeclaration>(&payload)) {
      classRows_.try_emplace(item->id, row);
      parameters(item->genericParameters, row);
    } else if (const auto *item =
                   std::get_if<LoweredFunctionDeclaration>(&payload)) {
      functionRows_.try_emplace(item->id, row);
      parameters(item->genericParameters, row);
    } else if (const auto *item =
                   std::get_if<LoweredConstructorDeclaration>(&payload)) {
      constructorRows_.try_emplace(item->id, row);
      parameters(item->genericParameters, row);
    } else if (const auto *item =
                   std::get_if<LoweredStorageDeclaration>(&payload)) {
      storageRows_.try_emplace(item->symbol, row);
    } else if (const auto *item =
                   std::get_if<LoweredEnumDeclaration>(&payload)) {
      enumRows_.try_emplace(item->type.enumId, row);
    }
  }
  for (std::size_t row = 0; row < symbols_.size(); ++row) {
    symbolRows_.try_emplace(symbols_[row].id, row);
  }
}

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
  return findIndexedDeclaration<LoweredClassDeclaration>(
      declarations_, classRows_, id,
      [id](const LoweredClassDeclaration &payload) {
        return payload.id == id;
      });
}

const LoweredEnumDeclaration *
LoweredProgram::findEnumDeclaration(EnumId id) const {
  return findEnumDeclaration(SemanticType::enumType(id));
}

const LoweredDeclaration *
LoweredProgram::findClassDeclarationRow(ClassId id) const {
  const LoweredClassDeclaration *payload = findClassDeclaration(id);
  if (payload == nullptr) {
    return nullptr;
  }
  if (const auto row = classRows_.find(id);
      row != classRows_.end() && row->second < declarations_.size() &&
      std::get_if<LoweredClassDeclaration>(
          &declarations_[row->second].payload) == payload) {
    return &declarations_[row->second];
  }
  for (const LoweredDeclaration &declaration : declarations_) {
    if (std::get_if<LoweredClassDeclaration>(&declaration.payload) == payload) {
      return &declaration;
    }
  }
  return nullptr;
}

const LoweredDeclaration *
LoweredProgram::findEnumDeclarationRow(const SemanticType &type) const {
  if (const auto row = enumRows_.find(type.enumId);
      row != enumRows_.end() && row->second < declarations_.size()) {
    if (const auto *payload = std::get_if<LoweredEnumDeclaration>(
            &declarations_[row->second].payload);
        payload != nullptr && payload->type == type) {
      return &declarations_[row->second];
    }
  }
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredEnumDeclaration>(&declaration.payload);
        payload != nullptr && payload->type == type) {
      return &declaration;
    }
  }
  return nullptr;
}

const LoweredEnumDeclaration *
LoweredProgram::findEnumDeclaration(const SemanticType &type) const {
  if (const LoweredDeclaration *row = findEnumDeclarationRow(type)) {
    return std::get_if<LoweredEnumDeclaration>(&row->payload);
  }
  return nullptr;
}

const LoweredFunctionDeclaration *
LoweredProgram::findFunctionDeclaration(FunctionId id) const {
  return findIndexedDeclaration<LoweredFunctionDeclaration>(
      declarations_, functionRows_, id,
      [id](const LoweredFunctionDeclaration &payload) {
        return payload.id == id;
      });
}

const LoweredConstructorDeclaration *
LoweredProgram::findConstructorDeclaration(ConstructorId id) const {
  return findIndexedDeclaration<LoweredConstructorDeclaration>(
      declarations_, constructorRows_, id,
      [id](const LoweredConstructorDeclaration &payload) {
        return payload.id == id;
      });
}

const LoweredStorageDeclaration *
LoweredProgram::findStorageDeclaration(SymbolId id) const {
  return findIndexedDeclaration<LoweredStorageDeclaration>(
      declarations_, storageRows_, id,
      [id](const LoweredStorageDeclaration &payload) {
        return payload.symbol == id;
      });
}

const LoweredGenericParameter *
LoweredProgram::findGenericParameter(GenericParameterId id) const {
  const auto find = [id](const std::vector<LoweredGenericParameter> &items) {
    const auto found = std::find_if(
        items.begin(), items.end(),
        [id](const LoweredGenericParameter &item) { return item.id == id; });
    return found == items.end() ? nullptr : &*found;
  };
  const auto inRow = [&](const LoweredDeclaration &declaration)
      -> const LoweredGenericParameter * {
    if (const auto *payload =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      return find(payload->genericParameters);
    }
    if (const auto *payload =
            std::get_if<LoweredFunctionDeclaration>(&declaration.payload)) {
      return find(payload->genericParameters);
    }
    if (const auto *payload =
            std::get_if<LoweredConstructorDeclaration>(&declaration.payload)) {
      return find(payload->genericParameters);
    }
    return nullptr;
  };
  if (const auto row = genericParameterRows_.find(id);
      row != genericParameterRows_.end() &&
      row->second < declarations_.size()) {
    if (const LoweredGenericParameter *parameter =
            inRow(declarations_[row->second])) {
      return parameter;
    }
  }
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
  if (const auto row = symbolRows_.find(id); row != symbolRows_.end() &&
                                             row->second < symbols_.size() &&
                                             symbols_[row->second].id == id) {
    return &symbols_[row->second];
  }
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
