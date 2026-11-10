#!/usr/bin/env python3
"""Execute checked lifetime programs and retain native representation receipts."""

import argparse
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

from backend_modes import native_modes


CASES = {
    'composed_record_two_owners': r"""
import std.memory;
struct Pair<lifetime a, lifetime b> {
  int&<a> left; int&<b> right;
  Pair(int&<a> first,int&<b> second):left(first),right(second) {}
};
struct Box<T> { T value; Box(T input):value(std::move(input)) {} };
Box<T> relay<T>(T input) { return Box<T>(std::move(input)); }
int main() {
  int left=4; int right=9; Pair pair{left,right};
  auto result=relay(std::move(pair));
  return result.value.left*10+result.value.right-49;
}
""",
    'generic_stored_callback_two_owners': r"""
class Holder<F> { mut F callable; public: Holder(F f) : callable(std::move(f)) {} int run() mut { return this.callable(); } };
Holder<F> store<F>(F f) { return Holder<F>(std::move(f)); }
int main() { int left = 4; int right = 9; auto callback = [&left, &right]() -> int { return left + right; }; mut auto stored = store(std::move(callback)); return stored.run() - 13; }
""",
    'callback_with_owned_reference_carrier': r"""
class Holder<F> { mut F callable; public: Holder(F f) : callable(std::move(f)) {} int run() mut { return this.callable(); } };
Holder<F> store<F>(F f) { return Holder<F>(std::move(f)); }
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
int main() { int value = 7; View view{value}; auto callback = [view]() -> int { return view.value; }; mut auto stored = store(std::move(callback)); return stored.run() - 7; }
""",
    'copied_callback_after_source_consumption': r"""
void consume<T>(T value) {}
class Holder<F> { mut F callable; public: Holder(F f):callable(std::move(f)) {} int run() mut { return this.callable(); } };
Holder<F> store<F>(F f) { return Holder<F>(std::move(f)); }
struct View<lifetime a> { int&<a> value; View(int&<a> input):value(input) {} };
int main() { int value=7; View view{value}; auto callback=[view]()->int { return view.value; }; consume(std::move(view)); mut auto stored=store(std::move(callback)); return stored.run()-7; }
""",
    'copied_callback_multiple_owners': r"""
struct Pair<lifetime a, lifetime b> { int&<a> first; int&<b> second; Pair(int&<a> left,int&<b> right):first(left),second(right) {} };
int main() { int first=4; int second=7; Pair pair{first,second}; auto callback=[pair]()->int { return pair.first+pair.second; }; return callback()-11; }
""",
    'copied_callback_parameter_owner': r"""
struct View<lifetime a> { int&<a> value; View(int&<a> input):value(input) {} };
int execute<lifetime a>(View<a> view) { auto callback=[view]()->int { return view.value; }; return callback(); }
int main(){ int value=7; return execute(View(value))-7; }
""",
    'copied_record_alternative_owners': r"""
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; } return right;
}
struct Reading<lifetime a> {
  int&<a> value;
  Reading(int&<a> input):value(input) {}
};
int copied_and_moved(bool first) {
  int left=3; int right=9;
  int& selected=choose(left,right,first);
  Reading reading{selected}; Reading copied{reading};
  Reading moved{std::move(copied)};
  return moved.value;
}
int main() { return copied_and_moved(true)-3 + copied_and_moved(false)-9; }
""",
    'owned_closure_relay': r"""
F relay<F>(F callback) { return std::move(callback); }
int main() {
  mut int value = 4;
  mut auto callback = [mut &value]() -> int { value += 1; return value; };
  mut auto moved = relay(std::move(callback));
  return moved() - 5;
}
""",
    'generic_stored_callback': r"""
import std.memory;
class Holder<F> { mut F callable; public:
 Holder(F callable) : callable(std::move(callable)) {}
 int run() mut { return this.callable(); }
};
Holder<F> store<F>(F callable) { return Holder<F>(std::move(callable)); }
int main() { mut int value=4;
 mut auto update=[mut &value]() -> int { value+=1; return value; };
 mut auto stored=store(std::move(update));
 return stored.run()-5;
}
""",
    "generated_record_move_failure": r"""
struct Outer {
  Outer() = default;
  ~Outer() { std::println("outer"); }
};
struct Observed<lifetime a> {
  int&<a> value;
  Observed(int&<a> input) : value(input) {}
  ~Observed() { std::println(this.value); }
};
int later(int divisor) { return 1 / divisor; }
int main() {
  Outer outer{};
  int value = 7;
  Observed original{value};
  Observed moved{std::move(original)};
  return later(0);
}
""",
    "generated_record_move_cleanup": r"""
mut int trace = 0;
int&<a> choose<lifetime a>(int&<a> left,int&<a> right,bool first) {
  if (first) { return left; } return right;
}
struct Observed<lifetime a> {
  int&<a> value;
  Observed(int&<a> input) : value(input) {}
  ~Observed() { trace += this.value; }
};
int observe(bool first) {
  int left = 3; int right = 9;
  int& selected = choose(left,right,first);
  Observed original{selected};
  Observed moved{std::move(original)};
  if (trace != 0) { return 1; }
  if (first) { return moved.value - 3; }
  return moved.value - 9;
}
int main() {
  if (observe(true) != 0 || trace != 3) { return 1; }
  trace = 0;
  if (observe(false) != 0 || trace != 9) { return 2; }
  return 0;
}
""",
    "generated_record_move_suppliers": """
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; } return right;
}
struct Reading<lifetime a> {
  int&<a> value;
  Reading(int&<a> input) : value(input) {}
};
int observe(bool first) {
  int left = 3; int right = 9;
  int& selected = choose(left, right, first);
  Reading reading{selected}; Reading copied{reading};
  Reading moved{std::move(copied)};
  if (first) { return moved.value - 3; }
  return moved.value - 9;
}
int main() { return observe(true) + observe(false); }
""",
    "generated_record_copy_owners": """
mut int retired = 0;
struct Owner {
  int value;
  Owner(int input) : value(input) {}
  ~Owner() { retired += value; }
};
struct View<lifetime a, lifetime b> {
  int&<a> first;
  int&<b> second;
  View(int&<a> x, int&<b> y) : first(x), second(y) {}
  ~View() = default;
};
int typed<lifetime a, lifetime b>(View<a,b>& input) {
  View<a,b> copied = input; return copied.first + copied.second;
}
int inferred<lifetime a, lifetime b>(View<a,b>& input) {
  auto copied = input; return copied.first + copied.second;
}
int braced<lifetime a, lifetime b>(View<a,b>& input) {
  View<a,b> copied{input}; return copied.first + copied.second;
}
int called<lifetime a, lifetime b>(View<a,b>& input) {
  auto copied = View<a,b>(input); return copied.first + copied.second;
}
int main() {
  {
    Owner x{2}; Owner y{5};
    View source{x.value,y.value};
    if (typed(source) != 7 || retired != 0) { return 1; }
    if (inferred(source) != 7 || retired != 0) { return 2; }
    if (braced(source) != 7 || retired != 0) { return 3; }
    if (called(source) != 7 || retired != 0) { return 4; }
  }
  return retired - 7;
}
""",
    "anonymous_typed_destination": "struct View { int& value; View(int& input):value(input){} }; int main(){int input=7;View original{input};View copied{original};View moved{std::move(original)};auto called=View(copied);return copied.value+moved.value+called.value-21;}",
    "anonymous_policy_destination": "struct View<T> { T& value; View(T& input):value(input){} View(View<T>&)=default; View(View<T>&&)=default; }; int main(){int input=7;View<int> original{input};View<int> copied{original};View<int> moved{std::move(original)};auto called=View<int>(copied);return copied.value+moved.value+called.value-21;}",
    "anonymous_repeated_destination": "struct Pair<lifetime a,lifetime b>{int&<a> first;int&<b> second;Pair(int&<a>x,int&<b>y):first(x),second(y){}}; using Equal<lifetime a> = Pair<a,a>; int read<lifetime a>(int&<a> input){Pair<a,a> source{input,input};Equal copy{source};return copy.first+copy.second;}int main(){int input=7;return read(input)-14;}",
    "anonymous_destination_elision": "struct View{int& value;View(int& x):value(x){} View(View&)=delete;View(View&&)=delete;};int main(){int input=7;View value{View(input)};return value.value-7;}",
    "anonymous_copied_reference_return": "struct Cell<lifetime a>{int&<a> value;Cell(int&<a> input):value(input){}}; int&<a> read<lifetime a>(int&<a> input){Cell<a> original{input};Cell copied{original};return copied.value;} int main(){int input=7;return read(input)-7;}",
    "elided_structural_special_members": """
struct Legacy {
  int& value;
  Legacy(int& input) : value(input) {}
  Legacy(Legacy& other) = default;
  Legacy(Legacy&& other) = default;
};
struct Generic<T> {
  T& value;
  Generic(T& input) : value(input) {}
  Generic(Generic<T>& other) = default;
  Generic(Generic<T>&& other) = default;
};
struct Named<lifetime a> {
  int&<a> value;
  Named(int&<a> input) : value(input) {}
  Named(Named& other) = default;
  Named(Named&& other) = default;
};
int read<lifetime a>(int&<a> input) {
  Named<a> source{input}; Named<a> copied{source};
  Named<a> moved{std::move(source)};
  return copied.value + moved.value;
}
int main() {
  int input = 7;
  Legacy source{input}; auto copied = source; auto moved = std::move(source);
  Generic<int> generic{input}; auto genericCopy = generic;
  auto genericMove = std::move(generic);
  return copied.value + moved.value + genericCopy.value + genericMove.value + read(input) - 42;
}
""",
    "explicit_structural_special_members": """
struct Plain {
  int value;
  Plain(int input) : value(input) {}
  Plain(Plain& other) = default;
  Plain(Plain&& other) = default;
};
Plain copy(Plain& input) { return Plain(input); }
mut int drops = 0;
struct Guard {
  int value;
  Guard(int input) : value(input) {}
  Guard(Guard&& other) = default;
  ~Guard() { drops += this.value; }
};
int moveOnce() { Guard source{5}; Guard moved{std::move(source)}; return moved.value; }
int main() {
  Plain original{3}; Plain direct{original}; Plain forwarded{copy(original)};
  Plain source{4}; Plain moved{std::move(source)};
  int guard = moveOnce();
  return original.value + direct.value + forwarded.value + moved.value + guard + drops - 23;
}
""",
    "carrier_reference_shell_inputs": 'int shared = 7;\nint independent = 9;\nstruct Borrowed<lifetime storage> {\n  int&<storage> value;\n  Borrowed(int&<storage> input) : value(input) {}\n};\nBorrowed<storage>&<shell> relay<lifetime storage, lifetime shell>(\n    Borrowed<storage>&<shell> input) { return input; }\nint main() {\n  Borrowed<static> first{shared};\n  Borrowed<static> second{shared};\n  Borrowed<static> third{independent};\n  Borrowed<static>& firstRef = relay(first);\n  Borrowed<static>& secondRef = relay(second);\n  Borrowed<static>& thirdRef = relay(third);\n  return firstRef.value + secondRef.value + thirdRef.value - 23;\n}\n',
    "deferred_callable_copy_evidence": """
import std.utility;
T duplicate<T>(T& value) {
  if constexpr (std::copy_constructible<T>) { return value; }
  else { return value.clone(); }
}
int identity(int input) { return input; }
int&<a> referenceIdentity<lifetime a>(int&<a> input) { return input; }
int global = 9;
int&<static> fixedReference(int ignored) { return global; }
int main() {
  auto first = duplicate(identity); auto second = std::clone(identity);
  auto reference = duplicate(referenceIdentity);
  auto fixed = duplicate(fixedReference); int&<static> globalReference = fixed(0);
  int left = 3; int right = 7;
  return first(3) + second(7) + reference(left) + reference(right) + globalReference - 29;
}
""",
    "receiver_pack_alias_regions": """
struct Slots<T> {
  int count; Slots(int input) : count(input) {}
  int size() { return this.count; }
};
class Cursor<T> {
  Slots<T>& slots;
public:
  Cursor(Slots<T>& input) : slots(input) {}
  int read() { return this.slots.size(); }
};
struct Buffer<T> {
  Slots<T> data;
  using iterator = Cursor<T>;
  using nested = iterator;
  using borrow<lifetime a> = Slots<T>&<a>;
  Buffer(int input) : data(Slots<T>(input)) {}
  nested begin() { return nested(this.data); }
  borrow<a> access<lifetime a>(Slots<T>&<a> input) { return input; }
};
struct Box {
  mut int value; Box(int input) : value(input) {}
  mut int& get<Args...>(Args... ignored) mut { return this.value; }
};
int main() {
  Buffer<int> buffer{7}; auto cursor = buffer.begin();
  Slots<int> separate{4}; int borrowed = buffer.access(separate).size();
  mut Box box{3}; box.get() = 5; box.get(false) = 7;
  return cursor.read() + box.get(true, 2) + borrowed - 18;
}
""",
    "symbolic_callable_pack_results": """
class Forward<R&<result>(Args...)> {
public:
  Forward() = default;
  R&<result> invoke<F>(F& callable, Args... args) {
    auto relay = [](F& target, Args... values) -> R&<result> {
      return target(values...);
    };
    return relay(callable, args...);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() {
  Forward<Selection> forward{}; auto callable = identity; int value = 7;
  return forward.invoke(callable, value) - 7;
}
""",
    "lambda_fixed_environment_premises": """
int needsStorage<T, lifetime a>() requires outlives<T, a> { return 1; }
int enclosing<T, lifetime a>(T&<a> input, int&<a> token) {
  auto body = [](int&<a> value) -> int { return value + needsStorage<T, a>(); };
  return body(token);
}
int main() { int input = 7; return enclosing(input, input) - 8; }
""",
    "checked_owner_witness_suppliers": """
unsafe T&<a> borrow<T, Owner, lifetime a>(const T* address, Owner&<a> owner) { unsafe { return *address; } }
struct View<lifetime a> {
  int&<a> external; int local = 7;
  View(int&<a> source) : external(source) {}
  int& read() { unsafe { return borrow(&this.local, this); } }
};
struct Wrapper<lifetime a, lifetime owner> {
  View<a>&<owner> value;
  Wrapper(View<a>&<owner> input) : value(input) {}
  int& read() { return this.value.read(); }
};
int&<shell> read<lifetime a, lifetime shell>(View<a>&<shell> first, View<a>&<shell> second) {
  return first.read();
}
int copied<lifetime a, lifetime shell>(View<a>&<shell> first, View<a>&<shell> second) {
  return first.read();
}
int main() {
  int value = 3;
  View first{value}; View second{value};
  Wrapper wrapper{first};
  return read(first, second) + wrapper.read() + copied(first, second) - 21;
}
""",
    "owned_field_reference": """
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} int& read() { return this.local; } }; int main() { int value = 3; View view{value}; return view.read() - 7; }
""",
    "mutable_owned_field_reference": """
struct View<lifetime a> {
 int&<a> external; mut int local = 7;
 View(int&<a> input) : external(input) {}
 int& read() { return this.local; }
 mut int& edit() mut { return this.local; }
};
int main() { int value = 3; mut View view{value}; view.edit() = 11; return view.read() - 11; }
""",
    "formal_owned_field_reference": """
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} };
int&<owner> read<lifetime a, lifetime owner>(View<a>&<owner> view) { return view.local; }
int main() { int value = 3; View view{value}; return read(view) - 7; }
""",
    "checked_physical_carrier_owner": """
unsafe T&<a> borrow<T, Owner, lifetime a>(const T* address, Owner&<a> owner) { unsafe { return *address; } }
struct View<lifetime a> {
  int&<a> external; int local = 7;
  View(int&<a> input) : external(input) {}
  int& read() { unsafe { return borrow(&this.local, this); } }
};
int main() { int value = 3; View view{value}; return view.read() - 7; }
""",
    "physical_reference_primitive_carrier": """
struct ViewRef<lifetime data, lifetime shell> {
  std::string_view<data>&<shell> value;
  ViewRef(std::string_view<data>&<shell> source) : value(source) {}
};
ViewRef<data, shell> borrowed<lifetime data, lifetime shell>(std::string_view<data>&<shell> source) {
  return ViewRef<data, shell>(source);
}
int main() {
  std::string_view text = "word";
  ViewRef direct{text};
  auto forwarded = borrowed(text);
  return direct.value.size() == 4 && forwarded.value.size() == 4 ? 0 : 1;
}
""",
    "physical_reference_record_carrier": """
struct View<lifetime data> { int&<data> value; View(int&<data> source) : value(source) {} };
struct Reference<T, lifetime shell> { T&<shell> value; Reference(T&<shell> source) : value(source) {} };
Reference<View<data>, shell> borrowed<lifetime data, lifetime shell>(View<data>&<shell> source) {
  return Reference<View<data>, shell>(source);
}
int main() {
  int data = 4; View view{data};
  auto reference = borrowed(view);
  return reference.value.value - 4;
}
""",
    "stored_reference_carrier_constructor": """
struct View<lifetime a> {
  int&<a> external; int local = 11;
  View(int&<a> input) : external(input) {}
};
struct Holder<lifetime a, lifetime owner> {
  View<a>&<owner> value;
  Holder(View<a>&<owner> first, View<a>&<owner> second) : value(first) {}
};
int main() {
  int first = 7; int second = 9;
  View left{first}; View right{second};
  Holder holder{left, right};
  return holder.value.local - 11;
}
""",
    "checked_scalar_owner_witness": """
unsafe T&<a> borrow<T, Owner, lifetime a>(const T* address, Owner&<a> owner) { unsafe { return *address; } }
unsafe mut T&<a> borrow_mut<T, Owner, lifetime a>(T* address, mut Owner&<a> owner) { unsafe { return *address; } }
struct Storage { mut int value = 7; Storage() {} };
int rewrite(mut int& value) { value = 5; return value; }
int main() {
  mut Storage storage{};
  if (rewrite(storage.value) != 5) { return 1; }
  unsafe {
    borrow_mut(&storage.value, storage) = 11;
    return borrow(&storage.value, storage) - 11;
  }
}
""",
    "checked_mutable_owner_witness": """
unsafe mut T&<a> borrow_mut<T, Owner, lifetime a>(T* address, mut Owner&<a> owner) { unsafe { return *address; } }
struct View<lifetime a> {
  int&<a> external; mut int local = 7;
  View(int&<a> source) : external(source) {}
  mut int& edit() mut { unsafe { return borrow_mut(&this.local, this); } }
  int&<this> read() { unsafe { return *(&this.local); } }
};
int main() {
  int external = 3; mut View view{external};
  view.edit() = 11;
  return view.read() - 11;
}
""",
    "opaque_stored_type_implications": """
struct Cell<T> {
  T value;
  Cell(T input) : value(input) {}
  int size() { return 1; }
};
struct Outer<T> {
  Cell<T> cell;
  Outer(T input) : cell(Cell<T>(input)) {}
  int size() { return this.cell.size(); }
};
void need<T>() requires outlives<T, static> {}
void relay<T>(Cell<T>&<static> input) { need<T>(); }
Cell<int> stored{3};
int main() {
  Outer<int> value{7};
  relay(stored);
  return value.size() - 1;
}
""",
    "raw_pointer_storage_outlives": """
void needs_static<T>(T* pointer) requires outlives<T*, static> {}
void relay<T>(T* pointer) { needs_static(pointer); }
struct Borrowed<lifetime a> { int&<a> value; Borrowed(int&<a> source) : value(source) {} };
void exercise<lifetime a>(int&<a> value) {
  Borrowed<a>* pointer = nullptr;
  relay(pointer);
}
int main() { int value = 7; exercise(value); return 0; }
""",
    "patterned_invocation_environment": """
struct Environment<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Environment(R&<environment> input) : value(input) {}
};
int read<lifetime a>(int&<a> input) {
  Environment<int&<a>(int&), a> value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
""",
    "patterned_fixed_environment": """
struct Callback<R&<environment>(R&<environment>), lifetime environment> {
  R&<environment> value;
  Callback(R&<environment> input) : value(input) {}
};
struct StaticView<R&<static>()> {
  R&<static> value;
  StaticView(R&<static> input) : value(input) {}
};
int stored = 11;
int read<lifetime a>(int&<a> input) {
  Callback<int&<a>(int&<a>), a> value{input};
  return value.value;
}
int main() {
  int value = 7;
  if (read(value) != 7) { return 1; }
  StaticView<int&<static>()> global{stored};
  return global.value - 11;
}
""",
    "reference_pack_reborrow": """
void bump<T>(mut T& value) requires std::numeric<T> { value++; }
void update<Args...>(mut Args&... values) { (bump(values), ...); }
int main() {
  mut int left = 3; mut int right = 4;
  update(left, right);
  return left + right - 9;
}
""",
    "consuming_static_mapper_results": """
int stored = 41;
mut int calls = 0;
int&<static> map(int input) { calls++; return stored; }
expected<int, int> read(bool fail) {
  if (fail) { return unexpected(9); }
  return 7;
}
int main() {
  auto value = read(false).transform(map);
  if (!value.has_value() || value.value() != 41 || calls != 1) { return 1; }
  auto error = read(true).transform_error(map);
  if (error.has_value() || error.error() != 41 || calls != 2) { return 2; }
  auto skipped_value = read(true).transform(map);
  if (skipped_value.has_value() || skipped_value.error() != 9 || calls != 2) {
    return 3;
  }
  auto skipped_error = read(false).transform_error(map);
  if (!skipped_error.has_value() || skipped_error.value() != 7 || calls != 2) {
    return 4;
  }
  return 0;
}
""",
    "consuming_external_results": """
mut int drops = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
class Consumer<lifetime a> {
  View<a> external;
public:
  Consumer(int&<a> input) : external(View<a>(input)) {}
  ~Consumer() { drops++; }
  int&<a> take() && { return this.external.value; }
  int&<a> operator()(int&<a> input) && { return input; }
  View<a> carrier() && { return this.external; }
};
int main() {
  int value = 7;
  int& first = Consumer(value).take();
  if (drops != 1 || first != 7) { return 1; }
  int& second = Consumer(value)(value);
  if (drops != 2 || second != 7) { return 2; }
  View copy = Consumer(value).carrier();
  if (drops != 3 || copy.value != 7) { return 3; }
  return 0;
}
""",
    "fresh_receiver_storage_regions": """
mut int drops = 0;
struct External<lifetime a> {
  int&<a> value;
  External(int&<a> input) : value(input) {}
  ~External() { drops++; }
  int&<a> release() && { return this.value; }
};
External<a> makeExternal<lifetime a>(int&<a> input) { return External<a>(input); }
int&<a> direct<lifetime a>(int&<a> input) { return External<a>(input).release(); }
int&<a> factory<lifetime a>(int&<a> input) { return makeExternal(input).release(); }
int&<a> grouped<lifetime a>(int&<a> input) { return (External<a>(input)).release(); }
int&<a> sequenced<lifetime a>(int&<a> input) { return (0, External<a>(input)).release(); }
int main() {
  int value = 7;
  int& first = direct(value); int& second = factory(value);
  int& third = grouped(value); int& fourth = sequenced(value);
  if (drops != 4) { return 1; }
  return first + second + third + fourth - 28;
}
""",
    "evaluated_reference_receiver_regions": """
struct View<lifetime storage> {
  int&<storage> value;
  View(int&<storage> input) : value(input) {}
  int read() { return this.value; }
  int touch() mut { return this.value; }
};
T&<a> forward_reference<T, lifetime a>(T&<a> input) requires outlives<T, a> { return input; }
int retained<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  View<storage>&<shell> same = forward_reference(input);
  return same.read();
}
int forwarded<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  return forward_reference(input).read();
}
int grouped<lifetime storage, lifetime shell>(View<storage>&<shell> input) { return (input).read(); }
int sequenced<lifetime storage, lifetime shell>(View<storage>&<shell> input) { return (0, input).read(); }
int exclusive<lifetime storage, lifetime shell>(mut View<storage>&<shell> input) { return input.touch(); }
int main() {
  int input = 7; mut View value{input};
  return retained(value) + forwarded(value) + grouped(value) + sequenced(value) + exclusive(value) - 35;
}
""",
    "complete_scalar_constructor_inputs": """
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct Selected<lifetime a> {
  int&<a> value;
  Selected(int&<a> left, int&<a> right, bool first)
      : value(choose(left, right, first)) {}
};
int main() {
  int left = 3; int right = 8;
  Selected selected{left, right, false};
  return selected.value - 8;
}
""",
    "checked_erasure_regions": """
import std.memory;
T&<region> round_trip<T, lifetime region>(T&<region> source) {
  unsafe { return std::restore_ref<T>(std::erase_ref(source)); }
}
mut T&<region> mutable_round_trip<T, lifetime region>(mut T&<region> source) {
  unsafe { return std::restore_mut<T>(std::erase_mut(source)); }
}
struct Borrowed<lifetime storage> {
  int&<storage> value;
  Borrowed(int&<storage> source) : value(source) {}
};
int main() {
  mut int value = 3;
  mut int& restored = mutable_round_trip(value);
  restored = 7;
  if (round_trip(value) != 7) { return 1; }
  Borrowed view{value};
  Borrowed& same = round_trip(view);
  return same.value - 7;
}
""",
    "checked_erasure_subobject_inputs": """
import std.memory;
struct Pair { int first; int second; Pair(int value):first(value),second(value+1){} };
std::erased_object&<a> field<lifetime a>(Pair&<a> input, Pair&<a> other) {
  [[discard]] std::erase_ref(input.second);
  [[discard]] std::erase_ref(other.first);
  return std::erase_ref(input.first);
}
int main(){Pair pair{3};Pair other{7};unsafe{return std::restore_ref<int>(field(pair,other))-3;}}
""",
    "independent_text_storage_regions": """
bool same(std::string_view left, std::string_view right) { return left == right; }
bool different(std::string_view left, std::string_view right) { return left != right; }
int main() {
  std::string_view text = "hello";
  if (text.size() != 5) { return 1; }
  if (!same(text, "hello")) { return 2; }
  if (!different("other", text)) { return 3; }
  return 0;
}
""",
    "constexpr_text_storage_region": """
constexpr uint64_t count_spaces(std::string_view text) {
  mut uint64_t count = 0;
  mut uint64_t index = 0;
  while (index < text.size()) {
    if (text[index] == ' ') { count = count + 1; }
    index = index + 1;
  }
  return count;
}
consteval uint64_t checked_spaces(std::string_view text) { return count_spaces(text); }
int main() {
  constexpr uint64_t expected = checked_spaces("one two three");
  uint64_t actual = count_spaces("one two three");
  if (expected != 2 or actual != expected) { return 1; }
  return 0;
}
""",
    "explicit_identity": """
int&<region> identity<lifetime region>(int&<region> value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
""",
    "selected_base_constructor_inputs": """
struct RegionBase<lifetime a> {
  int&<a> value;
  int marker;
  RegionBase<lifetime input>(int&<input> source, int selected = 7)
      requires outlives<input, a> : value(source), marker(selected) {}
};
struct RegionDerived<lifetime a> : public RegionBase<a> {
  RegionDerived(int&<a> source) : RegionBase<a>(source) {}
};
struct SumBase {
  int value;
  SumBase(int first, int second) : value(first + second) {}
};
struct SumDerived<Args...> : public SumBase {
  SumDerived(Args... values) : SumBase(values...) {}
};
int main() {
  int source = 5;
  RegionDerived derived{source};
  SumDerived<int, int> sum{3, 8};
  return derived.value + derived.marker + sum.value - 23;
}
""",
    "forwarded_pack_borrow_paths": """
struct View<lifetime owner> {
  int&<owner> value;
  View(int&<owner> source) : value(source) {}
};
int&<b> select<lifetime a, lifetime b>(int&<a> ignored, int&<b> selected) {
  return selected;
}
int&<b> select<lifetime a, lifetime b, lifetime shell>(int&<a> ignored, View<b>&<shell> selected) {
  return selected.value;
}
class Relay<R(Args...)> {
public:
  Relay() = default;
  R invoke(Args... values) { return select(values...); }
};
using ScalarChoice = for<lifetime a, lifetime b> int&<b>(int&<a>, int&<b>);
using CarrierChoice = for<lifetime a, lifetime b, lifetime shell> int&<b>(int&<a>, View<b>&<shell>);
int main() {
  Relay<ScalarChoice> scalar{}; Relay<CarrierChoice> carrier{};
  int first = 3; int second = 8; View view{second};
  if (scalar.invoke(first, second) != 8) { return 1; }
  return carrier.invoke(first, view) - 8;
}
""",
    "grouped_reference_function_contracts": """
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
View<a>&<shell> fixedShell<lifetime a, lifetime shell, Args...>(View<a>&<shell> first, Args&... ignored) {
  return first;
}
int&<a> fixedExternal<lifetime a, lifetime shell, Args...>(View<a>&<shell> first, Args&... ignored) {
  return first.value;
}
struct Scoped<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Scoped(R&<environment> input) : value(input) {}
  R&<environment> get() { return this.value; }
};
int&<a> read<lifetime a>(int&<a> input) {
  Scoped<int&<a>(int&), a> owner{input};
  return owner.get();
}
int main() {
  int value = 7; bool ignored = false; View view{value};
  return fixedShell(view).value + fixedShell(view, ignored).value +
         fixedExternal(view, ignored) + read(value) - 28;
}
""",
    "borrowed_from_pack_ordinals": """
int&<a> select<lifetime a>(int ignored, int&<a> value) { return value; }
class Relay<R(Args...)> {
public:
  Relay() = default;
  R invoke(int tag, Args... values) { return select(values...); }
};
using Selected = for<lifetime a> int&<a>(int, int&<a>);
int main() {
  int value = 7; Relay<Selected> relay{};
  int& selected = relay.invoke(0, 4, value);
  return selected - 7;
}
""",
    "fresh_function_item_regions": """
int global = 7;
int&<static> map(int ignored) { return global; }
int observe<F>(F& callback) { return callback(0); }
int main() { return observe(map) - 7; }
""",
    "polymorphic_function_item_regions": """
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int invoke<F>(F& callback, int& value) { return callback(value); }
int main() {
  int first = 3; int second = 8;
  auto callback = identity;
  if (callback(first) != 3 || callback(second) != 8) { return 1; }
  return invoke(identity, first) + invoke(identity, second) - 11;
}
""",
    "reference_pack_results": """
int&<a> second<lifetime a>(int&<a> first, int&<a> selected) { return selected; }
int&<a> relay<lifetime a, Args...>(Args&<a>... values) { return second(values...); }
int&<a> fixed<lifetime a, Args...>(int&<a> first, Args&... ignored) { return first; }
void increment<T>(mut T& value) requires std::numeric<T> { value++; }
void update<Args...>(mut Args&... values) { (increment(values), ...); }
int main() {
  mut int first = 3; mut int selected = 8;
  update(first, selected);
  bool ignored = false;
  return relay(first, selected) + fixed(first) + fixed(first, ignored) - 17;
}
""",
    "fixed_pack_call_defaults": """
int choose(int first, int second = 5) { return first + second; }
int choose(bool first, int second) { return second; }
int defaulted(int value = 9) { return value; }
int relay<Args...>(Args... values) { return choose(values...); }
int emptyRelay<Args...>(Args... values) { return defaulted(values...); }
int main() { return relay(3) + relay(true, 4) + emptyRelay() - 21; }
""",
    "elided_identity": """
int& identity(int& value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
""",
    "common_region": """
int&<region> choose<lifetime region>(int&<region> left, int&<region> right, bool first) {
  if (first) { return left; }
  return right;
}
int main() {
  int left = 3; int right = 8;
  if (choose(left, right, true) != 3) { return 1; }
  return choose(left, right, false) - 8;
}
""",
    "copied_carrier_result": """
struct View<lifetime a> {
  int&<a> first; int&<a> second;
  View(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int&<a> choose<lifetime a>(View<a> view, bool first) {
  if (first) { return view.first; }
  return view.second;
}
int main() {
  int left = 3; int right = 7;
  View view{left, right};
  return choose(view, false) - 7;
}
""",
    "selected_sources_in_multiple_fields": """
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct Pair<lifetime a> {
  int&<a> first; int&<a> second;
  Pair(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int main() {
  int left = 3; int right = 9;
  int& selected = choose(left, right, false);
  Pair pair{selected, selected};
  return pair.first + pair.second - 18;
}
""",
    "subobject": """
struct Number { int value = 7; Number() = default; };
int&<region> field<lifetime region>(Number&<region> number) { return number.value; }
int main() { Number number{}; return field(number) - 7; }
""",
    "borrowed_record": """
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
};
int main() { int number = 7; View view{number}; return view.value - 7; }
""",
    "anonymous_record": """
struct Pair {
  int& first;
  int& second;
  Pair(int& left, int& right) : first(left), second(right) {}
};
int main() { int left = 3; int right = 8; Pair pair{left, right}; return pair.second - 8; }
""",
    "borrowed_record_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
void observe<lifetime region>(int&<region> input) { View<region> view{input}; }
int main() {
  int number = 7;
  observe(number);
  return cleanup_trace - 7;
}
""",
    "borrowed_record_move_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
int read<lifetime a>(View<a> view) { return view.value; }
int observe<lifetime a>(int&<a> input) {
  View<a> view{input};
  int value = read(std::move(view));
  if (cleanup_trace != 7) { return 1; }
  return value - 7;
}
int main() {
  int number = 7;
  if (observe(number) != 0) { return 1; }
  return cleanup_trace - 7;
}
""",
    "borrowed_temporary_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
View<a> make<lifetime a>(int&<a> input) { return View<a>(input); }
int read<lifetime a>(View<a> view) { return view.value; }
int observe<lifetime a>(int&<a> input) {
  int value = read(View<a>(input)) + read(make(input));
  return value - 14;
}
int main() {
  int number = 7;
  if (observe(number) != 0) { return 1; }
  return cleanup_trace - 14;
}
""",
    "borrowed_enum_move_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
enum Box<T> { item(T value) };
void observe<lifetime a>(int&<a> input) {
  View<a> view{input};
  auto box = Box<View<_>>::item(std::move(view));
}
int main() {
  int number = 7;
  observe(number);
  return cleanup_trace - 7;
}
""",
    "borrowed_prepared_argument_failure": """
struct Outer {
  Outer() = default;
  ~Outer() { std::println("outer"); }
};
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { std::println(this.value); }
};
View<a> make_view<lifetime a>(int&<a> input) { return View<a>(input); }
int later(int divisor) { return 1 / divisor; }
int consume<lifetime a>(View<a> view, int unused) { return view.value; }
int main() {
  Outer outer{};
  int number = 7;
  return consume(make_view(number), later(0));
}
""",
    "lambda_suppliers": """
int main() {
  auto choose = [](int& left, int& right, bool first) -> int& {
    if (first) { return left; }
    return right;
  };
  int left = 3; int right = 8;
  if (choose(left, right, true) != 3) { return 1; }
  return choose(left, right, false) - 8;
}
""",
    "projected_only_class": """
struct Fixed<int&<source>> {
  int&<source> value;
  Fixed(int&<source> input) : value(input) {}
  int&<source> get() { return this.value; }
};
int&<a> read<lifetime a>(int&<a> input) {
  Fixed<int&<a>> holder{input};
  return holder.get();
}
int main() { int value = 7; return read(value) - 7; }
""",
    "inherited_inline_disjoint_fields": """
struct Base {
  mut int left = 1;
  Base() = default;
};
struct Derived : public Base {
  mut int right = 2;
  Derived() = default;
};
int bump(mut int& first, mut int& second) {
  first = first + 1;
  second = second + 1;
  return first + second;
}
int main() { mut Derived pair{}; return bump(pair.left, pair.right) - 5; }
""",
    "stored_callback_environment": """
import std.functional;
struct Adder<lifetime data> {
  int&<data> base;
  Adder(int&<data> value) : base(value) {}
  int operator()(int value) { return this.base + value; }
};
int apply<F, lifetime environment>(F&<environment> target) {
  std::function_ref<int(int), environment> callback{target};
  return callback(4);
}
int main() { int value = 3; Adder add{value}; return apply(add) - 7; }
""",
    "user_defined_callback_environment": """
import std.functional;
import std.memory;
class BorrowedCall<R(Args...), lifetime environment> {
  std::erased_object&<environment> object;
  std::function_ptr<R(std::erased_object&<environment>, Args...)> invoke;
public:
  BorrowedCall<F>(F&<environment> target)
    : object(std::erase_ref(target)),
      invoke(std::function_ptr<R(std::erased_object&<environment>, Args...)>(
        [](std::erased_object&<environment> object, Args... args) -> R {
          unsafe {
            F&<environment> callable = std::restore_ref<F>(object);
            return callable(args...);
          }
        })) {}
  R operator()(Args... args) { return this.invoke(this.object, args...); }
};
struct Adder<lifetime data> {
  int&<data> base;
  Adder(int&<data> value) : base(value) {}
  int operator()(int value) { return this.base + value; }
};
int main() { int value = 3; Adder add{value}; BorrowedCall<int(int)> callback{add}; return callback(4) - 7; }
""",
    "class_alias_lifetime_inference": """
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
class Collection<T> { public: using view = View<T>; };
int read<lifetime a>(int&<a> input) {
  Collection<int>::view value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
""",
    "patterned_nominal_environment": """
struct Callback<R(Args...), lifetime environment> {
  int&<environment> value;
  Callback(int&<environment> input) : value(input) {}
  int read() { return this.value; }
};
int read<lifetime a>(int&<a> input) {
  Callback<int(int), a> explicitView{input};
  Callback<int(int)> inferredView{input};
  return explicitView.read() + inferredView.read();
}
int main() { int value = 7; return read(value) - 14; }
""",
    "elided_conformance_head_regions": """
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept Inspect<T> { int inspect(T& input); };
for concept Inspect<View> { int inspect(View& input) { return input.value; } };
int main() { int value = 7; View view{value}; return Inspect<View>::inspect(view) - 7; }
""",
    "checked_operator_lifetime_applications": """
struct Reader<lifetime a> {
  int&<a> value;
  Reader(int&<a> input) : value(input) {}
  int operator()(int&<a> other) { return value + other; }
  int&<a> operator*() { return value; }
  operator bool() { return value != 0; }
};
int read<lifetime a>(int&<a> first, int&<a> second) {
  Reader<a> reader{first};
  if (reader) { return reader(second) + *reader; }
  return 0;
}
int main() { int first = 3; int second = 4; return read(first, second) - 10; }
""",
    "nested_inherited_reference": """
struct Root<T, lifetime a> {
  T&<a> value;
  Root(T&<a> input) : value(input) {}
};
struct Middle<T, lifetime a> : public Root<T, a> {
  Middle(T&<a> input) : Root<T, a>(input) {}
};
struct Leaf<lifetime a> : public Middle<int, a> {
  Leaf(int&<a> input) : Middle<int, a>(input) {}
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Leaf<a>& view) { return view.value; }
int main() { int value = 7; Leaf view{value}; return read(view) + view.get() - 14; }
""",
    "inherited_borrowed_base_cleanup": """
mut int observed = 0;
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { observed = observed + value; }
};
struct Derived<lifetime a> : public Marker, public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
  int mark() override { return 0; }
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Derived<a>& view) { return view.value; }
int main() {
  int value = 7;
  { Derived view{value}; if (read(view) + view.get() != 14) { return 1; } }
  return observed - 7;
}
""",
    "local_carrier_second_reference": """
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{local, input};
  return pair.other;
}
int main() { int value = 7; return external(value) - 7; }
""",
    "local_carrier_nested_reference": """
struct Inner<lifetime a> { int&<a> value; Inner(int&<a> input) : value(input) {} };
struct Pair<lifetime a, lifetime b> { Inner<a> first; int&<b> other; Pair(int&<a> firstValue, int&<b> secondValue) : first(Inner<a>(firstValue)), other(secondValue) {} };
int&<a> external<lifetime a>(int&<a> input) { int local = 9; Pair pair{input, local}; return pair.first.value; }
int main() { int value = 7; return external(value) - 7; }
""",
    "local_carrier_reference_cleanup": """
mut int observed = 0;
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { observed = observed + this.value; }
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
  ~Pair() { observed = observed + this.other; }
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int main() { int value = 7; int result = external(value); return result + observed - 23; }
""",
    "copied_reference_before_argument_failure": """
struct Outer {
  Outer() = default;
  ~Outer() { std::println("outer"); }
};
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { std::println(this.value); }
};
int&<a> copied<lifetime a>(int&<a> input) {
  View view{input};
  return view.value;
}
int later(int divisor) { return 1 / divisor; }
int consume<lifetime a>(int&<a> input, int unused) { return input; }
int main() {
  Outer outer{};
  int number = 7;
  return consume(copied(number), later(0));
}
""",
    "copied_static_reference_cleanup": """
int global = 7;
int otherGlobal = 11;
mut int observed = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { observed = observed + this.value; }
};
int&<static> selected();
int read() { return selected(); }
int&<static> selected() { View<static> view{global}; return view.value; }
int main() { return read() + observed - 14; }
""",
    "generic_static_reference_cleanup": """
int global = 7;
int otherGlobal = 11;
mut int observed = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { observed = observed + this.value; }
};
int&<static> selected<T>(T unused) { View<static> view{global}; return view.value; }
int main() {
  int first = selected(1);
  int second = selected(true);
  return first + second + observed - 28;
}
""",
    "inherited_independent_referents": """
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int main() { int value = 7; return external(value) - 7; }
""",
    "inherited_reference_parameter_read": """
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
};
int&<a> through<lifetime a>(Derived<a>& view) { return view.value; }
int&<a> direct<lifetime a>(Base<a>& view) { return view.value; }
int main() {
  int value = 7;
  Derived view{value};
  return through(view) + direct(view) - 14;
}
""",
    "inherited_swapped_suppliers": """
struct Root<lifetime a, lifetime b> {
  int&<a> first;
  int&<b> second;
  Root(int&<a> first, int&<b> second) : first(first), second(second) {}
};
struct Swapped<lifetime a, lifetime b> : public Root<b, a> {
  Swapped(int&<a> first, int&<b> second) : Root<b, a>(second, first) {}
};
int&<a> pick<lifetime a>(int&<a> input) {
  int local = 9;
  Swapped view{local, input};
  return view.first;
}
int main() { int value = 7; return pick(value) - 7; }
""",
    "selected_base_input_lanes": """
struct SumBase {
  int value;
  SumBase(int first, int second) : value(first * 10 + second) {}
};
struct SumDerived<Args...> : public SumBase {
  SumDerived(Args... values) : SumBase(values...) {}
};
struct RegionBase<lifetime a> {
  int&<a> value;
  int marker;
  RegionBase<lifetime input>(int&<input> source, int selected = 7)
      requires outlives<input, a> : value(source), marker(selected) {}
};
struct RegionDerived<lifetime a> : public RegionBase<a> {
  RegionDerived(int&<a> source) : RegionBase<a>(source) {}
};
int main() {
  int number = 5;
  SumDerived<int, int> sum{3, 8};
  RegionDerived view{number};
  return sum.value + view.value + view.marker - 50;
}
""",
    "inherited_borrowed_base_rollback": """
struct Outer {
  Outer() = default;
  ~Outer() { std::println("outer"); }
};
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { std::println(this.value); }
};
int later(int divisor) { return 1 / divisor; }
struct Derived<lifetime a> : public Marker, public Base<a> {
  int after = 0;
  Derived(int&<a> input, int divisor) : Base<a>(input), after(later(divisor)) {}
  ~Derived() { std::println("derived"); }
  int mark() override { return 0; }
};
int main() {
  Outer outer{};
  int number = 7;
  Derived view{number, 0};
  return 0;
}
""",
    "projected_owner_field_defaults": """
int initial_score() { return 7; }
enum Reading<lifetime a> { empty, value(int&<a> item) };
struct Default<int&<source>> {
  Reading<source> reading = Reading<source>::empty;
  Default(int count) {}
  Default(int&<source> item, int count) : reading(Reading<source>::value(item)) {}
};
struct Fixed<int&<source>> {
  int&<source> value;
  int score = initial_score();
  Fixed(int&<source> input, int initial_score) : value(input) {}
  int&<source> get() { return this.value; }
};
int main() {
  Default<int&<static>> empty{0};
  int value = 2;
  Fixed<int&> holder{value, 99};
  if (holder.get() != 2) { return 1; }
  if (holder.score != 7) { return 2; }
  Default<int&> active{value, 0};
  switch (active.reading) {
    case Reading<_>::empty: return 3;
    case Reading<_>::value(auto& item): return item - 2;
  }
}
""",
    "physical_payload": """
enum State<T> { value(T item) };
T&<region> get<T, lifetime region>(State<T>&<region> state) {
  switch (state) { case State<T>::value(auto& payload): return payload; }
}
int main() { State<int> state = State<int>::value(7); return get(state) - 7; }
""",
    "borrowed_enum": """
enum Borrowed<lifetime a> { value(int&<a> item) };
int read<lifetime a>(Borrowed<a>& value) {
  switch (value) { case Borrowed<a>::value(auto& item): return item; }
}
int main() {
  int number = 7;
  auto value = Borrowed<_>::value(number);
  auto copied = value;
  return read(copied) - 7;
}
""",
    "stored_payload_reference_return": """
enum Link<lifetime a> { value(int&<a> item) };
int&<a> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
int main() {
  int value = 7;
  auto link = Link<_>::value(value);
  return select(link) - 7;
}
""",
    "generic_borrowed_enum": """
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int read<lifetime a>(Box<View<a>>& box) {
  switch (box) { case Box<View<a>>::item(auto& payload): return payload.value; }
}
int main() {
  int number = 7;
  View view{number};
  auto box = Box<View<_>>::item(view);
  return read(box) - 7;
}
""",
    "native_borrowed_text": """
using Count = (std::string_view) -> int32_t;
int32_t count<lifetime a>(std::string_view<a> value) { return 7; }
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = count; return invoke(callback, "text") - 7; }
""",
    "projected_callable_lambda": """
class Local<R&<result>(R&<result>)> {
public:
  Local() = default;
  R&<result> invoke(R&<result> value) {
    auto relay = [](R&<result> input) -> R&<result> { return input; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() {
  Local<Selection> callback{};
  int value = 7;
  return callback.invoke(value) - 7;
}
""",
    "complete_callable_projection": """
class Captured<R(A)> {
public:
  Captured() = default;
  A invoke(A value) {
    auto relay = [](A input) -> A { return input; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() {
  Captured<Selection> callback{};
  int value = 7;
  return callback.invoke(value) - 7;
}
""",
    "higher_ranked_pointer": """
import std.functional;
using Selection = for<lifetime region> int&<region>(int&<region>, int&<region>, bool);
int&<shorter> select<lifetime longer, lifetime shorter>(
    int&<longer> left, int&<shorter> right, bool first)
    requires outlives<longer, shorter> {
  if (first) { return left; }
  return right;
}
int main() {
  std::function_ptr<Selection> pointer{select};
  int left = 3; int right = 8;
  if (pointer(left, right, true) != 3) { return 1; }
  return pointer(left, right, false) - 8;
}
""",
    "closed_signature_field_projection": """
struct Badge<S> { int value; Badge(int input) : value(input) {} };
struct Envelope<S> {
  Badge<S> value;
  Envelope(int input) : value(Badge<S>(input)) {}
  Badge<S> get() { return this.value; }
};
using Bound = for<lifetime a> int&<a>(int&<a>);
int main() {
  Envelope<Bound> envelope{7};
  auto value = envelope.get();
  return value.value - 7;
}
""",
    "synthetic_assignment_receiver_origins": """
struct Counter {
  mut int value;
  Counter(int initial) : value(initial) {}
  void operator=(int replacement) mut { this.value = replacement; }
  void operator+=(int amount) mut { this.value += amount; }
  int get() { return this.value; }
};
int main() {
  mut Counter first{1};
  mut Counter second{9};
  first = 3;
  second += 4;
  return first.get() + second.get() - 16;
}
""",
    "inherited_dispatch_lifetime_views": """
mut int retired = 0;
interface Marker { int marker(); };
class Root<lifetime a, lifetime b> {
  int&<a> first;
  int&<b> second;
public:
  Root(int&<a> first, int&<b> second) : first(first), second(second) {}
  virtual int read() { return this.first; }
  ~Root() { retired += this.first; }
};
class Middle<lifetime a, lifetime b> : public Marker, public Root<b, a> {
public:
  Middle(int&<a> first, int&<b> second) : Root<b, a>(second, first) {}
  int marker() override { return 1; }
};
class Leaf<lifetime a, lifetime b> : public Middle<a, b> {
public:
  Leaf(int&<a> first, int&<b> second) : Middle<a, b>(first, second) {}
  int read() override { return 4; }
};
int exercise() {
  int first = 3; int second = 4;
  Leaf leaf{first, second};
  return leaf.read() + leaf.marker() - 5;
}
int main() { int result = exercise(); return result + retired - 4; }
""",
    # Each case below is a regression from completing RFC 0004 (2026-11-08).
    'view_of_call_returned_reference': r"""
import std.string;
int use(std::string_view v) { return int(v.size()); }
class Box { std::string s; public: Box(std::string v) : s(std::move(v)) {} std::string& get() { return this.s; } };
int main() { Box b = Box(std::string("ab")); std::string& r = b.get(); return use(r.view()) - 2; }
""",
    'consuming_receiver_value': r"""
import std.string;
struct Owner { int n; Owner(int v) : n(v) {} };
expected<Owner, int> make(int v) { if (v < 0) { return unexpected(1); } return Owner(v); }
int main() { mut auto made = make(3); if (!made) { return 9; } Owner owner = std::move(made).value(); return owner.n - 3; }
""",
    'range_element_method_and_argument': r"""
import std.string;
import std.vector;
bool positive(int32_t& v) { return v > 0; }
int main() {
  std::vector<std::string> names = {std::string("a"), std::string("bc")};
  std::vector<int32_t> values = std::vector<int32_t>({1, 2});
  mut int n = 0;
  for (auto& name : names) { n = n + int(name.view().size()); }
  for (auto& value : values) { if (positive(value)) { n = n + 1; } }
  return n - 5;
}
""",
    'range_over_stored_reference_field': r"""
import std.vector;
class Holder {
  std::string_view& label;
  std::vector<int32_t>& items;
public:
  Holder(std::string_view& l, std::vector<int32_t>& i) : label(l), items(i) {}
  int32_t sum() mut { mut int32_t s = 0; for (auto& x : this.items) { s = s + x; } return s; }
};
int main() {
  std::string_view label = "x";
  std::vector<int32_t> v = std::vector<int32_t>({1, 2});
  mut Holder h = Holder(label, v);
  return h.sum() - 3;
}
""",
    'field_default_initializer_call': r"""
import std.string;
int id(int& x) { return x; }
int g = 3;
struct S { std::string name = std::string("ab"); int v = id(g); S() = default; };
int main() { S s{}; return int(s.name.size()) + s.v - 5; }
""",
    'arrow_through_user_dereference': r"""
struct P { mut int v; P(int x) : v(x) {} };
class It {
  mut P p;
public:
  It() : p(P(1)) {}
  mut P& operator*() mut { return this.p; }
  mut P& operator->() mut { return *this; }
};
int main() { mut It it = It(); it->v = 5; return (*it).v - 5; }
""",
    'immutable_receiver_contents_across_mut_calls': r"""
import std.string;
struct Scanner {
  std::string_view& text;
  mut { std::size_t offset; }
  Scanner(std::string_view& t) : text(t), offset(0) {}
  void step() mut { this.offset = this.offset + 1; }
  std::size_t twice() mut { this.step(); this.step(); return this.offset; }
};
int main() { std::string_view v = "abc"; mut Scanner s = Scanner(v); return int(s.twice()) - 2; }
""",
    'closure_environment_in_generic_class': r"""
class Accumulator<std::numeric T> {
  mut T total;
public:
  Accumulator(T start) : total(start) {}
  T apply<F>(F update) mut { this.total = update(this.total); return this.total; }
  T add(T operand) mut { return this.apply([operand](T observed) -> T { return T(observed + operand); }); }
};
int main() { mut Accumulator<int> a = Accumulator<int>(1); return a.add(4) - 5; }
""",
    # Reference parameter packs (RFC 0004): a result region names the
    # supplying lanes; MIR binds each lane's entry authority.
    'multi_supplier_result_subset': r"""
int&<a> second<lifetime a>(int&<a> first, int&<a> selected) { return selected; }
int&<a> relay<lifetime a>(int&<a> x, int&<a> y) { return second(x, y); }
int main() { int first = 3; int selected = 8; return relay(first, selected) - 8; }
""",
    'unnamed_pack_lane_not_retained': r"""
int&<a> fixed<lifetime a, Args...>(int&<a> first, Args&... ignored) { return first; }
int main() { int first = 3; mut int other = 4; int& kept = fixed(first, other); other = 5; return kept - 3; }
""",
    'local_carrier_relays_parameter_region': r"""
struct Scoped<lifetime environment> {
  int&<environment> value;
  Scoped(int&<environment> input) : value(input) {}
  int&<environment> get() { return this.value; }
};
int&<a> read<lifetime a>(int&<a> input) { Scoped<a> owner{input}; return owner.get(); }
int main() { int value = 7; return read(value) - 7; }
""",
    'callable_pattern_scalar_lanes': r"""
int&<b> select<lifetime a, lifetime b>(int&<a> ignored, int&<b> selected) { return selected; }
class Relay<R(Args...)> {
public:
  Relay() = default;
  R invoke(Args... values) { return select(values...); }
};
using ScalarChoice = for<lifetime a, lifetime b> int&<b>(int&<a>, int&<b>);
int main() { Relay<ScalarChoice> scalar{}; int first = 3; int second = 8; return scalar.invoke(first, second) - 8; }
""",
    'pack_forwarding_class_result': r"""
import std.string;
std::string make<Args...>(Args&... a) { return std::string("x"); }
std::string relay<Args...>(Args&... a) { mut auto r = make(a...); return std::move(r); }
int main() { int x = 1; auto s = relay(x); return int(s.size()) - 1; }
""",
}

