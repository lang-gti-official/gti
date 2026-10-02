#!/usr/bin/env python3
"""Composed owner diagnostics and field navigation use compiler snapshots."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession


def main():
    source = """struct View {
  int& first;
  int& second;
  View(int& a, int& b) : first(a), second(b) {}
};
int main() {
  mut int left = 20;
  mut int right = 22;
  View view{left, right};
  // edit here
  return view.first + view.second - 42;
}
"""
    with tempfile.TemporaryDirectory(prefix="gti-composed-borrow-lsp-") as root:
        path = pathlib.Path(root) / "main.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            published = session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]
            return [diagnostic for diagnostic in published if diagnostic.get("severity") == 1]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": source}}})
            initial = diagnostics(1)
            assert not initial, initial
            position = {"textDocument": {"uri": uri},
                        "position": {"line": 10, "character": source.splitlines()[10].index("second")}}
            assert "int32_t" in str(request("textDocument/hover", position))
            definition = request("textDocument/definition", position)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"] == {
                "line": 2, "character": 7}, definition
            for version, owner in ((2, "left"), (4, "right")):
                line = f"  /*😀*/ {owner} = 7;"
                change(version, source.replace("  // edit here", line))
                errors = diagnostics(version)
                conflict = next((d for d in errors if d.get("code") == "GTI-S2017"), None)
                assert conflict is not None, errors
                column = len(line[:line.index(owner)].encode("utf-16-le")) // 2
                assert conflict["range"]["start"] == {"line": 9, "character": column}, conflict
                assert conflict.get("relatedInformation"), conflict
                change(version + 1, source)
                assert not diagnostics(version + 1)
            change(6, source.replace("view.second", "view."))
            assert diagnostics(6)
            change(7, source)
            assert not diagnostics(7)
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": uri}}})
            session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and not message["params"]["diagnostics"])
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 8, "text": source}}})
            assert not diagnostics(8)
            ordered = """class Box {
  mut int value = 1;
public:
  int& read() { return this.value; }
  int bump() mut { this.value += 1; return this.value; }
};
int inspect(int& borrowed, int changed) { return borrowed + changed; }
int main() {
  mut Box box{};
  /*😀*/ return inspect(box.read(), box.bump());
}
"""
            change(9, ordered)
            errors = diagnostics(9)
            conflict = next((d for d in errors if d.get("code") == "GTI-S2017"), None)
            assert conflict is not None, errors
            operand_line = ordered.splitlines()[9]
            def column(token):
                return len(operand_line[:operand_line.index(token)].encode("utf-16-le")) // 2
            assert conflict["range"] == {
                "start": {"line": 9, "character": column("bump")},
                "end": {"line": 9, "character": column("bump") + 4}}, conflict
            related = conflict.get("relatedInformation", [])
            assert len(related) == 1 and related[0]["location"]["uri"] == uri, conflict
            assert related[0]["location"]["range"]["start"] == {
                "line": 9, "character": column("read")}, conflict
            assert "Complete the mutation" in conflict["message"], conflict
            safe_order = ordered.replace("int& borrowed, int changed",
                                         "int changed, int& borrowed").replace(
                "box.read(), box.bump()", "box.bump(), box.read()")
            change(10, safe_order)
            assert not diagnostics(10)
            change(11, safe_order.replace("box.read()", "box."))
            assert diagnostics(11)
            change(12, safe_order)
            assert not diagnostics(12)
        finally:
            session.close()


if __name__ == "__main__":
    main()
