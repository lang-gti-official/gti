#!/usr/bin/env python3
"""Concrete class/lifetime execution through independent native endpoints."""

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


CASES = {
    "initializer-list-and-defaults": r'''
mut int events = 0;
int mark(int value) { events = events * 10 + value; return value; }
class Mixed {
  int data;
  int defaulted = mark(2);
  int overridden = mark(9);
public:
  Mixed(int input) : data(mark(input)), overridden(mark(3)) {}
  int total() { return this.data + this.defaulted + this.overridden; }
};
int main() {
  Mixed value{1};
  return value.total() == 6 && events == 123 ? 0 : 1;
}
''',
    "ordinary": r'''
mut int events = 0;
class Ticket {
public:
  mut int value = 1;
  Ticket(int input) : value(input) { events = events * 10 + 1; }
  ~Ticket() { events = events * 10 + 2; }
  int read() { return this.value; }
public:
  Ticket() = default;
};
int main() {
  { Ticket ticket{7}; if (ticket.read() != 7) { return 1; } }
  return events == 12 ? 0 : 2;
}
''',
    "default-and-generic": r'''
struct Plain { int value = 7;  public: Plain() = default; };
class Cell<T> {
  mut T value;
public:
  Cell(T value) : value(value) {}
  T read() { return this.value; }
  void set(T value) mut { this.value = value; }
};
int main() {
  Plain plain{};
  mut Cell<int> cell{5};
  cell.set(9);
  return plain.value == 7 && cell.read() == 9 ? 0 : 1;
}
''',
    "copy-snapshot": r'''
class Cell {
public:
  mut int value;
  Cell(int value) : value(value) {}
};
int inspect(Cell copy, mut Cell& source) { source.value = 99; return copy.value; }
int main() {
  mut Cell original{7};
  Cell snapshot = original;
  original.value = 8;
  if (snapshot.value != 7 || inspect(original, original) != 8) { return 1; }
  return original.value == 99 ? 0 : 2;
}
''',
    "move-owner": r'''
mut int destroyed = 0;
mut int observed = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { destroyed += this.id; }
  int read() { return this.id; }
};
void consume(Ticket value) { observed = value.read(); }
int main() {
  { mut Ticket first{3}; Ticket second = Ticket(std::move(first)); consume(std::move(second)); }
  return destroyed == 3 && observed == 3 ? 0 : 1;
}
''',
    "nested-cleanup": r'''
mut int events = 0;
void record(int value) { events = events * 10 + value; }
class Child {
  int id;
public:
  Child(int id) : id(id) { record(id); }
  ~Child() { record(this.id + 4); }
};
class Parent {
  Child left;
  Child right;
public:
  Parent() : left(Child(1)), right(Child(2)) { record(3); }
  ~Parent() { record(4); }
};
int main() { { Parent value{}; } return events == 123465 ? 0 : 1; }
''',
    "return-and-replace": r'''
mut int destroyed = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { destroyed = destroyed * 10 + this.id; }
  int read() { return this.id; }
};
Ticket make(int id) { Ticket value{id}; return value; }
int main() {
  { mut Ticket value = make(1); value = make(2); if (value.read() != 2) { return 1; } }
  return destroyed == 12 ? 0 : 2;
}
''',
    "class-array": r'''
mut int destroyed = 0;
class Item {
  int id;
public:
  Item(int id) : id(id) {}
  ~Item() { destroyed = destroyed * 10 + this.id; }
  int read() { return this.id; }
};
int main() {
  { Item values[2] = {Item(1), Item(2)}; if (values[0].read() != 1) { return 1; } }
  return destroyed == 21 ? 0 : 2;
}
''',
    "static-storage": r'''
struct Plain { int value = 7;  public: Plain() = default; };
mut int initialized = 0;
Plain make_plain() { initialized += 1; return Plain(); }
Plain global{};
Plain dynamic_global = make_plain();
class Counter {
public:
  static mut int value = 3;
  static Plain plain{};
  static Plain dynamic = make_plain();
public:
  Counter() = default;
};
int main() {
  Counter::value += 4;
  return global.value == 7 && dynamic_global.value == 7 &&
         Counter::plain.value == 7 && Counter::dynamic.value == 7 &&
         Counter::value == 7 && initialized == 2 ? 0 : 1;
}
''',
    "tls-passive-dynamic": r'''
mut int initialized = 0;
int initialize() { initialized += 1; return 4; }
thread_local mut int passive = 3;
thread_local bool enabled = true;
thread_local mut int dynamic = initialize();
class State { public: static thread_local mut int count = initialize();  public: State() = default; };
int main() {
  if (initialized != 0 || !enabled || passive != 3 || dynamic != 4 || dynamic != 4) { return 1; }
  if (State::count != 4 || State::count != 4) { return 2; }
  return initialized == 2 ? 0 : 3;
}
''',
    "tls-object": r'''
#include <std/panic>
mut int events = 0;
class Item {
  int id;
public:
  Item(int id) : id(id) { events = events * 10 + id; }
  ~Item() {
    if (this.id == 2 && events != 12) { std::panic("bad TLS second cleanup"); }
    if (this.id == 1 && events != 122) { std::panic("bad TLS first cleanup"); }
    events = events * 10 + this.id;
  }
  int read() { return this.id; }
};
thread_local Item first{1};
thread_local Item second{2};
int main() { return first.read() == 1 && second.read() == 2 && first.read() == 1 && events == 12 ? 0 : 1; }
''',
    "borrowed-record": r'''
class View {
  mut int& target;
public:
  View(mut int& target) : target(target) {}
  int read() { return this.target; }
  void set(int value) mut { this.target = value; }
};
int main() {
  mut int value = 3;
  { mut View view{value}; view.set(8); if (view.read() != 8) { return 1; } }
  return value == 8 ? 0 : 2;
}
''',
    "designated-and-overloaded": r'''
struct Point { int x = 1; int y = 2;  public: Point() = default; };
class Cell {
  int value;
public:
  Cell() : value(3) {}
  Cell(int value) : value(value) {}
  int read() { return this.value; }
};
int main() {
  Point point{.x = 4, .y = 5}; Cell first{}; Cell second{6};
  return point.x == 4 && point.y == 5 && first.read() == 3 && second.read() == 6 ? 0 : 1;
}
''',
    # A field reborrowed through a reference names a loan root no place spells.
    "loan-root-field-reborrow": r'''
class Counter {
public:
  mut int value;
  Counter(int initial) : value(initial) {}
};
void bump(mut Counter& parent) {
  mut int& field = parent.value;
  field += 3;
}
int main() {
  mut Counter counter = Counter(10);
  mut Counter& parent = counter;
  mut int& field = parent.value;
  field += 2;
  bump(counter);
  return counter.value == 15 ? 0 : 1;
}
''',
    # A by-value unsafe call result receives further calls through a temporary borrow.
    "unsafe-value-receiver": r'''
class Source {
public:
  int base;
  Source(int initial) : base(initial) {}
  unsafe expected<int, int> fetch(int offset) {
    if (offset < 0) { return unexpected(offset); }
    return this.base + offset;
  }
};
int main() {
  Source source = Source(40);
  mut int result = 0;
  mut bool present = false;
  unsafe {
    result = source.fetch(2).expect("fetch failed");
    present = source.fetch(-1).has_value();
  }
  if (present) { return 2; }
  return result == 42 ? 0 : 1;
}
''',
    # A type recursing through its own unique owner, built, read and destroyed.
    "recursive-owned-links": r'''
#include <std/memory>
mut int destroyed = 0;
struct Link {
  mut int value;
  mut std::unique_ptr<Link> next = std::unique_ptr<Link>();
  Link(int initial) : value(initial) {}
  ~Link() { destroyed += 1; }
};
class Chain {
public:
  mut std::unique_ptr<Link> head = std::unique_ptr<Link>();
  Chain() {}
  void push(int value) mut {
    mut std::unique_ptr<Link> node = std::make_unique<Link>(value);
    node->next = std::move(this.head);
    this.head = std::move(node);
  }
};
int total(Chain& chain) {
  mut int sum = 0;
  if (chain.head != nullptr) {
    sum += chain.head->value;
    if (chain.head->next != nullptr) {
      sum += chain.head->next->value;
    }
  }
  return sum;
}
int main() {
  {
    mut Chain chain = Chain();
    chain.push(1);
    chain.push(2);
    chain.push(3);
    if (total(chain) != 5) { return 1; }
  }
  return destroyed == 3 ? 0 : 2;
}
''',
    # Destructor-bearing results produced inside conditional operands are materialized in place.
    "conditional-temporary-materialization": r'''
mut int events = 0;
class Tracked {
public:
  int value;
  Tracked(int initial) : value(initial) {}
  int read() { return this.value; }
  ~Tracked() { events = events * 10 + this.value; }
};
Tracked make(int value) { return Tracked(value); }
int main() {
  bool early = false;
  if (early || make(7).read() != 7) { return 1; }
  if (!early && make(8).read() != 8) { return 2; }
  int chosen = early ? 0 : make(9).read();
  if (chosen != 9) { return 3; }
  return events == 789 ? 0 : 4;
}
''',
    # Methods reached through raw pointers stage read and mutable receivers.
    "raw-pointer-method-receivers": r'''
class Node {
public:
  mut int value;
  Node(int initial) : value(initial) {}
  int read() { return this.value; }
  void add(int amount) mut { this.value += amount; }
};
int main() {
  mut Node node = Node(5);
  Node* pointer = &node;
  const Node* view = &node;
  mut int total = 0;
  unsafe {
    total = view->read();
    pointer->add(3);
    total += pointer->read();
  }
  return total == 13 && node.value == 8 ? 0 : 1;
}
''',
    # By-value borrow carriers produced inside conditional operands are built in their hidden temporaries.
    "conditional-carrier-temporaries": r'''
mut int destroyed = 0;
class Counter {
  mut int value;
public:
  Counter(int initial) : value(initial) {}
  int read() { return this.value; }
  void bump() mut { this.value += 1; }
};
class Note {
  int tag;
public:
  Note(int value) : tag(value) {}
  int read() { return this.tag; }
  ~Note() { destroyed += 1; }
};
class View {
  Counter& source;
  Note label;
public:
  View(Counter& counter, int tag) : source(counter), label(Note(tag)) {}
  int sum() { return this.source.read() + this.label.read(); }
};
class Writer {
  mut Counter& target;
public:
  Writer(mut Counter& counter) : target(counter) {}
  int bump() mut { this.target.bump(); return this.target.read(); }
};
class Hub {
  mut Counter counter;
public:
  Hub() : counter(Counter(10)) {}
  View view(int tag) { return View(this.counter, tag); }
  Writer writer() mut { return Writer(this.counter); }
};
int main() {
  mut Hub hub{};
  bool early = false;
  if (early || hub.view(5).sum() != 15) { return 1; }
  if (!early && hub.view(2).sum() != 12) { return 2; }
  int chosen = early ? 0 : hub.view(1).sum();
  if (chosen != 11) { return 3; }
  if (early || hub.writer().bump() != 11) { return 4; }
  return destroyed == 3 ? 0 : 5;
}
''',
    # Array elements take the implicit scalar initialization conversion from differently typed values.
    "aggregate-element-conversions": r'''
double third(double values[3]) { return values[2]; }
class Mix {
  double values[3];
public:
  Mix(double initial[3]) : values(initial) {}
  double at(uint64_t index) { return this.values[index]; }
};
int main() {
  double floats[3] = {1.0, -0.0, 3.0};
  if (floats[0] != double(1.0) || floats[2] != double(3.0)) { return 1; }
  int64_t widened[2] = {int32_t(-5), uint8_t(200)};
  if (widened[0] != int64_t(-5) || widened[1] != int64_t(200)) { return 3; }
  if (third(floats) != double(3.0)) { return 4; }
  Mix mix = Mix(floats);
  if (mix.at(uint64_t(0)) != double(1.0)) { return 5; }
  return 0;
}
''',
    # A stored borrow carrier returned by value only states its provenance.
    "returned-view-provenance": r'''
class IntView {
public:
  int& target;
  IntView(int& value) : target(value) {}
  int read() { return this.target; }
};
class Holder {
public:
  IntView view;
  Holder(int& value) : view(IntView(value)) {}
  IntView get() { return this.view; }
};
int main() {
  int value = 41;
  Holder holder = Holder(value);
  IntView copy = holder.get();
  return copy.read() == 41 ? 0 : 1;
}
''',
    # A by-value parameter formed from a named place is the callee's own copy: moving or reassigning it leaves the caller's object intact.
    "by-value-place-parameters": r'''
enum Token { word(int value), stop };
enum Slot { ready(int& value), empty };
int peek(Token token) {
  switch (token) {
  case Token::word(auto& value): return value;
  case Token::stop: return -1;
  }
}
int take(Token token) { Token kept = std::move(token); return peek(kept); }
int reset(mut Token token) { token = Token::stop; return peek(token); }
int weigh(Slot slot) {
  switch (slot) {
  case Slot::ready(auto& value): return value;
  case Slot::empty: return 0;
  }
}
class Holder {
  Token token;
public:
  Holder(Token token) : token(std::move(token)) {}
  int read() { return peek(this.token); }
  Holder clone() { return Holder(this.token); }
};
class Shelf {
  Slot slot;
public:
  Shelf(Slot slot) : slot(std::move(slot)) {}
  int get() { return weigh(this.slot); }
};
int main() {
  Token named = Token::word(7);
  Holder holder = Holder(named);
  if (peek(named) != 7 || holder.read() != 7) { return 1; }
  if (take(named) != 7 || peek(named) != 7) { return 2; }
  if (reset(named) != -1 || peek(named) != 7) { return 3; }
  Holder second = holder.clone();
  if (holder.read() != 7 || second.read() != 7) { return 4; }
  int owner = 3;
  Slot slot = Slot::ready(owner);
  Shelf shelf = Shelf(slot);
  if (weigh(slot) != 3 || shelf.get() != 3) { return 5; }
  return 0;
}
''',
    # An unsafe call's by-value result moves into its consumer, and a fallible unsafe call's reference result is read and passed on in its success block.
    "unsafe-result-staging": r'''
mut int32_t divisor = 1;
class Window {
  int32_t& value;
public:
  Window(int32_t& value) : value(value) {}
  int32_t read() { return this.value; }
};
unsafe Window open(int32_t& value) { return Window(value); }
unsafe Window checked(int32_t& value) {
  int32_t guard = 10 / divisor;
  return Window(value);
}
int32_t consume(Window window) { return window.read(); }
class Slots {
  mut int32_t values[3];
public:
  Slots() : values({1, 2, 3}) {}
  unsafe int32_t& at(uint64_t index) { return this.values[index]; }
};
int32_t twice(int32_t& value) { return value * 2; }
int main() {
  int32_t value = 7;
  mut Slots slots{};
  unsafe {
    if (consume(open(value)) != 7) { return 1; }
    if (consume(checked(value)) != 7) { return 2; }
    if (slots.at(uint64_t(1)) != 2) { return 3; }
    if (twice(slots.at(uint64_t(2))) != 6) { return 4; }
  }
  return 0;
}
''',
    # Field initializers publish null addresses, address values and whole array values directly into their fields.
    "field-publication-values": r'''
enum class Mode : int32_t { off = 0, on = 1 };
class Defaults {
public:
  c_string native = nullptr;
  int32_t* cursor = nullptr;
  std::string_view names[2] = {"first", "second"};
  int32_t values[3] = {1, 2, 3};
  Mode modes[2] = {Mode::on, Mode::off};
  Defaults() = default;
};
class Tagged {
public:
  c_string tag;
  c_string kept;
  Tagged(c_string tag, c_string kept) : tag(tag), kept(std::move(kept)) {}
};
int main() {
  Defaults defaults{};
  if (defaults.native != nullptr || defaults.cursor != nullptr) { return 1; }
  if (defaults.names[1] != "second" || defaults.values[2] != 3) { return 2; }
  if (defaults.modes[0] != Mode::on || defaults.modes[1] != Mode::off) { return 3; }
  Tagged tagged{"alpha", "beta"};
  if (tagged.tag == nullptr || tagged.kept == nullptr) { return 4; }
  return 0;
}
''',
    # Unqualified fields in a constructor body are the constructed object's fields, exactly like this.field.
    "implicit-constructor-fields": r'''
mut int32_t seen = 0;
class Meter {
  mut int32_t level;
  mut int32_t peak = 1;
  int32_t limit;
public:
  Meter(int32_t start, int32_t cap) : level(start), limit(cap) {
    if (level > limit) { level = limit; }
    peak += level;
    seen = peak * 10 + level;
  }
  int32_t read() { return this.level + this.peak; }
};
int main() {
  Meter low{3, 5};
  Meter high{9, 4};
  if (low.read() != 7 || high.read() != 9) { return 1; }
  return seen == 54 ? 0 : 2;
}
''',
    # Methods on a field reached through a raw pointer use that field as their receiver; an unsafe method keeps its own permission, and a temporary dropped in that statement is an ordinary drop.
    "raw-field-receivers": r'''
mut int32_t dropped = 0;
class Counter {
  mut int32_t value = 0;
public:
  Counter() {}
  void bump() mut { this.value += 1; }
  int32_t read() { return this.value; }
};
class Label {
  int32_t id;
public:
  Label(int32_t id) : id(id) {}
  ~Label() { dropped += this.id; }
  int32_t read() { return this.id; }
};
class Node {
public:
  mut Counter counter;
  mut Label label;
  Node() : counter(Counter()), label(Label(1)) {}
  unsafe int32_t total() { return this.counter.read() + this.label.read(); }
};
Node* same(Node* node) { return node; }
int main() {
  mut Node node{};
  mut Node* pointer = &node;
  unsafe {
    pointer->counter.bump();
    same(pointer)->counter.bump();
    if (pointer->counter.read() != 2) { return 1; }
    same(pointer)->label = Label(10);
    if (pointer->label.read() != 10 || dropped != 1) { return 2; }
    if (same(pointer)->total() != 12) { return 3; }
  }
  return 0;
}
''',
    # A reference reborrowed from a carrier's stored reference designates the referent, and a payload returned with a borrowed view only states provenance.
    "carrier-reborrow-provenance": r'''
#include <std/string>
struct Pair {
  int32_t& left;
  mut int32_t& right;
  Pair(int32_t& left, mut int32_t& right) : left(left), right(right) {}
};
enum Word { text(std::string_view value), none };
Word word_of(std::string& input) { return Word::text(input.view()); }
int main() {
  int32_t a = 1;
  mut int32_t b = 2;
  mut Pair pair{a, b};
  mut int32_t& right = pair.right;
  right = right + pair.left + 4;
  if (b != 7) { return 1; }
  std::string source{"word"};
  Word word = word_of(source);
  switch (word) {
  case Word::text(value): return value.size() == std::size_t(4) ? 0 : 2;
  case Word::none: return 3;
  }
}
''',
    # A consumed receiver selected by a conditional is destroyed exactly once: a fresh one at the full-expression end, a moved one through its moved-from owner.
    "consuming-conditional-receivers": r'''
mut int log = 0;
class Tracked {
  int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() { log = log * 10 + this.id; }
  int read() { return this.id; }
};
class Single {
  mut Tracked item;
public:
  Single(int value) : item(Tracked(value)) {}
  Tracked release() && { return std::move(this.item); }
  int take() && { return this.item.read(); }
};
Single make(int value) { return Single(value); }
int fresh(bool left) {
  log = 0;
  int taken = (left ? make(1) : Single(2)).take();
  return taken * 100 + log;
}
int named(bool left) {
  log = 0;
  {
    Single a{3};
    Single b{4};
    Tracked chosen = left ? std::move(a).release() : std::move(b).release();
    if (chosen.read() != (left ? 3 : 4)) { return -1; }
  }
  return log;
}
int main() {
  if (fresh(true) != 101 || fresh(false) != 202) { return 1; }
  if (named(true) != 34 || named(false) != 43) { return 2; }
  return 0;
}
''',
}

