"""Array admission and unsupported-owner diagnostics share compiler authority."""

from pathlib import Path
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def run_enum_array_lsp(server):
    source = """class Resource {
public:
  Resource() {}
  ~Resource() {}
};
enum Event<T> { Empty, Owned(T value) };
int main() {
  Event<Resource> values[2] = {Event<Resource>::Owned(Resource()), Event<Resource>::Empty};
  return 0;
}
"""
    invalid = source + """class Unsupported {
  Event<Resource> items[2] = {Event<Resource>::Empty, Event<Resource>::Empty};
public:
  virtual int marker() { return 0; }
};
"""
    with tempfile.TemporaryDirectory(prefix="gti-enum-array-lsp-") as directory:
        uri = (Path(directory) / "array.gti").as_uri()
        session = LspSession(server)
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {}}})
            session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            for version, text in enumerate((source, invalid, source), 1):
                if version == 1:
                    params = {"textDocument": {"uri": uri, "languageId": "gti",
                                              "version": version, "text": text}}
                    method = "textDocument/didOpen"
                else:
                    params = {"textDocument": {"uri": uri, "version": version},
                              "contentChanges": [{"text": text}]}
                    method = "textDocument/didChange"
                session.send({"jsonrpc": "2.0", "method": method, "params": params})
                published = session.receive_until(lambda item:
                    item.get("method") == "textDocument/publishDiagnostics"
                    and item["params"]["uri"] == uri
                    and item["params"].get("version") == version)["params"]
                diagnostics = published["diagnostics"]
                if version != 2:
                    assert diagnostics == [], published
                    continue
                assert len(diagnostics) == 1, published
                diagnostic = diagnostics[0]
                assert diagnostic["code"] == "GTI-S2067", diagnostic
                assert "Instance field 'items'" in diagnostic["message"], diagnostic
                start = invalid.index("items[2]")
                assert diagnostic["range"] == {
                    "start": lsp_position(invalid, start),
                    "end": lsp_position(invalid, start + len("items")),
                }, diagnostic
                assert "directly or in a fixed array" in diagnostic["message"], diagnostic
        finally:
            session.close()
    print("Owning enum array LSP admission, owner span and overlay recovery passed")
