#!/usr/bin/env python3
"""A diagnostic inside a macro body is also marked at the offending line.

The compiler reports a macro-body diagnostic at the invocation, where its
arguments come from, with the body location as related information. Editors
that do not draw related information would mark nothing at the body, so the
LSP publishes an informational companion there (issue #265).
"""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, initialize_session, open_document

SOURCE = """import std.source_location;
import std.string;
#macro check_value(int value, std::source_location loc) {
  std::string hidden = loc.file;
}
int main() {
  check_value!(1, std::source_location::current());
  return 0;
}
"""


def main():
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="gti-lsp-macro-body-") as directory:
        path = pathlib.Path(directory) / "macro_body.gti"
        path.write_text(SOURCE, encoding="utf-8")
        uri = path.resolve().as_uri()
        session = LspSession(executable)
        try:
            initialize_session(session, {"textDocument": {"publishDiagnostics": {
                "relatedInformation": True}}})
            publication = open_document(session, uri, SOURCE, expect_clean=False)
            diagnostics = publication["params"]["diagnostics"]
            body_line = SOURCE.splitlines().index("  std::string hidden = loc.file;")
            call_line = next(index for index, line in enumerate(SOURCE.splitlines())
                             if "check_value!(" in line)
            errors = [item for item in diagnostics if item.get("severity", 1) == 1]
            assert any(item["range"]["start"]["line"] == call_line for item in errors), diagnostics
            # One informational companion per error raised in the body.
            companions = [item for item in diagnostics
                          if item.get("severity") == 3 and
                          item["message"].startswith("A macro expansion fails here:")]
            assert len(companions) == len(errors), diagnostics
            private = [item for item in companions if item.get("code") == "GTI-S2007"]
            assert len(private) == 1, diagnostics
            companion = private[0]
            assert companion["range"]["start"]["line"] == body_line, companion
            back = companion["relatedInformation"][0]["location"]
            assert back["uri"] == uri and back["range"]["start"]["line"] == call_line, companion
        finally:
            session.close()
    print("lsp macro body companion diagnostic passed")


if __name__ == "__main__":
    main()