FAILURES = {
    "moved-argument-failure": (r'''
#include <std/panic>
mut int dropped = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { dropped = dropped * 10 + this.id; }
};
class Guard {
public:
  Guard() {}
  ~Guard() { if (dropped != 21) { std::panic("incorrect moved argument cleanup"); } }
};
int fail() { std::panic("argument failure"); return 0; }
void consume(Ticket first, Ticket second, int other) {}
int main() {
  Guard guard{};
  mut Ticket first{1};
  mut Ticket second{2};
  consume(std::move(first), std::move(second), fail());
  return 0;
}
''', "GTI-R0017", "argument failure"),
    "moved-constructor-argument-failure": (r'''
#include <std/panic>
mut int dropped = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { dropped += this.id; }
};
class Guard {
public:
  Guard() {}
  ~Guard() { if (dropped != 7) { std::panic("incorrect constructor argument cleanup"); } }
};
class Consumer {
public:
  Consumer(Ticket ticket, int other) { std::panic("incorrect constructor entry"); }
public:
  Consumer() = default;
};
int fail() { std::panic("constructor argument failure"); return 0; }
int main() {
  Guard guard{};
  mut Ticket ticket{7};
  Consumer consumer{std::move(ticket), fail()};
  return 0;
}
''', "GTI-R0017", "constructor argument failure"),
    "moved-argument-callee-failure": (r'''
#include <std/panic>
mut int dropped = 0;
class Ticket {
  int id;
public:
  Ticket(int id) : id(id) {}
  ~Ticket() { dropped += this.id; }
  int read() { return this.id; }
};
class Guard {
public:
  Guard() {}
  ~Guard() { if (dropped != 7) { std::panic("incorrect callee argument cleanup"); } }
};
void consume(Ticket ticket) {
  if (ticket.read() != 7) { std::panic("incorrect argument publication"); }
  std::panic("callee failure");
}
int main() {
  Guard guard{};
  mut Ticket ticket{7};
  consume(std::move(ticket));
  return 0;
}
''', "GTI-R0017", "callee failure"),
    "constructor-rollback": (r'''
#include <std/panic>
mut int events = 0;
class Guard {
public:
  Guard() {}
  ~Guard() { if (events != 21) { std::panic("incorrect constructor rollback"); } }
};
class Field {
  int id;
public:
  Field(int id) : id(id) { if (id == 3) { std::panic("constructor failure"); } }
  ~Field() { events = events * 10 + this.id; }
};
class Outer {
  Field first;
  Field second;
  Field third;
public:
  Outer() : first(Field(1)), second(Field(2)), third(Field(3)) {}
  ~Outer() { std::panic("incomplete object destructor executed"); }
};
int main() { Guard guard{}; Outer value{}; return 0; }
''', "GTI-R0017", "constructor failure"),
    "recursive-tls": (r'''
thread_local int value = read();
int read() { return value; }
int main() { return value; }
''', "GTI-R0013", "recursive_thread_local_initialization"),
    "tls-shutdown": (r'''
int initialize() { return 2; }
thread_local int fresh = initialize();
mut int observed = 0;
class Item { public: Item() {} ~Item() { observed = fresh; } int read() { return 1; } };
thread_local Item late{};
int main() { return late.read() - 1; }
''', "GTI-R0013", "thread_local_initialization_during_shutdown"),
}

