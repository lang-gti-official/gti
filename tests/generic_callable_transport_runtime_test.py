"""Exact copy/move lifecycle and lambda return publication composition."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    sources = {
        "relay_copyable": root / "coverage/reproducers/a16-relay-copyable-lambda.gti",
        "vector_callable": root / "coverage/reproducers/a32-vector-push-back-move-only-function.gti",
        "optional_callable": root / "coverage/reproducers/a32-optional-emplace-move-only-function.gti",
        "generic_callable_transport": root / "fixtures/generic_callable_transport.gti",
    }
    main(names=tuple(sources), fixture_paths=sources)
