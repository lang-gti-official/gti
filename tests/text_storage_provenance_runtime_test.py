#!/usr/bin/env python3
"""Text read from arrays, raw memory or callables runs alike on both backends.

A view copied out of an array of views holds the ownerless text the array
holds, a C string loaded through a raw pointer is the inert native address the
caller stored there, and a callable's text result is what its target returns.
MIR proves each result ownerless, as it does a reassigned local's text, a
method's call result through its receiver's field, and an erased callable's
result through a virtual root or std::function. An override keeps its root's
relay in its own instance of the root's class, and an overloaded operator's
text result keeps its relayed owner borrowed. An invocation pointer's text
result is ownerless, so only a target returning ownerless text forms one, and
a derived method returns an inherited view field as ownerless text. A lambda's
view over its captured text is used only locally and keeps the captured view's
owner borrowed. Each program must build and behave identically on C++ and LLVM
at -O0 and -O2.

    python3 tests/text_storage_provenance_runtime_test.py <gti> <repo-root>
"""

from pathlib import Path
import os
import sys
import tempfile

from backend_execution import Endpoint, execute


ARRAY_ELEMENTS = r'''
#include <std/string_view>
std::string_view table[3] = {"zero", "one", "two"};
mut std::string_view kept = "";
std::string_view pick(std::string_view names[3], uint64_t index) { return names[index]; }
std::string_view fromTable(uint64_t index) { return table[index]; }
std::string_view fromLocal(uint64_t index) {
  std::string_view local[2] = {"left", "right"};
  return local[index];
}
std::string_view place(std::string_view text, uint64_t at) {
  mut std::string_view slots[2] = {"first", "second"};
  slots[at] = text;
  return slots[at];
}
int main() {
  std::string_view names[3] = {"ab", "cde", ""};
  kept = pick(names, uint64_t(1));
  std::println(kept);
  std::println(pick(names, uint64_t(0)));
  if (!pick(names, uint64_t(2)).empty() || kept.size() != uint64_t(3)) { return 1; }
  std::println(fromTable(uint64_t(2)));
  std::println(fromLocal(uint64_t(1)));
  std::println(place("placed", uint64_t(1)));
  return pick(names, uint64_t(1)) == "cde" ? 0 : 2;
}
'''

ARRAY_FIELDS_AND_LAMBDAS = r'''
#include <std/string_view>
struct Shelf {
  std::string_view titles[2];
  int count;
  Shelf() : titles({"x", "yz"}), count(2) {}
};
std::string_view fromShelf(Shelf shelf, uint64_t index) { return shelf.titles[index]; }
std::string_view labelled() {
  auto label = []() -> std::string_view { return "label"; };
  std::string_view labels[2] = {label(), "other"};
  return labels[0];
}
int main() {
  std::println(fromShelf(Shelf(), uint64_t(1)));
  std::println(labelled());
  return fromShelf(Shelf(), uint64_t(0)) == "x" ? 0 : 1;
}
'''

ELEMENT_REFERENCES = r'''
#include <std/string_view>
void setTo(mut std::string_view& out, std::string_view value) { out = value; }
std::string_view throughReferences() {
  mut std::string_view words[2] = {"a", "bc"};
  for (mut std::string_view& word : words) { word = "q"; }
  setTo(words[0], "zz");
  std::string_view& last = words[1];
  return last == "q" ? words[0] : "missed";
}
int main() {
  std::println(throughReferences());
  return throughReferences() == "zz" ? 0 : 1;
}
'''

CALLABLE_RESULTS = r'''
#include <std/functional>
#include <std/string_view>
// A callable's text result through a GTI invocation pointer is what its
// target returns: a user-defined callable class, a lambda or a function item.
class Greeter {
public:
  std::string_view greeting;
  Greeter(std::string_view greeting) : greeting(greeting) {}
  std::string_view operator()() { return this.greeting; }
};
std::string_view small(int n) { return n < 10 ? "small" : "large"; }
std::string_view invoke(std::function_ptr<std::string_view(int)> pick, int n) { return pick(n); }
mut std::string_view kept = "";
int main() {
  Greeter hello = Greeter("hello");
  std::function_ref<std::string_view()> greet{hello};
  kept = greet();
  std::println(kept);
  auto lit = []() -> std::string_view { return "lit"; };
  std::function_ref<std::string_view()> viewed{lit};
  std::println(viewed());
  kept = invoke(std::function_ptr<std::string_view(int)>(small), 30);
  std::println(kept);
  auto zero = [](int n) -> std::string_view { return n == 0 ? "zero" : "other"; };
  std::function_ptr<std::string_view(int)> viaLambda{zero};
  std::println(viaLambda(1));
  return kept == "large" ? 0 : 1;
}
'''

