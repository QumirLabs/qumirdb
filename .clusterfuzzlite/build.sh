#!/bin/bash -eu
# ClusterFuzzLite build of the fuzz/ targets. CFLAGS and CXXFLAGS carry the
# sanitizer of the job and the coverage instrumentation; asserts stay enabled.

# Arrow and LLVM come from packages built against libstdc++.
export CXXFLAGS="${CXXFLAGS//-stdlib=libc++/}"

build="$WORK/build"
cmake -S . -B "$build" -G Ninja \
    -DLLVM_DIR="$(llvm-config-22 --cmakedir)" \
    -DQDB_LLVM_LINK_SHARED=ON \
    -DQDB_USE_CCACHE=OFF \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DQDB_FUZZ=ON \
    -DQDB_FUZZ_SANITIZERS=
cmake --build "$build"

# The fuzzers find the .oz modules in modules/ beside them.
cmake --install "$build" --component engine --prefix "$WORK/install"
rm -rf "$OUT/modules"
cp -r "$WORK"/install/lib/qumirdb-*/modules "$OUT/modules"

targets="qdb_fuzz_parse qdb_fuzz_plan qdb_fuzz_vm"
rm -f "$WORK/seed_corpus.zip"
zip -q -j "$WORK/seed_corpus.zip" "$build"/fuzz/corpus/*
for target in $targets; do
    cp "$build/fuzz/$target" "$OUT/"
    cp "$WORK/seed_corpus.zip" "$OUT/${target}_seed_corpus.zip"
    cp fuzz/sql.dict "$OUT/$target.dict"
done

# The runner image has none of the shared libraries: ship them in lib/ beside
# the fuzzers, except the C library of the image itself.
mkdir -p "$OUT/lib"
for target in $targets; do
    ldd "$OUT/$target" | awk '$2 == "=>" && $3 ~ /^\// { print $3 }'
done | sort -u | while read -r library; do
    case "$(basename "$library")" in
        libc.so.*|libm.so.*|libdl.so.*|librt.so.*|libpthread.so.*|ld-linux*) ;;
        *) cp -L "$library" "$OUT/lib/" ;;
    esac
done
for target in $targets; do
    patchelf --set-rpath '$ORIGIN/lib' "$OUT/$target"
done
for library in "$OUT"/lib/*; do
    patchelf --set-rpath '$ORIGIN' "$library"
done
