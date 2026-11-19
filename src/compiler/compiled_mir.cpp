#include "gti/compiled_mir.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace lang {
namespace {
using T = SemanticType;
using Op = MirOperation;
constexpr unsigned maxSteps = 192;
constexpr std::string_view magic = "GTIMIR01";

bool integer(T::Kind t) {
  return t == T::Int32 || t == T::UInt32 || t == T::Int64 || t == T::UInt64;
}
bool numeric(T::Kind t) {
  return integer(t) || t == T::Float || t == T::Double;
}
bool scalar(const T &t) {
  return t == T(t.kind) && (numeric(t.kind) || t.kind == T::Bool);
}

bool supportedParameters(std::span<const T> parameters) {
  unsigned references = 0;
  for (const auto &t : parameters) {
    if (scalar(t))
      continue;
    if (t.kind != T::Reference || t.arguments.size() != 1 || ++references > 1)
      return false;
    auto &target = t.arguments.front();
    if (!numeric(target.kind) && target.kind != T::Class)
      return false;
  }
  return true;
}
std::optional<CompiledMirField>
cellType(unsigned cell, std::span<const T> parameters,
         std::span<const CompiledMirField> fields) {
  for (const auto &parameter : parameters) {
    if (parameter.kind != T::Reference)
      continue;
    const auto &target = parameter.arguments.front();
    const bool mut = parameter.referenceAccess == AccessMode::Mutable;
    if (!cell && numeric(target.kind))
      return CompiledMirField{target, mut};
    if (target.kind == T::Class && cell && cell <= fields.size() &&
        numeric(fields[cell - 1].type.kind))
      return CompiledMirField{fields[cell - 1].type,
                              mut && fields[cell - 1].writable};
  }
  return {};
}
const MirClassInstance *referenceRecord(const MirProgram &program,
                                        std::span<const T> parameters) {
  for (const auto &p : parameters) {
    if (p.kind != T::Reference || p.arguments.size() != 1 ||
        p.arguments.front().kind != T::Class)
      continue;
    for (const auto &c : program.classInstances())
      if (c.type == p.arguments.front() && c.bases.empty())
        return &c;
  }
  return nullptr;
}

bool validStep(const CompiledMirStep &s, std::span<const T> types) {
  if (!scalar(s.type) || s.inputs.size() > 2 ||
      std::any_of(s.inputs.begin(), s.inputs.end(),
                  [&](unsigned i) { return i >= types.size(); }))
    return false;
  if (s.operation == Op::Literal && s.intrinsic == IntrinsicKind::None)
    return s.inputs.empty() && (s.type != T::Bool || s.literalBits <= 1) &&
           ((s.type != T::Float && s.type != T::UInt32 && s.type != T::Int32) ||
            s.literalBits <= UINT32_MAX);
  if (s.literalBits || s.inputs.empty())
    return false;
  const auto a = types[s.inputs[0]].kind;
  if (s.intrinsic != IntrinsicKind::None) {
    auto intrinsic = integerArithmeticIntrinsic(s.intrinsic);
    return s.operation == Op::None && intrinsic &&
           intrinsic->mode == IntegerArithmeticMode::Wrapping &&
           s.inputs.size() == 2 && integer(a) && types[s.inputs[1]] == a &&
           s.type == a;
  }
  if (s.inputs.size() == 1) {
    switch (s.operation) {
    case Op::Identity:
      return s.type == a;
    case Op::Convert:
      return numeric(a) && numeric(s.type);
    case Op::Positive:
    case Op::Negate:
      return numeric(a) && s.type == a;
    case Op::BitwiseNot:
      return integer(a) && s.type == a;
    case Op::LogicalNot:
      return a == T::Bool && s.type == T::Bool;
    default:
      return false;
    }
  }
  if (types[s.inputs[1]] != a)
    return false;
  switch (s.operation) {
  case Op::Equal:
  case Op::NotEqual:
  case Op::Less:
  case Op::LessEqual:
  case Op::Greater:
  case Op::GreaterEqual:
    return numeric(a) && s.type == T::Bool;
  case Op::Add:
  case Op::Subtract:
  case Op::Multiply:
  case Op::Divide:
    return numeric(a) && a == s.type;
  case Op::Remainder:
  case Op::BitwiseAnd:
  case Op::BitwiseOr:
  case Op::BitwiseXor:
  case Op::ShiftLeft:
  case Op::ShiftRight:
    return integer(a) && a == s.type;
  default:
    return false;
  }
}

std::optional<std::uint64_t> literalBits(const Literal &literal) {
  if (auto *i = std::get_if<std::uint64_t>(&literal))
    return *i;
  if (auto *f = std::get_if<BinaryFloat>(&literal))
    return f->bits;
  if (auto *b = std::get_if<bool>(&literal))
    return *b;
  return {};
}

// Session-specific declaration IDs, binding IDs and original MIR value IDs
// never enter the wire format. Limits bound both projection cost and import.
class Projection {
public:
  Projection(const MirProgram &program, const MirFunctionInstance &f,
             unsigned &budget, unsigned depth)
      : program(program), function(f), budget(budget), depth(depth),
        record(referenceRecord(program, f.parameterTypes)) {}
  std::shared_ptr<const CompiledMirBody> build() {
    const auto &body = function.body;
    if (depth > 6 || function.definitionKind != MirDefinitionKind::Source ||
        function.owner || function.requiresUnsafe || function.tracksCaller ||
        function.neverReturns || function.codegenAttributes.cold ||
        function.codegenAttributes.inlining == FunctionInlining::Never ||
        function.parameterBindings.size() != function.parameterTypes.size() ||
        !scalar(function.returnType) ||
        !supportedParameters(function.parameterTypes) ||
        !body.dropObligations.empty() || body.blocks.size() > maxSteps)
      return {};
    for (unsigned i = 0; i < function.parameterBindings.size(); ++i)
      bindings.emplace(function.parameterBindings[i], i);
    std::set<MirBlockId> visited;
    auto next = body.entry;
    while (const auto *block = body.findBlock(next)) {
      if (!visited.insert(next).second || !block->parameters.empty())
        return {};
      for (const auto &i : block->instructions) {
        if (budget == 0)
          return {};
        --budget;
        if (!i.tombstone && !project(i))
          return {};
      }
      const auto &term = block->terminator;
      if (term.kind == MirTerminatorKind::Return && term.value) {
        const auto returned = operand(*term.value);
        if (!returned)
          return {};
        result.returned = *returned;
        if (!verifyCompiledMir(
                result, function.parameterTypes, function.returnType,
                compiledMirFields(program, function.parameterTypes)))
          return {};
        return std::make_shared<const CompiledMirBody>(std::move(result));
      }
      if (term.kind != MirTerminatorKind::Goto &&
          term.kind != MirTerminatorKind::Invoke)
        return {};
      // A guard transfers to the untouched native body, which reproduces the
      // original failure site. This profile never touches observable storage.
      if (!term.targetArguments.empty())
        return {};
      next = term.target;
    }
    return {};
  }

private:
  const MirProgram &program;
  const MirFunctionInstance &function;
  unsigned &budget;
  unsigned depth;
  const MirClassInstance *record;
  CompiledMirBody result;
  std::unordered_map<MirValueId, unsigned> values;
  std::unordered_map<HirBindingId, unsigned> bindings;