RAW_C_STRINGS = r'''
struct Slots {
  c_string* items = nullptr;
  int32_t count = 0;
public:
  Slots() = default;
};
mut c_string kept = nullptr;
void put(c_string* items, int32_t at, c_string value) { unsafe { items[at] = value; } }
c_string second(c_string* items) { unsafe { c_string* next = items + 1; return *next; } }
c_string at(c_string* items, int32_t index) { unsafe { return items[index]; } }
c_string viaLocal(c_string* items, int32_t index) {
  unsafe {
    c_string* base = items;
    return base[index];
  }
}
c_string first(Slots slots) { unsafe { return slots.items[0]; } }
c_string viaReference(c_string*& items) { unsafe { return items[1]; } }
int main() {
  mut c_string slots[3] = {nullptr, nullptr, nullptr};
  if (second(&slots[0]) != nullptr) { return 1; }
  put(&slots[0], 1, "b");
  kept = second(&slots[0]);
  if (kept == nullptr || kept != slots[1] || at(&slots[0], 1) != kept) { return 2; }
  if (viaLocal(&slots[0], 0) != nullptr || at(&slots[0], 2) != nullptr) { return 3; }
  mut c_string* base = &slots[0];
  Slots view{.items = base, .count = 3};
  if (first(view) != nullptr || viaReference(base) != kept) { return 4; }
  unsafe { slots[1] = nullptr; }
  return second(&slots[0]) == nullptr ? 0 : 5;
}
'''

RAW_TEXT_VIEWS = r'''
#include <std/string_view>
// A view read through a raw pointer, its index or `->` is the view stored
// there; the pointer formal is not its owner.
struct Entry {
  std::string_view name;
  int id;
};
std::string_view viewAt(std::string_view* item) { unsafe { return *item; } }
std::string_view wordAt(std::string_view* items, int32_t index) { unsafe { return items[index]; } }
std::string_view nameAt(Entry* entry) { unsafe { return entry->name; } }
mut std::string_view kept = "";
int main() {
  mut std::string_view words[2] = {"alpha", "beta"};
  kept = wordAt(&words[0], 1);
  std::println(kept);
  std::println(viewAt(&words[0]));
  mut Entry entry{.name = "entry", .id = 1};
  std::println(nameAt(&entry));
  std::string_view blank[2] = {};
  std::string_view grid[2][2] = {{"a", "b"}, {"c", "d"}};
  std::println(grid[1][0]);
  return blank[1].empty() && kept == "beta" ? 0 : 1;
}
'''

CAPTURED_TEXT_LOCAL_USE = r'''
#include <std/string>
// A lambda's view over its captured text keeps the captured view's owner
// borrowed for each use, as the view itself does: callers use it in an
// expression, a local binding or a copy of one, a call that only reads or
// relays it, and another lambda over the closure, and may change the owner
// once the last of those uses is done.
class Printer {
public:
  mut int printed;
  Printer() : printed(0) {}
  void show<F>(F view) mut {
    std::string_view shown = view();
    std::println(shown);
    this.printed += 1;
  }
};
uint64_t measure(std::string_view text) { return text.size(); }
std::string_view same(std::string_view text) { return text; }
std::string_view keepLocal(std::string_view text) {
  auto keep = [text]() -> std::string_view { return text; };
  std::string_view held = keep();
  std::println(held);
  return "kept";
}
mut std::string_view kept = "";
int main() {
  mut std::string owner{"dynamic"};
  std::string_view view = owner.view();
  auto captured = [view]() -> std::string_view { return view; };
  std::println(captured());
  std::string_view bound = captured();
  std::string_view copied = bound;
  std::println(same(captured()));
  auto again = captured;
  auto nested = [again]() -> std::string_view { return again(); };
  mut Printer printer{};
  printer.show(nested);
  uint64_t size = measure(copied);
  owner.append("!");
  std::println(owner.view());
  std::println(keepLocal(owner.view()));
  std::string_view fixedText = "fixed";
  auto fixed = [fixedText]() -> std::string_view { return fixedText; };
  mut std::string_view reassigned = "";
  reassigned = fixed();
  auto literal = []() -> std::string_view { return "literal"; };
  kept = literal();
  std::println(reassigned);
  std::println(kept);
  return size == uint64_t(7) && printer.printed == 1 ? 0 : 1;
}
'''

