"""Explicit test-only public maps for exercising stdlib implementation helpers.

Production maps intentionally exclude these names. Tests register a disposable
interface instead of making implementation details part of the shipped API.
"""

import json
import shutil


def iterator_library(repository, destination):
    if destination.exists():
        return destination
    shutil.copytree(repository / "stdlib", destination)
    facade = destination / "std/test_iterator.gti"
    facade.write_text('export module detail_iterator;\n'
                      'export import "detail/iterator.gti";\n', encoding="utf-8")
    map_path = destination / "module-map.json"
    mapping = json.loads(map_path.read_text(encoding="utf-8"))
    mapping["modules"]["detail_iterator"] = "std/test_iterator.gti"
    map_path.write_text(json.dumps(mapping, indent=2) + "\n", encoding="utf-8")
    return destination
