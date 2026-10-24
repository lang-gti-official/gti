// Appended to generated C++ so the test can exercise TU-private transport.
namespace passive_copy_transport_test {
using gti_internal::backend::payload_variant;
inline int copies = 0;
struct Reading {
  int &value;
  explicit Reading(int &input) noexcept : value(input) {}
  Reading(const Reading &other) noexcept : value(other.value) { ++copies; }
  Reading &operator=(const Reading &) = delete;
};
struct ThrowingCopy {
  ThrowingCopy(const ThrowingCopy &) noexcept(false);
  ThrowingCopy &operator=(const ThrowingCopy &) = delete;
};
struct ActiveDestructor {
  ActiveDestructor(const ActiveDestructor &) noexcept = default;
  ActiveDestructor &operator=(const ActiveDestructor &) = delete;
  ~ActiveDestructor() {}
};
using Passive = payload_variant<Reading, std::monostate>;
static_assert(std::is_copy_assignable_v<Passive>);
static_assert(
    !std::is_copy_assignable_v<payload_variant<ThrowingCopy, std::monostate>>);
static_assert(!std::is_copy_assignable_v<
              payload_variant<ActiveDestructor, std::monostate>>);
inline void require(bool value) {
  if (!value)
    std::abort();
}
inline const bool executed = [] {
  int first = 17;
  int second = 29;
  Passive source(std::in_place_index<0>, first);
  Passive target(std::in_place_index<0>, second);
  Passive empty(std::in_place_index<1>);
  copies = 0;
  target = source;
  require(copies == 1 && target.index() == 0 &&
          &target.template get<0>().value == &first);
  target = target;
  require(copies == 1 && target.template get<0>().value == 17);
  target = empty;
  require(copies == 1 && target.index() == 1);
  target = source;
  require(copies == 2 && target.template get<0>().value == 17);
  empty = empty;
  require(copies == 2 && empty.index() == 1);
  first = 31;
  require(source.template get<0>().value == 31 &&
          target.template get<0>().value == 31);
  source.retire_cleaned_alternative();
  target = source;
  require(copies == 2 && target.index() == source.index());
  target = target;
  require(copies == 2 && target.index() == source.index());
  return true;
}();
} // namespace passive_copy_transport_test