NATIVE_ONLY = {
    # The C++ vocabulary also rejects this array-field transfer. Native
    # execution must nevertheless retire each moved source element's state.
    "move-array-field": r'''
mut int destroyed = 0;
class Item {
  int id;
public:
  Item(int id) : id(id) {}
  ~Item() { destroyed = destroyed * 10 + this.id; }
};
struct Envelope { mut Item items[2] = {Item(1), Item(2)};  public: Envelope() = default; };
struct Grid { mut Item items[2][2] = {{Item(1), Item(2)}, {Item(3), Item(4)}};  public: Grid() = default; };
int main() {
  { mut Envelope envelope{}; { Item moved[2] = std::move(envelope.items); } }
  if (destroyed != 21) { return 1; }
  destroyed = 0;
  { mut Grid grid{}; { Item moved[2][2] = std::move(grid.items); } }
  return destroyed == 4321 ? 0 : 2;
}
''',
    # Existing C++ vocabulary rejects mutation after a dynamic local TLS
    # initializer. This is retained as an explicit comparator limitation.
    "function-local-tls": r'''
mut int initialized = 0;
int initialize() { initialized += 1; return 4; }
int local() { thread_local mut int count = initialize(); count += 1; return count; }
int main() { return local() == 5 && local() == 6 && initialized == 1 ? 0 : 1; }
''',
    "tls-compound-operators": r'''
int initialize() { return 4; }
int local() {
  thread_local mut int value = initialize();
  value += 3; value *= 2; value -= 1; value /= 3; value %= 4;
  value |= 8; value &= 15; value ^= 2; value <<= 1; value >>= 1;
  return value;
}
int main() { return local() == 10 ? 0 : 1; }
''',
}