REASSIGNED_TEXT_LOCALS = r'''
#include <std/string>
// A local text binding holds what any of its writes stored, including a
// store through a mutable borrow of it: each holds ownerless text here.
struct Note {
  std::string_view text;
  int line;
};
void setTo(mut std::string_view& out, std::string_view value) { out = value; }
std::string_view pick(bool c) {
  mut std::string_view v = "first";
  if (c) { v = "second"; }
  return v;
}
std::string_view looped(int n) {
  mut std::string_view current = "start";
  mut std::string_view previous = "none";
  for (mut int i = 0; i < n; i += 1) {
    previous = current;
    current = "loop";
  }
  return previous;
}
std::string_view through() {
  mut std::string_view v = "before";
  setTo(v, "after");
  return v;
}
mut std::string_view kept = "";
int main() {
  mut std::string_view w = "x";
  w = "y";
  kept = w;
  Note note{.text = w, .line = 1};
  std::println(pick(true));
  std::println(pick(false));
  std::println(looped(2));
  std::println(through());
  std::println(note.text);
  return kept == "y" ? 0 : 1;
}
'''

RECEIVER_FIELD_CALL_RETURNS = r'''
#include <std/string_view>
// A method whose relay its body never uses returns an ownerless call result
// through its receiver's field or a nested one.
class Inner {
public:
  int n;
  std::string_view text;
  Inner(int n) : n(n), text("inner-text") {}
  std::string_view label() { return this.n < 10 ? "small" : "large"; }
  std::string_view field() { return this.text; }
};
class Middle {
public:
  Inner inner;
  Middle(int n) : inner(Inner(n)) {}
};
class Box {
  Inner inner;
  Middle middle;
public:
  Box(int n) : inner(Inner(n)), middle(Middle(n)) {}
  std::string_view name() { return this.inner.label(); }
  std::string_view deep() { return this.middle.inner.label(); }
  std::string_view relay() { return this.inner.field(); }
};
int main() {
  Box small = Box(3);
  Box large = Box(30);
  std::println(small.name());
  std::println(large.deep());
  std::println(small.relay());
  return small.name() == "small" ? 0 : 1;
}
'''

ERASED_TEXT_RESULTS = r'''
#include <std/functional>
#include <std/polymorphic>
#include <std/string>
// A virtual text result follows its root's contract, a lambda's result is
// what its returns hold, and a generic result its instance makes text relays
// at the call: an erased callable over a literal lambda returns ownerless text
// through a user-defined interface and through std::function.
interface Labeler {
  std::string_view label();
};
class Fixed : public Labeler {
public:
  Fixed() {}
  std::string_view label() override { return "fixed"; }
};
class Wrapped<F> : public Labeler {
  F fn;
public:
  Wrapped(F fn) : fn(fn) {}
  std::string_view label() override { return this.fn(); }
};
class Erased {
  mut std::inline_polymorphic<Labeler, 32, 8> target{};
public:
  Erased() {}
  void set<F>(F fn) mut { this.target.replace(Wrapped<F>(fn)); }
  std::string_view operator()() { return this.target.get().label(); }
};
class Echo<R> {
public:
  int n;
  Echo() : n(0) {}
  R same(R text) { return text; }
};
mut std::string_view kept = "";
int main() {
  auto lit = []() -> std::string_view { return "lit"; };
  mut Erased erased{};
  erased.set(std::move(lit));
  kept = erased();
  std::println(kept);
  mut std::inline_polymorphic<Labeler, 32, 8> slot{};
  slot.replace(Fixed());
  std::println(slot.get().label());
  auto word = []() -> std::string_view { return "word"; };
  std::function<std::string_view()> fn{word};
  std::println(fn());
  std::string_view held = fn();
  Echo<std::string_view> echo{};
  mut std::string owner{"echoed"};
  std::string_view relayed = echo.same(owner.view());
  std::println(relayed);
  owner.append("!");
  std::println(held);
  return kept == "lit" ? 0 : 1;
}
'''

