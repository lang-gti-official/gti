#!/usr/bin/env python3
"""A misplaced export publishes one exact move edit; formatting spaces attributes."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: export_placement_lsp_test.py <gti_lsp>")
    valid = (
        "export module api;\n"
        "// 😀 UTF-16 control\n"
        "export [[track_caller]] /* 😀 */ [[discardable]] int check(bool ready) { return 1; }\n"
    )
    misplaced = valid.replace(
        "export [[track_caller]] /* 😀 */ [[discardable]]",
        "[[track_caller]] /* 😀 */ [[discardable]]\nexport"
    )
    with tempfile.TemporaryDirectory(prefix="gti-export-placement-lsp-") as directory:
        path = pathlib.Path(directory) / "api.gti"
        path.write_text(misplaced)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {
                    "workspace": {"workspaceEdit": {"documentChanges": True}},
                    "textDocument": {
                        "publishDiagnostics": {"dataSupport": True},
                        "codeAction": {"isPreferredSupport": True,
                                       "codeActionLiteralSupport": {"codeActionKind": {"valueSet": ["quickfix"]}}}
                    }}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": misplaced}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            def change(text, version):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]}})
                return diagnostics(version)

            def action_for(diagnostic, request_id, automatic=False):
                session.send({"jsonrpc": "2.0", "id": request_id, "method": "textDocument/codeAction", "params": {
                    "textDocument": {"uri": uri}, "range": diagnostic["range"],
                    "context": {"diagnostics": [diagnostic], "only": ["quickfix"],
                                "triggerKind": 2 if automatic else 1}}})
                return session.receive_until(lambda message: message.get("id") == request_id)["result"]

            errors = diagnostics(1)
            assert len(errors) == 1, errors
            error = errors[0]
            keyword = misplaced.index("export int")
            assert error["code"] == "GTI-P0001", error
            assert "'export' must begin the declaration" in error["message"], error
            assert error["range"] == {"start": lsp_position(misplaced, keyword),
                                      "end": lsp_position(misplaced, keyword + 6)}, error

            assert [fix["applicability"] for fix in error["data"]["fixes"]] == ["requiresReview"]
            discovered = action_for(error, 6, automatic=True)
            assert len(discovered) == 1 and discovered[0]["isPreferred"] is False, discovered
            actions = action_for(error, 2)
            assert len(actions) == 1 and actions[0]["isPreferred"] is False, actions
            edit = actions[0]["edit"]["documentChanges"][0]
            assert edit["textDocument"] == {"uri": uri, "version": 1}, edit
            prefix = misplaced.index("[[track_caller]]")
            assert edit["edits"] == [{"range": {
                "start": lsp_position(misplaced, prefix),
                "end": lsp_position(misplaced, keyword + 6)},
                "newText": "export [[track_caller]] /* 😀 */ [[discardable]]"}], edit
            moved = misplaced[:prefix] + edit["edits"][0]["newText"] + misplaced[keyword + 6:]
            assert moved == valid, moved
            assert change(moved, 2) == []
            assert not action_for(error, 3), "a stale move cannot edit the new snapshot"

            # Moving 'export' past a comment directly before it would discard
            # or reattach that comment, so the diagnostic offers no edit.
            commented = valid.replace("export [[track_caller]]", "[[track_caller]] /* 😀 */ export")
            withheld = change(commented, 3)
            assert len(withheld) == 1 and withheld[0]["code"] == "GTI-P0001", withheld
            assert not action_for(withheld[0], 4), "a comment before export withholds the edit"

            unspaced = valid.replace("export [[", "export[[")
            assert change(unspaced, 4) == []
            session.send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/formatting", "params": {
                "textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}}})
            formatted = session.receive_until(lambda message: message.get("id") == 5)["result"]
            assert formatted and (
                "\nexport [[track_caller]] /* 😀 */ [[discardable]] int check(bool ready) {"
                in formatted[0]["newText"]), formatted
        finally:
            session.close()


if __name__ == "__main__":
    main()
