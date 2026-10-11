#!/usr/bin/env python3
"""Retained callable carriers delay signature completeness until instantiation."""

import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main
from harness_timeouts import scaled


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/generic_callable_adapter_declaration_order.gti"
    with tempfile.TemporaryDirectory(prefix="gti-generic-callable-adapter-") as temporary:
        emitted = pathlib.Path(temporary) / "adapters.cpp"
        result = subprocess.run([str(pathlib.Path(sys.argv[1]).resolve()),
                                 str(fixture), "--emit-cpp", "-o", str(emitted)],
                                capture_output=True, text=True, timeout=scaled(120))
        assert result.returncode == 0, (result.stdout, result.stderr)
        text = emitted.read_text()
        packet_layout = text.index("struct Packet {")
        for owner, capability, receiver in (
                ("Target", "read_call", "const Target &"),
                ("MutableTarget", "mutable_call", "MutableTarget &"),
                ("OnceTarget", "once_call", "OnceTarget &&")):
            identity = f"std::conditional_t<true, ::__gti_program::api::Packet, {owner}>"
            declaration = f"friend {identity} __gti_invoke({receiver}__gti_callable, "
            adapter = text.index(declaration)
            assert text.index(f"class {owner} {{") < adapter < packet_layout
            line = text[adapter:text.index("\n", adapter)]
            assert f"callable_capability::{capability}" in line
            assert f"{identity} __gti_arg_0" in line
            assert "std::move(__gti_arg_0)" in line
            assert " { return " in line

    lines = [index + 1 for index, line in enumerate(fixture.read_text().splitlines())
             if "return Packet(result / divisor);" in line]

    def failure(index):
        return (f"{fixture.name}:{lines[index]}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "6\n5\n3\n9\n", "")),
        (("read",), (70, "9\n", failure(0))),
        (("mutable", "failure"), (70, "6\n9\n", failure(1))),
        (("once", "callback", "failure"), (70, "6\n5\n9\n", failure(2))),
    ]})
