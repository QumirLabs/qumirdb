#!/bin/bash -eu
# Files the crash that run_fuzzers saved as an issue with the labels bug and
# fuzz; an open fuzz issue pauses the nightly batch job. A finding with the
# same key as an open issue (see fuzz_issue.sh) gets a comment instead.
# Environment: GH_TOKEN, SANITIZER, RUN_URL and the GitHub Actions variables.

source "$(dirname "$0")/fuzz_issue.sh"

artifacts="$GITHUB_WORKSPACE/out/artifacts"
testcase=$(find "$artifacts" -type f -name '*-minimized' | head -n 1)
if [ -z "$testcase" ]; then
    testcase=$(find "$artifacts" -type f ! -name '*.summary' | head -n 1)
fi
if [ -z "$testcase" ]; then
    echo "run_fuzzers failed without saving a testcase: nothing to report"
    exit 0
fi
original=${testcase%-minimized}
# run_fuzzers saves out/artifacts/<target>/<sanitizer>/<testcase>.
target=$(basename "$(dirname "$(dirname "$original")")")
# shellcheck disable=SC2153  # SANITIZER comes from the workflow
fuzz_report "$target" "$SANITIZER" "$testcase" "$original.summary" \
    "ClusterFuzzLite batch fuzzing ($SANITIZER) of $GITHUB_SHA found this in $target."
