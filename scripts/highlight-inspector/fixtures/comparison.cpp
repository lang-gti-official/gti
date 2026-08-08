namespace benchmark {

// @probe generic.parameter.declaration | T
template <typename T>
// @probe type.definition.box | Box
class Box {
public:
  // @probe constructor.declaration.box | Box
  // @probe parameter.declaration.initial | initial
  explicit Box(const T initial)
      : value(initial) {}

  // @probe method.declaration.current | current
  T current() const {
    // @probe keyword.return.current | return
    // @probe receiver.this.current | this
    // @probe property.reference.current | value
    return this->value;
  }

  // @probe method.declaration.replace | replace
  // @probe parameter.declaration.next | next
  void replace(const T next) {
    // @probe parameter.reference.next | next
    // @probe operator.assignment | =
    value = next;
  }

private:
  // @probe property.declaration.value | value
  T value;
};

template <typename T>
// @probe function.declaration.add | add
// @probe parameter.declaration.left | left
T add(const T left, const T right) {
  // @probe parameter.reference.left | left
  // @probe operator.addition | +
  return left + right;
}

// @probe constant.namespace.offset | offset
// @probe number.integer.offset | 2
constexpr int offset = 2;

} // namespace benchmark

int main() {
  // @probe type.user.box | Box
  // @probe local.readonly.box | readonly_box
  // @probe constructor.usage.box | Box
  const benchmark::Box<int> readonly_box{40};

  // @probe local.mutable.box | mutable_box
  benchmark::Box<int> mutable_box{1};

  // @probe type.builtin.int | int
  // @probe method.call.current | current
  const int base = readonly_box.current();

  // @probe local.mutable.total | total
  // @probe function.call.add | add
  int total = benchmark::add(base, benchmark::offset);

  // @probe method.call.replace | replace
  mutable_box.replace(total);

  // @probe number.float.ratio | 0.5
  const double ratio = 0.5;

  // @probe literal.string.message | "highlight"
  const char* message = "highlight";

  // @probe literal.boolean.ready | true
  const bool ready = true;

  // @probe literal.null.empty | nullptr
  const auto empty = nullptr;

  // @probe keyword.conditional.if | if
  // @probe operator.logical.and | &&
  if (ready && total == 42) {
    return 0;
  }
  return ratio > 0.0 && message != empty ? 0 : 1;
}
