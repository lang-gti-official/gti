#!/usr/bin/env python3
"""Repair-contract pilots: explicit review, source identity and round trips."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession


def apply_action(action, uri, version, source):
    changes = action["edit"]["documentChanges"]
    assert len(changes) == 1, changes
    assert changes[0]["textDocument"] == {"uri": uri, "version": version}

    def offset(position):
        lines = source.splitlines(keepends=True)
        prefix = sum(len(line) for line in lines[:position["line"]])
        line = lines[position["line"]] if position["line"] < len(lines) else ""
        units = 0
        for index, character in enumerate(line):
            if units == position["character"]:
                return prefix + index
            units += len(character.encode("utf-16-le")) // 2
        assert units == position["character"]
        return prefix + len(line)

    edits = [(offset(edit["range"]["start"]), offset(edit["range"]["end"]), edit["newText"])
             for edit in changes[0]["edits"]]
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    return source


def run(executable, root):
    session = LspSession(executable)
    uri = (root / "main.gti").as_uri()
    request_id = 0
    version = 0

    def request(method, params):
        nonlocal request_id
        request_id += 1
        session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        return session.receive_until(lambda message: message.get("id") == request_id)["result"]

    def notify(method, params):
        session.send({"jsonrpc": "2.0", "method": method, "params": params})

    def change(source):
        nonlocal version
        version += 1
        if version == 1:
            notify("textDocument/didOpen", {"textDocument": {
                "uri": uri, "languageId": "gti", "version": version, "text": source}})
        else:
            notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                                             "contentChanges": [{"text": source}]})
        return session.receive_until(lambda message:
            message.get("method") == "textDocument/publishDiagnostics"
            and message["params"].get("uri") == uri
            and message["params"].get("version") == version, timeout=30
        )["params"]["diagnostics"]

    def actions(diagnostic, automatic=False):
        return request("textDocument/codeAction", {
            "textDocument": {"uri": uri}, "range": diagnostic["range"],
            "context": {"diagnostics": [diagnostic], "only": ["quickfix"],
                        "triggerKind": 2 if automatic else 1}})

    def repair(source, code, expected):
        diagnostics = change(source)
        diagnostic = next(d for d in diagnostics if d["code"] == code)
        assert diagnostic["data"]["repairSnapshot"]
        assert all(f["applicability"] == "requiresReview" for f in diagnostic["data"]["fixes"])
        discovered = actions(diagnostic, automatic=True)
        assert len(discovered) == 1 and discovered[0]["isPreferred"] is False, discovered
        offered = actions(diagnostic)
        assert len(offered) == 1 and offered[0]["isPreferred"] is False, offered
        changed = apply_action(offered[0], uri, version, source)
        assert changed == expected, (changed, expected)
        # Same text, new editor version: an old diagnostic is still stale.
        fresh = next(d for d in change(source) if d["code"] == code)
        assert actions(diagnostic) == [], "old snapshot accepted after identical-text change"
        assert len(actions(fresh)) == 1
        remaining = change(changed)
        assert not [d for d in remaining if d.get("severity") == 1], remaining
        assert all(d["code"] != code for d in remaining), remaining
        assert actions(fresh) == [], "applied repair repeated"
        assert not [d for d in change(changed) if d.get("data", {}).get("fixes")]

    try:
        request("initialize", {"capabilities": {
            "workspace": {"workspaceEdit": {"documentChanges": True}},
            "textDocument": {"publishDiagnostics": {"dataSupport": True},
                             "codeAction": {"isPreferredSupport": True,
                                            "codeActionLiteralSupport": {
                                                "codeActionKind": {"valueSet": ["quickfix"]}}}}}})
        notify("initialized", {})
        for newline in ("\n", "\r\n"):
            source = ("int inner() { return 1; }\nint outer(int v) { return v; }\n"
                      "int main() { /*\U0001f600 before*/ outer(\n"
                      "  inner() /* adjacent */\n); return 0; }\n").replace("\n", newline)
            repair(source, "GTI-S2009", source.replace("/*\U0001f600 before*/ outer(",
                                                       "/*\U0001f600 before*/ [[discard]] outer("))
        source = ("#macro int identity(int value) { return value; }\n"
                  "int main() { /* call site */ identity(7); return 0; }\n")
        repair(source, "GTI-S2009", source.replace("/* call site */ identity(7)",
                                                  "/* call site */ [[discard]] identity(7)"))
        (root / "calls.gti").write_text((
                                            'export module calls;\n'
                                            'export int calculate(int value) { return value; }\n'
                                        ))
        source = ('import "calls.gti";\n'
                  'int main() { calculate(calculate(1)); return 0; }\n')
        repair(source, "GTI-S2009", source.replace("{ calculate(calculate(1))",
                                                  "{ [[discard]] calculate(calculate(1))"))
        (root / "api.gti").write_text((
                                          'export module api;\n'
                                          'namespace api { export int value() { return 7; } }\n'
                                      ))
        source = "// \U0001f600\r\nint main() { return api::value(); }\r\n"
        repair(source, "GTI-S2089", 'import "api.gti";\r\n' + source)
        # Different valid includes are alternatives, not an atomic combined edit.
        (root / "other.gti").write_text("export module other;\nexport namespace api { int value() { return 8; } }\n")
        notify("workspace/didChangeWatchedFiles", {"changes": [
            {"uri": (root / "other.gti").as_uri(), "type": 1}]})
        diagnostic = next(d for d in change(source) if d.get("data", {}).get("fixes"))
        offered = actions(diagnostic)
        assert len(offered) == 2, offered
        assert len({f["repairId"] for f in diagnostic["data"]["fixes"]}) == 2
        for action in offered:
            assert len(action["edit"]["documentChanges"][0]["edits"]) == 1
        discovered = actions(diagnostic, automatic=True)
        assert len(discovered) == 2 and all(not item["isPreferred"] for item in discovered), discovered
        for index in range(2):
            diagnostic = next(d for d in change(source) if d.get("data", {}).get("fixes"))
            changed = apply_action(actions(diagnostic)[index], uri, version, source)
            remaining = change(changed)
            assert not [d for d in remaining if d.get("severity") == 1], remaining
            assert actions(diagnostic) == []
        # Type/ownership changes are not synthesized from explanations.
        diagnostics = change('int main() { int value = "wrong"; return 0; }\n')
        assert any(d.get("severity") == 1 for d in diagnostics)
        assert all(actions(d) == [] for d in diagnostics)
        for source, code in [
            ('import std.string;\nvoid consume(std::string value) {}\n'
             'int main() { std::string value = std::string("x"); consume(value); return 0; }\n',
             "GTI-S2018"),
            ('class Counter { public: mut int value = 0; Counter() = default; '
             'void update() { value = 1; } };\n', "GTI-S2002"),
        ]:
            diagnostic = next(d for d in change(source) if d["code"] == code)
            assert not diagnostic.get("data", {}).get("fixes"), diagnostic
            assert actions(diagnostic) == []
            assert actions(diagnostic, automatic=True) == []
    finally:
        session.close()
    print("discard/include repair round trips, alternatives, Unicode/CRLF and stale identity passed")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="gti-repair-contract-") as directory:
        run(str(pathlib.Path(sys.argv[1]).resolve()), pathlib.Path(directory).resolve())
