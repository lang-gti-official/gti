#!/usr/bin/env python3
"""Exercise class producers across the eight consumers from coverage S01/S02."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile


PRELUDE = """import std.vector;
class Value {
  int number;
public:
  Value(int value) : number(value) {}
  int get() { return this.number; }
  Value twin() { return Value(this.number); }
  Value operator+(Value& other) { return Value(this.number + other.number); }
};
int consume(Value value) { return value.get(); }
Value relay(Value value) { return value; }
"""
PRODUCERS = {
    "constructor": ("(Value(3))", 3),
    "operator": ("(named + named)", 10),
    "conditional": ("(flag ? Value(3) : Value(7))", None),
}
CONSUMERS = {
    "binding": "Value result = {value}; return result.get() == {expected} ? 0 : 1;",
    "receiver": "return {value}.get() == {expected} ? 0 : 1;",
    "argument": "return consume({value}) == {expected} ? 0 : 1;",
    "relay": "return consume(relay({value})) == {expected} ? 0 : 1;",
    "container": "bag.push_back({value}); return bag[std::size_t(0)].get() == {expected} ? 0 : 1;",
    "operator": "Value zero = Value(0); return ({value} + zero).get() == {expected} ? 0 : 1;",
    "chain": "return {value}.twin().get() == {expected} ? 0 : 1;",
    "discard": "[[discard]] {value}.get(); return 0;",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("repository", type=Path)
    parser.add_argument("--emit-only", action="store_true")
    parser.add_argument("--producer", action="append", choices=PRODUCERS)
    parser.add_argument("--consumer", action="append", choices=CONSUMERS)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    env = dict(os.environ, GTI_STDLIB_PATH=str(args.repository.resolve() / "stdlib"))
    cases = [(p, c, flag) for p in (args.producer or PRODUCERS)
             for c in (args.consumer or CONSUMERS) for flag in (True, False)]
    with tempfile.TemporaryDirectory(prefix="gti-class-consumers-") as scratch:
        root = Path(scratch)

        def check(case):
            producer, consumer, flag = case
            name = f"{producer}-{consumer}-{flag}"
            value, expected = PRODUCERS[producer]
            if expected is None:
                expected = 3 if flag else 7
            body = CONSUMERS[consumer].format(value=value, expected=expected)
            source = root / f"{name}.gti"
            source.write_text(PRELUDE + "\nint main() {\n"
                              "Value named = Value(5);\n"
                              "mut std::vector<Value> bag{};\n"
                              f"bool flag = {str(flag).lower()};\n{body}\n}}\n")
            modes = (("c++23", "O0"),) if args.emit_only else (
                (standard, level) for standard in ("c++20", "c++23")
                for level in ("O0", "O1", "O3"))
            for standard, level in modes:
                output = root / f"{name}-{standard}-{level}"
                command = [str(compiler), str(source), "--std", standard,
                           f"-{level}", "--progress", "never", "-o", str(output)]
                command += ["--emit-cpp"] if args.emit_only else ["--", "-fno-elide-constructors"]
                compiled = subprocess.run(command, env=env, text=True,
                                          capture_output=True, timeout=120)
                if compiled.returncode:
                    return f"FAIL {name} {standard}/{level}\n{compiled.stdout}{compiled.stderr}"
                if not args.emit_only:
                    result = subprocess.run([str(output)], text=True,
                                            capture_output=True, timeout=30)
                    if result.returncode or result.stdout or result.stderr:
                        return f"FAIL {name} execution: {result}"
            return None

        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            failures = [result for result in pool.map(check, cases) if result]
        for failure in failures:
            print(failure)
        print(f"{len(cases) - len(failures)}/{len(cases)} class consumer cases passed"
              + (" (emission only; not runtime evidence)" if args.emit_only else ""))
        return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