  const MirPlace *binding(MirPlaceId id) const {
    const auto *p = function.body.findPlace(id);
    return p && p->root == MirPlaceRootKind::Binding && p->binding &&
                   p->projections.empty() && !p->foreignStorage &&
                   scalar(p->type)
               ? p
               : nullptr;
  }
  std::optional<unsigned> cell(MirPlaceId id) const {
    const auto *p = function.body.findPlace(id);
    if (!p || p->root != MirPlaceRootKind::Binding || !p->binding ||
        p->foreignStorage || p->projections.empty() ||
        p->projections[0].kind != MirProjectionKind::Dereference ||
        !numeric(p->type.kind))
      return {};
    auto formal = std::find(function.parameterBindings.begin(),
                            function.parameterBindings.end(), p->binding);
    if (formal == function.parameterBindings.end())
      return {};
    const auto &parameter =
        function.parameterTypes[formal - function.parameterBindings.begin()];
    if (parameter.kind != T::Reference)
      return {};
    if (p->projections.size() == 1 && p->type == parameter.arguments.front())
      return 0;
    if (p->projections.size() != 2 ||
        p->projections[1].kind != MirProjectionKind::Field || !record)
      return {};
    for (unsigned i = 0; i < record->declaredFields.size(); ++i)
      if (record->declaredFields[i].symbol == p->projections[1].field &&
          record->declaredFields[i].type == p->type)
        return i + 1;
    return {};
  }
  std::optional<unsigned> operand(const MirOperand &o) {
    if (o.kind == MirOperandKind::Value) {
      auto it = values.find(o.value);
      if (it != values.end())
        return it->second;
    } else if (o.kind == MirOperandKind::Copy) {
      if (auto c = cell(o.place)) {
        if (auto it = result.writes.find(*c); it != result.writes.end())
          return it->second;
        if (result.steps.size() == maxSteps)
          return {};
        unsigned slot = function.parameterTypes.size() + result.steps.size();
        CompiledMirStep step;
        step.type = o.type.kind;
        step.loadedCell = c;
        result.steps.push_back(std::move(step));
        return slot;
      }
      if (auto *p = binding(o.place)) {
        auto it = bindings.find(p->binding);
        if (it != bindings.end())
          return it->second;
      }
    }
    return {};
  }
  bool inlineCall(const MirInstruction &i) {
    if (!i.result || !i.functionTarget || i.receiver ||
        i.dispatch != CallDispatch::Static)
      return false;
    const auto *callee = program.findFunctionInstance(*i.functionTarget);
    if (!callee || callee->returnType != i.info.type ||
        callee->parameterTypes.size() != i.operands.size() ||
        !std::all_of(callee->parameterTypes.begin(),
                     callee->parameterTypes.end(), scalar))
      return false;
    auto projected = Projection(program, *callee, budget, depth + 1).build();
    if (!projected)
      return false;
    std::vector<unsigned> slots;
    for (unsigned k = 0; k < i.operands.size(); ++k) {
      auto slot = operand(i.operands[k]);
      if (!slot || i.operands[k].type != callee->parameterTypes[k])
        return false;
      slots.push_back(*slot);
    }
    if (result.steps.size() > maxSteps ||
        projected->steps.size() > maxSteps - result.steps.size())
      return false;
    for (auto step : projected->steps) {
      for (auto &arg : step.inputs)
        arg = slots.at(arg);
      slots.push_back(function.parameterTypes.size() + result.steps.size());
      result.steps.push_back(std::move(step));
    }
    values.emplace(*i.result, slots.at(projected->returned));
    return true;
  }
  bool project(const MirInstruction &i) {
    if (i.kind == MirInstructionKind::Call &&
        i.intrinsic == IntrinsicKind::None)
      return inlineCall(i);
    if (i.kind == MirInstructionKind::EndBorrow)
      return true;
    if (i.kind == MirInstructionKind::Lifecycle)
      return std::none_of(
          i.lifecycle.begin(), i.lifecycle.end(),
          [](const auto &e) { return e.kind == MirLifecycleEventKind::Drop; });
    if (i.kind == MirInstructionKind::Load ||
        i.kind == MirInstructionKind::CallInput ||
        i.kind == MirInstructionKind::Initialize ||
        i.kind == MirInstructionKind::Assign) {
      if (i.operands.size() != 1)
        return false;
      auto value = operand(i.operands.front());
      if (!value)
        return false;
      if (i.kind == MirInstructionKind::Initialize ||
          i.kind == MirInstructionKind::Assign) {
        if (!i.destination ||
            (i.kind == MirInstructionKind::Assign && i.operation != Op::Assign))
          return false;
        if (auto c = cell(*i.destination)) {
          if (function.body.findPlace(*i.destination)->type !=
              i.operands.front().type)
            return false;
          result.writes[*c] = *value;
        } else {
          auto *p = binding(*i.destination);
          if (!p || p->type != i.operands.front().type)
            return false;
          bindings[p->binding] = *value;
        }
      }
      if (i.result)
        values[*i.result] = *value;
      return true;
    }
    const bool numericCall =
        i.kind == MirInstructionKind::Call &&
        integerArithmeticIntrinsic(i.intrinsic).has_value();
    if ((i.kind != MirInstructionKind::Compute && !numericCall) || !i.result ||
        !scalar(i.info.type) || result.steps.size() == maxSteps)
      return false;
    CompiledMirStep step{i.operation, i.intrinsic, i.info.type.kind, {}, 0};
    if (i.literal) {
      auto bits = literalBits(*i.literal);
      if (!bits || i.operation != Op::Literal)
        return false;
      step.literalBits = *bits;
    } else if (i.operation == Op::Literal)
      return false;
    for (const auto &o : i.operands) {
      auto value = operand(o);
      if (!value)
        return false;
      step.inputs.push_back(*value);
    }
    values.emplace(*i.result,
                   function.parameterTypes.size() + result.steps.size());
    result.steps.push_back(std::move(step));
    return true;
  }
};

void word(std::string &out, std::uint32_t v) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<char>(v >> shift));
}
struct Reader {
  std::string_view data;
  [[noreturn]] static void invalid() {
    throw std::invalid_argument("invalid compiled optimization MIR");
  }
  std::string_view take(unsigned length) {
    if (length > data.size())
      invalid();
    auto part = data.substr(0, length);
    data.remove_prefix(length);
    return part;
  }
  unsigned word() {
    const auto part = take(4);
    unsigned v = 0;
    for (unsigned i = 0; i < 4; ++i)
      v |= unsigned(static_cast<unsigned char>(part[i])) << (8 * i);
    return v;
  }
};
} // namespace

