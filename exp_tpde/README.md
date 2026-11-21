# TPDE kernel experiment (not for merge)

Scripts of the `--kernel:backend=tpde` experiment on AArch64 Linux.

- `run_suite.py`: runs the `RegQuery` cases of `test_reg` in parallel chunks and
  restarts a chunk after a crash or a stall, recording the running case.
- `compare.py`, `coverage.py`: classify a TPDE run against an LLVM run.
- `bench.py`: medians of `qdb --timing` kernel JIT times (llvm, llvm with a warm
  JIT cache, tpde) with the `QUMIR_KERNEL_TIMING` stage breakdown.
- `fixtures_to_parquet.py`: regtest JSON fixtures to Parquet through the duckdb CLI.
- `bool_and_const.oz`: `qumiri --core --tpde -i bool_and_const.oz` trips the TPDE
  assertion "found non-freed ValueAssignment" before Qumir 9a3b2a9.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQDB_BUILD_TESTS=ON \
  -DQDB_LLVM_LINK_SHARED=ON -DQUMIR_ENABLE_TPDE=ON -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build --target qdb test_reg qumiri -j3
python3 exp_tpde/run_suite.py --bin build/test/test_reg --root test/regtest --backend llvm --out runs/llvm
python3 exp_tpde/run_suite.py --bin build/test/test_reg --root test/regtest --backend tpde --out runs/tpde
python3 exp_tpde/compare.py runs/llvm/results.json runs/tpde/results.json
python3 exp_tpde/coverage.py runs/llvm/results.json runs/tpde/results.json
```
