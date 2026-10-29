"""Explicit test-only public maps for exercising stdlib implementation helpers.

Production maps intentionally exclude these names. Tests register a disposable
interface instead of making implementation details part of the shipped API.
"""

import json
import shutil


def register_modules(library, modules):
    """Extend a disposable library's explicit map; never modify shipped metadata."""
    map_path = library / "module-map.json"
    mapping = json.loads(map_path.read_text(encoding="utf-8"))
    mapping["modules"].update(modules)
    map_path.write_text(json.dumps(mapping, indent=2) + "\n", encoding="utf-8")


def private_library(repository, destination, modules):
    if "detail_iterator" in modules:
        iterator_library(repository, destination)
    if not destination.exists():
        shutil.copytree(repository / "stdlib", destination)
    register_modules(destination, {
        name: "std/" + name.replace(".", "/") + ".gti" for name in modules
        if name != "detail_iterator"
    })
    return destination


def private_source(source, destination, modules):
    # Keep the original filename and line numbers for exact runtime failures.
    prefix = " ".join(f"import std.{name};" for name in modules)
    destination.write_text(prefix + " " + source.read_text(encoding="utf-8"),
                           encoding="utf-8")
    return destination


def iterator_library(repository, destination):
    if not destination.exists():
        shutil.copytree(repository / "stdlib", destination)
    facade = destination / "std/test_iterator.gti"
    facade.write_text('export module detail_iterator;\n'
                      'export import "detail/iterator.gti";\n', encoding="utf-8")
    map_path = destination / "module-map.json"
    mapping = json.loads(map_path.read_text(encoding="utf-8"))
    mapping["modules"]["detail_iterator"] = "std/test_iterator.gti"
    map_path.write_text(json.dumps(mapping, indent=2) + "\n", encoding="utf-8")
    return destination
