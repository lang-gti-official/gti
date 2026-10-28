#!/usr/bin/env python3
"""Exercise direct owning payload-enum fields and their rollback boundary."""

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


SUCCESS_SOURCE = """\
mut int32_t drops = 0;

class resource {
public:
  resource(int32_t identity) : identity(identity) {}
  resource(resource&& other) = default;
  ~resource() { drops = drops + 1; }
private:
  int32_t identity;
};

enum class holder { one(resource value), none };

class wrapper {
public:
  wrapper(holder value) : value(std::move(value)) {}
  void replace(holder replacement) mut {
    this.value = std::move(replacement);
  }
private:
  mut holder value;
};

struct defaulted_wrapper {
  defaulted_wrapper() = default;
  holder value = holder::one(resource(4));
};

wrapper relay(wrapper value) { return std::move(value); }

void pass_and_return() {
  wrapper first = wrapper(holder::one(resource(5)));
  wrapper second = relay(std::move(first));
}

int32_t return_early() {
  wrapper value = wrapper(holder::one(resource(7)));
  return 9;
}

void loop_edges() {
  mut int32_t index = 0;
  while (index < 2) {
    wrapper value = wrapper(holder::one(resource(8 + index)));
    index = index + 1;
    if (index == 1) {
      continue;
    }
    break;
  }
}

int main() {
  {
    wrapper first = wrapper(holder::one(resource(1)));
    wrapper second = std::move(first);
    mut wrapper destination = wrapper(holder::one(resource(2)));
    destination = std::move(second);
    destination.replace(holder::one(resource(3)));
    defaulted_wrapper defaulted = defaulted_wrapper();
  }
  if (drops != 4) {
    return 1;
  }
  pass_and_return();
  if (drops != 5 || return_early() != 9 || drops != 6) {
    return 2;
  }
  loop_edges();
  return drops == 8 ? 0 : 3;
}
"""


FAILURE_SOURCE = """\
class resource {
public:
  resource(int32_t identity) : identity(identity) {}
  resource(resource&& other) = default;
  ~resource() { std::println(this.identity); }
private:
  int32_t identity;
};

enum class holder { one(resource value), none };

class partial {
public:
  partial(int32_t divisor)
      : published(holder::one(resource(30))),
        checked(int32_t(1) / divisor) {}
private:
  holder published;
  int32_t checked;
};

int main() {
  partial value = partial(0);
  return 1;
}
"""


UNSUPPORTED_SOURCE = """\
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };

class generic_owner<T> { holder value; };

class polymorphic_owner {
private:
  holder value;
public:
  virtual int32_t marker() { return 0; }
};

int main() {
  holder values[2] = {holder::one(resource()), holder::none};
  return 0;
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: payload_enum_containment_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-payload-containment-") as temp:
        root = pathlib.Path(temp)
        success_source = root / "payload-containment.gti"
        failure_source = root / "payload-containment-failure.gti"
        unsupported_source = root / "payload-containment-unsupported.gti"
        success_source.write_text(SUCCESS_SOURCE, encoding="utf8")
        failure_source.write_text(FAILURE_SOURCE, encoding="utf8")
        unsupported_source.write_text(UNSUPPORTED_SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                success = root / f"success-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(success_source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(success),
                    ]
                )
                if built.returncode != 0:
                    return fail(built)
                executed = run([str(success)])
                if executed.returncode != 0 or executed.stdout:
                    sys.stderr.write(
                        "payload-enum field transport/replacement failed for "
                        f"{standard} {optimization}: exit={executed.returncode} "
                        f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
                    )
                    return 1

                failure = root / f"failure-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(failure_source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(failure),
                    ]
                )
                if built.returncode != 0:
                    return fail(built)
                executed = run([str(failure)])
                if executed.returncode != 70 or executed.stdout != "30\n":
                    sys.stderr.write(
                        "partial construction must drop the published payload "
                        f"exactly once for {standard} {optimization}: "
                        f"exit={executed.returncode} stdout={executed.stdout!r} "
                        f"stderr={executed.stderr!r}\n"
                    )
                    return 1

        emitted_success = root / "success.cpp"
        emission = run(
            [str(compiler), str(success_source), "--emit-cpp", "-o", str(emitted_success)]
        )
        if emission.returncode != 0:
            return fail(emission)
        generated = emitted_success.read_text(encoding="utf8")
        enum_definition = generated.find("struct holder {")
        wrapper_definition = generated.find("class wrapper {")
        required = (
            ", std::monostate> __gti_value;",
            "std::move(__gti_mir_p_",
        )
        if (
            enum_definition < 0
            or wrapper_definition < 0
            or enum_definition >= wrapper_definition
            or any(fragment not in generated for fragment in required)
        ):
            sys.stderr.write(
                "generated C++ lost the interleaved type schedule or active "
                "payload-enum representation\n"
            )
            return 1

        emitted_failure = root / "failure.cpp"
        emission = run(
            [str(compiler), str(failure_source), "--emit-cpp", "-o", str(emitted_failure)]
        )
        if emission.returncode != 0:
            return fail(emission)
        failure_cpp = emitted_failure.read_text(encoding="utf8")
        if (
            "mir_empty_state_v1<std::remove_cv_t<decltype(published)>>" not in failure_cpp
            or "GTI MIR in-place payload-enum field reparent" not in failure_cpp
        ):
            sys.stderr.write(
                "failure-form C++ lost the disengaged shell or exact field "
                "publication protocol\n"
            )
            return 1

        rejected = run(
            [str(compiler), str(unsupported_source), "-o", str(root / "rejected")]
        )
        rejected_text = rejected.stdout + rejected.stderr
        if (
            rejected.returncode == 0
            or "GTI-S2067" not in rejected_text
            or "Fixed array 'values'" not in rejected_text
            or rejected_text.count(
                "direct owning payload-enum field is supported only"
            )
            != 1
            or "GTI-B0001" in rejected_text
        ):
            sys.stderr.write(
                "unsupported containment shapes must retain precise semantic "
                "diagnostics without reaching an ICE\n"
            )
            return fail(rejected)

    print("payload enum field containment runtime behavior verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
