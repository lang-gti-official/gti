#!/usr/bin/env python3
"""Module queries retain exact UTF-16 ranges and recover across missing imports."""

import json
import pathlib
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document, write
from lsp_smoke_test import LspSession, lsp_position, semantic_tokens_by_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-module-navigation-") as temporary:
        root = pathlib.Path(temporary)
        app, library = root / "app", root / "library"
        entry, api = app / "main.gti", library / "api.gti"
        interface = "/* 😀 */ export module net /* trivia */ . http;\nexport int answer() { return 42; }\n"
        source = "/* 😀 */ import dep.net.http;\nimport dep.missing;\nint main() { return answer(); }\n"
        write(entry, source)
        write(api, interface)
        write(app / "gti.toml", 'manifest-version = 1\n[package]\nname = "app"\n'
              'version = "0.1.0"\nsource-root = "."\n[dependencies]\n'
              'dep = { path = "../library" }\n[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
        manifest = library / "gti.toml"
        write(manifest, 'manifest-version = 1\n[package]\nname = "library"\n'
              'version = "0.1.0"\nsource-root = "."\n[modules]\n"net.http" = "api.gti"\n')
        session = LspSession(sys.argv[1])
        request_id = 40

        def query(method, text, offset):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                          "params": {"textDocument": {"uri": uri},
                                     "position": lsp_position(text, offset)}})
            reply = session.receive_until(lambda value: value.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})
            return diagnostics_for(session, uri, version)

        try:
            initialize(session, watched_files=True)
            uri = open_document(session, entry, source)
            errors = diagnostics_for(session, uri, 1)
            assert any(item.get("code") == "GTI-I0012" for item in errors), errors
            definition = query("textDocument/definition", source, source.index("net.http") + 2)
            assert definition["uri"] == api.resolve().as_uri(), definition
            assert definition["range"] == {
                "start": lsp_position(interface, interface.index("net")),
                "end": lsp_position(interface, interface.index("http") + 4)}, definition
            assert "net.http" in json.dumps(query("textDocument/hover", source, source.index("dep.net")))
            # A loader failure must retain independently available semantic queries.
            answer = query("textDocument/definition", source, source.index("answer") + 1)
            assert answer["uri"] == api.resolve().as_uri(), answer
            assert not query("textDocument/definition", source, source.index("dep.missing") + 5)
            tokens = semantic_tokens_by_position(query("textDocument/semanticTokens/full", source, 0)["data"])
            for spelling in ("dep.net", "net.http", "http;", "dep.missing", "missing;"):
                position = lsp_position(source, source.index(spelling))
                token = tokens[(position["line"], position["character"])]
                assert token["type"] == 3 and not token["modifiers"] & 8, token
            header_uri = open_document(session, api, interface)
            assert not diagnostics_for(session, header_uri, 1)
            session.send({"jsonrpc": "2.0", "id": 35, "method": "textDocument/semanticTokens/full",
                          "params": {"textDocument": {"uri": header_uri}}})
            header = session.receive_until(lambda value: value.get("id") == 35)["result"]["data"]
            header_tokens = semantic_tokens_by_position(header)
            for name in ("net", "http"):
                position = lsp_position(interface, interface.index(name))
                token = header_tokens[(position["line"], position["character"])]
                assert token["type"] == 3 and token["modifiers"] & 1, token
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": header_uri}}})

            incomplete = "/* 😀 */ import dep /* trivia */ . ne"
            assert change(2, incomplete)
            completion = query("textDocument/completion", incomplete, len(incomplete))
            assert "dep.net.http" in {item["label"] for item in completion["items"]}, completion

            replacement = library / "next.gti"
            write(replacement, "export module net.next;\nexport int next() { return 0; }\n")
            write(manifest, manifest.read_text().replace('"net.http" = "api.gti"',
                                                         '"net.next" = "next.gti"'))
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": manifest.resolve().as_uri(), "type": 2}]}})
            diagnostics_for(session, uri, 2)
            completion = query("textDocument/completion", incomplete, len(incomplete))
            labels = {item["label"] for item in completion["items"]}
            assert {"dep.net.next", "dep.net.http"} <= labels, labels
            # Removing an explicit selection leaves a saved interface discoverable.
            # Exclusion is the deliberate way to remove it from the package map.
            write(manifest, manifest.read_text() +
                  '\n[module-discovery]\nexclude = ["api.gti"]\n')
            session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                          "params": {"changes": [{"uri": manifest.resolve().as_uri(), "type": 2}]}})
            diagnostics_for(session, uri, 2)
            completion = query("textDocument/completion", incomplete, len(incomplete))
            labels = {item["label"] for item in completion["items"]}
            assert "dep.net.next" in labels and "dep.net.http" not in labels, labels
            valid = "import dep.net.next;\nint main() { return next(); }\n"
            assert change(3, valid) == []
            restored = query("textDocument/definition", valid, valid.index("dep.net"))
            assert restored["uri"] == replacement.resolve().as_uri(), restored

            # A nonexistent same-owner import is repaired by an unsaved file,
            # and closing that overlay restores the error without stale queries.
            missing_path = app / "missing.gti"
            missing_source = valid.replace("int main()", 'import "missing.gti";\nint main()')
            assert change(4, missing_source)
            missing_uri = open_document(session, missing_path,
                                        "export module missing;\nexport int available() { return 1; }\n")
            publications = {}
            versions = {uri: 4, missing_uri: 1}

            def collect(message):
                if message.get("method") == "textDocument/publishDiagnostics":
                    params = message["params"]
                    target = params.get("uri")
                    if target in versions and params.get("version") == versions[target]:
                        publications[target] = params["diagnostics"]
                return len(publications) == len(versions)

            session.receive_until(collect)
            assert publications[uri] == [] and publications[missing_uri] == [], publications
            opened = query("textDocument/definition", missing_source, missing_source.index("missing.gti"))
            assert opened["uri"] == missing_uri, opened
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": missing_uri}}})
            assert diagnostics_for(session, uri, 4)
            assert not query("textDocument/definition", missing_source, missing_source.index("missing.gti"))
            independent = query("textDocument/definition", missing_source, missing_source.index("dep.net"))
            assert independent["uri"] == replacement.resolve().as_uri(), independent
            assert not (app / "build").exists()
            assert not (app / "gti.lock").exists()
        finally:
            session.close()