NATIVE_FAILURES = {
    "tls-compound-narrow": (r'''
int8_t initialize() { return int8_t(127); }
int local() { thread_local mut int8_t value = initialize(); value += 1; return 0; }
int main() { return local(); }
''', "GTI-R0006"),
    "tls-compound-shift": (r'''
int initialize() { return 4; }
int local(int count) { thread_local mut int value = initialize(); value <<= count; return value; }
int main() { return local(32); }
''', "GTI-R0005"),
}


def success_cases(root: Path) -> dict[str, str]:
    cases = dict(CASES)
    cases["shipped-example"] = (
        root / "examples/llvm/classes-and-lifetimes.gti").read_text(
            encoding="utf-8")
    cases["fresh-transfer-placement"] = (
        root / "tests/fixtures/llvm_class_fresh_transfer_regression.gti").read_text(
            encoding="utf-8")
    return cases


def evidence_cases(root: Path) -> dict[str, str]:
    return {**success_cases(root),
            **{name: source for name, (source, _, _) in FAILURES.items()},
            **NATIVE_ONLY,
            **{name: source for name, (source, _) in NATIVE_FAILURES.items()}}


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    cases = evidence_cases(root)
    profiles = {name: (name,) for name in cases}
    assert all(case in cases for profile in profiles.values() for case in profile)
    return profiles


