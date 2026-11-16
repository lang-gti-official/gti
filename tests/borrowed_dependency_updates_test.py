#!/usr/bin/env python3
"""RFC 0003 executable contract for general retaining stores and replacement.

These programs deliberately exercise local values and unrelated user records,
not a recognized container spelling. During implementation, --case selects an
individual invariant; the full invocation requires every case to pass.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import subprocess
import threading

PREFIX = 'import std.string;\nimport std.utility;\n'
CASES = {
    'stored_carrier_reference_argument_relay': (True, '''
class ReadCarrier<lifetime a> {
public:
  int&<a> referent;
  mut int shell = 0;
  ReadCarrier(int&<a> source) : referent(source) {}
  int&<a> identity() { return this.referent; }
  int&<a> relay_call() { return this.identity(); }
};

int& relay_reference(int& source) { return source; }
int& relay_external(ReadCarrier carrier) {
  int& ordinary = relay_reference(carrier.referent);
  return carrier.referent;
}
ReadCarrier relay_carrier(ReadCarrier carrier) {
  int unrelated_owner = 11;
  ReadCarrier unrelated = ReadCarrier(unrelated_owner);
  ReadCarrier unused = unrelated;
  return carrier;
}

int main() {
  int owner = 7;
  ReadCarrier first = ReadCarrier(owner);
  ReadCarrier copied = first;
  ReadCarrier moved = std::move(copied);
  int& exact = relay_external(moved);
  int& relayed_exact = moved.relay_call();
  ReadCarrier relayed = relay_carrier(std::move(moved));
  return exact + relayed_exact + relayed.referent - 21;
}
'''),
    'observing_cleanup_after_nested_move': (True, '''
mut int closed = 0;
struct Counter {
  unsafe int read() { return 7; }
  unsafe void close() { closed += 1; }
public:
  Counter() = default;
};
concept readable<T> = requires(T& source) {
  unsafe { source.read() } -> int;
};
concept closable<T> = readable<T> && requires(T& source) {
  unsafe { source.close() } -> void;
};
int observe<T>(T& source) requires readable<T> {
  unsafe { return source.read(); }
}
class Monitor<closable T> {
  T& source;
  mut int initial = 0;
public:
  Monitor(T& source) : source(source) {
    unsafe { this.initial = this.source.read(); }
  }
  int read() { return observe(this.source); }
  ~Monitor() { unsafe { this.source.close(); } }
};
class Outer<closable T> {
  Monitor<T> inner;
public:
  Outer(Monitor<T> inner) : inner(std::move(inner)) {}
  int read() { return this.inner.read(); }
};
class Unused<closable T> { int value = 0;  public: Unused() = default; };
class Defaulted<closable T = Counter> { int value = 0;  public: Defaulted() = default; };
class Packed<closable T...> { int value = 0;  public: Packed() = default; };

int main() {
  Counter counter{};
  Defaulted<> defaulted{};
  Packed<Counter, Counter> packed{};
  { Monitor<Counter> inner{counter};
    Outer<Counter> monitor{std::move(inner)};
    if (monitor.read() != 7) { return 1; } }
  return closed - 1;
}
'''),
    'borrowed_enum_replacement': (True, '''class Carrier {
  int& source;
public:
  Carrier(int& value) : source(value) {}
  int read() { return this.source; }
};
enum Holder { one(Carrier value), none };
int read(Holder& held) {
  switch (held) {
    case Holder::one(value): return value.read();
    case Holder::none: return 0;
  }
}
int main() {
  mut int first = 3; mut int second = 8;
  Carrier left{first}; Carrier right{second};
  mut Holder held = Holder::one(left);
  Holder replacement = Holder::one(right);
  held = replacement;
  first = 4;
  return read(held) == 8 ? 0 : 1;
}
'''),
    'borrowed_enum_replacement_protects_new_owner': (False, '''class Carrier {
  int& source;
public:
  Carrier(int& value) : source(value) {}
  int read() { return this.source; }
};
enum Holder { one(Carrier value), none };
int read(Holder& held) {
  switch (held) {
    case Holder::one(value): return value.read();
    case Holder::none: return 0;
  }
}
int main() {
  mut int first = 3; mut int second = 8;
  Carrier left{first}; Carrier right{second};
  mut Holder held = Holder::one(left);
  Holder replacement = Holder::one(right);
  held = replacement;
  second = 9;
  return read(held) == 8 ? 0 : 1;
}
'''),
    'borrowed_enum_copy_retains_old_owner': (False, '''class Carrier {
  int& source;
public:
  Carrier(int& value) : source(value) {}
  int read() { return this.source; }
};
enum Holder { one(Carrier value), none };
int read(Holder& held) {
  switch (held) {
    case Holder::one(value): return value.read();
    case Holder::none: return 0;
  }
}
int main() {
  mut int first = 3; mut int second = 8;
  Carrier left{first}; Carrier right{second};
  mut Holder held = Holder::one(left);
  Holder replacement = Holder::one(right);
  Holder saved = held;
  held = replacement;
  first = 4;
  return read(saved) + read(held);
}
'''),
    'growable_observing_cleanup_protects_owner_scope': (False, 'import std.vector;\nimport std.string;\nclass Observed {\n  int& source;\npublic:\n  Observed(int& input) : source(input) {}\n  ~Observed() { std::println(this.source); }\n};\nint main() {\n  mut std::vector<Observed> values{};\n  { int owner = 11; values.push_back(Observed(owner)); }\n  return 0;\n}\n'),
    'growable_passive_cleanup_after_owner_scope': (True, 'import std.vector;\nimport std.string;\nclass Observed {\n  int& source;\npublic:\n  Observed(int& input) : source(input) {}\n};\nint main() {\n  mut std::vector<Observed> values{};\n  { int owner = 11; values.push_back(Observed(owner)); }\n  return 0;\n}\n'),
    'constructor_iterator_copy_is_independent': (True, 'import std.string;\nint main() {\n  mut std::string source{"hello"};\n  std::string copy{source.begin(), source.end()};\n  source.push_back(\'!\');\n  return copy == "hello" && source == "hello!" ? 0 : 1;\n}\n'),
    'constructor_owned_copy_outlives_input': (True, '''import std.string;
class Owned {
  std::string text;
public:
  Owned(std::string_view input) : text(std::string(input)) {}
  std::size_t size() { return this.text.size(); }
};
Owned make_owned() { std::string local{"kept"}; return Owned(local.view()); }
int main() { Owned value = make_owned(); return value.size() == 4 ? 0 : 1; }
'''),
    'constructor_retained_view_protects_owner': (False, '''import std.string;
class Borrowed {
  std::string_view text;
public:
  Borrowed(std::string_view input) : text(input) {}
  std::size_t size() { return this.text.size(); }
};
int main() {
  mut std::string local{"kept"};
  Borrowed value{local.view()};
  local.push_back('!');
  return value.size() == 4 ? 0 : 1;
}
'''),
    'borrowed_enum_fresh_payload_replacement': (True, '''class Carrier {
  int& source;
public:
  Carrier(int& value) : source(value) {}
  int read() { return this.source; }
};
enum Holder { one(Carrier value), none };
int read(Holder& held) {
  switch (held) {
    case Holder::one(value): return value.read();
    case Holder::none: return 0;
  }
}
int main() {
  mut int first = 3; mut int second = 8;
  Carrier left{first}; Carrier right{second};
  mut Holder held = Holder::one(Carrier(first));
  Holder replacement = Holder::one(right);
  held = Holder::one(Carrier(second));
  first = 4;
  return read(held) == 8 ? 0 : 1;
}
'''),
    'fixed_array_destructor_protects_owner': (False, '''mut int witness = 0;
class Observed {
  int& source;
public:
  Observed(int& input) : source(input) {}
  ~Observed() { witness += this.source; }
};
int main() {
  mut int owner = 11;
  { Observed values[1] = {Observed(owner)}; owner = 22; }
  return witness;
}
'''),
    'owning_elements_range_control': (True, '''import std.memory;
import std.vector;
int main() {
  mut std::vector<std::unique_ptr<int>> values{};
  values.push_back(std::make_unique<int>(11));
  values.push_back(std::make_unique<int>(22));
  mut int total = 0;
  for (std::unique_ptr<int>& value : values) { total += *value; }
  return total == 33 ? 0 : 1;
}'''),
    'owning_elements_range_rejects_storage_mutation': (False, '''import std.memory;
import std.vector;
int main() {
  mut std::vector<std::unique_ptr<int>> values{};
  values.push_back(std::make_unique<int>(11));
  for (std::unique_ptr<int>& value : values) {
    values.clear();
    if (*value == 11) { return 0; }
  }
  return 1;
}'''),
    'owned_copy_releases_source_borrow': (True, '''struct Box {
  mut int value;
  Box(int input):value(input){}
};
Box& observe_box(Box& box) { return box; }
int main() {
  mut Box source{1};
  auto copied = observe_box(source);
  source.value = 2;
  return copied.value == 1 && source.value == 2 ? 0 : 1;
}'''),
    'local_replacement': (True, '''int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view();
  view = second.view();
  return view == "second" ? 0 : 1;
}'''),
    'replacement_releases_old_owner': (True, '''int main() {
  mut std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view();
  view = second.view(); first.clear();
  return view == "second" ? 0 : 1;
}'''),
    'copy_retains_old_owner': (False, '''int main() {
  mut std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view(); auto copy = view;
  view = second.view(); first.clear();
  return copy == "first" && view == "second" ? 0 : 1;
}'''),
    'replacement_protects_new_owner': (False, '''int main() {
  std::string first{"first"}; mut std::string second{"second"};
  mut std::string_view view = first.view();
  view = second.view(); second.clear();
  return view == "second" ? 0 : 1;
}'''),
    'local_escape_through_store': (False, '''int main() {
  mut std::string_view view = "initial";
  { std::string local{"short"}; view = local.view(); }
  return view == "short" ? 0 : 1;
}'''),
    'two_owner_record': (True, '''struct Labels<lifetime a, lifetime b> {
  std::string_view<a> first; std::string_view<b> second;
  Labels(std::string_view<a> x, std::string_view<b> y):first(x),second(y){}
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  Labels labels{first.view(),second.view()};
  return labels.first == "first" && labels.second == "second" ? 0 : 1;
}'''),
}


# An outer binding may acquire its first dependency inside a branch. Scope
# projection must preserve that mapping even though it was ownerless on entry.
CASES['branch_first_dependency_both_arms'] = (True, '''int check(bool choose) {
  std::string owner{"borrowed"}; auto source = owner.view();
  mut std::string_view view = "";
  if (choose) { view = source; } else { view = source; }
  return view == "borrowed" ? 0 : 1;
}
int main() { return check(true) + check(false); }
''')
CASES['branch_first_dependency_surviving_arm'] = (True, '''int check(bool choose) {
  std::string owner{"borrowed"}; auto source = owner.view();
  mut std::string_view view = "";
  if (choose) { view = source; } else { return 0; }
  return view == "borrowed" ? 0 : 1;
}
int main() { return check(true) + check(false); }
''')

# A literal alternative is a real known-empty dependency set, not missing
# provenance. Keep the borrowed alternative live after joining either path.
CASES['branch_first_dependency_one_arm'] = (True, r'''int check(bool choose) {
  std::string owner{"borrowed"};
  mut std::string_view view = "literal";
  if (choose) { view = owner.view(); }
  return view == (choose ? "borrowed" : "literal") ? 0 : 1;
}
int main() { return check(true) + check(false); }
''')
CASES['branch_first_dependency_protects_owner'] = (
    False, CASES['branch_first_dependency_one_arm'][1]
    .replace('std::string owner{', 'mut std::string owner{')
    .replace('  return view ==', '  owner.clear();\n  return view =='))

# Replacement must follow actual control flow, not source traversal order.
CASES['replacement_branch_join'] = (True, '''int check(bool replace) {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view();
  if (replace) { view = second.view(); }
  if (replace) { return view == "second" ? 0 : 1; }
  return view == "first" ? 0 : 2;
}
int main() { return check(true) + check(false); }
''')
# Copying a joined local retains every possible owner, independent of the
# original slot's subsequent replacement.
CASES['replacement_branch_copy'] = (True, CASES['replacement_branch_join'][1]
    .replace('  if (replace) { return view',
             '  auto copied = view; view = "replacement";\n  if (replace) { return copied')
    .replace('  return view == "first"', '  return copied == "first"'))
for owner in ('first', 'second'):
    CASES['replacement_branch_copy_protects_' + owner] = (
        False, CASES['replacement_branch_copy'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  if (replace) { return copied',
                 '  ' + owner + '.clear();\n  if (replace) { return copied'))

CASES['replacement_loop_join'] = (True, '''int check(int count) {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view();
  for (mut int i = 0; i < count; i += 1) { view = second.view(); }
  if (count == 0) { return view == "first" ? 0 : 1; }
  return view == "second" ? 0 : 2;
}
int main() { return check(0) + check(3); }
''')
CASES['replacement_loop_copy'] = (True, CASES['replacement_loop_join'][1]
    .replace('  if (count == 0) { return view',
             '  auto copied = view; view = "replacement";\n  if (count == 0) { return copied')
    .replace('  return view == "second"', '  return copied == "second"'))
for owner in ('first', 'second'):
    CASES['replacement_loop_copy_protects_' + owner] = (
        False, CASES['replacement_loop_copy'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  if (count == 0) { return copied',
                 '  ' + owner + '.clear();\n  if (count == 0) { return copied'))

CASES['replacement_branch_move'] = (True, CASES['replacement_branch_copy'][1]
    .replace('auto copied = view;', 'auto copied = std::move(view);'))
CASES['nested_branch_copy_outlives_slot'] = (True, '''int check(bool choose) {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view outer = "";
  {
    mut std::string_view inner = first.view();
    if (choose) { inner = second.view(); }
    outer = inner;
  }
  if (choose) { return outer == "second" ? 0 : 1; }
  return outer == "first" ? 0 : 2;
}
int main() { return check(true) + check(false); }
''')
for owner in ('first', 'second'):
    CASES['nested_branch_copy_protects_' + owner] = (
        False, CASES['nested_branch_copy_outlives_slot'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  if (choose) { return outer',
                 '  ' + owner + '.clear();\n  if (choose) { return outer'))

# A later loop iteration can fail before the store that first created the
# retained group. Cleanup must account for the previous iteration's value.
CASES['replacement_loop_fallible_prefix'] = (
    True, CASES['replacement_loop_join'][1].replace(
        '{ view = second.view(); }',
        '{ std::string scratch{"scratch"}; '
        'if (scratch.size() != 7) { return 3; } view = second.view(); }'))

# Separate an early failure edge from an early normal scope exit. Both can
# be reached after an earlier loop iteration retained a different owner.
CASES['replacement_loop_fallible_no_return'] = (
    True, CASES['replacement_loop_join'][1].replace(
        '{ view = second.view(); }',
        '{ std::string scratch{"scratch"}; view = second.view(); }'))
CASES['replacement_loop_early_return'] = (
    True, CASES['replacement_loop_join'][1].replace(
        '{ view = second.view(); }',
        '{ if (i == 1) { return 0; } view = second.view(); }'))

# Loop control exits only the iteration scope. It must preserve an outer
# carrier's dependency for the read after the loop or on the next iteration.
CASES['replacement_loop_early_break'] = (
    True, CASES['replacement_loop_join'][1].replace(
        '{ view = second.view(); }',
        '{ if (i == 1) { break; } view = second.view(); }'))
CASES['replacement_loop_early_continue'] = (
    True, CASES['replacement_loop_join'][1].replace(
        '{ view = second.view(); }',
        '{ if (i == 1) { continue; } view = second.view(); }'))

CASES['nested_copy_outlives_slot'] = (True, '''int main() {
  std::string owner{"retained"}; mut std::string_view outer = "";
  { mut std::string_view inner = ""; inner = owner.view(); outer = inner; }
  return outer == "retained" ? 0 : 1;
}''')
CASES['nested_copy_protects_owner'] = (False,
    CASES['nested_copy_outlives_slot'][1]
        .replace('std::string owner', 'mut std::string owner')
        .replace('  return outer', '  owner.clear(); return outer'))
CASES['replacement_loop_protects_inner_owner'] = (False, '''int main() {
  mut std::string_view view = "";
  for (mut int i = 0; i < 2; i += 1) {
    std::string owner{"short"}; view = owner.view();
  }
  return view == "short" ? 0 : 1;
}''')

CASES['replacement_branch_keeps_old_copy'] = (False, '''int check(bool replace) {
  mut std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view(); auto copied = view;
  if (replace) { view = second.view(); }
  first.clear();
  return copied == "first" && view == "second" ? 0 : 1;
}
int main() { return check(true); }
''')


# Either predecessor may supply the live view. Protect both possible owners,
# including the zero-iteration and repeated-store paths through a loop.
for control in ('branch', 'loop'):
    for owner in ('first', 'second'):
        store = ('if (replace) { view = second.view(); }' if control == 'branch'
                 else 'for (mut int i = 0; i < count; i += 1) { view = second.view(); }')
        parameter = 'bool replace' if control == 'branch' else 'int count'
        argument = 'true' if control == 'branch' else '3'
        CASES[f'replacement_{control}_protects_{owner}'] = (False,
            f'int check({parameter}) {{\n'
            '  mut std::string first{"first"}; mut std::string second{"second"};\n'
            '  mut std::string_view view = first.view();\n'
            f'  {store}\n'
            f'  {owner}.clear();\n'
            '  return view == "second" ? 0 : 1;\n'
            '}\n'
            f'int main() {{ return check({argument}); }}\n')


# Scalar fields and forwarding methods exercise the same retaining effects as
# inline arrays, including a direct field read after the source call returns.
CELL = '''struct BorrowedCell<T> {
  mut T value;
  BorrowedCell(T initial):value(initial){}
  void put(T input) mut { this.value = input; }
  void forward(T input) mut { this.put(input); }
};
'''
CASES['field_forward_borrowed'] = (True, CELL + '''int main() {
  std::string owner{"borrowed"};
  mut BorrowedCell<std::string_view> cell{"initial"};
  cell.forward(owner.view());
  return cell.value == "borrowed" ? 0 : 1;
}''')
CASES['field_forward_protects_owner'] = (
    False, CASES['field_forward_borrowed'][1]
    .replace('std::string owner{', 'mut std::string owner{')
    .replace('  return cell.value', '  owner.clear();\n  return cell.value'))
CASES['field_forward_rejects_inner_owner'] = (False, CELL + '''int main() {
  mut BorrowedCell<std::string_view> cell{"initial"};
  { std::string owner{"borrowed"}; cell.forward(owner.view()); }
  return cell.value == "borrowed" ? 0 : 1;
}''')


# End-to-end acceptance: ordinary inline storage, generic over its element and
# capacity. No compiler recognition of this declaration is permitted.
FIXED_SLOTS = '''struct FixedSlots<T, uint64_t Capacity> {
  mut T values[Capacity];
  mut uint64_t used;
  FixedSlots():values({}),used(0){}
  bool push(T value) mut {
    if (this.used == Capacity) { return false; }
    this.values[this.used] = value;
    this.used += 1;
    return true;
  }
  T at(uint64_t index) { return this.values[index]; }
  uint64_t size() { return this.used; }
};
'''
CASES['fixed_slots_owned_control'] = (True, FIXED_SLOTS + '''int main() {
  mut FixedSlots<int, 2> slots{};
  if (!slots.push(11) || !slots.push(22) || slots.push(33)) { return 1; }
  return slots.size() == 2 && slots.at(0) == 11 && slots.at(1) == 22 ? 0 : 2;
}''')
CASES['fixed_slots_two_owners'] = (True, FIXED_SLOTS + '''int main() {
  std::string first{"first"}; std::string second{"second"};
  mut FixedSlots<std::string_view, 2> slots{};
  if (!slots.push(first.view()) || !slots.push(second.view())) { return 1; }
  return slots.size() == 2 && slots.at(0) == "first" && slots.at(1) == "second" ? 0 : 2;
}''')
CASES['fixed_slots_literal_control'] = (
    True, CASES['fixed_slots_two_owners'][1].replace('first.view()', '"first"')
          .replace('second.view()', '"second"'))
# The container must retain every successful insertion's owner, independently
# of the generic declaration's spelling or which insertion comes first.
for owner in ('first', 'second'):
    CASES['fixed_slots_protects_' + owner] = (
        False, CASES['fixed_slots_two_owners'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  return slots.size()', '  ' + owner + '.clear();\n  return slots.size()'))
CASES['fixed_slots_renamed_three_capacity'] = (
    True, CASES['fixed_slots_two_owners'][1]
    .replace('FixedSlots', 'BorrowedEntries')
    .replace('BorrowedEntries<std::string_view, 2>', 'BorrowedEntries<std::string_view, 3>'))
for retained, replaced in (('first', 'second'), ('second', 'first')):
    CASES['fixed_slots_' + retained + '_only'] = (
        True, CASES['fixed_slots_two_owners'][1]
        .replace(replaced + '.view()', '"' + replaced + '"'))

CASES['fixed_slots_selected_outlives_storage'] = (True, FIXED_SLOTS + '''int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view selected = "";
  {
    mut FixedSlots<std::string_view, 2> slots{};
    if (!slots.push(first.view()) || !slots.push(second.view())) { return 1; }
    selected = slots.at(0);
  }
  return selected == "first" ? 0 : 2;
}''')
for owner in ('first', 'second'):
    CASES['fixed_slots_selected_protects_' + owner] = (
        False, CASES['fixed_slots_selected_outlives_storage'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  return selected ==', '  ' + owner + '.clear();\n  return selected =='))

# Owner-aware allocation primitive: the pointer conveys no lifetime proof.
# A selected constructor must retain a by-value text field through the generic
# owner-aware construction wrapper, independently of destruction behaviour.
CASES['owned_construction_text_field'] = (True, """import std.memory;
struct Label {
  std::string_view text;
  Label(std::string_view input) : text(input) {}
};
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<Label> storage{};
  unsafe {
    Label* address = storage.data();
    std::construct_owned_at<Label, std::uninitialized<Label>, std::string_view>(address, storage, owner.view());
    auto selected = std::borrow_owned_at(address, storage);
    return selected.text == "borrowed" ? 0 : 1;
  }
}
""")
CASES['owned_construction_text_field_protects_owner'] = (False,
    CASES['owned_construction_text_field'][1].replace('std::string owner{',
        'mut std::string owner{').replace('    auto selected =',
        '    owner.clear();\n    auto selected ='))

TEXT_FIELD_OUTLIVES_STORAGE = CASES['owned_construction_text_field'][1].replace(
    '  mut std::uninitialized<Label> storage{};\n  unsafe {',
    '  mut Label selected{""};\n  { mut std::uninitialized<Label> storage{};\n  unsafe {').replace(
    '    auto selected = std::borrow_owned_at(address, storage);',
    '    selected = std::borrow_owned_at(address, storage);').replace(
    '    return selected.text == "borrowed" ? 0 : 1;\n  }',
    '    std::destroy_at(address);\n  } }\n  return selected.text == "borrowed" ? 0 : 1;')
CASES['owned_construction_text_field_outlives_storage'] = (True, TEXT_FIELD_OUTLIVES_STORAGE)
CASES['owned_construction_text_field_outlives_storage_protects_owner'] = (False,
    TEXT_FIELD_OUTLIVES_STORAGE.replace('std::string owner{', 'mut std::string owner{').replace(
        '  return selected.text', '  owner.clear();\n  return selected.text'))
CASES['owned_construction_text_field_rejects_expired_owner'] = (False,
    TEXT_FIELD_OUTLIVES_STORAGE.replace('  std::string owner{"borrowed"};\n', '').replace(
        '  { mut std::uninitialized<Label> storage{};',
        '  { std::string owner{"borrowed"}; mut std::uninitialized<Label> storage{};'))

CASES['owned_storage_initialize_scalar'] = (True, "import std.memory;\n" + """int main() {
  mut std::uninitialized<int> storage{};
  unsafe {
    int* address = storage.data();
    std::initialize_owned_at(address, storage, 42);
    return *address == 42 ? 0 : 1;
  }
}
""")
CASES['owned_storage_initialize_view'] = (True, "import std.memory;\n" + """int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe {
    std::string_view* address = storage.data();
    std::initialize_owned_at(address, storage, owner.view());
    std::string_view selected = std::borrow_owned_at(address, storage);
    return selected == "borrowed" ? 0 : 1;
  }
}
""")
CASES['owned_storage_initialize_protects_owner'] = (False,
    CASES['owned_storage_initialize_view'][1]
      .replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

# Multiple raw slots share one storage owner but retain distinct text owners.
CASES['owned_storage_two_owners'] = (True, "import std.memory;\n" + """class RawSlots<std::layout_queryable T> {
public:
  mut std::uninitialized<T> left{};
  mut std::uninitialized<T> right{};
  RawSlots() = default;
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  mut RawSlots<std::string_view> storage{};
  unsafe {
    std::string_view* left = storage.left.data();
    std::string_view* right = storage.right.data();
    std::initialize_owned_at(left, storage, first.view());
    std::initialize_owned_at(right, storage, second.view());
    std::string_view selected = std::borrow_owned_at(right, storage);
    return selected == "second" ? 0 : 1;
  }
}
""")
for retained in ('first', 'second'):
    CASES['owned_storage_two_owners_protects_' + retained] = (False,
        CASES['owned_storage_two_owners'][1]
          .replace('std::string ' + retained + '{', 'mut std::string ' + retained + '{')
          .replace('    std::string_view selected', '    ' + retained + '.clear();\n    std::string_view selected'))

MULTIPLE_OWNER_COPY = CASES['owned_storage_two_owners'][1].split('int main() {')[0] + """int main() {
  std::string first{"first"};
  std::string second{"second"};
  mut std::string_view selected = "";
  {
    mut RawSlots<std::string_view> storage{};
    unsafe {
      std::string_view* left = storage.left.data();
      std::string_view* right = storage.right.data();
      std::initialize_owned_at(left, storage, first.view());
      std::initialize_owned_at(right, storage, second.view());
      selected = std::borrow_owned_at(right, storage);
    }
  }
  return selected == "second" ? 0 : 1;
}
"""
CASES['owned_storage_multiple_copy_outlives_storage'] = (True, MULTIPLE_OWNER_COPY)
for retained in ('first', 'second'):
    declaration = 'std::string ' + retained + '{"' + retained + '"};'
    CASES['owned_storage_multiple_copy_protects_' + retained] = (False,
        MULTIPLE_OWNER_COPY.replace('  ' + declaration + '\n', '')
          .replace('    mut RawSlots', '    ' + declaration + '\n    mut RawSlots'))

# A source wrapper must forward the contents contract independently of names.
CASES['owned_storage_read_wrapper'] = (True,
    CASES['owned_storage_initialize_view'][1]
      .replace('int main() {', 'unsafe T&<a> read_slot<T, Owner, lifetime a>(const T* address, Owner&<a> owner) {\n  unsafe { return std::borrow_owned_at(address, owner); }\n}\nint main() {')
      .replace('std::string_view selected = std::borrow_owned_at(address, storage);',
               'std::string_view selected = read_slot(address, storage);'))

# Receiver-based containers must expose the same opaque contents relation.
CASES['owned_storage_receiver_read'] = (True, "import std.memory;\n" + """class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    std::initialize_owned_at(box.slot.data(), box, owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}
""")
CASES['owned_storage_receiver_protects_owner'] = (False,
    CASES['owned_storage_receiver_read'][1]
      .replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

# A real container receives elements through a method: the caller must retain
# the method's value dependencies without naming its storage owner explicitly.
RECEIVER_WRITE = CASES['owned_storage_receiver_read'][1].replace(
    '  unsafe T& read() {',
    '  unsafe void write(T value) mut requires std::move_constructible<T> { unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }\n  unsafe T& read() {').replace(
        'std::initialize_owned_at(box.slot.data(), box, owner.view());',
        'box.write(owner.view());')
CASES['owned_storage_receiver_write'] = (True, RECEIVER_WRITE)
CASES['owned_storage_receiver_write_protects_owner'] = (False,
    RECEIVER_WRITE.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

RECEIVER_WRITE_RETURN = RECEIVER_WRITE.replace('class RawBox',
    'T relay<T>(T value) { return std::move(value); }\nclass RawBox').replace(
        'this, std::move(value)', 'this, relay(std::move(value))')
CASES['owned_storage_receiver_return_helper'] = (True, RECEIVER_WRITE_RETURN)
CASES['owned_storage_receiver_return_helper_protects_owner'] = (False,
    RECEIVER_WRITE_RETURN.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

RECEIVER_WRITE_CONSTRUCTION = RECEIVER_WRITE.replace('class RawBox', """class Scratch {
  int value;
public:
  Scratch(int input) : value(input) {}
};
class RawBox""").replace(' { unsafe { std::initialize_owned_at',
    ' { Scratch scratch{7}; unsafe { std::initialize_owned_at')
CASES['owned_storage_receiver_local_construction'] = (True, RECEIVER_WRITE_CONSTRUCTION)
CASES['owned_storage_receiver_local_construction_protects_owner'] = (False,
    RECEIVER_WRITE_CONSTRUCTION.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

RECEIVER_WRITE_CLEANUP = RECEIVER_WRITE_CONSTRUCTION.replace('  int value;',
    '  mut int value;').replace('  Scratch(int input) : value(input) {}',
    '  Scratch(int input) : value(input) {}\n  ~Scratch() { this.value = 0; }')
CASES['owned_storage_receiver_local_cleanup'] = (True, RECEIVER_WRITE_CLEANUP)
CASES['owned_storage_receiver_local_cleanup_protects_owner'] = (False,
    RECEIVER_WRITE_CLEANUP.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

RECEIVER_WRITE_DESTROY = RECEIVER_WRITE_CLEANUP.replace('Scratch scratch{7};',
    'mut std::uninitialized<Scratch> scratch{}; unsafe { '
    'std::initialize_owned_at(scratch.data(), scratch, Scratch(7)); '
    'std::destroy_at(scratch.data()); }')
CASES['owned_storage_receiver_explicit_cleanup'] = (True, RECEIVER_WRITE_DESTROY)
CASES['owned_storage_receiver_explicit_cleanup_protects_owner'] = (False,
    RECEIVER_WRITE_DESTROY.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))
CASES['owned_storage_receiver_scalar_cleanup'] = (True,
    RECEIVER_WRITE_DESTROY.replace('std::uninitialized<Scratch>', 'std::uninitialized<int>')
      .replace('scratch, Scratch(7)', 'scratch, 7'))

RECEIVER_WRITE_COUNTER = RECEIVER_WRITE.replace('write(T value)',
    'write(T value, uint64_t* count)').replace(
    'unsafe { std::initialize_owned_at(this.slot.data()',
    'unsafe { if (*count == uint64_t(0)) { *count = uint64_t(1); } std::initialize_owned_at(this.slot.data()')
RECEIVER_WRITE_COUNTER = RECEIVER_WRITE_COUNTER.replace(
    '  mut RawBox<std::string_view> box{};',
    '  mut RawBox<std::string_view> box{};\n  mut uint64_t count = 0;').replace(
    'box.write(owner.view());', 'box.write(owner.view(), &count);').replace(
    'selected == "borrowed" ? 0 : 1', 'selected == "borrowed" && count == 1 ? 0 : 1')
CASES['owned_storage_receiver_scalar_bookkeeping'] = (True, RECEIVER_WRITE_COUNTER)
CASES['owned_storage_receiver_scalar_bookkeeping_protects_owner'] = (False,
    RECEIVER_WRITE_COUNTER.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

RECEIVER_WRITE_RECORD = RECEIVER_WRITE.replace('class RawBox', """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
class RawBox""").replace('std::string owner{"borrowed"};', 'int owner = 42;').replace(
    'std::string_view', 'Reading').replace('owner.view()', 'Reading(owner)').replace(
    'selected == "borrowed"', 'selected.read() == 42')
CASES['owned_storage_receiver_write_record'] = (True, RECEIVER_WRITE_RECORD)
CASES['owned_storage_receiver_write_record_protects_owner'] = (False,
    RECEIVER_WRITE_RECORD.replace('int owner = 42;', 'mut int owner = 42;').replace(
        '    Reading selected', '    owner = 99;\n    Reading selected'))

CASES['owned_storage_receiver_forward'] = (True,
    CASES['owned_storage_receiver_read'][1].replace(
        '  unsafe T& read() {',
        '  unsafe T& read() { unsafe { return this.read_inner(); } }\n  unsafe T& read_inner() {'))
NESTED_RECEIVER = CASES['owned_storage_receiver_read'][1].replace('int main() {', """class RawEnvelope<std::layout_queryable T> {
public:
  mut RawBox<T> inner{};
  RawEnvelope() = default;
  unsafe T& read() { unsafe { return this.inner.read(); } }
};
int main() {""").replace('mut RawBox<std::string_view> box{};',
                       'mut RawEnvelope<std::string_view> box{};').replace(
                           'box.slot.data()', 'box.inner.slot.data()')
CASES['owned_storage_nested_receiver'] = (True, NESTED_RECEIVER)
CASES['owned_storage_nested_receiver_protects_owner'] = (False,
    NESTED_RECEIVER.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

# Copied payload borrows survive the raw-storage shell, while the payload's
# original owner must remain alive. This is independent of any vector API.
CASES['owned_storage_selected_outlives_storage'] = (True, "import std.memory;\n" + """int main() {
  std::string owner{"borrowed"};
  mut std::string_view selected = "";
  {
    mut std::uninitialized<std::string_view> storage{};
    unsafe {
      std::string_view* address = storage.data();
      std::initialize_owned_at(address, storage, owner.view());
      selected = std::borrow_owned_at(address, storage);
    }
  }
  return selected == "borrowed" ? 0 : 1;
}
""")
CASES['owned_storage_selected_rejects_expired_owner'] = (False,
    CASES['owned_storage_selected_outlives_storage'][1]
      .replace('  std::string owner{"borrowed"};\n', '')
      .replace('    mut std::uninitialized', '    std::string owner{"borrowed"};\n    mut std::uninitialized'))

# An ordinary user record with a checked reference exercises a different
# payload representation from text; no library container name is involved.
CASES['owned_storage_reference_record'] = (True, "import std.memory;\n" + """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  unsafe {
    Reading* address = storage.data();
    std::initialize_owned_at(address, storage, Reading(owner));
    auto selected = std::borrow_owned_at(address, storage);
    return selected.read() == 42 ? 0 : 1;
  }
}
""")

# The same external dependency must survive for ordinary borrowed records.
CASES['owned_storage_reference_record_protects_owner'] = (False,
    CASES['owned_storage_reference_record'][1]
      .replace('  int owner = 42;', '  mut int owner = 42;')
      .replace('    auto selected =', '    owner = 99;\n    auto selected ='))

# Construct at the final address using a checked storage owner, independently
# of any container. Cover generated value transfer and a selected constructor.
for name, original in (
        ('view_copy', CASES['owned_storage_selected_outlives_storage'][1]),
        ('record_copy', CASES['owned_storage_reference_record'][1]),
        ('record_constructor', CASES['owned_storage_reference_record'][1])):
    if name == 'record_constructor':
        fixture = original.replace(
            'std::initialize_owned_at(address, storage, Reading(owner))',
            'std::construct_owned_at<Reading, std::uninitialized<Reading>, int&>(address, storage, owner)')
    else:
        fixture = original.replace('std::initialize_owned_at(', 'std::construct_owned_at(')
    # These payloads have no custom destructor, but still exercise the explicit
    # lifetime end separately from the independent copied value's use.
    if name != 'view_copy':
        fixture = fixture.replace('    return selected.read()',
                                  '    std::destroy_at(address);\n    return selected.read()')
    CASES['owned_construction_' + name] = (True, fixture)
    if name == 'view_copy':
        negative = fixture.replace('std::string owner{', 'mut std::string owner{')
        negative = negative.replace('      selected =', '      owner.clear();\n      selected =')
    else:
        negative = fixture.replace('int owner = 42;', 'mut int owner = 42;')
        negative = negative.replace('    auto selected =', '    owner = 99;\n    auto selected =')
    CASES['owned_construction_' + name + '_protects_owner'] = (False, negative)

# Retained reference arguments must follow their actual owner's scope, even
# for scalars without destructors, and may designate ordinary object fields.
CONSTRUCTED_FIELD = CASES['owned_construction_record_constructor'][1].replace(
    'class Reading {', 'class Number { public: mut int value; Number(int input) : value(input) {} };\nclass Reading {').replace(
    '  int owner = 42;', '  mut Number owner{42};').replace(
    '(address, storage, owner);', '(address, storage, owner.value);')
CASES['owned_construction_record_field'] = (True, CONSTRUCTED_FIELD)
CASES['owned_construction_record_field_protects_owner'] = (False,
    CONSTRUCTED_FIELD.replace('    auto selected =',
                             '    owner.value = 99;\n    auto selected ='))
CONSTRUCTED_SCOPE = """import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  {
    unsafe {
      std::construct_owned_at<Reading, std::uninitialized<Reading>, int&>(storage.data(), storage, owner);
    }
  }
  unsafe {
    auto selected = std::borrow_owned_at(storage.data(), storage);
    std::destroy_at(storage.data());
    return selected.read() == 42 ? 0 : 1;
  }
}
"""
CASES['owned_construction_record_copy_releases_storage'] = (True, CONSTRUCTED_SCOPE)
CONSTRUCTED_SCOPE = CONSTRUCTED_SCOPE.replace(
    '    auto selected = std::borrow_owned_at(storage.data(), storage);',
    '    Reading* address = storage.data();\n    auto selected = std::borrow_owned_at(address, storage);').replace(
    '    std::destroy_at(storage.data());', '    std::destroy_at(address);')
CASES['owned_construction_record_scope'] = (True, CONSTRUCTED_SCOPE)
CASES['owned_construction_record_rejects_expired_owner'] = (False,
    CONSTRUCTED_SCOPE.replace('  int owner = 42;\n', '').replace(
        '  {\n    unsafe {', '  {\n    int owner = 42;\n    unsafe {'))

# Isolate mutable access from container construction and growth. Copying from
# the mutable reference must retain the payload's external owners as well.
for payload in ('view_copy', 'record_copy'):
    for suffix in ('', '_protects_owner'):
        accepted, source = CASES['owned_construction_' + payload + suffix]
        CASES['owned_construction_mutable_' + payload + suffix] = (
            accepted, source.replace('std::borrow_owned_at(', 'std::borrow_owned_mut_at('))

# An actual exclusive reference must protect both the storage and the owner.
MUTABLE_RECORD_REFERENCE = CASES['owned_construction_mutable_record_copy'][1].replace(
    'auto selected =', 'mut Reading& selected =').replace(
    '    std::destroy_at(address);\n', '')
CASES['owned_construction_mutable_record_reference'] = (True, MUTABLE_RECORD_REFERENCE)
CASES['owned_construction_mutable_record_reference_protects_owner'] = (False,
    MUTABLE_RECORD_REFERENCE.replace('int owner = 42;', 'mut int owner = 42;').replace(
        '    return selected.read()', '    owner = 99;\n    return selected.read()'))
CASES['owned_construction_mutable_record_reference_exclusive'] = (False,
    MUTABLE_RECORD_REFERENCE.replace('    return selected.read()',
        '    mut Reading& second = std::borrow_owned_mut_at(address, storage);\n'
        '    return selected.read()'))

# Two reference arguments occupy distinct lanes of the constructor pack.
# Both external owners must remain protected after the wrapper returns.
OWNED_CONSTRUCTION_PAIR = """import std.memory;
class PairReading {
  int& first;
  int& second;
public:
  PairReading(int& left, int& right) : first(left), second(right) {}
  int read() { return this.first + this.second; }
};
int main() {
  int first = 11;
  int second = 22;
  mut std::uninitialized<PairReading> storage{};
  unsafe {
    PairReading* address = storage.data();
    std::construct_owned_at<PairReading, std::uninitialized<PairReading>, int&, int&>(address, storage, first, second);
    auto selected = std::borrow_owned_at(address, storage);
    std::destroy_at(address);
    return selected.read() == 33 ? 0 : 1;
  }
}
"""
CASES['owned_construction_pair'] = (True, OWNED_CONSTRUCTION_PAIR)
for retained in ('first', 'second'):
    CASES['owned_construction_pair_protects_' + retained] = (False,
        OWNED_CONSTRUCTION_PAIR.replace('  int ' + retained + ' =',
                                        '  mut int ' + retained + ' =').replace(
            '    auto selected =', '    ' + retained + ' = 99;\n    auto selected ='))

MULTIPLE_RECORDS = CASES['owned_storage_two_owners'][1].replace(
    'class RawSlots', """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
