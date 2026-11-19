// Reusable MIR uses typed previous values and can commit only to admitted
// mutable storage.
#include "gti/compiled_mir.h"
#include "unit_test.h"

#include <array>
#include <stdexcept>

GTI_UNIT_TEST(reusable_library_mir) {
  using namespace lang;
  using T = SemanticType;
  const std::array<T, 2> integers{T::Int32, T::Int32};
  const auto accepts = [&](const CompiledMirBody &body,
                           std::span<const T> parameters) {
    return verifyCompiledMir(body, parameters, T::Int32, {});
  };
  CompiledMirBody sum;
  sum.steps.push_back(
      {MirOperation::Add, IntrinsicKind::None, T::Int32, {0, 1}});
  sum.returned = 2;
  if (!accepts(sum, integers))
    throw std::runtime_error("valid scalar MIR rejected");
  const auto bytes = encodeCompiledMir(
      {{"sum", std::make_shared<const CompiledMirBody>(sum)}});
  const auto roundtrip = decodeCompiledMir(bytes);
  if (encodeCompiledMir(roundtrip) != bytes ||
      !accepts(*roundtrip.at("sum"), integers))
    throw std::runtime_error("MIR round trip changed the body");
  for (std::size_t i = 1; i < bytes.size(); ++i) {
    try {
      (void)decodeCompiledMir(std::string_view(bytes).substr(0, i));
    } catch (const std::invalid_argument &) {
      continue;
    }
    throw std::runtime_error("accepted truncated MIR");
  }
  auto bad = sum;
  bad.steps[0].inputs = {0, 2};
  if (accepts(bad, integers))
    throw std::runtime_error("accepted forward value use");
  bad = sum;
  bad.steps[0].operation = MirOperation::PointerAdd;
  if (accepts(bad, integers))
    throw std::runtime_error("accepted non-scalar operation");
  bad = sum;
  bad.steps[0].type = T::Float;
  if (accepts(bad, integers))
    throw std::runtime_error("accepted wrong operation result");
  bad = sum;
  bad.returned = 17;
  if (accepts(bad, integers))
    throw std::runtime_error("accepted unknown return slot");

  std::array<T, 2> borrowed{T::referenceTo(T::Int32, AccessMode::Mutable),
                            T::Int32};
  CompiledMirBody store;
  store.returned = 1;
  store.writes[0] = 1;
  if (!accepts(store, borrowed))
    throw std::runtime_error("valid scalar store rejected");
  borrowed[0] = T::referenceTo(T::Int32, AccessMode::ReadOnly);
  if (accepts(store, borrowed))
    throw std::runtime_error("accepted write to readonly reference");
  borrowed[0] = T::referenceTo(T::Int32, AccessMode::Mutable);
  store.writes.clear();
  store.writes[1] = 1;
  if (accepts(store, borrowed))
    throw std::runtime_error("accepted unknown field");
  store.writes.clear();
  store.writes[0] = 0;
  if (accepts(store, borrowed))
    throw std::runtime_error("accepted reference as numeric value");

  const std::array<T, 2> byteParameters{T::UInt8, T::UInt8};
  CompiledMirBody predicate;
  predicate.steps.push_back({MirOperation::None,
                             IntrinsicKind::IntegerAddOverflows,
                             T::Bool,
                             {0, 1}});
  predicate.returned = 2;
  if (!verifyCompiledMir(predicate, byteParameters, T::Bool, {}))
    throw std::runtime_error("rejected byte overflow predicate");
  predicate.steps[0].type = T::UInt8;
  if (verifyCompiledMir(predicate, byteParameters, T::UInt8, {}))
    throw std::runtime_error("accepted integer result for overflow predicate");
  CompiledMirBody byteLiteral;
  byteLiteral.steps.push_back(
      {MirOperation::Literal, IntrinsicKind::None, T::UInt8, {}, 255});
  if (!verifyCompiledMir(byteLiteral, {}, T::UInt8, {}))
    throw std::runtime_error("rejected maximum byte literal");
  byteLiteral.steps.front().literalBits = 256;
  if (verifyCompiledMir(byteLiteral, {}, T::UInt8, {}))
    throw std::runtime_error("accepted truncated byte literal");
  return 0;
}
