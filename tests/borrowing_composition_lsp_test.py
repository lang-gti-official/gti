#!/usr/bin/env python3
"""Composed owner diagnostics and field navigation use compiler snapshots."""

import os
import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
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
                "publishDiagnostics": {"relatedInformation": True,
                                       "dataSupport": True}}}})
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
            # A destructor would read the field as ownerless text, so this
            # struct keeps the ownerless field rule (L-OWN-02).
            moved_view = """#include <std/string>
struct Holder { std::string_view text; ~Holder() {} };
int main() {
  std::string owner{"owned"};
  std::string_view view = owner.view();
  /*😀*/ Holder held{.text = std::move(view)};
  return 0;
}
"""
            change(13, moved_view)
            errors = diagnostics(13)
            storage = next((d for d in errors if d.get("code") == "GTI-S2045"), None)
            assert storage is not None, errors
            assert "stored in a field" in storage["message"], storage
            assert storage["data"]["phase"] == "semantics", storage
            assert storage["data"]["hints"] and not storage["data"].get("fixes"), storage
            start = moved_view.index(")", moved_view.index("std::move(view"))
            assert storage["range"] == {
                "start": lsp_position(moved_view, start),
                "end": lsp_position(moved_view, start + 1)}, storage
            assert not any(d.get("code", "").startswith("GTI-B") for d in errors), errors
            local_view = moved_view.replace(
                "Holder held{.text = std::move(view)};",
                "std::string_view local = std::move(view);\n"
                "  if (local != owner.view()) { return 1; }")
            change(14, local_view)
            assert not diagnostics(14)
            change(15, local_view.replace("std::move(view)", "std::move("))
            assert diagnostics(15)
            change(16, local_view)
            assert not diagnostics(16)
            borrowed_payload = """#include <std/optional>
struct View {
  int& value;
  View(int& source) : value(source) {}
};
int main() {
  /*😀*/ std::optional<View> item{};
  return 0;
}
"""
            change(17, borrowed_payload)
            publication = session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("version") == 17
                and any(d.get("code") == "GTI-S2045"
                        for d in message["params"]["diagnostics"]))["params"]
            storage = next(d for d in publication["diagnostics"]
                           if d.get("code") == "GTI-S2045"
                           and "complete checked dependency contract" in d["message"])
            # Source-defined optional is a conditional stored-reference
            # carrier: construction needs a complete dependency contract for
            # every active borrowed field and alternative.
            assert "active borrowed field and alternative" in storage["message"], storage
            assert storage["severity"] == 1, storage
            assert storage["data"]["phase"] == "semantics", storage
            assert not storage["data"].get("fixes"), storage
            start = borrowed_payload.index("{", borrowed_payload.index("item{}"))
            request_location = {"uri": uri, "range": {
                "start": lsp_position(borrowed_payload, start),
                "end": lsp_position(borrowed_payload, start + 1)}}
            locations = [{"uri": publication["uri"], "range": storage["range"]}]
            locations += [related["location"]
                          for related in storage.get("relatedInformation", [])]
            assert request_location in locations, storage
            # The user-site UTF-16 location must survive and the rejected
            # generation must clear after selecting an admissible payload.
            rejected_uri = publication["uri"]
            change(18, borrowed_payload.replace("optional<View>", "optional<int>"))
            cleared = set()

            def cleared_storage(message):
                if message.get("method") != "textDocument/publishDiagnostics":
                    return False
                publication = message["params"]
                target = publication["uri"]
                if target == uri and publication.get("version") == 18:
                    assert not publication["diagnostics"], publication
                    cleared.add(uri)
                if target == rejected_uri and not publication["diagnostics"]:
                    cleared.add(rejected_uri)
                return {uri, rejected_uri} <= cleared

            session.receive_until(cleared_storage)
            concrete_errors = """class Broken<uint64_t N> {
  static_assert(N > 2, "class constraint");
public:
  Broken() {
    static_assert(N > 0, "first constructor constraint");
    static_assert(N > 1, "second constructor constraint");
  }
};
int main() {
  Broken<0> first = Broken<0>();
  Broken<1> second = Broken<1>();
  return 0;
}
"""
            change(19, concrete_errors)
            errors = diagnostics(19)
            # Publication coalesces identical primary errors, but must retain
            # all five concrete failures' substitution and requesting sites.
            assert len(errors) == 3, errors
            assert all(d.get("code") == "GTI-S2079" for d in errors), errors
            for value in (0, 1):
                for condition in ("N > 2", "N > 0", "N > 1"):
                    start = concrete_errors.index(condition)
                    expected_range = {
                        "start": lsp_position(concrete_errors, start),
                        "end": lsp_position(concrete_errors, start + len(condition))}
                    substitution = f"Concrete substitution: 'N' = '{value}'."
                    matching = [d for d in errors if d["range"] == expected_range
                                and any(r["message"] == substitution
                                        for r in d.get("relatedInformation", []))]
                    assert len(matching) == int(value == 0 or condition != "N > 0"), (
                        value, condition, errors)
                    if matching:
                        construction = f"Broken<{value}>()"
                        if condition == "N > 2":
                            name = "first" if value == 0 else "second"
                            start = concrete_errors.index(name + " =")
                            length = len(name)
                        else:
                            start = concrete_errors.index(construction)
                            length = len("Broken")
                        request_location = {"uri": uri, "range": {
                            "start": lsp_position(concrete_errors, start),
                            "end": lsp_position(concrete_errors, start + length)}}
                        assert request_location in [r["location"] for r in
                            matching[0]["relatedInformation"] if r["message"] ==
                            "Concrete generic instance requested here."], matching
            change(20, concrete_errors.replace("Broken<0>", "Broken<3>").replace(
                "Broken<1>", "Broken<4>"))
            assert not diagnostics(20)
            carrier_access = """struct Counter { mut int value = 7; };
class Shared {
  Counter& counter;
public:
  Shared(Counter& source) : counter(source) {}
  int read() { return this.counter.value; }
};
class Client {
  Shared stored;
public:
  Client(Shared source) : stored(source) {}
  Shared& auth() { return this.stored; }
};
int main() {
  Counter owner{};
  Client client{Shared(owner)};
  Shared& reference = client.auth();
  return reference.read() - 7;
}
"""
            change(21, carrier_access)
            errors = diagnostics(21)
            assert not errors, errors
            use = carrier_access.index("auth();")
            position = {"textDocument": {"uri": uri},
                        "position": lsp_position(carrier_access, use)}
            assert "Shared&" in str(request("textDocument/hover", position))
            definition = request("textDocument/definition", position)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri, definition
            assert location["range"]["start"] == lsp_position(
                carrier_access, carrier_access.index("auth() {")), definition
            escaped = carrier_access.replace(
                "return this.stored;",
                "Shared local = this.stored; /*😀*/ return local;")
            change(22, escaped)
            errors = diagnostics(22)
            escape = next((d for d in errors if d.get("code") == "GTI-S2017"), None)
            assert escape is not None, errors
            assert "not a local or by-value object" in escape["message"], escape
            start = escaped.index("local;", escaped.index("/*😀*/"))
            assert escape["range"] == {
                "start": lsp_position(escaped, start),
                "end": lsp_position(escaped, start + 5)}, escape
            assert escape["data"]["phase"] == "semantics", escape
            assert escape["data"]["hints"] and not escape["data"].get("fixes"), escape
            assert not any(d.get("code", "").startswith("GTI-B") for d in errors), errors
            change(23, carrier_access.replace("client.auth()", "client."))
            assert diagnostics(23)
            change(24, carrier_access)
            assert not diagnostics(24)
            mixed = """struct Services {
  int& input;
  mut int& output;
  int& offset;
  mut int& total;
  Services(int& a, mut int& b, int& c, mut int& d)
      : input(a), output(b), offset(c), total(d) {}
  mut int& write() mut { return this.output; }
};
int main() {
  int a = 40; mut int b = 0; int c = 2; mut int d = 0;
  mut Services services{a, b, c, d};
  mut int& selected = services.write();
  // edit here
  selected = 42;
  return b - 42;
}
"""
            change(25, mixed)
            errors = diagnostics(25)
            assert not errors, errors
            position = {"textDocument": {"uri": uri},
                        "position": lsp_position(mixed, mixed.index("write();"))}
            hover = str(request("textDocument/hover", position))
            assert "mut int32_t&" in hover and "write" in hover, hover
            definition = request("textDocument/definition", position)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"] == lsp_position(
                mixed, mixed.index("write() mut")), definition
            conflict_source = mixed.replace("// edit here", "/*😀*/ b = 9;")
            change(26, conflict_source)
            errors = diagnostics(26)
            conflict = next((d for d in errors if d.get("code") == "GTI-S2017"), None)
            assert conflict is not None, errors
            start = conflict_source.index("b = 9")
            assert conflict["range"] == {
                "start": lsp_position(conflict_source, start),
                "end": lsp_position(conflict_source, start + 1)}, conflict
            assert conflict.get("relatedInformation"), conflict
            assert not conflict["data"].get("fixes"), conflict
            change(27, mixed)
            assert not diagnostics(27)
            change(28, mixed.replace("services.write()", "services."))
            assert diagnostics(28)
            change(29, mixed)
            assert not diagnostics(29)
        finally:
            session.close()


if __name__ == "__main__":
    main()
