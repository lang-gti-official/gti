#!/usr/bin/env python3
"""The shared frontend owns unsafe-call diagnostics and visible signatures."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unsafe_function_lsp_test.py <gti_lsp>")
    source = "// 😀 UTF-16 control\nunsafe int32_t raw() { return 1; }\nint main() { return raw(); }\n"
    with tempfile.TemporaryDirectory(prefix="gti-unsafe-lsp-") as directory:
        path = pathlib.Path(directory) / "unsafe.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {
                    "workspace": {"workspaceEdit": {"documentChanges": True}},
                    "textDocument": {
                        "publishDiagnostics": {"relatedInformation": True, "dataSupport": True,
                                               "tagSupport": {"valueSet": [1]}},
                        "codeAction": {"isPreferredSupport": True,
                                       "codeActionLiteralSupport": {"codeActionKind": {"valueSet": ["quickfix"]}}}
                    }}}})
            initialization = session.receive_until(lambda message: message.get("id") == 1)
            token_types = initialization["result"]["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            errors = diagnostics(1)
            error = next(d for d in errors if d["code"] == "GTI-S2055")
            closing = source.rindex("raw()") + 4
            assert error["range"] == {"start": lsp_position(source, closing),
                                      "end": lsp_position(source, closing + 1)}, error
            related = error["relatedInformation"][0]["location"]
            keyword = source.index("unsafe")
            assert related == {"uri": uri, "range": {
                "start": lsp_position(source, keyword),
                "end": lsp_position(source, keyword + 6)}}, error
            valid = source.replace("return raw();", "unsafe { return raw(); }")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid}]}})
            assert diagnostics(2) == []
            session.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(valid, valid.rindex("raw()") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 2)["result"]
            assert "unsafe" in hover["contents"]["value"], hover
            assert "[[unsafe]]" not in hover["contents"]["value"], hover
            session.send({"jsonrpc": "2.0", "id": 8, "method": "textDocument/semanticTokens/full", "params": {
                "textDocument": {"uri": uri}}})
            tokens = semantic_tokens_by_position(
                session.receive_until(lambda message: message.get("id") == 8)["result"]["data"])
            for offset in (valid.index("unsafe"), valid.rindex("unsafe")):
                position = lsp_position(valid, offset)
                token = tokens[(position["line"], position["character"])]
                assert token["length"] == 6 and token_types[token["type"]] == "keyword", token
            session.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/codeAction", "params": {
                "textDocument": {"uri": uri}, "range": error["range"], "context": {"diagnostics": [error]}}})
            actions = session.receive_until(lambda message: message.get("id") == 3)["result"]
            assert not actions, actions

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

            obsolete = valid.replace("unsafe int32_t", "[[unsafe]] int32_t")
            migration = next(d for d in change(obsolete, 3) if d["code"] == "GTI-P0001")
            assert [fix["applicability"] for fix in migration["data"]["fixes"]] == ["requiresReview"]
            discovered = action_for(migration, 30, automatic=True)
            assert len(discovered) == 1 and discovered[0]["isPreferred"] is False, discovered
            actions = action_for(migration, 4)
            assert len(actions) == 1 and actions[0]["isPreferred"] is False, actions
            edit = actions[0]["edit"]["documentChanges"][0]
            assert edit["textDocument"] == {"uri": uri, "version": 3}, edit
            offset = obsolete.index("[[unsafe]]")
            assert edit["edits"] == [{"range": {
                "start": lsp_position(obsolete, offset),
                "end": lsp_position(obsolete, offset + 10)}, "newText": "unsafe"}], edit
            assert change(valid, 4) == []
            assert not action_for(migration, 5), "stale migration cannot change the new snapshot"

            unused = "// 😀 UTF-16 control\nint main() { unsafe { return 0; } }\n"
            warning = next(d for d in change(unused, 5) if d["code"] == "GTI-S2100")
            offset = unused.index("unsafe")
            assert warning["severity"] == 2 and warning["tags"] == [1], warning
            assert warning["data"]["group"] == "unused-unsafe", warning
            assert warning["range"] == {"start": lsp_position(unused, offset),
                                        "end": lsp_position(unused, offset + 6)}, warning
            assert [fix["applicability"] for fix in warning["data"]["fixes"]] == ["requiresReview"]
            discovered = action_for(warning, 31, automatic=True)
            assert len(discovered) == 1 and discovered[0]["isPreferred"] is False, discovered
            actions = action_for(warning, 6)
            assert len(actions) == 1 and actions[0]["isPreferred"] is False, actions
            edit = actions[0]["edit"]["documentChanges"][0]
            assert edit["textDocument"] == {"uri": uri, "version": 5}, edit
            assert edit["edits"] == [{"range": warning["range"], "newText": ""}], edit
            assert change(unused.replace("unsafe", "", 1), 6) == []
            assert not action_for(warning, 7), "stale unused-unsafe cannot edit the new snapshot"

            adjacent = valid.replace("unsafe int32_t", "[[unsafe]]int32_t")
            migration = next(d for d in change(adjacent, 7) if d["code"] == "GTI-P0001")
            assert [fix["applicability"] for fix in migration["data"]["fixes"]] == ["requiresReview"]
            discovered = action_for(migration, 32, automatic=True)
            assert len(discovered) == 1 and discovered[0]["isPreferred"] is False, discovered
            actions = action_for(migration, 9)
            assert len(actions) == 1 and actions[0]["isPreferred"] is False, actions
            edit = actions[0]["edit"]["documentChanges"][0]
            assert edit["textDocument"] == {"uri": uri, "version": 7}, edit
            assert edit["edits"] == [{"range": migration["range"], "newText": "unsafe "}], edit
            assert change(valid, 8) == []
            assert not action_for(migration, 10), "stale unspaced migration cannot edit the new snapshot"

            members = (
                "// 😀 UTF-16 control\n"
                "concept readable<T> = requires(T& x) { unsafe { x.read() } -> int; };\n"
                "struct Meter { Meter() = default; unsafe int read() { return 7; } };\n"
                "int main() { Meter meter{}; return meter.read(); }\n"
            )
            member_error = next(d for d in change(members, 9) if d["code"] == "GTI-S2055")
            closing = members.rindex("read()") + 5
            assert member_error["range"] == {"start": lsp_position(members, closing),
                                              "end": lsp_position(members, closing + 1)}, member_error
            keyword = members.index("unsafe int")
            assert member_error["relatedInformation"][0]["location"] == {
                "uri": uri, "range": {"start": lsp_position(members, keyword),
                                         "end": lsp_position(members, keyword + 6)}}, member_error
            fixed_members = members.replace("return meter.read();", "unsafe { return meter.read(); }")
            assert change(fixed_members, 10) == []
            for request_id, offset, expected in (
                (11, fixed_members.rindex("read()") + 1, "unsafe int32_t Meter::read"),
                (12, fixed_members.index("readable") + 1, "unsafe { x.read() } -> int"),
            ):
                session.send({"jsonrpc": "2.0", "id": request_id, "method": "textDocument/hover", "params": {
                    "textDocument": {"uri": uri}, "position": lsp_position(fixed_members, offset)}})
                result = session.receive_until(lambda message: message.get("id") == request_id)["result"]
                assert expected in result["contents"]["value"], result
            broken_members = fixed_members.replace("unsafe int read()", "unsafe int read(")
            assert change(broken_members, 11)
            assert change(fixed_members, 12) == []
            constructor = (
                "// 😀 UTF-16 control\n"
                "struct Packet { int n; unsafe Packet(int v) : n(v) {} };\n"
                "int main() { Packet packet{1}; return packet.n; }\n"
            )
            error = next(d for d in change(constructor, 13) if d["code"] == "GTI-S2055")
            keyword = constructor.index("unsafe")
            assert error["relatedInformation"][0]["location"] == {
                "uri": uri, "range": {"start": lsp_position(constructor, keyword),
                                         "end": lsp_position(constructor, keyword + 6)}}, error
            fixed = constructor.replace("Packet packet{1}; return packet.n;",
                                        "unsafe { Packet packet{1}; return packet.n; }")
            assert change(fixed, 14) == []
            session.send({"jsonrpc": "2.0", "id": 15, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(fixed, fixed.index("Packet(int") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 15)["result"]
            assert "unsafe Packet(" in hover["contents"]["value"], hover
            assert change(fixed.replace("Packet(int v)", "Packet(int"), 15)
            assert change(fixed, 16) == []
            expression = constructor.replace("Packet packet{1}; return packet.n;",
                                             "auto packet = unsafe { Packet(1) }; return packet.n;")
            assert change(expression, 17) == []
            session.send({"jsonrpc": "2.0", "id": 16, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": uri}, "position": lsp_position(expression, expression.rindex("packet.n") + 1)}})
            hover = session.receive_until(lambda message: message.get("id") == 16)["result"]
            assert "Packet" in hover["contents"]["value"], hover
            session.send({"jsonrpc": "2.0", "id": 17, "method": "textDocument/semanticTokens/full", "params": {
                "textDocument": {"uri": uri}}})
            tokens = semantic_tokens_by_position(
                session.receive_until(lambda message: message.get("id") == 17)["result"]["data"])
            position = lsp_position(expression, expression.rindex("unsafe"))
            assert token_types[tokens[(position["line"], position["character"])]["type"]] == "keyword"
            assert change(expression.replace("Packet(1) }", "Packet( }"), 18)
            assert change(expression, 19) == []
            unused_expression = "int main() { /* 😀 */ return unsafe { 1 }; }"
            warning = next(d for d in change(unused_expression, 20) if d["code"] == "GTI-S2100")
            start = unused_expression.index("unsafe")
            assert warning["range"] == {"start": lsp_position(unused_expression, start),
                                        "end": lsp_position(unused_expression, start + 6)}, warning
            assert not action_for(warning, 18), "keyword-only removal would create invalid brace syntax"
            assert change(expression, 21) == []
        finally:
            session.close()


if __name__ == "__main__":
    main()
