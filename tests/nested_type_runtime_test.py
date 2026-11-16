#!/usr/bin/env python3
"""Nested C ABI records and opaque handles cross the native boundary.

The other nested-type fixtures, imports and rejections are tests/ui cases
(tests/ui/types/nested/); this one links a C object.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert result.returncode == expected, (
        f"{command}: exit {result.returncode}\n{result.stdout}\n{result.stderr}"
    )
    return result


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    with tempfile.TemporaryDirectory(prefix="gti-nested-types-") as directory:
        scratch = Path(directory)
        native_source = scratch / "nested_native.c"
        native_source.write_text('''#include <stdint.h>
struct Native { int32_t value; };
struct Native make_native(void) { return (struct Native){7}; }
int32_t consume(struct Native value) { return value.value; }
typedef int32_t (*Callback)(struct Native);
Callback native_callback(void) { return consume; }
struct Handle { int32_t value; };
struct Handle *native_handle(void) { static struct Handle value = {1}; return &value; }
''', encoding="utf-8")
        native_object = scratch / "nested_native.o"
        run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-c",
             str(native_source), "-o", str(native_object)])
        native_consumer = scratch / "nested_native.gti"
        native_consumer.write_text('''struct Container {
  [[c_abi]] struct Native { int32_t value; };
  [[c_opaque]] struct Handle;
public:
  Container() = default;
};
using NativeCallback = (Container::Native) -> int32_t;
extern "C" {
  Container::Native make_native();
  int32_t consume(Container::Native value);
  NativeCallback native_callback();
  Container::Handle* native_handle();
}
int main() {
  Container::Native value = make_native();
  unsafe {
    Container::Handle* handle = native_handle();
    if (handle == nullptr) { return 1; }
    NativeCallback callback = native_callback();
    return consume(value) + callback(value) - 14;
  }
}
''', encoding="utf-8")
        for mode, flags, _native in native_modes():
            executable = scratch / f"native-{mode}"
            run([str(compiler), str(native_consumer), *flags,
                 "--progress", "never", "-o", str(executable), "--", str(native_object)])
            executed = run([str(executable)])
            assert executed.stdout == "" and executed.stderr == "", executed

    print("nested types: C ABI records, callbacks and opaque handles passed")


if __name__ == "__main__":
    main()
