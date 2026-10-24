// Tests the exact backend helper emitted by the compiler, not a copied model.
#include GTI_TEST_GENERATED_PATH

#include <cassert>
#include <stdexcept>

namespace {
using gti_internal::backend::payload_get;
using gti_internal::backend::payload_variant;

struct tracked {
  static inline int live = 0;
  static inline int constructed = 0;
  static inline int destroyed = 0;
  static inline int failure = 0;
  std::unique_ptr<int> value;
  explicit tracked(int input) {
    if (failure == 1)
      throw std::runtime_error("construct");
    value = std::make_unique<int>(input);
    ++live;
    ++constructed;
  }
  tracked(const tracked &other) {
    if (failure == 2)
      throw std::runtime_error("copy");
    value = std::make_unique<int>(*other.value);
    ++live;
    ++constructed;
  }
  tracked(tracked &&other) {
    if (failure == 3)
      throw std::runtime_error("move");
    value = std::move(other.value);
    ++live;
    ++constructed;
  }
  tracked &operator=(const tracked &other) {
    if (failure == 4)
      throw std::runtime_error("assign");
    value = std::make_unique<int>(*other.value);
    return *this;
  }
  tracked &operator=(tracked &&other) {
    if (failure == 4)
      throw std::runtime_error("assign");
    value = std::move(other.value);
    return *this;
  }
  ~tracked() {
    --live;
    ++destroyed;
  }
  friend bool operator==(const tracked &a, const tracked &b) {
    return (!a.value && !b.value) ||
           (a.value && b.value && *a.value == *b.value);
  }
};
struct alignas(128) aligned {
  int value;
};
struct unit {};
struct trivial_move_only {
  int value;
  explicit trivial_move_only(int input) : value(input) {}
  trivial_move_only(const trivial_move_only &) = delete;
  trivial_move_only(trivial_move_only &&) = default;
  trivial_move_only &operator=(const trivial_move_only &) = delete;
  trivial_move_only &operator=(trivial_move_only &&) = default;
};
struct partial {
  tracked first;
  tracked second;
  static int fail_input() {
    tracked::failure = 1;
    return 0;
  }
  partial() : first(31), second(fail_input()) {}
};
using carrier = payload_variant<tracked, tracked, unit, std::monostate>;
using passive = payload_variant<int, unit, std::monostate>;
static_assert(std::is_trivially_copy_constructible_v<passive>);
static_assert(std::is_trivially_move_constructible_v<passive>);
static_assert(std::is_trivially_copy_assignable_v<passive>);
static_assert(std::is_trivially_move_assignable_v<passive>);
static_assert(std::is_trivially_destructible_v<passive>);
static_assert(std::is_trivially_copyable_v<passive>);
using passive_move_only = payload_variant<trivial_move_only, std::monostate>;
static_assert(!std::is_copy_constructible_v<passive_move_only>);
static_assert(std::is_trivially_move_constructible_v<passive_move_only>);
static_assert(!std::is_copy_assignable_v<passive_move_only>);
static_assert(std::is_trivially_move_assignable_v<passive_move_only>);
static_assert(std::is_trivially_destructible_v<passive_move_only>);
static_assert(!std::is_copy_constructible_v<
              payload_variant<std::unique_ptr<int>, std::monostate>>);
static_assert(std::is_move_constructible_v<
              payload_variant<std::unique_ptr<int>, std::monostate>>);
static_assert(!std::is_copy_assignable_v<
              payload_variant<std::unique_ptr<int>, std::monostate>>);
static_assert(alignof(payload_variant<aligned, std::monostate>) == 128);

template <typename F> void throws(F &&operation) {
  bool caught = false;
  try {
    operation();
  } catch (const std::runtime_error &) {
    caught = true;
  }
  assert(caught);
  tracked::failure = 0;
}

void test_lifetimes() {
  // Default construction selects the final, backend-private empty shell;
  // source alternatives keep their unshifted ordinals.
  carrier empty;
  assert(empty.index() == 3);
  carrier first(std::in_place_index<0>, 7);
  carrier duplicate(std::in_place_index<1>, 9);
  assert(first.index() == 0 && duplicate.index() == 1);
  assert(*payload_get<0>(first).value == 7);
  assert(*payload_get<1>(duplicate).value == 9);
  carrier copied(first);
  assert(*payload_get<0>(copied).value == 7);
  carrier moved(std::move(first));
  assert(first.index() == 0 && !payload_get<0>(first).value);
  assert(*payload_get<0>(moved).value == 7);
  duplicate = copied; // Different active index, including the same native type.
  assert(duplicate.index() == 0 && *payload_get<0>(duplicate).value == 7);
  copied = std::move(moved); // Same index: assignment retains one lifetime.
  assert(*payload_get<0>(copied).value == 7);
  copied = copied;
  copied = std::move(copied);
  assert(*payload_get<0>(copied).value == 7);
  duplicate = carrier(std::in_place_index<2>);
  assert(duplicate.index() == 2);
  empty = copied;
  assert(*payload_get<0>(empty).value == 7);
}

void test_failure() {
  carrier source(std::in_place_index<1>, 11);
  carrier target(std::in_place_index<0>, 13);
  const int live = tracked::live;
  tracked::failure = 1;
  throws([] { carrier failed(std::in_place_index<1>, 0); });
  assert(tracked::live == live);
  tracked::failure = 2;
  throws([&] { carrier failed(source); });
  assert(tracked::live == live && *payload_get<1>(source).value == 11);
  tracked::failure = 3;
  throws([&] { carrier failed(std::move(source)); });
  assert(tracked::live == live && *payload_get<1>(source).value == 11);
  tracked::failure = 2;
  throws([&] { target = source; });
  assert(target.index() == 255 && tracked::live == live - 1);
  carrier vacantCopy(target);
  carrier vacantMove(std::move(vacantCopy));
  assert(vacantCopy.index() == 255 && vacantMove.index() == 255);
  target = source; // Reconstruct after failed cross-alternative publication.
  assert(target.index() == 1 && tracked::live == live);
  tracked::failure = 4;
  throws([&] { target = source; });
  assert(target.index() == 1 && *payload_get<1>(target).value == 11);
  tracked::failure = 4;
  throws([&] { target = std::move(source); });
  assert(target.index() == 1 && *payload_get<1>(source).value == 11);
  target = carrier(std::in_place_index<2>);
  tracked::failure = 3;
  throws([&] { target = std::move(source); });
  assert(target.index() == 255);
  target = std::move(source);
  assert(target.index() == 1 && *payload_get<1>(target).value == 11);
  const int beforePartial = tracked::live;
  throws([] {
    payload_variant<int, partial, std::monostate> failed(
        std::in_place_index<1>);
  });
  assert(tracked::live == beforePartial);
}

void test_nested_and_alignment() {
  passive_move_only one(std::in_place_index<0>, 37);
  passive_move_only two(std::move(one));
  passive_move_only three;
  three = std::move(two);
  assert(payload_get<0>(three).value == 37);
  payload_variant<aligned, std::monostate> wide(std::in_place_index<0>, 17);
  assert(reinterpret_cast<std::uintptr_t>(&payload_get<0>(wide)) % 128 == 0);
  assert(payload_get<0>(wide).value == 17);
  using nested =
      payload_variant<carrier, std::array<carrier, 2>, std::monostate>;
  nested inner(std::in_place_index<0>, std::in_place_index<1>, 19);
  nested moved(std::move(inner));
  assert(*payload_get<1>(payload_get<0>(moved)).value == 19);
  std::array<carrier, 2> array{carrier(std::in_place_index<0>, 23),
                               carrier(std::in_place_index<1>, 29)};
  nested elements(std::in_place_index<1>, std::move(array));
  assert(*payload_get<0>(payload_get<1>(elements)[0]).value == 23);
  assert(*payload_get<1>(payload_get<1>(elements)[1]).value == 29);
  using equal_carrier = payload_variant<int, int, std::monostate>;
  equal_carrier a(std::in_place_index<0>, 3), b(a),
      c(std::in_place_index<1>, 3);
  assert(a == b && !(a == c));
  const auto &read = a;
  static_assert(std::is_same_v<decltype(payload_get<0>(read)), const int &>);
  static_assert(std::is_same_v<decltype(payload_get<0>(std::move(a))), int &&>);
}
} // namespace

int main() {
  test_lifetimes();
  assert(tracked::live == 0);
  test_failure();
  assert(tracked::live == 0);
  test_nested_and_alignment();
  assert(tracked::live == 0 && tracked::constructed == tracked::destroyed);
}