VIRTUAL_ROOT_INSTANCES = r'''
#include <std/string>
// An override's text result keeps its root's contract in the override's
// instance of the root's class: under a generic root that relays its
// argument, an override relays it or returns a literal.
class Picker<T> {
public:
  int n;
  Picker() : n(0) {}
  virtual T pick(T a) { return a; }
};
class Literal : public Picker<std::string_view> {
public:
  Literal() : Picker<std::string_view>() {}
  std::string_view pick(std::string_view a) override { return "literal"; }
};
class Same<T> : public Picker<T> {
public:
  Same() : Picker<T>() {}
  T pick(T a) override { return a; }
};
int main() {
  std::string owner{"owned"};
  Picker<std::string_view> base{};
  Literal literal{};
  Same<std::string_view> same{};
  Picker<std::string_view>& first = base;
  Picker<std::string_view>& second = literal;
  Picker<std::string_view>& third = same;
  std::println(first.pick(owner.view()));
  std::println(second.pick(owner.view()));
  std::println(third.pick(owner.view()));
  return 0;
}
'''

OPERATOR_TEXT_RELAYS = r'''
#include <std/string>
// An overloaded operator's text result relays its receiver or an argument as
// its summary says, as a method's does: the relayed owner stays borrowed
// while the result is used, and ownerless text relays nothing.
class Same {
public:
  Same() {}
  std::string_view operator()(std::string_view s) { return s; }
  std::string_view operator[](std::string_view s) { return s; }
};
class Plain {
  mut std::string s;
public:
  Plain() : s("plain") {}
  std::string_view operator()() { return this.s.view(); }
  void grow() mut { this.s.append("!"); }
};
class Echo<R> {
public:
  int n;
  Echo() : n(0) {}
  R operator()(R text) { return text; }
};
int main() {
  Same same{};
  mut Plain plain{};
  Echo<std::string_view> echo{};
  mut std::string owner{"owned"};
  std::println(same(owner.view()));
  std::println(same[owner.view()]);
  std::println(echo(owner.view()));
  std::string_view literal = same("literal");
  owner.append("!");
  std::println(literal);
  std::println(plain());
  plain.grow();
  std::println(plain());
  std::println(owner);
  return 0;
}
'''

INVOCATION_POINTER_TARGETS = r'''
#include <std/functional>
#include <std/string>
// An invocation pointer's text result is ownerless (owner decision
// 2026-10-27): a function or lambda returning ownerless text forms one, and
// its results keep no argument's owner borrowed.
std::string_view tag(std::string_view s) {
  if (s.empty()) { return "empty"; }
  return "full";
}
std::string_view call(std::function_ptr<std::string_view(std::string_view)> pointer,
                      std::string_view text) {
  return pointer(text);
}
int main() {
  auto quiet = [](std::string_view s) -> std::string_view {
    return s.empty() ? "none" : "some";
  };
  std::function_ptr<std::string_view(std::string_view)> named{tag};
  std::function_ptr<std::string_view(std::string_view)> lambda{quiet};
  mut std::string owner{"owned"};
  std::string_view first = named(owner.view());
  std::string_view second = lambda(owner.view());
  owner.append("!");
  std::println(first);
  std::println(second);
  std::println(call(named, ""));
  std::println(call(lambda, owner.view()));
  return 0;
}
'''

