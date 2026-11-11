#include "gti/driver/borrow_dependencies.h"

#include "gti/ast.h"
#include "gti/semantic_model.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

namespace lang::driver {
namespace {

std::string json(std::string_view text) {
  std::string result = "\"";
  for (const char character : text) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof escaped, "\\u%04x",
                      static_cast<unsigned>(character));
        result += escaped;
      } else {
        result += character;
      }
    }
  }
  return result + '"';
}

struct Dependency {
  std::string root;
  std::vector<std::string> fields;
  std::vector<std::string> resultFields;

  friend auto operator<=>(const Dependency &, const Dependency &) = default;
};

struct Inputs {
  std::vector<std::size_t> parameters;
  bool receiver = false;
};

class Reporter {
public:
  explicit Reporter(const FrontendResult &frontend)
      : frontend(frontend), mir(frontend.mir), hir(frontend.hir),
        printer(frontend.semantics) {
    std::map<std::string, std::size_t> seen;
    for (const HirLambda &lambda : hir.lambdaInstances()) {
      const std::string signature = lambdaSignature(lambda);
      lambdas.push_back(signature + " #" + std::to_string(seen[signature]++));
    }
  }

  std::string report() {
    std::vector<std::string> calls;
    for (const auto address : enumerateMirBodyAddresses(mir))
      if (const MirBody *body = findMirBody(mir, address))
        collectCalls(address, *body, calls);
    std::sort(calls.begin(), calls.end());
    std::vector<std::string> contracts = collectContracts();
    std::sort(contracts.begin(), contracts.end());
    std::string result =
        "{\"schema\":\"gti-borrow-dependencies-v1\",\n\"calls\":[";
    for (std::size_t index = 0; index < calls.size(); ++index)
      result += (index == 0 ? "\n" : ",\n") + calls[index];
    result += "],\n\"contracts\":[";
    for (std::size_t index = 0; index < contracts.size(); ++index)
      result += (index == 0 ? "\n" : ",\n") + contracts[index];
    return result + "]}\n";
  }

private:
  std::string type(const SemanticType &value) const {
    return printer.print(value);
  }

