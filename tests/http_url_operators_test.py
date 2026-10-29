#!/usr/bin/env python3
"""URL path operators preserve lvalues and consume owned intermediates."""

from pathlib import Path
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main


def check_rejections():
    prefix = '''import std.http;
int main() {
  auto parsed = std::http::url::parse("https://example.com");
  auto base = std::move(parsed).value();
'''
    cases = {
        "use-after-move": '''
  auto endpoint = std::move(base) / "api";
  std::println(base.view());
''',
        "consume-borrowed": '''
  auto view = base.view();
  auto endpoint = std::move(base) / "api";
  std::println(view);
''',
        "read-only-mutation": '''
  base /= "api";
''',
        "component-view-owner-mutation": '''
  mut auto mutable_url = base.clone();
  auto host = mutable_url.host();
  mutable_url.push_path_segment("api");
  std::println(host);
''',
        "component-view-owner-consumption": '''
  auto scheme = base.scheme();
  auto moved = std::move(base);
  std::println(scheme);
''',
        "temporary-owner-view": '''
  auto dangling = std::http::url::parse("https://example.com")
      .expect("valid fixture URL").view();
  std::println(dangling);
''',
    }
    with tempfile.TemporaryDirectory(prefix="gti-url-operators-") as temporary:
        for name, body in cases.items():
            path = Path(temporary) / f"{name}.gti"
            path.write_text(prefix + body + "return 0;\n}\n")
            result = subprocess.run([str(Path(sys.argv[1]).resolve()), str(path), "--emit-mir",
                                     "-o", str(Path(temporary) / f"{name}.mir")],
                                    text=True, capture_output=True, timeout=60)
            assert result.returncode != 0 and "error[GTI-S" in result.stderr, (name, result)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)


if __name__ == "__main__":
    check_rejections()
    main(("http_url_operators",))
