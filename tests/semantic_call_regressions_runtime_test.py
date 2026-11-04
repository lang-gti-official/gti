"""Generic borrowing and constrained callable results preserve native behavior."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("semantic_call_regressions",), llvm_modes=True)