  std::string types(const std::vector<SemanticType> &values) const {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index)
      result += (index == 0 ? "" : ",") + type(values[index]);
    return result;
  }

  std::string symbol(SymbolId id) const {
    const SymbolRecord *record = frontend.semantics.database().findSymbol(id);
    return record ? record->name : "symbol?";
  }

  std::string globalName(SymbolId id) const {
    const SymbolRecord *record = frontend.semantics.database().findSymbol(id);
    return record ? record->qualifiedName : "symbol?";
  }

  const HirClassInstance *classInstance(std::size_t id) const {
    const auto &classes = hir.classInstances();
    return id == 0 || id > classes.size() ? nullptr : &classes[id - 1];
  }

  std::string classType(std::size_t id) const {
    const HirClassInstance *instance = classInstance(id);
    return instance ? type(instance->type) : "class?";
  }

  std::string functionIdentity(HirFunctionInstanceId id) const {
    const auto &functions = hir.functionInstances();
    if (id == 0 || id > functions.size())
      return "function?";
    const HirFunctionInstance &function = functions[id - 1];
    const FunctionInfo *info =
        frontend.semantics.findFunction(function.declaration);
    std::string result =
        "function " + (info ? info->qualifiedName : std::string("?"));
    if (function.owner)
      result += " of " + classType(*function.owner);
    if (!function.typeArguments.empty())
      result += '<' + types(function.typeArguments) + '>';
    return result + '(' + types(function.parameterTypes) + ')';
  }

  std::string constructorIdentity(HirConstructorInstanceId id) const {
    const auto &constructors = hir.constructorInstances();
    if (id == 0 || id > constructors.size())
      return "constructor?";
    const HirConstructorInstance &constructor = constructors[id - 1];
    std::string result = "constructor " + classType(constructor.owner);
    if (!constructor.typeArguments.empty())
      result += '<' + types(constructor.typeArguments) + '>';
    return result + '(' + types(constructor.parameterTypes) + ')';
  }

  // A lambda's semantic signature. AST nodes are not dereferenced: procedural
  // expansions do not outlive analysis. Lambdas of one signature are told
  // apart by their ordinal in program order.
  std::string lambdaSignature(const HirLambda &lambda) const {
    std::vector<SemanticType> captures;
    for (const auto &capture : lambda.captures)
      captures.push_back(capture.type);
    return "lambda(" + types(lambda.parameterTypes) + ") -> " +
           type(lambda.returnType) + " captures(" + types(captures) + ')';
  }

  std::string lambdaIdentity(HirLambdaId id) const {
    return id == 0 || id > lambdas.size() ? "lambda?" : lambdas[id - 1];
  }

  std::string bodyIdentity(MirBodyAddress address) const {
    switch (address.kind) {
    case MirBodyKind::Module:
      return "module";
    case MirBodyKind::FieldInitializers:
      return "field-initializers " + classType(address.owner);
    case MirBodyKind::StaticFieldInitializers:
      return "static-field-initializers " + classType(address.owner);
    case MirBodyKind::Function:
      return functionIdentity(address.owner);
    case MirBodyKind::Constructor:
      return constructorIdentity(address.owner);
    case MirBodyKind::Destructor: {
      const auto &destructors = hir.destructorInstances();
      return address.owner == 0 || address.owner > destructors.size()
                 ? "destructor?"
                 : "destructor " +
                       classType(destructors[address.owner - 1].owner);
    }
    case MirBodyKind::Lambda:
      return lambdaIdentity(address.owner);
    case MirBodyKind::HostedStartup:
      return "hosted-startup";
    case MirBodyKind::ThreadLocalInitializer:
      return "thread-local " + globalName(address.owner);
    }
    return "body?";
  }

  // Borrowed state follows the verified MIR type inventory, as lowering's
  // own classification does: references and text views always borrow.
  bool carriesBorrow(const SemanticType &value) const {
    switch (value.kind) {
    case SemanticType::Reference:
    case SemanticType::StringView:
    case SemanticType::CString:
      return true;
    case SemanticType::Class:
      for (const auto &instance : mir.classInstances())
        if (instance.type == value)
          return instance.traits.containsBorrowedState;
      return false;
    case SemanticType::Enum:
      if (const auto *payload = mir.findPayloadEnum(value))
        return payload->traits.containsBorrowedState;
      return false;
    case SemanticType::Lambda:
      if (const auto *lambda = mir.findLambdaForType(value)) {
        if (lambda->lifecycleBoundBorrowCarrier)
          return true;
        for (std::size_t index = 0; index < lambda->captureModes.size();
             ++index)
          if (isBorrowingCapture(lambda->captureModes[index]) ||
              (index < lambda->captureTypes.size() &&
               carriesBorrow(lambda->captureTypes[index])))
            return true;
      }
      return false;
    case SemanticType::Array:
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
      return std::any_of(value.arguments.begin(), value.arguments.end(),
                         [&](const SemanticType &argument) {
                           return carriesBorrow(argument);
                         });
    default:
      return false;
    }
  }

  Inputs inputs(const std::vector<SemanticType> &parameters,
                bool receiver) const {
    Inputs result{.receiver = receiver};
    for (std::size_t index = 0; index < parameters.size(); ++index)
      if (carriesBorrow(parameters[index]))
        result.parameters.push_back(index);
    return result;
  }

  std::string origin(BorrowOriginKind kind, std::size_t parameter,
                     SymbolId global) const {
    switch (kind) {
    case BorrowOriginKind::Receiver:
      return "receiver";
    case BorrowOriginKind::Argument:
      return "parameter:" + std::to_string(parameter);
    case BorrowOriginKind::Global:
      return "global:" + globalName(global);
    case BorrowOriginKind::None:
      break;
    }
    return {};
  }

  std::vector<std::string> names(const std::vector<SymbolId> &fields) const {
    std::vector<std::string> result;
    for (const SymbolId field : fields)
      result.push_back(symbol(field));
    return result;
  }

  void addFields(std::set<Dependency> &set,
                 const std::vector<BorrowedFieldOrigin> &fields) const {
    for (const auto &field : fields) {
      std::string root =
          origin(field.origin, field.parameter, field.globalRoot);
      if (!root.empty())
        set.insert({std::move(root), names(field.sourceFields),
                    names(field.resultFields)});
    }
  }

  // The union of every contract representation a row records: its primary
  // origin, additional parameters, field paths and complete summary.
  std::set<Dependency> dependencies(
      BorrowOriginKind kind, std::size_t parameter,
      const std::vector<std::size_t> &parameters,
      const std::optional<BorrowOriginPlace> &place,
      const std::vector<BorrowedFieldOrigin> &fields,
      const std::optional<FunctionBorrowDependencySummary> &summary) const {
    std::set<Dependency> result;
    if (std::string root = origin(kind, parameter, place ? place->root : 0);
        !root.empty())
      result.insert({std::move(root), {}, {}});
    for (const std::size_t index : parameters)
      result.insert({"parameter:" + std::to_string(index), {}, {}});
    addFields(result, fields);
    if (summary)
      addFields(result, summary->dependencies);
    return result;
  }

  std::string row(const std::set<Dependency> &set, const Inputs &inputs) const {
    const auto list = [&](const std::vector<std::string> &values) {
      std::string text = "[";
      for (std::size_t index = 0; index < values.size(); ++index)
        text += (index == 0 ? "" : ",") + json(values[index]);
      return text + ']';
    };
    std::string text = "\"dependencies\":[";
    bool first = true;
    for (const auto &dependency : set) {
      text += first ? "" : ",";
      first = false;
      text += "{\"root\":" + json(dependency.root) +
              ",\"fields\":" + list(dependency.fields) +
              ",\"result_fields\":" + list(dependency.resultFields) + '}';
    }
    text += "],\"reference_inputs\":{\"parameters\":[";
    for (std::size_t index = 0; index < inputs.parameters.size(); ++index)
      text +=
          (index == 0 ? "" : ",") + std::to_string(inputs.parameters[index]);
    return text + "],\"receiver\":" + (inputs.receiver ? "true" : "false") +
           '}';
  }

  struct Callee {
    std::string identity;
    Inputs inputs;
  };

  Callee callee(const MirInstruction &instruction) const {
    if (instruction.constructorTarget) {
      const auto *constructor =
          mir.findConstructorInstance(*instruction.constructorTarget);
      return {constructorIdentity(*instruction.constructorTarget),
              inputs(constructor ? constructor->parameterTypes
                                 : instruction.parameterTypes,
                     false)};
    }
    if (instruction.functionTarget) {
      const auto *function =
          mir.findFunctionInstance(*instruction.functionTarget);
      return {functionIdentity(*instruction.functionTarget),
              inputs(function ? function->parameterTypes
                              : instruction.parameterTypes,
                     function && function->owner && !function->staticMember)};
    }
    if (instruction.lambdaTarget) {
      const auto *lambda = mir.findLambda(*instruction.lambdaTarget);
      return {
          lambdaIdentity(*instruction.lambdaTarget),
          inputs(lambda ? lambda->parameterTypes : instruction.parameterTypes,
                 lambda && lambda->lifecycleBoundBorrowCarrier)};
    }
    return {
        "indirect " + type(instruction.receiver ? instruction.receiver->type
                                                : SemanticType{}),
        inputs(instruction.parameterTypes, instruction.receiver.has_value())};
  }

  static const char *callKind(MirInstructionKind kind) {
    switch (kind) {
    case MirInstructionKind::Call:
      return "call";
    case MirInstructionKind::Construct:
      return "construct";
    case MirInstructionKind::CallBody:
      return "call-body";
    default:
      return nullptr;
    }
  }

  void collectCalls(MirBodyAddress address, const MirBody &body,
                    std::vector<std::string> &calls) const {
    const std::string bodyName = bodyIdentity(address);
    std::map<std::string, std::size_t> ordinals;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const char *kind = callKind(instruction.kind);
        if (kind == nullptr || instruction.tombstone || !instruction.result)
          continue;
        const MirValue *result = body.findValue(*instruction.result);
        const bool contract =
            instruction.borrowOrigin != BorrowOriginKind::None ||
            !instruction.borrowArguments.empty() ||
            !instruction.borrowFields.empty();
        if (!contract && !(result && carriesBorrow(result->info.type)))
          continue;
        const Callee target = callee(instruction);
        const std::size_t ordinal = ordinals[target.identity]++;
        const auto set =
            dependencies(instruction.borrowOrigin, instruction.borrowArgument,
                         instruction.borrowArguments, instruction.borrowPlace,
                         instruction.borrowFields, std::nullopt);
        calls.push_back(
            "{\"body\":" + json(bodyName) +
            ",\"ordinal\":" + std::to_string(ordinal) +
            ",\"callee\":" + json(target.identity) + ",\"kind\":" + json(kind) +
            ",\"hir_value\":" + std::to_string(instruction.hirValue) + ',' +
            row(set, target.inputs) + '}');
      }
    }
  }

  std::vector<std::string> collectContracts() const {
    std::vector<std::string> contracts;
    for (const MirFunctionInstance &function : mir.functionInstances()) {
      const auto set = dependencies(
          function.returnBorrowOrigin, function.returnBorrowParameter,
          function.returnBorrowParameters, function.returnBorrowPlace,
          function.returnBorrowFields, function.completeReturnBorrow);
      if (set.empty() && !carriesBorrow(function.returnType))
        continue;
      contracts.push_back(
          "{\"callee\":" + json(functionIdentity(function.id)) +
          ",\"kind\":\"function\",\"explicit\":" +
          (function.explicitReturnBorrowOrigin ? "true" : "false") + ',' +
          row(set, inputs(function.parameterTypes,
                          function.owner && !function.staticMember)) +
          '}');
    }
    for (const MirConstructorInstance &constructor :
         mir.constructorInstances()) {
      const auto set =
          dependencies(constructor.borrowOrigin, constructor.borrowParameter,
                       constructor.borrowParameters, std::nullopt, {},
                       constructor.completeBorrow);
      if (set.empty())
        continue;
      contracts.push_back(
          "{\"callee\":" + json(constructorIdentity(constructor.id)) +
          ",\"kind\":\"constructor\",\"explicit\":false," +
          row(set, inputs(constructor.parameterTypes, false)) + '}');
    }
    return contracts;
  }

  const FrontendResult &frontend;
  const MirProgram &mir;
  const HirProgram &hir;
  SemanticTypePrinter printer;
  std::vector<std::string> lambdas;
};

} // namespace

std::string borrowDependencyReport(const FrontendResult &frontend) {
  return Reporter(frontend).report();
}

} // namespace lang::driver