class RawSlots""").replace('std::string first{"first"}; std::string second{"second"};',
    'int first = 11; int second = 22;').replace('std::string_view', 'Reading').replace(
    'first.view()', 'Reading(first)').replace('second.view()', 'Reading(second)').replace(
        'selected == "second"', 'selected.read() == 22')
CASES['owned_storage_multiple_records'] = (True, MULTIPLE_RECORDS)
for retained in ('first', 'second'):
    CASES['owned_storage_multiple_records_protects_' + retained] = (False,
        MULTIPLE_RECORDS.replace('int ' + retained + ' =', 'mut int ' + retained + ' =')
          .replace('    Reading selected', '    ' + retained + ' = 99;\n    Reading selected'))

# Multiple retained groups must also cross a receiver accessor, with the
# exact same independently checked content contract as a free function.
MULTIPLE_RECORDS_RECEIVER = MULTIPLE_RECORDS.replace(
    '  RawSlots() = default;',
    '  RawSlots() = default;\n'
    '  unsafe T& read_right() {\n'
    '    unsafe { return std::borrow_owned_at(this.right.data(), this); }\n'
    '  }').replace('std::borrow_owned_at(right, storage)', 'storage.read_right()')
CASES['owned_storage_multiple_records_receiver'] = (True, MULTIPLE_RECORDS_RECEIVER)
for retained in ('first', 'second'):
    CASES['owned_storage_multiple_records_receiver_protects_' + retained] = (False,
        MULTIPLE_RECORDS_RECEIVER.replace('int ' + retained + ' =',
            'mut int ' + retained + ' =').replace('    Reading selected',
            '    ' + retained + ' = 99;\n    Reading selected'))

# Relocation must transfer external dependencies to a different storage owner,
# including when the original storage owner ends before the destination read.
OWNED_RELOCATION = "import std.memory;\n" + """int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> destination{};
  {
    mut std::uninitialized<std::string_view> source{};
    unsafe {
      std::initialize_owned_at(source.data(), source, owner.view());
      std::relocate_owned_at(destination.data(), destination, source.data(), source);
    }
  }
  unsafe {
    std::string_view selected = std::borrow_owned_at(destination.data(), destination);
    return selected == "borrowed" ? 0 : 1;
  }
}
"""
CASES['owned_storage_relocate_view'] = (True, OWNED_RELOCATION)
CASES['owned_storage_relocate_protects_owner'] = (False,
    OWNED_RELOCATION.replace('std::string owner{', 'mut std::string owner{').replace(
        '    std::string_view selected', '    owner.clear();\n    std::string_view selected'))
CASES['owned_storage_relocate_record'] = (True,
    OWNED_RELOCATION.replace('int main()', """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main()""").replace('std::string owner{"borrowed"};', 'int owner = 42;').replace(
        'std::string_view', 'Reading').replace('owner.view()', 'Reading(owner)').replace(
        'selected == "borrowed"', 'selected.read() == 42'))

OWNED_RELOCATION_METHOD = "import std.memory;\n" + """class TransferBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> storage{};
  TransferBox() = default;
  unsafe void transfer(mut TransferBox<T>& destination) mut requires std::move_constructible<T> {
    unsafe { std::relocate_owned_at(destination.storage.data(), destination,
                                   this.storage.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut TransferBox<std::string_view> destination{};
  {
    mut TransferBox<std::string_view> source{};
    unsafe {
      std::initialize_owned_at(source.storage.data(), source, owner.view());
      source.transfer(destination);
    }
  }
  unsafe {
    std::string_view selected = std::borrow_owned_at(destination.storage.data(), destination);
    return selected == "borrowed" ? 0 : 1;
  }
}
"""
CASES['owned_storage_relocate_method'] = (True, OWNED_RELOCATION_METHOD)
CASES['owned_storage_relocate_method_protects_owner'] = (False,
    OWNED_RELOCATION_METHOD.replace('std::string owner{', 'mut std::string owner{')
      .replace('    std::string_view selected', '    owner.clear();\n    std::string_view selected'))

# Growable storage must preserve external owners across allocation changes.
GROWABLE_VIEWS = "import std.vector;\n" + """int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::vector<std::string_view> values{};
  values.push_back(first.view());
  values.push_back(second.view());
  values.reserve(32);
  return values.size() == 2 && values[0] == "first" && values[1] == "second" ? 0 : 1;
}
"""
GROWABLE_RECORDS = GROWABLE_VIEWS.replace('int main()', """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main()""").replace('std::string first{"first"}; std::string second{"second"};',
    'int first = 11; int second = 22;').replace('std::string_view', 'Reading').replace(
    'first.view()', 'Reading(first)').replace('second.view()', 'Reading(second)').replace(
    'values[0] == "first"', 'values[0].read() == 11').replace(
    'values[1] == "second"', 'values[1].read() == 22')
# Consuming iteration still uses borrow-free raw value extraction.
CASES['borrow_free_consuming_iteration_control'] = (True, """import std.vector;
int main() {
  mut std::vector<int> values{};
  values.push_back(11); values.push_back(22);
  mut int total = 0;
  for (int value : std::move(values).into_range()) { total += value; }
  return total == 33 ? 0 : 1;
}
""")
for payload, source, element in (
        ('views', GROWABLE_VIEWS, 'std::string_view'),
        ('records', GROWABLE_RECORDS, 'Reading')):
    start = source.index('  return values.size()')
    source = source[:start] + (
        '  for (' + element + ' value : std::move(values).into_range()) {}\n'
        '  return 0;\n}\n')
    CASES['borrowed_consuming_iteration_requires_extraction_' + payload] = (False, source)

CASES['growable_records_two_owners'] = (True, GROWABLE_RECORDS)
for owner in ('first', 'second'):
    CASES['growable_records_protects_' + owner] = (False,
        GROWABLE_RECORDS.replace('int ' + owner + ' =', 'mut int ' + owner + ' =').replace(
            '  return values.size()', '  ' + owner + ' = 99;\n  return values.size()'))

CASES['growable_records_element_reference_blocks_growth'] = (False,
    GROWABLE_RECORDS.replace('  values.reserve(32);',
        '  Reading& element = values[0];\n  values.reserve(32);\n'
        '  if (element.read() != 11) { return 2; }'))

CASES['growable_records_copy_survives_growth'] = (True,
    GROWABLE_RECORDS.replace('  values.reserve(32);',
        '  Reading selected = values[0];\n  values.reserve(32);').replace(
        '  return values.size()', '  return selected.read() == 11 && values.size()'))
for owner in ('first', 'second'):
    CASES['growable_records_copy_protects_' + owner] = (False,
        CASES['growable_records_copy_survives_growth'][1]
        .replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
        .replace('  return selected.read()', '  ' + owner + ' = 99;\n  return selected.read()'))

GROWABLE_RECORD_COPY = "import std.vector;\n" + """class Reading<lifetime a> {
  int&<a> value;
public:
  Reading(int&<a> input) : value(input) {}
  int read() { return this.value; }
};
Reading<a> pick<lifetime a>(int&<a> first, int&<a> second) {
  mut std::vector<Reading<a>> values{};
  values.push_back(Reading<a>(first)); values.push_back(Reading<a>(second));
  Reading<a> selected = values[0];
  values.reserve(32);
  return selected;
}
int main() {
  int first = 11; int second = 22;
  Reading selected = pick(first, second);
  return selected.read() == 11 ? 0 : 1;
}
"""
CASES['growable_records_selected_outlives_storage'] = (True, GROWABLE_RECORD_COPY)
CASES['growable_records_return_local_owner'] = (False,
    GROWABLE_RECORD_COPY.replace('  mut std::vector<Reading<a>> values{};',
        '  int local = 99;\n  mut std::vector<Reading<a>> values{};').replace(
        'Reading<a>(first)', 'Reading<a>(local)'))
CASES['growable_records_return_unrelated_owner'] = (False,
    GROWABLE_RECORD_COPY.replace('pick<lifetime a>(int&<a> first, int&<a> second)',
        'pick<lifetime a, lifetime b>(int&<a> first, int&<b> second)').replace(
        'Reading<a>(second)', 'Reading<b>(second)'))

CASES['growable_records_selected_protects_owner'] = (False,
    GROWABLE_RECORD_COPY.replace('int first =', 'mut int first =').replace(
        '  return selected.read()', '  first = 99;\n  return selected.read()'))

# A helper copying through an element reference must retain the element's
# external owners, without retaining the temporary reference into the vector.
# Exercise ordinary user code as well as the library helper so this cannot be
# repaired by recognizing a particular function name. Only the copied value is
# read after mutation in the negative cases: the vector is not the witness.
for payload, source, value_type, result in (
        ('views', GROWABLE_VIEWS, 'std::string_view', 'selected == "first"'),
        ('records', GROWABLE_RECORDS, 'Reading', 'selected.read() == 11')):
    for helper in ('user', 'stdlib'):
        function = 'copy_borrowed' if helper == 'user' else 'std::clone'
        fixture = source.replace('int main()', '''T copy_borrowed<T>(T& value)
    requires std::copy_constructible<T> { return value; }
int main()''').replace('  values.reserve(32);',
            f'  {value_type} selected = {function}(values[0]);\n'
            '  values.reserve(32);')
        original_return = next(line for line in fixture.splitlines()
                               if line.startswith('  return values.size()'))
        fixture = fixture.replace(original_return, f'  return {result} ? 0 : 1;')
        name = f'growable_{payload}_{helper}_copy'
        CASES[name] = (True, fixture)
        for owner in ('first', 'second'):
            negative = fixture.replace('  values.reserve(32);\n', '')
            if payload == 'views':
                negative = negative.replace(f'std::string {owner}' + '{',
                    f'mut std::string {owner}' + '{')
                mutation = f'{owner}.clear();'
            else:
                negative = negative.replace(f'int {owner} =', f'mut int {owner} =')
                mutation = f'{owner} = 99;'
            CASES[name + '_protects_' + owner] = (False,
                negative.replace('  return selected',
                    f'  {mutation}\n  return selected'))

CASES['growable_owned_control'] = (True, "import std.vector;\n" + """int main() {
  mut std::vector<int> values{};
  values.push_back(11); values.push_back(22);
  values.reserve(32);
  return values.size() == 2 && values[0] == 11 && values[1] == 22 ? 0 : 1;
}
""")
CASES['growable_views_two_owners'] = (True, GROWABLE_VIEWS)
# Exercise the shared accessor separately from indexing a mutable container.
# Both paths must work; these cases isolate content forwarding from exclusive
# element access instead of letting one hide failures in the other.
for payload, source, element in (
        ('views', GROWABLE_VIEWS, 'std::string_view'),
        ('records', GROWABLE_RECORDS, 'Reading')):
    shared_source = source.replace(
        '  return values.size()',
        '  std::vector<' + element + '>& shared = values;\n  return shared.size()')
    shared_source = shared_source.replace('values[0]', 'shared[0]').replace(
        'values[1]', 'shared[1]')
    CASES['growable_' + payload + '_shared_access'] = (True, shared_source)
    CASES['growable_' + payload + '_shared_access_without_reserve'] = (
        True, shared_source.replace('  values.reserve(32);\n', ''))

# Read-only adapters must forward the owners of copied elements. The final
# read uses only the selected copy, so the container itself cannot mask a
# dropped dependency in the negative cases.
for payload, source in (('views', GROWABLE_VIEWS), ('records', GROWABLE_RECORDS)):
    element = 'std::string_view' if payload == 'views' else 'Reading'
    for adapter, expression in (('span', 'auto adapter = values.as_span();\n  auto selected = adapter[0];'),
                                ('iterator', 'auto adapter = values.begin();\n  auto selected = *adapter;'),
                                ('user', 'auto adapter = BorrowedAccess<' + element +
                                 '>(values);\n  auto selected = adapter.get(0);')):
        check = 'selected == "first"' if payload == 'views' else 'selected.read() == 11'
        prefix = source[:source.index('  return values.size()')]
        fixture = prefix + '  ' + expression + '\n  return ' + check + ' ? 0 : 1;\n}\n'
        if adapter == 'user':
            fixture = fixture.replace('int main()', '''class BorrowedAccess<std::vector_element T, lifetime source> {
  std::vector<T>&<source> owner;
public:
  BorrowedAccess(std::vector<T>&<source> input) : owner(input) {}
  T& get(std::size_t index) { return this.owner[index]; }
};
int main()''', 1)
        name = 'growable_' + payload + '_adapter_' + adapter
        CASES[name] = (True, fixture)
        # Stage boundary: adapter-derived copies conservatively keep the
        # physical source borrowed. Direct container copies have separate
        # positive independence coverage; adapter detachment is deferred.
        CASES[name + '_copy_keeps_storage_borrow'] = (False, fixture.replace(
            '  return ' + check, '  values.reserve(64);\n  return ' + check))
        element = 'std::string_view' if payload == 'views' else 'Reading'
        initializer = ' = ""' if payload == 'views' else '{first}'
        detached = fixture.replace(
            '  mut std::vector<' + element + '> values{};',
            '  mut ' + element + ' selected' + initializer + ';\n  {\n'
            '  mut std::vector<' + element + '> values{};').replace(
                'auto selected =', 'selected =').replace(
                '  return ' + check, '  }\n  return ' + check)
        CASES[name + '_copy_rejects_ended_storage'] = (False, detached)
        for owner in ('first', 'second'):
            if payload == 'views':
                negative = fixture.replace('std::string ' + owner + '{',
                                           'mut std::string ' + owner + '{')
                mutation = owner + '.clear();'
            else:
                negative = fixture.replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
                mutation = owner + ' = 99;'
            CASES[name + '_protects_' + owner] = (False, negative.replace(
                '  return ' + check, '  ' + mutation + '\n  return ' + check))

# In-place construction must preserve borrowed constructor inputs just as
# insertion of an already constructed value does.
for payload, source in (('views', GROWABLE_VIEWS), ('records', GROWABLE_RECORDS)):
    fixture = source
    for owner in ('first', 'second'):
        argument = owner + '.view()' if payload == 'views' else 'Reading(' + owner + ')'
        constructed = argument
        fixture = fixture.replace('values.push_back(' + argument + ');',
                                  'values.emplace_back(' + constructed + ');')
    name = 'growable_' + payload + '_emplace'
    CASES[name] = (True, fixture)
    for owner in ('first', 'second'):
        if payload == 'views':
            negative = fixture.replace('std::string ' + owner + '{',
                                       'mut std::string ' + owner + '{')
            mutation = owner + '.clear();'
        else:
            negative = fixture.replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
            mutation = owner + ' = 99;'
        CASES[name + '_protects_' + owner] = (False, negative.replace(
            '  return values.size()', '  ' + mutation + '\n  return values.size()'))

# Consume emplacement's returned exclusive reference, then keep only a value
# copy across growth. The copied payload must protect its external owner without
# pinning the vector's physical allocation.
for payload in ('views', 'records'):
    source = CASES['growable_' + payload + '_emplace'][1]
    typename = 'std::string_view' if payload == 'views' else 'Reading'
    argument = 'second.view()' if payload == 'views' else 'Reading(second)'
    check = 'selected == "second"' if payload == 'views' else 'selected.read() == 22'
    fixture = source.replace('  values.emplace_back(' + argument + ');',
        '  mut ' + typename + '& added = values.emplace_back(' + argument + ');\n'
        '  auto selected = added;').replace('  return values.size()',
        '  return ' + check + ' && values.size()')
    name = 'growable_' + payload + '_emplace_return'
    CASES[name] = (True, fixture)
    for owner in ('first', 'second'):
        if payload == 'views':
            negative = fixture.replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
            mutation = owner + '.clear();'
        else:
            negative = fixture.replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
            mutation = owner + ' = 99;'
        CASES[name + '_protects_' + owner] = (False, negative.replace(
            '  return ' + check, '  ' + mutation + '\n  return ' + check))

# Replacement through a container's mutable element reference must retain the
# new owner, while an independent earlier copy continues to retain the old one.
for payload in ('views', 'records'):
    fixture = CASES['growable_' + payload + '_emplace'][1]
    second = 'second.view()' if payload == 'views' else 'Reading(second)'
    old_check = 'old == "first"' if payload == 'views' else 'old.read() == 11'
    current_check = 'current == "second"' if payload == 'views' else 'current.read() == 22'
    fixture = fixture.replace('  values.emplace_back(' + second + ');',
        '  auto old = values[0];\n  values[0] = ' + second + ';')
    beginning = fixture.index('  return values.size()')
    fixture = fixture[:beginning] + ('  auto current = values[0];\n  return ' +
        old_check + ' && ' + current_check + ' ? 0 : 1;\n}\n')
    name = 'growable_' + payload + '_element_replacement'
    CASES[name] = (True, fixture)
    # Isolate replacement itself from the separate old-copy storage-release
    # requirement. Keep the composition above as the stronger acceptance case.
    without_copy = fixture.replace('  auto old = values[0];\n', '').replace(
        old_check + ' && ', '')
    CASES[name + '_without_prior_copy'] = (True, without_copy)
    for owner in ('first', 'second'):
        if payload == 'views':
            negative = fixture.replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
            mutation = owner + '.clear();'
        else:
            negative = fixture.replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
            mutation = owner + ' = 99;'
        CASES[name + '_protects_' + owner] = (False, negative.replace(
            '  auto current =', '  ' + mutation + '\n  auto current ='))
    declaration = 'std::string second{"second"};' if payload == 'views' else 'int second = 22;'
    CASES[name + '_rejects_expired_owner'] = (False,
        fixture.replace(declaration, '').replace('  values[0] = ' + second + ';',
            '  { ' + declaration + '\n    values[0] = ' + second + ';\n  }'))

SHARED_RECORD_REPLACEMENT = """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  mut int first = 11;
  int second = 22;
  mut Reading selected{first};
  selected = Reading(second);
  first = 99;
  return selected.read() == 22 ? 0 : 1;
}
"""
CASES['owned_construction_record_local_replacement'] = (True, SHARED_RECORD_REPLACEMENT)
CASES['owned_construction_record_local_replacement_protects_owner'] = (False,
    SHARED_RECORD_REPLACEMENT.replace('int second =', 'mut int second =').replace(
        '  first = 99;', '  second = 99;'))
CASES['owned_construction_record_local_replacement_live_alias'] = (False,
    SHARED_RECORD_REPLACEMENT.replace('  selected = Reading(second);',
        '  Reading& alias = selected;\n  selected = Reading(second);').replace(
        '  first = 99;\n', '').replace('return selected.read()', 'return alias.read()'))

# An unrelated source-defined owner exercises the same mutable accessor store
# contract without vector, allocation growth, or a nominal library exemption.
LOCAL_ACCESSOR_REPLACEMENT = CASES['owned_storage_two_owners'][1].replace(
    '  RawSlots() = default;',
    '  RawSlots() = default;\n'
    '  mut T& operator[](int index) mut {\n'
    '    unsafe { return std::borrow_owned_mut_at(this.left.data(), this); }\n'
    '  }').replace(
    '    std::string_view selected = std::borrow_owned_at(right, storage);',
    '    auto old = storage[0];\n'
    '    storage[0] = second.view();\n'
    '    std::string_view selected = storage[0];').replace(
    'return selected == "second"', 'return old == "first" && selected == "second"').replace(
    'std::initialize_owned_at(right, storage, second.view());',
    'std::initialize_owned_at(right, storage, "unused");')
CASES['owned_construction_custom_accessor_replacement'] = (True, LOCAL_ACCESSOR_REPLACEMENT)
for retained in ('first', 'second'):
    CASES['owned_construction_custom_accessor_replacement_protects_' + retained] = (False,
        LOCAL_ACCESSOR_REPLACEMENT.replace('std::string ' + retained + '{',
            'mut std::string ' + retained + '{').replace(
            '    std::string_view selected =',
            '    ' + retained + '.clear();\n    std::string_view selected ='))

CASES['growable_views_literals'] = (True, GROWABLE_VIEWS
    .replace('first.view()', '"first"').replace('second.view()', '"second"'))
for owner in ('first', 'second'):
    CASES['growable_views_protects_' + owner] = (False, GROWABLE_VIEWS
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  return values.size()', '  ' + owner + '.clear();\n  return values.size()'))
CASES['growable_views_selected_outlives_storage'] = (True, "import std.vector;\n" + """int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view selected = "";
  {
    mut std::vector<std::string_view> values{};
    values.push_back(first.view()); values.push_back(second.view());
    selected = values[0];
    values.reserve(32);
  }
  return selected == "first" ? 0 : 1;
}
""")

CASES['growable_views_element_reference_blocks_growth'] = (False,
    GROWABLE_VIEWS.replace('  values.reserve(32);',
        '  std::string_view& element = values[0];\n'
        '  values.reserve(32);\n'
        '  if (element != "first") { return 2; }'))
for owner, other in (('first', 'second'), ('second', 'first')):
    CASES['growable_views_rejects_inner_' + owner + '_owner'] = (False,
        "import std.vector;\n" + 'int main() {\n'
        + '  std::string ' + other + '{"' + other + '"};\n'
        + '  mut std::vector<std::string_view> values{};\n'
        + '  { std::string ' + owner + '{"' + owner + '"};\n'
        + '    values.push_back(first.view()); values.push_back(second.view()); }\n'
        + '  values.reserve(32);\n'
        + '  return values[0] == "first" && values[1] == "second" ? 0 : 1;\n'
        + '}\n')

# Public operation compositions must retain external contents, not just the
# allocation's physical lifetime. Keep both nominal views and borrowed records.
CASES['growable_views_coverage_expired_owner'] = (False,
    (Path(__file__).resolve().parent / 'coverage' / 'rejections' /
     'own-x12-view-in-vector.gti').read_text())

for payload, source, element in (
        ('views', GROWABLE_VIEWS, 'std::string_view'),
        ('records', GROWABLE_RECORDS, 'Reading')):
    fixture = source.replace(f'  mut std::vector<{element}> values{{}};',
        f'  mut std::vector<{element}> copied{{}};\n  {{\n'
        f'  mut std::vector<{element}> values{{}};')
    fixture = fixture.replace('  values.reserve(32);',
        f'  mut std::vector<{element}> temporary = values.clone();\n'
        '  temporary.swap(copied);\n  }\n  copied.reserve(32);')
    fixture = fixture.replace('return values.size()', 'return copied.size()').replace(
        'values[0]', 'copied[0]').replace('values[1]', 'copied[1]')
    name = f'growable_{payload}_clone_outlives_source'
    CASES[name] = (True, fixture)
    for owner in ('first', 'second'):
        declaration = (f'std::string {owner}' + '{"' + owner + '"};'
                       if payload == 'views' else
                       f'int {owner} = ' + ('11;' if owner == 'first' else '22;'))
        mutation = f'{owner}.clear();' if payload == 'views' else f'{owner} = 99;'
        CASES[name + '_protects_' + owner] = (False,
            fixture.replace(declaration, 'mut ' + declaration).replace(
                '  copied.reserve(32);', '  ' + mutation + '\n  copied.reserve(32);'))
        # Moving the original referent into the inner scope must not become
        # safe just because the cloned allocation survives that scope.
        CASES[name + '_rejects_inner_' + owner] = (False,
            fixture.replace(declaration, '').replace('  {\n', '  {\n  ' + declaration + '\n', 1))

# Isolate owner-scope expiry from cloning: moving the source container must
# preserve exactly the same external lifetime obligations.
for payload in ('views', 'records'):
    for suffix in ('', '_rejects_inner_first', '_rejects_inner_second'):
        expected, fixture = CASES['growable_' + payload + '_clone_outlives_source' + suffix]
        CASES['growable_' + payload + '_move_outlives_source' + suffix] = (
            expected, fixture.replace('= values.clone();', '= std::move(values);'))

# A field's storage ends with its enclosing owner even when that owner has
# no destructor. This exercises projected places, not just scalar bindings.
for transfer in ('clone', 'move'):
    for suffix in ('', '_rejects_inner_first', '_rejects_inner_second'):
        expected, fixture = CASES['growable_records_' + transfer + '_outlives_source' + suffix]
        fixture = fixture.replace('class Reading',
            'class Number { public: int value; Number(int input) : value(input) {} };\nclass Reading', 1)
        for owner, value in (('first', 11), ('second', 22)):
            fixture = fixture.replace(f'int {owner} = {value};', f'Number {owner}' + '{' + str(value) + '};')
            fixture = fixture.replace(f'Reading({owner})', f'Reading({owner}.value)')
        CASES['growable_record_fields_' + transfer + '_outlives_source' + suffix] = (expected, fixture)

for payload, source in (('views', GROWABLE_VIEWS), ('records', GROWABLE_RECORDS)):
    for operation, statements in (
            ('insert', 'values.insert(1, FIRST);'),
            ('erase', 'values.erase(1);'),
            ('swap_elements', 'values.swap_elements(0, 1);'),
            ('shrink', 'values.shrink_to_fit();'),
            ('pop_append', 'values.pop_back(); values.push_back(SECOND);'),
            ('clear_append', 'values.clear(); values.push_back(FIRST); values.push_back(SECOND);'),
            ('clone', 'auto copied = values.clone();'),
            ('swap', 'mut std::vector<ELEMENT> other{}; values.swap(other);')):
        statements = statements.replace('ELEMENT', 'std::string_view' if payload == 'views' else 'Reading')
        statements = statements.replace('FIRST', 'first.view()' if payload == 'views' else 'Reading(first)')
        statements = statements.replace('SECOND', 'second.view()' if payload == 'views' else 'Reading(second)')
        fixture = source.replace('  return values.size()', '  ' + statements + '\n  return values.size()')
        if operation == 'insert':
            fixture = fixture.replace('values.size() == 2', 'values.size() == 3')
            fixture = fixture.replace('values[1]', 'values[2]')
            middle = 'values[1] == "first"' if payload == 'views' else 'values[1].read() == 11'
            fixture = fixture.replace(' ? 0 : 1;', ' && ' + middle + ' ? 0 : 1;')
        elif operation == 'erase':
            extra = 'second.view()' if payload == 'views' else 'Reading(second)'
            fixture = fixture.replace('  values.reserve(32);', '  values.push_back(' + extra + ');\n  values.reserve(32);')
        elif operation == 'swap_elements':
            fixture = fixture.replace('values[0]', 'values[SWAPPED]').replace('values[1]', 'values[0]').replace('values[SWAPPED]', 'values[1]')
        if operation in ('clone', 'swap'):
            target = 'copied' if operation == 'clone' else 'other'
            fixture = fixture.replace('return values.size()', 'return ' + target + '.size()')
            fixture = fixture.replace('values[0]', target + '[0]').replace('values[1]', target + '[1]')
        name = 'growable_' + payload + '_operation_' + operation
        CASES[name] = (True, fixture)
        for owner in ('first', 'second'):
            if payload == 'views':
                negative = fixture.replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
                mutation = owner + '.clear();'
            else:
                negative = fixture.replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
                mutation = owner + ' = 99;'
            # Insert after the operation, immediately before the final use.
            negative = negative.replace('  return ', '  ' + mutation + '\n  return ')
            CASES[name + '_protects_' + owner] = (False, negative)


# A retained reference does not make an element copyable. Relocating a
# move-only record must preserve both its owned payload and external owner.
MOVE_ONLY_BORROWED_RECORDS = """import std.vector;
import std.memory;
class Reading {
  int& value;
  std::unique_ptr<int> offset;
public:
  Reading(int& input, std::unique_ptr<int> owned)
      : value(input), offset(std::move(owned)) {}
  Reading(Reading& other) = delete;
  Reading(Reading&& other) = default;
  int read() { return this.value + *this.offset; }
};
int main() {
  int first = 11; int second = 22;
  mut std::vector<Reading> values{};
  values.push_back(Reading(first, std::make_unique<int>(1)));
  values.push_back(Reading(second, std::make_unique<int>(2)));
  values.reserve(32);
  mut std::vector<Reading> moved = std::move(values);
  moved.reserve(64);
  return moved.size() == 2 && moved[0].read() == 12 && moved[1].read() == 24 ? 0 : 1;
}
"""
CASES['growable_move_only_borrowed_records'] = (True, MOVE_ONLY_BORROWED_RECORDS)
for owner in ('first', 'second'):
    CASES['growable_move_only_borrowed_records_protects_' + owner] = (
        False, MOVE_ONLY_BORROWED_RECORDS
        .replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
        .replace('  return moved', '  ' + owner + ' = 99;\n  return moved'))


CASES['fixed_array_two_owners'] = (True, '''int main() {
  std::string first{"first"}; std::string second{"second"};
  std::string_view views[2] = {first.view(), second.view()};
  return views[0] == "first" && views[1] == "second" ? 0 : 1;
}''')


# Inline aggregate literals obey the same rule for ordinary reference-bearing
# records, not only the builtin text-view representation.
ARRAY_REFERENCE_RECORD = """class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int first = 11; int second = 22;
  Reading values[2] = {Reading(first), Reading(second)};
  auto copied = values;
  return copied[0].read() == 11 && copied[1].read() == 22 ? 0 : 1;
}
"""
CASES['fixed_array_reference_records'] = (True, ARRAY_REFERENCE_RECORD)
for owner in ('first', 'second'):
    CASES['fixed_array_reference_records_protects_' + owner] = (
        False, ARRAY_REFERENCE_RECORD.replace('int ' + owner + ' =', 'mut int ' + owner + ' =')
        .replace('  return copied', '  ' + owner + ' = 99;\n  return copied'))

CASES['fixed_array_reference_record_outlives_storage'] = (True,
    ARRAY_REFERENCE_RECORD.replace('  Reading values[2]',
        '  mut Reading selected{first};\n  {\n  Reading values[2]')
    .replace('  auto copied = values;', '  selected = values[1];\n  }')
    .replace('copied[0].read() == 11 && copied[1].read() == 22', 'selected.read() == 22'))
CASES['fixed_array_reference_record_outlives_storage_protects_owner'] = (
    False, CASES['fixed_array_reference_record_outlives_storage'][1]
    .replace('int second =', 'mut int second =')
    .replace('  return selected', '  second = 99;\n  return selected'))


# Fixed storage must retain all elements' owners through a whole-array copy,
# and a selected value must outlive the physical array it was copied from.
# An unannotated helper must not erase an array element's external owner
# when publishing it to global storage. Literal-only calls remain a control.
ARRAY_SELECTED_PUBLICATION = '''std::string_view pick(std::string_view values[2]) { return values[0]; }
mut std::string_view selected_global = "";
int main() {
  std::string owner{"dynamic"};
  std::string_view values[2] = {owner.view(), "literal"};
  selected_global = pick(values);
  return selected_global == "dynamic" ? 0 : 1;
}'''
CASES['fixed_array_selected_publication_rejects_owner'] = (False, ARRAY_SELECTED_PUBLICATION)
CASES['fixed_array_selected_publication_literals'] = (
    True, ARRAY_SELECTED_PUBLICATION.replace('owner.view()', '"dynamic"'))

ARRAY_GLOBAL_PUBLICATION = """mut std::string_view escaped = "";
void publish(std::string_view values[2]) { escaped = values[0]; }
int main() {
  std::string owner{"dynamic"};
  std::string_view values[2] = {owner.view(), "literal"};
  publish(values);
  return 0;
}
"""
CASES['fixed_array_global_publication_rejects_owner'] = (False, ARRAY_GLOBAL_PUBLICATION)
CASES['fixed_array_global_publication_literals'] = (
    True, ARRAY_GLOBAL_PUBLICATION.replace('owner.view()', '"dynamic"'))

CASES['fixed_array_return_local_owner'] = (False, '''std::string_view[2] invalid() {
  std::string first{"first"}; std::string second{"second"};
  return {first.view(), second.view()};
}
int main() {
  auto values = invalid();
  return values[0] == "first" ? 0 : 1;
}
''')
# Returning borrowed storage must preserve the caller's owners, rather than
# treating all array elements as static or tying them to the callee's shell.
CASES['fixed_array_return_external'] = (True, '''std::string_view<a>[2] pair_views<lifetime a>(std::string_view<a> first, std::string_view<a> second) {
  return {first, second};
}
int main() {
  std::string first{"first"}; std::string second{"second"};
  auto values = pair_views(first.view(), second.view());
  return values[0] == "first" && values[1] == "second" ? 0 : 1;
}
''')
for retained, replaced in (('first', 'second'), ('second', 'first')):
    CASES['fixed_array_return_' + retained + '_only'] = (
        True, CASES['fixed_array_return_external'][1]
        .replace(replaced + '.view()', '"' + replaced + '"'))
    CASES['fixed_array_return_' + retained + '_only_protects_owner'] = (
        False, CASES['fixed_array_return_' + retained + '_only'][1]
        .replace('std::string ' + retained + '{', 'mut std::string ' + retained + '{')
        .replace('  return values[0]', '  ' + retained + '.clear();\n  return values[0]'))
CASES['fixed_array_return_unrelated_region'] = (False, CASES['fixed_array_return_external'][1]
    .replace('pair_views<lifetime a>', 'pair_views<lifetime a, lifetime b>')
    .replace('std::string_view<a> second)', 'std::string_view<b> second)'))
CASES['fixed_array_return_literals'] = (True, CASES['fixed_array_return_external'][1]
    .replace('return {first, second};', 'return {"first", "second"};'))
for owner in ('first', 'second'):
    CASES['fixed_array_return_protects_' + owner] = (
        False, CASES['fixed_array_return_external'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  return values[0]', '  ' + owner + '.clear();\n  return values[0]'))

CASES['fixed_array_helper_value'] = (True, '''std::string_view<a> select_view<lifetime a>(std::string_view<a>[2] values, int index) {
  return values[index];
}
int check(bool choose) {
  std::string first{"first"}; std::string second{"second"};
  std::string_view values[2] = {first.view(), second.view()};
  auto selected = select_view(values, choose ? 1 : 0);
  if (choose) { return selected == "second" ? 0 : 1; }
  return selected == "first" ? 0 : 2;
}
int main() { return check(true) + check(false); }
''')
CASES['fixed_array_helper_reference'] = (True, CASES['fixed_array_helper_value'][1]
    .replace('select_view<lifetime a>(std::string_view<a>[2] values',
             'select_view<lifetime a, lifetime shell>(std::string_view<a>[2]&<shell> values'))
CASES['fixed_array_helper_reference_outlives_storage'] = (True,
    CASES['fixed_array_helper_reference'][1]
    .replace('  std::string_view values[2] = {first.view(), second.view()};\n'
             '  auto selected = select_view(values, choose ? 1 : 0);',
             '  mut std::string_view selected = "";\n'
             '  {\n'
             '    std::string_view values[2] = {first.view(), second.view()};\n'
             '    selected = select_view(values, choose ? 1 : 0);\n'
             '  }'))
CASES['fixed_array_helper_value_outlives_storage'] = (
    True, CASES['fixed_array_helper_reference_outlives_storage'][1]
    .replace('select_view<lifetime a, lifetime shell>', 'select_view<lifetime a>')
    .replace('std::string_view<a>[2]&<shell> values', 'std::string_view<a>[2] values'))
for mode in ('value', 'reference'):
    for owner in ('first', 'second'):
        CASES['fixed_array_helper_' + mode + '_outlives_storage_protects_' + owner] = (
            False, CASES['fixed_array_helper_' + mode + '_outlives_storage'][1]
            .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
            .replace('  if (choose) { return selected',
                     '  ' + owner + '.clear();\n  if (choose) { return selected'))
for owner in ('first', 'second'):
    CASES['fixed_array_helper_protects_' + owner] = (
        False, CASES['fixed_array_helper_value'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  if (choose) { return selected',
                 '  ' + owner + '.clear();\n  if (choose) { return selected'))
CASES['fixed_array_one_owner'] = (True, CASES['fixed_array_two_owners'][1]
    .replace('second.view()', '"second"'))
for suffix, expected, mutation in [
    ('control', True, ''),
    ('protects_first', False, 'first.clear();'),
    ('protects_second', False, 'second.clear();'),
]:
    CASES['fixed_array_local_alias_' + suffix] = (expected, '''int main() {
  mut std::string first{"first"}; mut std::string second{"second"};
  std::string_view views[2] = {first.view(), second.view()};
  auto& alias = views;
  ''' + mutation + '''
  return alias[0] == "first" && alias[1] == "second" ? 0 : 1;
}''')

CASES['fixed_array_nested'] = (True, CASES['fixed_array_two_owners'][1]
    .replace('views[2] = {first.view(), second.view()}',
             'views[2][1] = {{first.view()}, {second.view()}}')
    .replace('views[0] ==', 'views[0][0] ==')
    .replace('views[1] ==', 'views[1][0] =='))
CASES['fixed_array_selected_copy_protects_inner_owner'] = (False, '''int main() {
  mut std::string_view selected = "";
  {
    std::string first{"first"}; std::string second{"second"};
    std::string_view views[2] = {first.view(), second.view()};
    selected = views[0];
  }
  return selected == "first" ? 0 : 1;
}
''')
CASES['fixed_array_copy'] = (True, CASES['fixed_array_two_owners'][1]
    .replace('  return views[0]', '  auto copied = views;\n  return copied[0]')
    .replace('&& views[1]', '&& copied[1]'))
for owner in ('first', 'second'):
    CASES['fixed_array_copy_protects_' + owner] = (
        False, CASES['fixed_array_copy'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  return copied[0]',
                 '  ' + owner + '.clear();\n  return copied[0]'))
CASES['fixed_array_selected_copy_outlives_storage'] = (True, '''int check(bool choose) {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view selected = "";
  {
    std::string_view views[2] = {first.view(), second.view()};
    selected = views[choose ? 1 : 0];
  }
  if (choose) { return selected == "second" ? 0 : 1; }
  return selected == "first" ? 0 : 2;
}
int main() { return check(true) + check(false); }
''')
for owner in ('first', 'second'):
    CASES['fixed_array_selected_copy_protects_' + owner] = (
        False, CASES['fixed_array_selected_copy_outlives_storage'][1]
        .replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        .replace('  if (choose) { return selected',
                 '  ' + owner + '.clear();\n  if (choose) { return selected'))


# Copy and move preserve both dependencies without changing runtime contents.
for operation in ('copy', 'move'):
    for spelling in ('auto', 'Labels'):
        _, source = CASES['two_owner_record']
        initializer = 'labels' if operation == 'copy' else 'std::move(labels)'
        source = source.replace('  return labels.first',
                                '  ' + spelling + ' transferred = ' + initializer + ';\n  return transferred.first')
        source = source.replace('&& labels.second', '&& transferred.second')
        CASES['two_owner_record_' + operation + '_' + spelling] = (True, source)


# The same aggregate publication must retain every owner through copies.
for owner in ('first', 'second'):
    for copy in (False, True):
        _, source = CASES['two_owner_record']
        source = source.replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        source = source.replace('  return labels.first',
            ('  auto copied = labels;\n' if copy else '') +
            '  ' + owner + '.clear();\n  return labels.first')
        if copy:
            source = source.replace('return labels.first', 'return copied.first').replace('&& labels.second', '&& copied.second')
        CASES['record_' + owner + '_owner_write' + ('_copy' if copy else '')] = (False, source)



# A returned view keeps the external owner, not the short-lived record passed
# to a relay. A field-path mapping must preserve each publication's lifetime.
for operation, expression in (
        ('field', 'Label(owner.view()).text'),
        ('argument', 'relay(Label(owner.view()))'),
        ('receiver', 'Label(owner.view()).read()'),
        ('factory', 'make_label(owner.view()).text')):
    source = """class Label {
public:
  std::string_view text;
  Label(std::string_view input) : text(input) {}
  std::string_view read() { return this.text; }
};
std::string_view relay(Label value) { return value.text; }
Label make_label(std::string_view input) { return Label(input); }
int main() {
  mut std::string owner{"borrowed"};
  std::string_view selected = EXPRESSION;
  return selected == "borrowed" ? 0 : 1;
}
""".replace('EXPRESSION', expression)
    CASES['temporary_field_relay_' + operation] = (True, source)
    CASES['temporary_field_relay_' + operation + '_protects_owner'] = (
        False, source.replace('  return selected',
                              '  owner.clear();\n  return selected'))


# Returning a restored carrier reference must preserve the actual address
# through local aliases as well as its erased owner dependency.
CASES['restored_reference_alias_return'] = (True, r'''
import std.memory;
class Cell {
  mut int& value;
public:
  Cell(mut int& value) : value(value) {}
  int read() { return this.value; }
  void add(int amount) mut { this.value += amount; }
};
unsafe Cell& relay_read(std::erased_object& object) {
  unsafe { Cell& cell = std::restore_ref<Cell>(object); return cell; }
}
unsafe mut Cell& relay_mut(mut std::erased_object& object) {
  unsafe { mut Cell& cell = std::restore_mut<Cell>(object); return cell; }
}
int main() {
  mut int value = 3;
  mut Cell cell{value};
  { std::erased_object& object = std::erase_ref(cell);
    unsafe { if (relay_read(object).read() != 3) return 1; } }
  { mut std::erased_object& object = std::erase_mut(cell);
    unsafe { relay_mut(object).add(2); } }
  return cell.read() == 5 ? 0 : 2;
}
''')

CASES['receiver_return_rejects_unrelated_parameter'] = (False, r'''
class View {
  int& value;

public:
  View(int& source) : value(source) {}
  int& get() { return this.value; }
};

class WrongFactory {
public:
  View make(int& source) { return View(source); }
public:
  WrongFactory() = default;
};

int main() { int n=1; WrongFactory factory{}; auto v=factory.make(n); return 0; }
''')


CASES['record_replacement_releases_previous_owner'] = (True, r'''

class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int first_owner = 1;
  mut int second_owner = 2;
  mut ReadCarrier left = ReadCarrier(first_owner);
  ReadCarrier right = ReadCarrier(second_owner);
  left = right;
  first_owner = 3;
  return left.referent - 2;
}
int main() { return use(); }
''')
CASES['record_replacement_protects_new_owner'] = (
    False, CASES['record_replacement_releases_previous_owner'][1]
    .replace('first_owner = 3;', 'second_owner = 3;'))
CASES['record_replacement_protects_independent_copy'] = (
    False, CASES['record_replacement_releases_previous_owner'][1]
    .replace('  left = right;', '  ReadCarrier copied = left;\n  left = right;')
    .replace('return left.referent - 2;', 'return copied.referent - 1;'))


CASES['borrowed_cursor_algorithm_compositions'] = (True, r'''

import std.algorithm;
import std.iterator;
import std.vector;

int main() {
  mut std::vector<int32_t> source = std::vector<int32_t>();
  source.push_back(5);
  source.push_back(1);
  source.push_back(4);
  mut std::vector<int32_t> destination = std::vector<int32_t>(std::size_t(3));
  std::copy(source.begin(), source.end(), destination.begin_mut());
  int32_t nine = 9;
  std::fill(destination, nine);
  mut std::vector<int32_t> doubled = std::vector<int32_t>(std::size_t(3));
  std::transform(source, doubled,
                 [](int32_t& value) -> int32_t { return value * 2; });
  std::sort(source);
  std::reverse(source);
  int32_t head = source[std::size_t(0)];
  mut auto cursor = source.begin();
  ++cursor;
  --cursor;
  cursor += std::ptrdiff_t(2);
  cursor -= std::ptrdiff_t(1);
  auto last = source.end();
  std::ptrdiff_t left = cursor - last;
  auto stepped = std::prev(std::next(source.begin(), std::ptrdiff_t(2)));
  return (*cursor == 4 && left == std::ptrdiff_t(-2) && *stepped == 4 &&
          destination[std::size_t(0)] == 9 &&
          doubled[std::size_t(0)] == 10 && head == 5)
             ? 0
             : 1;
}
''')
CASES['borrowed_cursor_assignment_rejects_live_child'] = (False, r'''
import std.vector;
int overwrite(mut std::vector<int32_t>::iterator cursor) {
  int32_t& held = *cursor;
  *cursor = 7;
  return held;
}
int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>(std::size_t(1));
  return overwrite(values.begin_mut());
}
''')


CASES['record_replacement_reference_argument'] = (
    True, 'int read_reference(int& value) { return value; }\n' +
    CASES['record_replacement_releases_previous_owner'][1]
    .replace('return left.referent - 2;',
             'return read_reference(left.referent) - 2;'))
CASES['record_replacement_reference_argument_protects_new_owner'] = (
    False, CASES['record_replacement_reference_argument'][1]
    .replace('first_owner = 3;', 'second_owner = 3;'))

CASES['record_replacement_reference_return'] = (True, r'''
int& read_reference(int& value) { return value; }

class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int first_owner = 1;
  mut int second_owner = 2;
  mut ReadCarrier left = ReadCarrier(first_owner);
  ReadCarrier right = ReadCarrier(second_owner);
  left = right;
  first_owner = 3;
  return read_reference(left.referent) - 2;
}
int main() { return use(); }
''')

CASES['record_replacement_reference_return_protects_new_owner'] = (False, r'''
int& read_reference(int& value) { return value; }

class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int first_owner = 1;
  mut int second_owner = 2;
  mut ReadCarrier left = ReadCarrier(first_owner);
  ReadCarrier right = ReadCarrier(second_owner);
  left = right;
  second_owner = 3;
  return read_reference(left.referent) - 2;
}
int main() { return use(); }
''')

CASES['record_replacement_captured_reference'] = (True, r'''
int& read_reference(int& value) { return value; }

class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int first_owner = 1;
  mut int second_owner = 2;
  mut ReadCarrier left = ReadCarrier(first_owner);
  ReadCarrier right = ReadCarrier(second_owner);
  left = right;
  first_owner = 3;
  int& held = read_reference(left.referent);
  left = ReadCarrier(first_owner);

  return held - 2;
}
int main() { return use(); }
''')

CASES['record_replacement_captured_reference_protects_owner'] = (False, r'''
int& read_reference(int& value) { return value; }

class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int first_owner = 1;
  mut int second_owner = 2;
  mut ReadCarrier left = ReadCarrier(first_owner);
  ReadCarrier right = ReadCarrier(second_owner);
  left = right;
  first_owner = 3;
  int& held = read_reference(left.referent);
  left = ReadCarrier(first_owner);
  second_owner = 4;
  return held - 2;
}
int main() { return use(); }
''')

CASES['mixed_reference_pack_contents'] = (True, r'''
import std.string;
class Reading {
public:
  int& value;
  Reading(int& source) : value(source) {}
};
int read(std::string_view& first, Reading& middle, std::string_view& last) {
  return first == "first" && middle.value == 7 && last == "last" ? 0 : 1;
}
int through_pack<Args...>(Args&... args) { return read(args...); }
int main() {
  mut std::string first{"first"};
  std::string last{"last"};
  int number = 7;
  Reading middle{number};
  std::string_view a = first.view();
  std::string_view b = last.view();
  return through_pack(a, middle, b);
}
''')
CASES['mixed_reference_pack_protects_text_owner'] = (False,
    CASES['mixed_reference_pack_contents'][1].replace(
        'return through_pack(a, middle, b);',
        'first.clear(); return through_pack(a, middle, b);'))


# A reference formal's opaque content witness does not borrow unrelated
# scalar fields. Keep the aggregate itself owned; nested borrowed aggregates
# remain outside this delivery's container boundary.
CASES['projected_owned_storage_reference_shell'] = (True, r'''
import std.string;
import std.vector;
struct State {
  mut std::size_t position = 0;
  std::vector<std::string> values;
  State(std::vector<std::string> values) : values(std::move(values)) {}
};
int inspect(mut State& state) {
  state.position = state.position + std::size_t(1);
  return state.values[0].size() == std::size_t(5) && state.position == std::size_t(1) ? 0 : 1;
}
int main() {
  mut std::vector<std::string> values{};
  values.push_back(std::string("hello"));
  mut State state{std::move(values)};
  return inspect(state);
}
''')


CASES['projected_record_reference_staged_receiver'] = (True, r'''
import std.vector;
struct Item { int value; Item(int value) : value(value) {} };
struct State {
  mut std::vector<Item> values;
  State(std::vector<Item> values) : values(std::move(values)) {}
};
int inspect(mut State& state) {
  Item item = state.values.back();
  state.values.pop_back();
  return item.value;
}
int main() {
  mut std::vector<Item> values{};
  values.push_back(Item(42));
  mut State state{std::move(values)};
  return inspect(state) == 42 ? 0 : 1;
}
''')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=sorted(CASES))
    parser.add_argument('--jobs', type=int, default=2,
                        help='maximum independent cases to run concurrently (default: 2)')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    evidence = {'cases': {}, 'steps': []}
    evidence_lock = threading.Lock()
    environment = dict(os.environ)
    for key in ('SDKROOT', 'DEVELOPER_DIR', 'MACOSX_DEPLOYMENT_TARGET'):
        environment.pop(key, None)

    def run(label, command):
        result = subprocess.run(list(map(str, command)), capture_output=True,
                                text=True, timeout=240, env=environment, cwd=output)
        (output / (label + '.log')).write_text(result.stdout + result.stderr)
        with evidence_lock:
            evidence['steps'].append({'label': label, 'command': list(map(str, command)),
                                      'exit': result.returncode})
        return result

    def check_case(name):
        accepted, source = CASES[name]
        path = output / (name + '.gti')
        path.write_text(PREFIX + source + '\n')
        passed = True
        for mode in ['O0', 'O3']:
            label = name + '-' + mode
            binary = output / label
            result = run(label + '-compile',
                         [compiler, path, '--backend', 'llvm', '-' + mode, '-o', binary])
            if accepted:
                ok = result.returncode == 0
                if ok:
                    executed = run(label + '-execute', [binary])
                    ok = executed.returncode == 0 and not executed.stdout and not executed.stderr
            else:
                # A parser failure, ICE or backend failure is not a borrow rejection.
                diagnostic = result.stdout + result.stderr
                ok = result.returncode != 0 and bool(re.search(r'error\[GTI-S\d+\]', diagnostic))
                ok = ok and 'GTI-B0001' not in diagnostic
                # Admission failures do not prove owner protection.
                ok = ok and 'single-owner' not in diagnostic
                ok = ok and 'second owner dependency' not in diagnostic
                ok = ok and 'error[GTI-S2045]' not in diagnostic
                if name.startswith(('growable_', 'owned_construction_')):
                    errors = [line for line in diagnostic.splitlines() if 'error[' in line]
                    # A failure inside an instantiated library helper must
                    # not masquerade as rejection of the fixture's escape.
                    ok = ok and bool(errors) and all(
                        line.startswith(str(path) + ':') and
                        'error[GTI-S2017]' in line for line in errors)
                if name == 'borrowed_consuming_iteration_requires_extraction_views':
                    ok = ok and 'error[GTI-S2083]' in diagnostic and 'independent owned value' in diagnostic
                if name == 'borrowed_consuming_iteration_requires_extraction_records':
                    ok = ok and 'error[GTI-S2029]' in diagnostic and 'relocatable' in diagnostic
                if name == 'replacement_loop_protects_inner_owner':
                    ok = ok and 'Cannot use a borrowed value after its owner' in diagnostic
                    ok = ok and 'A returned borrow' not in diagnostic
                if name == 'borrowed_cursor_assignment_rejects_live_child':
                    ok = ok and 'error[GTI-S2017]' in diagnostic
                if name.startswith('record_') or '_protects_' in name or name in (
                        'copy_retains_old_owner', 'replacement_protects_new_owner',
                        'replacement_branch_keeps_old_copy'):
                    ok = ok and 'error[GTI-S2017]' in diagnostic
                    ok = ok and 'Retained borrow originates here.' in diagnostic
            if not ok:
                print(label, (result.stdout + result.stderr)[-2500:], flush=True)
            passed = passed and ok
            binary.unlink(missing_ok=True)
        return name, accepted, passed

    # Every case has distinct source, executable and log paths. Only the
    # evidence inventory is shared; compilation and O0/O3 execution stay
    # sequential within each case. Deduplicate selectors to avoid path races.
    selected = list(dict.fromkeys(args.case or CASES))
    try:
        with ThreadPoolExecutor(max_workers=min(args.jobs, len(selected))) as pool:
            futures = [pool.submit(check_case, name) for name in selected]
            for future in as_completed(futures):
                name, accepted, passed = future.result()
                evidence['cases'][name] = {'expected_acceptance': accepted, 'passed': passed}
                print(name, 'PASS' if passed else 'FAIL', flush=True)
    finally:
        (output / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    return 0 if all(item['passed'] for item in evidence['cases'].values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
