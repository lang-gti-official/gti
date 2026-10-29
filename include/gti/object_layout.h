#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace lang {

// Target storage facts, not a cross-version or foreign ABI promise. The same
// checked arithmetic serves early constant queries and native representation
// verification; all sizes include the padding required for array stride.
struct ObjectLayout {
  std::uint64_t size = 0;
  std::uint64_t alignment = 1;
  bool valid = true;

  friend bool operator==(const ObjectLayout &, const ObjectLayout &) = default;
};

inline ObjectLayout alignObjectExtent(ObjectLayout layout) {
  if (!layout.valid || layout.alignment == 0 ||
      (layout.alignment & (layout.alignment - 1)) != 0)
    return {.valid = false};
  const auto padding =
      (layout.alignment - layout.size % layout.alignment) % layout.alignment;
  if (padding > std::numeric_limits<std::uint64_t>::max() - layout.size)
    return {.valid = false};
  layout.size += padding;
  return layout;
}

inline ObjectLayout objectArrayLayout(ObjectLayout element,
                                      std::uint64_t count) {
  if (!element.valid ||
      (element.size &&
       count > std::numeric_limits<std::uint64_t>::max() / element.size))
    return {.valid = false};
  element.size *= count;
  return element;
}

inline ObjectLayout
objectRecordLayout(std::span<const ObjectLayout> fields,
                   std::vector<std::uint64_t> *offsets = nullptr) {
  ObjectLayout result;
  for (const auto field : fields) {
    if (!field.valid)
      return {.valid = false};
    auto position = alignObjectExtent({result.size, field.alignment});
    if (!position.valid ||
        field.size > std::numeric_limits<std::uint64_t>::max() - position.size)
      return {.valid = false};
    if (offsets)
      offsets->push_back(position.size);
    result.size = position.size + field.size;
    result.alignment = std::max(result.alignment, field.alignment);
  }
  return alignObjectExtent(result);
}

inline ObjectLayout objectUnionLayout(std::span<const ObjectLayout> fields) {
  ObjectLayout result;
  for (const auto field : fields) {
    if (!field.valid)
      return {.valid = false};
    result.size = std::max(result.size, field.size);
    result.alignment = std::max(result.alignment, field.alignment);
  }
  return alignObjectExtent(result);
}

} // namespace lang