INHERITED_TEXT_FIELDS = r'''
#include <std/string>
// A view field a base class declares is the derived value's field too, and a
// class with a base keeps its text fields ownerless: a derived method,
// virtual or not, returns an inherited view field as ownerless text.
class Base {
public:
  std::string_view text;
  Base(std::string_view text) : text(text) {}
  virtual std::string_view name() { return "base"; }
};
class Derived : public Base {
public:
  Derived(std::string_view text) : Base(text) {}
  std::string_view name() override { return this.text; }
  std::string_view plain() { return this.text; }
};
class Holder<T> {
public:
  std::string_view text;
  T value;
  Holder(std::string_view text, T value) : text(text), value(value) {}
  virtual std::string_view name() { return this.text; }
};
class Special<T> : public Holder<T> {
public:
  Special(T value) : Holder<T>("special", value) {}
  std::string_view name() override { return this.text; }
};
Derived make(std::string_view text) { return Derived(text); }
int main() {
  Derived derived = make("derived");
  Base& base = derived;
  Special<int> special{1};
  Holder<int>& holder = special;
  Holder<int> plain{"held", 2};
  std::println(base.name());
  std::println(derived.plain());
  std::println(holder.name());
  std::println(plain.name());
  return 0;
}
'''

CASES = {
    'array-elements': (ARRAY_ELEMENTS, (0, 'cde\nab\ntwo\nright\nplaced\n', '')),
    'array-fields-and-lambdas': (ARRAY_FIELDS_AND_LAMBDAS, (0, 'yz\nlabel\n', '')),
    'element-references': (ELEMENT_REFERENCES, (0, 'zz\n', '')),
    'callable-results': (CALLABLE_RESULTS, (0, 'hello\nlit\nlarge\nother\n', '')),
    'raw-c-strings': (RAW_C_STRINGS, (0, '', '')),
    'raw-text-views': (RAW_TEXT_VIEWS, (0, 'beta\nalpha\nentry\nc\n', '')),
    'captured-text-local-use': (CAPTURED_TEXT_LOCAL_USE, (
        0, 'dynamic\ndynamic\ndynamic\ndynamic!\ndynamic!\nkept\nfixed\n'
        'literal\n', '')),
    'reassigned-text-locals': (REASSIGNED_TEXT_LOCALS, (
        0, 'second\nfirst\nloop\nafter\ny\n', '')),
    'receiver-field-call-returns': (RECEIVER_FIELD_CALL_RETURNS, (
        0, 'small\nlarge\ninner-text\n', '')),
    'erased-text-results': (ERASED_TEXT_RESULTS, (
        0, 'lit\nfixed\nword\nechoed\nword\n', '')),
    'virtual-root-instances': (VIRTUAL_ROOT_INSTANCES, (
        0, 'owned\nliteral\nowned\n', '')),
    'operator-text-relays': (OPERATOR_TEXT_RELAYS, (
        0, 'owned\nowned\nowned\nliteral\nplain\nplain!\nowned!\n', '')),
    'invocation-pointer-targets': (INVOCATION_POINTER_TARGETS, (
        0, 'full\nsome\nempty\nsome\n', '')),
    'inherited-text-fields': (INHERITED_TEXT_FIELDS, (
        0, 'derived\nderived\nspecial\nheld\n', '')),
}

ENDPOINTS = (Endpoint('cpp', 'O0', 'c++20'), Endpoint('cpp', 'O2', 'c++20'),
             Endpoint('llvm', 'O0'), Endpoint('llvm', 'O2'))


def main() -> None:
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    env = dict(os.environ)
    env.setdefault('GTI_STDLIB_PATH', str(root / 'stdlib'))
    with tempfile.TemporaryDirectory(prefix='gti-text-storage-') as directory:
        work = Path(directory)
        for name, (text, expected) in CASES.items():
            source = work / f'{name}.gti'
            source.write_text(text, encoding='utf-8')
            for endpoint in ENDPOINTS:
                case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
                result = execute(gti, root, case, source, endpoint, env)
                if result.error or result.observable != expected:
                    raise AssertionError(f'{name} {endpoint}: {result}')
            print(f'{name}: C++ and LLVM at O0/O2 agree', flush=True)
    print('array-element, raw-memory, callable, captured and local text passed')


if __name__ == '__main__':
    main()