def evidence_manifest(root: Path) -> dict:
    return build_manifest(
        "llvm_class_execution", evidence_cases(root), evidence_profiles(root),
        {"shipped-example": "examples/llvm/classes-and-lifetimes.gti",
         "fresh-transfer-placement": "tests/fixtures/llvm_class_fresh_transfer_regression.gti"})


def list_evidence(root: Path) -> None:
    print(json.dumps(evidence_manifest(root), sort_keys=True))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        list_evidence(Path(sys.argv[2]).resolve())
        return
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    runnable_cases = success_cases(root)
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected and selected not in runnable_cases and selected not in FAILURES and selected not in NATIVE_ONLY and selected not in NATIVE_FAILURES:
        raise ValueError(f"unknown class execution case: {selected}")
    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    endpoints = (Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", mode) for mode in ("O0", "O1", "O2", "O3")))
    with tempfile.TemporaryDirectory(prefix="gti-llvm-classes-") as directory:
        work = Path(directory)
        for name, text in runnable_cases.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for endpoint in endpoints:
                case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard}"
                result = execute(gti, root, case, source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: C++20/23 O3 and LLVM O0-O3 passed", flush=True)
        for name, (text, code, message) in FAILURES.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            observables = []
            for endpoint in endpoints:
                case = work / f"{name}-{endpoint.backend}-{endpoint.optimization}-{endpoint.standard}"
                result = execute(gti, root, case, source, endpoint, env)
                if (result.error or result.returncode != 70 or code not in result.stderr
                        or message not in result.stderr or "incorrect" in result.stderr
                        or "incomplete object" in result.stderr):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observables.append(result.observable)
            if len(set(observables)) != 1:
                raise AssertionError(f"failure attribution drift in {name}: {observables}")
            print(f"{name}: exact C++20/23 and LLVM O0-O3 failure passed", flush=True)
        for name, text in NATIVE_ONLY.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for endpoint in endpoints[2:]:
                result = execute(gti, root, work / f"{name}-{endpoint.optimization}", source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: LLVM O0-O3 passed (C++ comparator has a recorded vocabulary limitation)", flush=True)
        for name, (text, code) in NATIVE_FAILURES.items():
            if selected and name != selected:
                continue
            source = work / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            for endpoint in endpoints[2:]:
                result = execute(gti, root, work / f"{name}-{endpoint.optimization}", source, endpoint, env)
                if result.error or result.returncode != 70 or code not in result.stderr:
                    raise AssertionError(f"{name} {endpoint}: {result}")
            print(f"{name}: LLVM O0-O3 checked failure passed", flush=True)


if __name__ == "__main__":
    main()
