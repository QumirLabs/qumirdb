#!/bin/bash -eu
# Files the crash that run_fuzzers saved as an issue with the labels bug and
# fuzz; an open fuzz issue pauses the nightly batch job. A finding that is
# already open under the same title gets a comment instead.
# Environment: GH_TOKEN, SANITIZER, RUN_URL and the GitHub Actions variables.

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
summary=$(grep -m 1 '^SUMMARY:' "$original.summary" || echo "SUMMARY: $(basename "$original")")
# Abort-type summaries are generic ("deadly signal", timeout, OOM), so the title
# also carries the exception text or the first frame of the crashing thread that
# is not libFuzzer, sanitizer or libstdc++ code.
detail=
case $summary in
*libFuzzer:*)
    detail=$(sed -n -E 's/^ *what\(\): +//p' "$original.summary" | head -n 1)
    ;;
esac
if [ -z "$detail" ] && [[ $summary == *libFuzzer:* ]]; then
    detail=$(grep -E '^ *#[0-9]+ ' "$original.summary" \
        | grep -v -E 'libFuzzer|fuzzer::|__sanitizer|__asan|__ubsan|__interceptor|/usr/(lib|include)|libc\.so|std::|__gnu_cxx|LLVMFuzzerTestOneInput' \
        | head -n 1 | sed -E 's/^ *#[0-9]+ +(0x[0-9a-f]+ +)?(in +)?//; s/ +\(?[^ ]*(:[0-9]+)+\)?$//' || true)
fi
title="Fuzzing: $target: ${summary#SUMMARY: }${detail:+ [$detail]}"
title=${title:0:200}

body="$RUNNER_TEMP/finding.md"
{
    echo "ClusterFuzzLite batch fuzzing ($SANITIZER) of $GITHUB_SHA found this in $target."
    echo
    echo "Run and artifacts: $RUN_URL"
    echo
    if iconv -f UTF-8 -t UTF-8 "$testcase" > /dev/null 2>&1 && tr -d '\000' < "$testcase" | cmp -s - "$testcase"; then
        echo "Reproducer:"
        echo
        echo '`````sql'
        head -c 8000 "$testcase"
        echo
        echo '`````'
        echo
    fi
    echo "Reproducer in base64 (\`base64 -d > testcase\`, then run the target on the file):"
    echo
    echo '```'
    head -c 8000 "$testcase" | base64 -w 76
    echo '```'
    echo
    echo "Stack:"
    echo
    echo '`````'
    head -c 40000 "$original.summary"
    echo
    echo '`````'
    echo
    echo "The fuzz label pauses the nightly fuzzing until this issue is closed."
} > "$body"

existing=$(TITLE="$title" gh issue list -R "$GITHUB_REPOSITORY" --label fuzz --state open \
    --json number,title --jq '.[] | select(.title == $ENV.TITLE) | .number' | head -n 1)
if [ -n "$existing" ]; then
    gh issue comment "$existing" -R "$GITHUB_REPOSITORY" --body-file "$body"
else
    gh issue create -R "$GITHUB_REPOSITORY" --label bug --label fuzz --title "$title" --body-file "$body"
fi
