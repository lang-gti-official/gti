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
  explicit Projection(const MirFunctionInstance &f) : function(f) {}
  std::shared_ptr<const CompiledMirBody> build() {
    const auto &body = function.body;
    if (function.definitionKind != MirDefinitionKind::Source ||
        function.owner || function.requiresUnsafe || function.tracksCaller ||
        function.neverReturns || function.codegenAttributes.cold ||
        function.codegenAttributes.inlining == FunctionInlining::Never ||
        function.parameterBindings.size() != function.parameterTypes.size() ||
        !scalar(function.returnType) ||
        !std::all_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(), scalar) ||
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
        if (!i.tombstone && !project(i))
          return {};
      }
      const auto &term = block->terminator;
      if (term.kind == MirTerminatorKind::Return && term.value) {
        const auto returned = operand(*term.value);
        if (!returned)
          return {};
        result.returned = *returned;
        if (!verifyCompiledMir(result, function.parameterTypes,
                               function.returnType))
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
  const MirFunctionInstance &function;
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
  std::optional<unsigned> operand(const MirOperand &o) const {
    if (o.kind == MirOperandKind::Value) {
      auto it = values.find(o.value);
      if (it != values.end())
        return it->second;
    } else if (o.kind == MirOperandKind::Copy) {
      if (auto *p = binding(o.place)) {
        auto it = bindings.find(p->binding);
        if (it != bindings.end())
          return it->second;
      }
    }
    return {};
  }
  bool project(const MirInstruction &i) {
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
        auto *p = binding(*i.destination);
        if (!p || p->type != i.operands.front().type)
          return false;
        bindings[p->binding] = *value;
      }
      if (i.result)
        values[*i.result] = *value;
      return true;
    }
    if (i.kind != MirInstructionKind::Compute || !i.result ||
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
                       std::span<const T> parameters, const T &result) {
  if (!scalar(result) ||
      !std::all_of(parameters.begin(), parameters.end(), scalar) ||
      body.steps.size() > maxSteps)
    return false;
  std::vector<T> types(parameters.begin(), parameters.end());
  for (const auto &step : body.steps) {
    if (!validStep(step, types))
      return false;
    types.emplace_back(step.type);
  }
  return body.returned < types.size() && types[body.returned] == result;
}
std::shared_ptr<const CompiledMirBody>
projectCompiledMir(const MirFunctionInstance &f) {
  return Projection(f).build();
}
std::string encodeCompiledMir(const CompiledMirBodies &bodies) {
  if (bodies.empty())
    return {};
  std::string out(magic);
  word(out, bodies.size());
  for (const auto &[key, body] : bodies) {
    word(out, key.size());
    out += key;
    word(out, body->steps.size());
    for (const auto &s : body->steps) {
      word(out, static_cast<unsigned>(s.operation));
      word(out, static_cast<unsigned>(s.intrinsic));
      word(out, static_cast<unsigned>(s.type));
      word(out, s.literalBits);
      word(out, s.literalBits >> 32);
      word(out, s.inputs.size());
      for (auto v : s.inputs)
        word(out, v);
    }
    word(out, body->returned);
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
    auto key = in.take(in.word());
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
      auto arity = in.word();
      if (arity > 2)
        Reader::invalid();
      for (unsigned k = 0; k < arity; ++k)
        step.inputs.push_back(in.word());
      body->steps.push_back(std::move(step));
    }
    body->returned = in.word();
    bodies.emplace(key, std::move(body));
  }
  if (!in.data.empty())
    Reader::invalid();
  return bodies;
}
} // namespace lang
