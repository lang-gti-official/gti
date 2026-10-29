#!/usr/bin/env python3
"""Compiler-owned what/why/where/options survive terminal and LSP presentation."""

import pathlib
import re
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def run(compiler, server, root):
    path = root / "explanations.gti"
    uri = path.as_uri()
    source = '''import std.string;
int main() {
  std::string owner = std::string("hello");
  /* 借 é */ std::string destination = std::move(owner);
  /* café 😀 */ return int(owner.size());
}
'''
    path.write_text(source)

    def terminal(text, color="never"):
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text)
        result = subprocess.run([compiler, str(path), "--emit-cpp", f"--color={color}"],
                                cwd=root, text=True,
                                capture_output=True, timeout=30)
        assert result.returncode == 65, result
        return result.stderr

    plain = terminal(source)
    assert "error[GTI-S2018]" in plain and "Ownership transferred here." in plain, plain
    assert "help: Use the value before transferring it" in plain, plain
    assert "\x1b" not in plain
    # Emoji occupies two terminal cells; neither UTF-8 byte count nor UTF-16
    # offsets are the terminal's column authority. The diagnostic header keeps
    # the compiler's one-based scalar column contract.
    lines = plain.splitlines()
    excerpt = next(i for i, line in enumerate(lines) if "|   /* café 😀 */" in line)
    displayed = lines[excerpt].split("| ", 1)[1]
    marker = lines[excerpt + 1].split("| ", 1)[1]
    assert marker.index("^") == displayed.index("owner") + 1, plain
    assert f":5:{source.splitlines()[4].index('owner') + 1}: error[GTI-S2018]" in plain, plain
    colored = terminal(source, "always")
    assert re.sub(r"\x1b\[[0-9;]*m", "", colored) == plain
    tabbed = terminal(source.replace("  /* café 😀 */", "\t/* café 😀 */"))
    assert "\t" not in tabbed and "|     /* café 😀 */" in tabbed, tabbed
    controlled = terminal(source.replace("café 😀", "café\x1b[31m"))
    assert "\x1b" not in controlled and "\\x1b[31m" in controlled, controlled
    for spelling, correction in (("cafe\u0301", -1), ("界", 1)):
        output = terminal(source.replace("café 😀", spelling))
        excerpt = next(i for i, line in enumerate(output.splitlines())
                       if "owner.size()" in line)
        displayed = output.splitlines()[excerpt].split("| ", 1)[1]
        marker = output.splitlines()[excerpt + 1].split("| ", 1)[1]
        assert marker.index("^") == displayed.index("owner") + correction, output
    invalid = terminal(b"/* a\x80 */\nint main() { return 0; }\n")
    assert "error[GTI-L0013]" in invalid and "/* a\\x80 */" in invalid, invalid
    invalid_lines = invalid.splitlines()
    excerpt = next(i for i, line in enumerate(invalid_lines) if "/* a\\x80 */" in line)
    displayed = invalid_lines[excerpt].split("| ", 1)[1]
    marker = invalid_lines[excerpt + 1].split("| ", 1)[1]
    assert marker.index("^") == displayed.index("\\x80") and "^~~~" in marker, invalid
    path.write_text(source)

    session = LspSession(server)
    try:
        session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                      "params": {"capabilities": {"textDocument": {
                          "publishDiagnostics": {"relatedInformation": True, "dataSupport": True},
                          "codeAction": {"codeActionLiteralSupport": {
                              "codeActionKind": {"valueSet": ["quickfix"]}}}}}}})
        session.receive_until(lambda message: message.get("id") == 1)
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

        def publication(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]

        diagnostics = publication(1)
        error = next(d for d in diagnostics if d.get("code") == "GTI-S2018")
        assert error["range"]["start"] == lsp_position(source, source.rindex("owner")), error
        assert error["range"]["end"] == lsp_position(source, source.rindex("owner") + 5), error
        note = next(n for n in error["relatedInformation"]
                    if n["message"] == "Ownership transferred here.")
        assert note["location"]["uri"] == uri
        assert note["location"]["range"]["start"] == lsp_position(source, source.index("owner);"))
        assert note["location"]["range"]["end"] == lsp_position(source, source.index("owner);") + 5)
        assert "Use the value before transferring it" in error["message"], error
        assert "\x1b" not in str(error)
        assert not error.get("data", {}).get("fixes"), error

        fixed = source.replace("std::move(owner)", "owner.clone()")
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": fixed}]}})
        assert not publication(2), "corrected overlay must clear the ownership error"
        session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri}, "range": error["range"],
            "context": {"diagnostics": [error], "only": ["quickfix"]}}})
        assert session.receive_until(lambda message: message.get("id") == 2)["result"] == []

        readonly = '''struct Command { mut int name = 0;  public: Command() = default; };
int main() { Command cmd{}; cmd.name = 1; return 0; }
'''
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": readonly}]}})
        diagnostic = next(d for d in publication(3) if "read-only receiver" in d["message"])
        assert "Declare 'cmd' with 'mut'" in diagnostic["message"]
        assert diagnostic["relatedInformation"][0]["location"]["range"]["start"] == lsp_position(
            readonly, readonly.index("cmd{}"))
        assert not diagnostic.get("data", {}).get("fixes")

        recovery = '''int main() {
  unsafe {
    int* pointer = nullptr;
    auto offset = missing_offset;
    auto first = pointer + (offset);
    auto second = pointer - first;
    auto independent = pointer + true;
  }
  return 0;
}
'''
        output = terminal(recovery)
        assert output.count("error[GTI-S2001]") == 1, output
        assert output.count("error[GTI-S2056]") == 1, output
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": recovery}]}})
        errors = publication(4)
        assert sum(d.get("code") == "GTI-S2001" for d in errors) == 1, errors
        assert sum(d.get("code") == "GTI-S2056" for d in errors) == 1, errors
        independent = next(d for d in errors if d.get("code") == "GTI-S2056")
        assert independent["range"]["start"] == lsp_position(
            recovery, recovery.index("+ true")), independent
        repaired = recovery.replace("missing_offset", "0")
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 5}, "contentChanges": [{"text": repaired}]}})
        remaining = publication(5)
        # After repairing the root, pointer subtraction is valid. Only the
        # independently invalid bool offset remains.
        assert not any(d.get("code") == "GTI-S2001" for d in remaining), remaining
        assert sum(d.get("code") == "GTI-S2056" for d in remaining) == 1, remaining

        branches = 'import std.string;\nvoid use(int path) {\n'
        branches += '  std::string owner{"text"};\n'
        for index in range(5):
            branches += (f'  if (path == {index}) {{ auto moved = std::move(owner); }} else ')
        branches += '{ auto moved = std::move(owner); }\n  std::println(owner.view());\n}\n'
        output = terminal(branches)
        assert "additional ownership-transfer location" in output, output
        assert output.count("note: Ownership transferred here.") == 4, output
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 6}, "contentChanges": [{"text": branches}]}})
        bounded = next(d for d in publication(6) if d.get("code") == "GTI-S2018")
        assert "additional ownership-transfer location" in bounded["message"], bounded
        assert len(bounded["relatedInformation"]) == 4, bounded

        contract_path = root / "member-contract.gti"
        contract = (
                       'export module member_contract;\n'
                       'export concept readable<T> = requires(T& value) { { value.snapshot() } -> int; };\n'
                       'export struct Wrapper<T> {\n'
                       '  int snapshot() requires std::integral<T> { return 7; }\n'
                       'public: Wrapper() = default;\n'
                       '};\n'
                       'export int read<T>(T& value) requires readable<T> { return value.snapshot(); }\n'
                   )
        contract_path.write_text(contract)
        member = '''import "member-contract.gti";
int main() { Wrapper<bool> value{}; return read(/* 😀 */ value); }
'''
        output = terminal(member)
        assert "Member requirement failed:" in output and "Type 'bool'" in output, output
        assert "std::integral" in output, output
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 7}, "contentChanges": [{"text": member}]}})
        failed = next(d for d in publication(7) if d.get("code") == "GTI-S2029")
        assert failed["range"]["start"] == lsp_position(member, member.index("value);")), failed
        assert not failed.get("data", {}).get("fixes"), failed
        notes = failed["relatedInformation"]
        assert any("Member requirement failed:" in note["message"] and
                   "bool" in note["message"] for note in notes), failed
        assert any(note["location"]["uri"] == contract_path.as_uri() and
                   note["location"]["range"]["start"] ==
                   lsp_position(contract, contract.index("integral<T>"))
                   for note in notes), failed
        fixed_member = member.replace("Wrapper<bool>", "Wrapper<int>")
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 8},
            "contentChanges": [{"text": fixed_member}]}})
        assert not publication(8), "repair must clear nested causes from the next snapshot"

        helper_path = root / "constant-helper.gti"
        helper = (
                     'export module constant_helper;\n'
                     'export constexpr int divide(int value) { return 100 / value; }\n'
                     'export constexpr int middle(int value) { /* 😀 */ return divide(value); }\n'
                     'export constexpr int outer(int value) { return middle(value); }\n'
                 )
        helper_path.write_text(helper)
        constant = '''import "constant-helper.gti";
constexpr int result = outer(0);
int main() { return 0; }
'''
        output = terminal(constant)
        assert output.count("Constant evaluation requested here.") == 3, output
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 9},
            "contentChanges": [{"text": constant}]}})
        # The failing operation belongs to the included file. The adapter
        # publishes it there, not at the root request's URI.
        constant_diagnostics = session.receive_until(lambda message:
            message.get("method") == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == helper_path.as_uri()
            and any(d.get("code") == "GTI-S2057"
                    for d in message["params"]["diagnostics"]))["params"]["diagnostics"]
        assert any(d.get("code") == "GTI-S2057" for d in constant_diagnostics), constant_diagnostics
        failed = next(d for d in constant_diagnostics if d.get("code") == "GTI-S2057")
        trace = [n for n in failed["relatedInformation"]
                 if n["message"] == "Constant evaluation requested here."]
        assert len(trace) == 3 and not failed.get("data", {}).get("fixes"), failed
        assert trace[0]["location"]["uri"] == uri, trace
        assert trace[1]["location"]["uri"] == helper_path.as_uri(), trace
        assert trace[2]["location"]["range"]["start"] == lsp_position(
            helper, helper.index("divide(value)") + len("divide(value")), trace
        # The source snapshot changes, not the library file on disk.
        session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 10},
            "contentChanges": [{"text": constant.replace("outer(0)", "outer(100)")}]}})
        cleared = session.receive_until(lambda message:
            message.get("method") == "textDocument/publishDiagnostics"
            and message["params"]["uri"] == helper_path.as_uri())["params"]["diagnostics"]
        assert not cleared, "repair must clear the constexpr call trace in the dependency"
    finally:
        session.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="gti-diagnostic-explanations-") as directory:
        run(str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve()),
            pathlib.Path(directory).resolve())
    print("Terminal and LSP diagnostic explanations passed")
