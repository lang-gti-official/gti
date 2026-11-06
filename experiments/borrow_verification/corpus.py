"""Hand-derived source obligations; deliberately outside registered test discovery."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    name: str
    rule: str
    source: str
    accepted: bool = True
    codes: tuple[str, ...] = ()
    exit_code: int = 0
    stdout: str = ""
    stderr_contains: str = ""


def cases():
    pair = 'struct Pair { mut int left = 1; mut int right = 2; Pair() = default; };\n'
    relay = 'int& relay(int& x) { return x; }\n'
    mut_relay = 'mut int& relay(mut int& x) { return x; }\n'
    result = [
        Case('shared_aliases', 'overlap', 'int main() { mut int x = 1; int& a = x; int& b = x; return a + b - 2; }'),
        Case('shared_write_conflict', 'overlap', 'int main() { mut int x = 1; int& a = x; x = 2; return a; }', False, ('GTI-S2017',)),
        Case('mutable_alias_conflict', 'overlap', 'int main() { mut int x = 1; mut int& a = x; mut int& b = x; a = 2; return b; }', False, ('GTI-S2017',)),
        Case('disjoint_fields', 'overlap', pair + 'int main() { mut Pair p{}; mut int& a = p.left; mut int& b = p.right; a += 1; b += 1; return a + b - 5; }'),
        Case('whole_owner_conflict', 'overlap', pair + 'int take(Pair& p) { return p.left; } int main() { mut Pair p{}; mut int& a = p.left; int n = take(p); a += 1; return n; }', False, ('GTI-S2017',)),
        Case('child_then_parent', 'reborrow', 'int main() { mut int x = 1; mut int& p = x; { mut int& c = p; c += 1; } p += 1; return p - 3; }'),
        Case('parent_while_child_live', 'reborrow', 'int main() { mut int x = 1; mut int& p = x; mut int& c = p; p = 9; return c; }', False, ('GTI-S2017',)),
        Case('returned_reference', 'escape', relay + 'int main() { int x = 7; int& r = relay(x); return r - 7; }'),
        Case('returned_local', 'escape', 'int& bad() { int x = 7; return x; } int main() { return bad(); }', False, ('GTI-S2017',)),
        Case('temporary_non_escape', 'temporary', 'int read(int& x) { return x; } int main() { return read(7) - 7; }'),
        Case('multiple_temporary_endpoints', 'cleanup', 'int compute(int& a, int& b, int& c, int& d) { return a / b + c / d; } int main() { return compute(42, 2, 6, 2) - 24; }'),
        Case('temporary_escape', 'temporary', relay + 'int main() { return relay(7); }', False, ('GTI-S2017',)),
        Case('temporary_mutable', 'temporary', mut_relay + 'int main() { return relay(7); }', False, ('GTI-S2017',)),
        Case('move_after_last_use', 'move', 'int main() { mut int x = 7; int& r = x; int n = r; int y = std::move(x); return n + y - 14; }'),
        Case('move_while_borrowed', 'move', 'int main() { mut int x = 7; int& r = x; int y = std::move(x); return r + y; }', False, ('GTI-S2017',)),
        Case('moved_read', 'initialization', 'int main() { mut int x = 7; int y = std::move(x); return x + y; }', False, ('GTI-S2018',)),
        Case('reinitialize_moved', 'initialization', 'int main() { mut int x = 7; int y = std::move(x); x = 9; return x + y - 16; }'),
        Case('branch_join_valid', 'control_flow', 'int f(bool b) { mut int x = 1; if (b) { x = 2; } else { x = 2; } int& r = x; return r; } int main() { return f(true) + f(false) - 4; }'),
        Case('branch_move_join', 'control_flow', 'int f(bool b) { mut int x = 1; if (b) { int y = std::move(x); } return x; } int main() { return f(false); }', False, ('GTI-S2018',)),
        Case('loop_reborrow', 'control_flow', 'int main() { mut int x = 0; for (mut int i = 0; i < 3; ++i) { mut int& r = x; r += 1; } return x - 3; }'),
        Case('loop_move', 'control_flow', 'int main() { mut int x = 1; for (mut int i = 0; i < 3; ++i) { int y = std::move(x); } return 0; }', False, ('GTI-S2018',)),
        Case('call_exclusive', 'call_contract', 'void bump(mut int& a, int& b) { a += b; } int main() { mut int x = 1; int y = 2; bump(x, y); return x - 3; }'),
        Case('call_overlap', 'call_contract', 'void bump(mut int& a, int& b) { a += b; } int main() { mut int x = 1; bump(x, x); return x; }', False, ('GTI-S2017',)),
        Case('returned_mutable_conflict', 'call_contract', mut_relay + 'int main() { mut int x = 1; mut int& r = relay(x); x = 2; return r; }', False, ('GTI-S2017',)),
        Case('view_retains_owner', 'carrier', 'import std.string; int main() { std::string s{"hello"}; auto v = s.view(); return int(v.size()) - 5; }'),
        Case('view_owner_move', 'carrier', 'import std.string; int main() { mut std::string s{"hello"}; auto v = s.view(); auto t = std::move(s); return int(v.size()); }', False, ('GTI-S2017',)),
        Case('user_view_retains_owner', 'carrier', 'struct View { int& value; View(int& x) : value(x) {} }; int main() { int x = 7; View v{x}; return v.value - 7; }'),
        Case('user_view_local_escape', 'carrier', 'struct View { int& value; View(int& x) : value(x) {} }; View bad() { int x = 7; return View(x); } int main() { auto v = bad(); return v.value; }', False, ('GTI-S2045',)),
        Case('generic_reference', 'generic', 'T& identity<T>(T& x) { return x; } int main() { int x = 7; return identity(x) - 7; }'),
        Case('generic_temporary_escape', 'generic', 'T& identity<T>(T& x) { return x; } int main() { return identity(7); }', False, ('GTI-S2017',)),
        Case('closure_local_capture', 'closure', 'int main() { mut int x = 1; auto f = [&x]() -> int { return x; }; return f() - 1; }'),
        Case('closure_borrow_conflict', 'closure', 'int main() { mut int x = 1; auto f = [&x]() -> int { return x; }; x = 2; return f(); }', False, ('GTI-S2017',)),
    ]
    cleanup = '''import std.io;
struct Guard { int value; Guard(int n) : value(n) {} ~Guard() { std::println(this.value); } };
'''
    result.extend([
        Case('normal_cleanup', 'cleanup', cleanup + 'int main() { Guard a{1}; { Guard b{2}; } return 0; }', stdout='2\n1\n'),
        Case('failure_cleanup', 'cleanup', cleanup + 'int divide(int& a, int& b) { return a / b; } int main() { Guard a{1}; Guard b{2}; int zero = 0; return divide(7, zero); }', exit_code=70, stdout='2\n1\n', stderr_contains='division'),
        Case('temporary_reference_failure_order', 'cleanup', 'struct Holder { int value; Holder(int x) : value(x) {} int& get() { return this.value; } }; int divide(int& a, int& b) { return a / b; } int main() { Holder h{2}; return divide(42, h.get()) - 21; }'),
        Case('partial_constructor_cleanup', 'cleanup', cleanup + 'struct Group { Guard a; Guard b; Group(int z) : a(Guard(1)), b(Guard(1 / z)) {} }; int main() { Guard outside{3}; Group inner{0}; return 0; }', exit_code=70, stdout='1\n3\n', stderr_contains='division'),
        Case('owner_drop_after_use', 'owner_liveness', cleanup + 'void consume(Guard value) {} int main() { Guard owner{1}; Guard& r = owner; int n = r.value; consume(std::move(owner)); return n - 1; }', stdout='1\n'),
        Case('owner_drop_while_borrowed', 'owner_liveness', cleanup + 'void consume(Guard value) {} int main() { Guard owner{1}; Guard& r = owner; consume(std::move(owner)); return r.value; }', False, ('GTI-S2017',)),
    ])
    return result