bool verifyCompiledMir(const CompiledMirBody &body,
                       std::span<const T> parameters, const T &result,
                       std::span<const CompiledMirField> fields) {
  if (!scalar(result) || !supportedParameters(parameters) ||
      body.steps.size() > maxSteps)
    return false;
  std::vector<T> types(parameters.begin(), parameters.end());
  for (const auto &step : body.steps) {
    if (step.loadedCell) {
      auto cell = cellType(*step.loadedCell, parameters, fields);
      if (!cell || cell->type != step.type || step.operation != Op::None ||
          step.intrinsic != IntrinsicKind::None || !step.inputs.empty() ||
          step.literalBits)
        return false;
    } else if (!validStep(step, types))
      return false;
    types.emplace_back(step.type);
  }
  for (auto [index, slot] : body.writes) {
    auto cell = cellType(index, parameters, fields);
    if (!cell || !cell->writable || slot >= types.size() ||
        types[slot] != cell->type)
      return false;
  }
  return body.returned < types.size() && types[body.returned] == result;
}
std::vector<CompiledMirField> compiledMirFields(const MirProgram &program,
                                                std::span<const T> parameters) {
  std::vector<CompiledMirField> fields;
  if (const auto *record = referenceRecord(program, parameters))
    for (const auto &field : record->declaredFields)
      fields.push_back({field.type, field.access == AccessMode::Mutable});
  return fields;
}
std::shared_ptr<const CompiledMirBody>
projectCompiledMir(const MirProgram &program, const MirFunctionInstance &f) {
  unsigned budget = 2048;
  return Projection(program, f, budget, 0).build();
}
std::string encodeCompiledMir(const CompiledMirBodies &bodies) {
  if (bodies.empty())
    return {};
  if (bodies.size() > 256)
    Reader::invalid();
  std::string out(magic);
  word(out, bodies.size());
  for (const auto &[key, body] : bodies) {
    if (key.empty() || key.size() > 255 || !body ||
        body->steps.size() > maxSteps || body->writes.size() > maxSteps)
      Reader::invalid();
    word(out, key.size());
    out += key;
    word(out, body->steps.size());
    for (const auto &s : body->steps) {
      if (s.inputs.size() > 2)
        Reader::invalid();
      word(out, static_cast<unsigned>(s.operation));
      word(out, static_cast<unsigned>(s.intrinsic));
      word(out, static_cast<unsigned>(s.type));
      word(out, s.literalBits);
      word(out, s.literalBits >> 32);
      word(out, s.loadedCell.value_or(UINT32_MAX));
      word(out, s.inputs.size());
      for (auto v : s.inputs)
        word(out, v);
    }
    word(out, body->returned);
    word(out, body->writes.size());
    for (auto [cell, value] : body->writes) {
      word(out, cell);
      word(out, value);
    }
  }
  return out;
}
CompiledMirBodies decodeCompiledMir(std::string_view bytes) {
  CompiledMirBodies bodies;
  if (bytes.empty())
    return bodies;
  Reader in{bytes};
  if (in.take(magic.size()) != magic)
    Reader::invalid();
  auto count = in.word();
  if (!count || count > 256)
    Reader::invalid();
  for (unsigned i = 0; i < count; ++i) {
    auto keySize = in.word();
    if (keySize > 255)
      Reader::invalid();
    auto key = in.take(keySize);
    if (key.empty() || (!bodies.empty() && key <= bodies.rbegin()->first))
      Reader::invalid();
    auto body = std::make_shared<CompiledMirBody>();
    auto n = in.word();
    if (n > maxSteps)
      Reader::invalid();
    for (unsigned j = 0; j < n; ++j) {
      auto op = in.word(), intrinsic = in.word(), type = in.word();
      if (op >= static_cast<unsigned>(Op::Count) ||
          intrinsic >
              static_cast<unsigned>(IntrinsicKind::IntegerWrappingMultiply) ||
          type > static_cast<unsigned>(T::Bool))
        Reader::invalid();
      CompiledMirStep step{static_cast<Op>(op),
                           static_cast<IntrinsicKind>(intrinsic),
                           static_cast<T::Kind>(type),
                           {},
                           0};
      step.literalBits = in.word();
      step.literalBits |= std::uint64_t(in.word()) << 32;
      auto cell = in.word();
      if (cell != UINT32_MAX)
        step.loadedCell = cell;
      auto arity = in.word();
      if (arity > 2)
        Reader::invalid();
      for (unsigned k = 0; k < arity; ++k)
        step.inputs.push_back(in.word());
      body->steps.push_back(std::move(step));
    }
    body->returned = in.word();
    auto writes = in.word();
    if (writes > maxSteps)
      Reader::invalid();
    for (unsigned j = 0; j < writes; ++j) {
      auto cell = in.word();
      if (!body->writes.empty() && cell <= body->writes.rbegin()->first)
        Reader::invalid();
      body->writes.emplace(cell, in.word());
    }
    bodies.emplace(key, std::move(body));
  }
  if (!in.data.empty())
    Reader::invalid();
  return bodies;
}
} // namespace lang