def symlink_refresh():
    for directory_link, named in ((False, False), (False, True), (True, False), (True, True)):
        with tempfile.TemporaryDirectory(prefix="gti-module-symlink-") as temporary:
            root = pathlib.Path(temporary)
            app = root / "app"
            app.mkdir()
            entry = app / "main.gti"
            link_node = app / ("alias" if directory_link else "interface.gti")
            link = link_node / "interface.gti" if directory_link else link_node
            first = app / "first" / "interface.gti" if directory_link else app / "first.gti"
            second = app / "second" / "interface.gti" if directory_link else app / "second.gti"

            def retarget(target):
                if link_node.exists() or link_node.is_symlink():
                    link_node.unlink()
                link_node.symlink_to(target.parent if directory_link else target,
                                     target_is_directory=directory_link)
            interface = "export module api;\nexport int answer() { return 0; }\n"
            write(first, interface)
            write(second, interface.replace("return 0", "return 1"))
            outside = root / "outside" / "interface.gti" if directory_link else root / "outside.gti"
            write(outside, "export module api;\nOUTSIDE_OWNER_SENTINEL forbidden;\n")
            manifest = ('manifest-version = 1\n[package]\nname = "symlink"\n'
                        'version = "0.1.0"\nsource-root = "."\n'
                        '[module-discovery]\nroots = []\n')
            # These authored-path tests deliberately keep both alternative
            # interfaces and aliases on disk; discovery has separate coverage.
            if named:
                manifest += f'[modules]\napi = "{link.relative_to(app).as_posix()}"\n'
                retarget(first)
            manifest += '[targets.app]\nkind = "executable"\nroot = "main.gti"\n'
            write(app / "gti.toml", manifest)
            spelling = "api" if named else f'"{link.relative_to(app).as_posix()}"'
            source = f"import {spelling};\nint main() {{ return answer(); }}\n"
            write(entry, source)
            session = LspSession(sys.argv[1])
            request_id = 100
            try:
                initialize(session, watched_files=True)
                uri = open_document(session, entry, source)
                initial = diagnostics_for(session, uri, 1)
                assert not initial if named else any(
                    item.get("code") == "GTI-I0012" for item in initial), initial

                def definition():
                    nonlocal request_id
                    request_id += 1
                    session.send({"jsonrpc": "2.0", "id": request_id,
                                  "method": "textDocument/definition", "params": {
                        "textDocument": {"uri": uri},
                        "position": lsp_position(source, source.index(spelling) + (0 if named else 1))}})
                    reply = session.receive_until(lambda item: item.get("id") == request_id)
                    assert "error" not in reply, reply
                    return reply.get("result")

                def changed(kind, target=None, expected_interface=interface):
                    # The URI must retain the authored link spelling; resolving
                    # it here would conceal exactly the topology being tested.
                    session.send({"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles",
                                  "params": {"changes": [{"uri": link.as_uri(), "type": kind}]}})
                    errors = diagnostics_for(session, uri, 1)
                    result = definition()
                    if target is None:
                        assert errors and not result, (directory_link, named, errors, result)
                        assert any(item.get("code") in {"GTI-I0012", "GTI-B1103", "GTI-B1104"} for item in errors), errors
                        assert "OUTSIDE_OWNER_SENTINEL" not in json.dumps(errors), errors
                    else:
                        assert not errors, (named, errors)
                        assert result["uri"] == target.resolve().as_uri(), result
                        assert result["range"] == {
                            "start": lsp_position(expected_interface, expected_interface.index("api")),
                            "end": lsp_position(expected_interface, expected_interface.index("api") + 3)}, result

                if not named:
                    retarget(first)
                    changed(1, first)
                overlay = "// dirty canonical target\n" + interface
                second_uri = open_document(session, second, overlay)
                assert not diagnostics_for(session, second_uri, 1)
                link_node.unlink()
                retarget(second)
                changed(2, second, overlay)
                session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                              "params": {"textDocument": {"uri": second_uri}}})
                assert not diagnostics_for(session, uri, 1)
                link_node.unlink()
                changed(3)
                retarget(first)
                changed(1, first)
                for index in range(4):
                    invalid = app / f"invalid-{index}" / "interface.gti" if directory_link else app / f"invalid-{index}.gti"
                    write(invalid, f"int hidden_{index}() {{ return 0; }}\n")
                    link_node.unlink()
                    retarget(invalid)
                    changed(2)
                link_node.unlink()
                retarget(outside)
                changed(2)
                link_node.unlink()
                retarget(second)
                changed(2, second)
                assert not (app / "build").exists()
                assert not (app / "gti.lock").exists()
            finally:
                session.close()


if __name__ == "__main__":
    main()
    symlink_refresh()
