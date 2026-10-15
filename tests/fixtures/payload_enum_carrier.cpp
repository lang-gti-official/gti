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
using carrier = payload_variant<std::monostate, tracked, tracked, unit>;
using passive = payload_variant<std::monostate, int, unit>;
static_assert(std::is_trivially_copy_constructible_v<passive>);
static_assert(std::is_trivially_move_constructible_v<passive>);
static_assert(std::is_trivially_copy_assignable_v<passive>);
static_assert(std::is_trivially_move_assignable_v<passive>);
static_assert(std::is_trivially_destructible_v<passive>);
static_assert(std::is_trivially_copyable_v<passive>);
using passive_move_only = payload_variant<std::monostate, trivial_move_only>;
static_assert(!std::is_copy_constructible_v<passive_move_only>);
static_assert(std::is_trivially_move_constructible_v<passive_move_only>);
static_assert(!std::is_copy_assignable_v<passive_move_only>);
static_assert(std::is_trivially_move_assignable_v<passive_move_only>);
static_assert(std::is_trivially_destructible_v<passive_move_only>);
static_assert(!std::is_copy_constructible_v<
              payload_variant<std::monostate, std::unique_ptr<int>>>);
static_assert(std::is_move_constructible_v<
              payload_variant<std::monostate, std::unique_ptr<int>>>);
static_assert(!std::is_copy_assignable_v<
              payload_variant<std::monostate, std::unique_ptr<int>>>);
static_assert(alignof(payload_variant<std::monostate, aligned>) == 128);

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
  carrier empty;
  assert(empty.index() == 0);
  carrier first(std::in_place_index<1>, 7);
  carrier duplicate(std::in_place_index<2>, 9);
  assert(first.index() == 1 && duplicate.index() == 2);
  assert(*payload_get<1>(first).value == 7);
  assert(*payload_get<2>(duplicate).value == 9);
  carrier copied(first);
  assert(*payload_get<1>(copied).value == 7);
  carrier moved(std::move(first));
  assert(first.index() == 1 && !payload_get<1>(first).value);
  assert(*payload_get<1>(moved).value == 7);
  duplicate = copied; // Different active index, including the same native type.
  assert(duplicate.index() == 1 && *payload_get<1>(duplicate).value == 7);
  copied = std::move(moved); // Same index: assignment retains one lifetime.
  assert(*payload_get<1>(copied).value == 7);
  copied = copied;
  copied = std::move(copied);
  assert(*payload_get<1>(copied).value == 7);
  duplicate = carrier(std::in_place_index<3>);
  assert(duplicate.index() == 3);
  empty = copied;
  assert(*payload_get<1>(empty).value == 7);
}

void test_failure() {
  carrier source(std::in_place_index<2>, 11);
  carrier target(std::in_place_index<1>, 13);
  const int live = tracked::live;
  tracked::failure = 1;
  throws([] { carrier failed(std::in_place_index<2>, 0); });
  assert(tracked::live == live);
  tracked::failure = 2;
  throws([&] { carrier failed(source); });
  assert(tracked::live == live && *payload_get<2>(source).value == 11);
  tracked::failure = 3;
  throws([&] { carrier failed(std::move(source)); });
  assert(tracked::live == live && *payload_get<2>(source).value == 11);
  tracked::failure = 2;
  throws([&] { target = source; });
  assert(target.index() == 255 && tracked::live == live - 1);
  carrier vacantCopy(target);
  carrier vacantMove(std::move(vacantCopy));
  assert(vacantCopy.index() == 255 && vacantMove.index() == 255);
  target = source; // Reconstruct after failed cross-alternative publication.
  assert(target.index() == 2 && tracked::live == live);
  tracked::failure = 4;
  throws([&] { target = source; });
  assert(target.index() == 2 && *payload_get<2>(target).value == 11);
  tracked::failure = 4;
  throws([&] { target = std::move(source); });
  assert(target.index() == 2 && *payload_get<2>(source).value == 11);
  target = carrier(std::in_place_index<3>);
  tracked::failure = 3;
  throws([&] { target = std::move(source); });
  assert(target.index() == 255);
  target = std::move(source);
  assert(target.index() == 2 && *payload_get<2>(target).value == 11);
  const int beforePartial = tracked::live;
  throws([] {
    payload_variant<std::monostate, int, partial> failed(
        std::in_place_index<2>);
  });
  assert(tracked::live == beforePartial);
}

void test_nested_and_alignment() {
  passive_move_only one(std::in_place_index<1>, 37);
  passive_move_only two(std::move(one));
  passive_move_only three;
  three = std::move(two);
  assert(payload_get<1>(three).value == 37);
  payload_variant<std::monostate, aligned> wide(std::in_place_index<1>, 17);
  assert(reinterpret_cast<std::uintptr_t>(&payload_get<1>(wide)) % 128 == 0);
  assert(payload_get<1>(wide).value == 17);
  using nested =
      payload_variant<std::monostate, carrier, std::array<carrier, 2>>;
  nested inner(std::in_place_index<1>, std::in_place_index<2>, 19);
  nested moved(std::move(inner));
  assert(*payload_get<2>(payload_get<1>(moved)).value == 19);
  std::array<carrier, 2> array{carrier(std::in_place_index<1>, 23),
                               carrier(std::in_place_index<2>, 29)};
  nested elements(std::in_place_index<2>, std::move(array));
  assert(*payload_get<1>(payload_get<2>(elements)[0]).value == 23);
  assert(*payload_get<2>(payload_get<2>(elements)[1]).value == 29);
  using equal_carrier = payload_variant<std::monostate, int, int>;
  equal_carrier a(std::in_place_index<1>, 3), b(a),
      c(std::in_place_index<2>, 3);
  assert(a == b && !(a == c));
  const auto &read = a;
  static_assert(std::is_same_v<decltype(payload_get<1>(read)), const int &>);
  static_assert(std::is_same_v<decltype(payload_get<1>(std::move(a))), int &&>);
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