REJECTIONS = {
    "unnamed_pack_lane_return": r"""
int&<a> second<lifetime a>(int&<a> first, int&<a> selected) { return selected; }
int&<a> wrong<lifetime a, Args...>(int&<a> first, Args&... others) { return second(others...); }
int main() { int a = 1; int b = 2; int c = 3; return wrong(a, b, c) - 3; }
""",
    "retained_pack_lane_supplier": r"""
int&<a> second<lifetime a>(int&<a> first, int&<a> selected) { return selected; }
int&<a> relay<lifetime a, Args...>(Args&<a>... values) { return second(values...); }
int main() { mut int first = 3; int selected = 8; int& result = relay(first, selected); first = 4; return result - 8; }
""",
    # Each concrete instance selects its fixed overload from exact lanes.
    "fixed_pack_target_per_instance": r"""
int choose(int first, int second) { return first + second; }
int relay<Args...>(Args... values) { return choose(values...); }
int main() { return relay(3); }
""",
    "aliased_mutable_pack_lanes": r"""
void increment(mut int& value) { value++; }
void update<Args...>(mut Args&... values) { (increment(values), ...); }
int main() { mut int first = 3; update(first, first); return first - 5; }
""",
    "range_element_escape": r"""
import std.vector;
int32_t& escape() {
  std::vector<int32_t> v = std::vector<int32_t>({1, 2});
  for (auto& x : v) { return x; }
  std::panic("empty");
}
int main() { return escape(); }
""",
    "mutation_before_moved_destructor": r"""

mut int trace = 0;
int&<a> choose<lifetime a>(int&<a> left,int&<a> right,bool first) {
  if (first) { return left; } return right;
}
struct Observed<lifetime a> {
  int&<a> value;
  Observed(int&<a> input) : value(input) {}
  ~Observed() { trace += this.value; }
};
int main() {
  mut int number = 7;
  Observed original{number};
  Observed moved{std::move(original)};
  number = 9;
  return 0;
}
""",
    "mutation_before_destructor": """
mut int cleanup_trace = 0;
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
int main() {
  mut int number = 7;
  View view{number};
  number = 9;
  return 0;
}
""",
    "unrelated_return": """
int&<left_region> wrong<lifetime left_region, lifetime right_region>(
    int&<left_region> left, int&<right_region> right) { return right; }
int main() { int left = 3; int right = 8; return wrong(left, right); }
""",
    "retained_other_supplier": """
int&<region> first<lifetime region>(int&<region> left, int&<region> right) { return left; }
int main() {
  int left = 3; mut int right = 8;
  int& result = first(left, right);
  right = 9;
  return result;
}
""",
    "retained_constructed_supplier": """
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
int main() {
  mut int left = 3; int right = 9;
  View view{choose(left, right, false)};
  left = 4;
  return view.value - 9;
}
""",
    "borrowed_destructor_copy": """
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() {}
};
int read<lifetime a>(View<a> view) { return view.value; }
int copy<lifetime a>(int&<a> input) {
  View<a> view{input};
  return read(view);
}
int main() { int number = 7; return copy(number) - 7; }
""",
    "local_record_escape": """
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
};
View<region> wrong<lifetime region>(int&<region> outside) {
  int local = 7; return View<region>(local);
}
int main() { int number = 3; View view = wrong(number); return view.value; }
""",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--census", type=Path)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler, emitter = args.compiler.resolve(), args.emitter.resolve()
    report = {"schema": "gti.lifetime-native.1", "steps": [], "cases": {},
              "allocation_scope": "generated GTI object undefined imports; process startup and dynamic-library internal allocation are outside this observation",
              "runtime_scope": "correctness process duration includes startup; no speedup claim"}

    def save():
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(command, label, expect_success=True, measure_build=False):
        command = [str(item) for item in command]
        metrics = output / (label + ".metrics")
        if measure_build:
            command = ["/usr/bin/time", "-l" if platform.system() == "Darwin" else "-v",
                       "-o", str(metrics), *command]
        started = time.perf_counter()
        result = subprocess.run(command, capture_output=True, text=True, timeout=240)
        log = output / (label + ".log")
        log.write_text(result.stdout + result.stderr)
        # Compiler backend diagnostics intentionally survive its temporary
        # build directory. Retain the exact reported file with this harness
        # before a remote runner removes its checkout and build artifacts.
        backend_logs = []
        for reported in re.findall(r"^gti: backend diagnostics saved at (.+)$",
                                   result.stdout + result.stderr, re.MULTILINE):
            diagnostic = Path(reported.strip())
            if diagnostic.name == "backend.log" and diagnostic.is_file():
                retained = output / (label + "-backend.log")
                shutil.copyfile(diagnostic, retained)
                backend_logs.append(str(retained))
        step = {"label": label, "command": command, "exit": result.returncode,
                "seconds": time.perf_counter() - started, "log": str(log)}
        if backend_logs:
            step["backend_diagnostics"] = backend_logs
        if measure_build:
            raw = metrics.read_text()
            pattern = (r"(\d+)\s+maximum resident set size" if platform.system() == "Darwin"
                       else r"Maximum resident set size \(kbytes\):\s*(\d+)")
            match = re.search(pattern, raw)
            if match:
                step["peak_rss_kib"] = int(match[1]) / (1024 if platform.system() == "Darwin" else 1)
        report["steps"].append(step)
        save()
        if expect_success and result.returncode:
            raise RuntimeError(f"{label}: exit {result.returncode}; {log}")
        return result

    failures = []

    def checked(label, operation):
        try:
            return operation()
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append({"label": label, "error": str(error)})
            report["failures"] = failures
            save()
            print(f"lifetime native {label}: FAILED: {error}", flush=True)
            return None

    root = Path(__file__).resolve().parent.parent
    example = root / "examples/192-lifetime-parameters.gti"
    if args.census:
        checked("public-example-census", lambda: run(
            [args.census.resolve(), root, example.name], "public-example-census"))
    inherited_example = Path(__file__).resolve().parent / "fixtures/llvm_dispatch_borrowed_base.gti"
    cases = {"public_example": example.read_text(),
             "public_variadic_example": (root / "examples/015-variadic-generics.gti").read_text(),
             "inherited_legacy_elision": inherited_example.read_text(), **CASES}
    for label, source in cases.items():
        path = output / (label + ".gti")
        path.write_text(source)
        observations = report["cases"].setdefault(label, {})
        before = len(failures)
        for mode, flags, _ in native_modes(backends="llvm"):
            def execute():
                binary = output / (label + "-" + mode)
                run([compiler, path, *flags, "-o", binary], label + "-" + mode + "-build", measure_build=True)
                failure_case = label in {
                    "borrowed_prepared_argument_failure",
                    "inherited_borrowed_base_rollback",
                    "generated_record_move_failure",
                    "copied_reference_before_argument_failure",
                }
                executed = run([binary], label + "-" + mode + "-run", expect_success=not failure_case)
                if failure_case:
                    site = source.index('1 / divisor')
                    line = source[:site].count("\n") + 1
                    expected = (f'{path.name}:{line}: runtime error[GTI-R0002]: '
                                'division_by_zero in integer_division\n')
                    if executed.returncode != 70 or executed.stdout != "7\nouter\n" or executed.stderr != expected:
                        raise RuntimeError(f"{label}/{mode}: initialized owner must clean up exactly once before outer cleanup")
                elif executed.stdout or executed.stderr:
                    raise RuntimeError(f"{label}/{mode}: unexpected runtime output")
                observations[mode] = {"binary_bytes": binary.stat().st_size}
                save()
            checked(label + "/" + mode, execute)
        if len(failures) == before:
            print(f"lifetime native {label}: LLVM O0/O3 passed", flush=True)

    rejection_codes = {
        "unnamed_pack_lane_return": {"GTI-S2017", "GTI-S2127"},
        "retained_pack_lane_supplier": {"GTI-S2017"},
        "aliased_mutable_pack_lanes": {"GTI-S2017"},
        "fixed_pack_target_per_instance": {"GTI-S2012"},
        "mutation_before_destructor": {"GTI-S2017"},
        "mutation_before_moved_destructor": {"GTI-S2017"},
        "unrelated_return": {"GTI-S2127"},
        "retained_other_supplier": {"GTI-S2017"},
        "retained_constructed_supplier": {"GTI-S2017"},
        "borrowed_destructor_copy": {"GTI-S2018"},
        "local_record_escape": {"GTI-S2045", "GTI-S2085", "GTI-S2127"},
        "range_element_escape": {"GTI-S2017", "GTI-S2127"},
    }
    if rejection_codes.keys() != REJECTIONS.keys():
        raise RuntimeError("every lifetime rejection needs an explicit owning diagnostic")
    for label, source in REJECTIONS.items():
        def reject():
            path = output / (label + ".gti")
            path.write_text(source)
            rejected = run([compiler, path, "--backend", "llvm", "-O3", "-o", output / label],
                           label + "-reject", expect_success=False)
            text = rejected.stdout + rejected.stderr
            expected = "|".join(sorted(rejection_codes[label]))
            owning_error = re.search(
                rf"(?m)^[^\n]*{re.escape(path.name)}:\d+:\d+: error\[({expected})\]:", text)
            if rejected.returncode == 0 or not owning_error or "GTI-B" in text:
                raise RuntimeError(f"{label}: expected its owning lifetime diagnostic in the fixture; {output / (label + '-reject.log')}")
        checked(label + "/reject", reject)

    # Compare the same identity operation with an explicit and an elided
    # contract. Object symbols prove generated allocation dependencies only.
    for label in ("explicit_identity", "elided_identity", "borrowed_record", "borrowed_enum", "generic_borrowed_enum", "higher_ranked_pointer"):
        def inspect_object():
            obj, ir = output / (label + ".o"), output / (label + ".ll")
            run([emitter, output / (label + ".gti"), obj, ir, 3, "--source-preflight"], label + "-object")
            symbols = run(["nm", "-u", obj], label + "-undefined-symbols").stdout
            forbidden = re.findall(r"(?:_?gti_rt_(?:[A-Za-z_]*(?:malloc|allocate|realloc|calloc)[A-Za-z_0-9]*)|_?(?:malloc|calloc|realloc|aligned_alloc)|_Z[nad][A-Za-z_0-9]*)", symbols)
            if forbidden:
                raise RuntimeError(f"{label}: unexpected generated allocation imports: {forbidden}")
            report["cases"][label]["object"] = {"bytes": obj.stat().st_size,
                "allocation_imports": forbidden, "ir": str(ir), "symbols": str(output / (label + '-undefined-symbols.log'))}
            save()
        checked(label + "/object", inspect_object)
    explicit = report["cases"]["explicit_identity"].get("object")
    elided = report["cases"]["elided_identity"].get("object")
    if explicit is not None and elided is not None:
        report["identity_object_byte_delta"] = explicit["bytes"] - elided["bytes"]
    report["complete"] = not failures
    save()
    print(f"lifetime native evidence: {output / 'evidence.json'}", flush=True)
    if failures:
        raise RuntimeError(f"{len(failures)} lifetime native checks failed; see retained evidence")


if __name__ == "__main__":
    main()
