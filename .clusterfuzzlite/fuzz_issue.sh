# shellcheck shell=bash
# Sourced by report_finding.sh and report_prune.sh: the dedup key, the issue
# marker and the "create or comment" step that batch and prune share, so a
# crash found by either is reported once.
# Environment: GH_TOKEN, GITHUB_REPOSITORY; DRY_RUN=1 prints instead of writing.

# fuzz_detail <summary file>: the exception text or the first frame of the
# crashing thread that is not runtime code. Only libFuzzer summaries need it:
# they are generic ("deadly signal", timeout, OOM).
fuzz_detail() {
    local detail
    detail=$(sed -n -E 's/^ *what\(\): +//p' "$1" | head -n 1)
    if [ -z "$detail" ]; then
        detail=$(grep -E '^ *#[0-9]+ ' "$1" \
            | grep -v -E 'libFuzzer|fuzzer::|__sanitizer|__asan|__ubsan|__interceptor|/usr/(lib|include)|libc\.so|std::|__gnu_cxx|LLVMFuzzerTestOneInput' \
            | head -n 1 | sed -E 's/^ *#[0-9]+ +(0x[0-9a-f]+ +)?(in +)?//; s/ +\(?[^ ]*(:[0-9]+)+\)?$//' || true)
    fi
    echo "$detail"
}

# fuzz_summary <summary file> <fallback>: the SUMMARY line.
fuzz_summary() {
    grep -m 1 '^SUMMARY:' "$1" || echo "SUMMARY: $2"
}

# fuzz_key <target> <sanitizer> <summary file>: target|sanitizer|type|location.
# The location is the first file:line of SUMMARY with the column dropped; for
# libFuzzer summaries it is fuzz_detail. A stack overflow has no location: the
# frame where the stack ran out depends on the input, not on the bug.
fuzz_key() {
    local rest type loc=
    rest=$(fuzz_summary "$3" unknown)
    rest=${rest#SUMMARY: }
    rest=${rest#*: }
    type=${rest%% in *}
    if [[ $type == "deadly signal" || $type == timeout* || $type == out-of-memory* ]]; then
        loc=$(fuzz_detail "$3")
    elif [[ $type == *" /"* ]]; then
        loc=$(echo "${type#* }" | sed -E 's#^/src/[^/]*/##; s/(:[0-9]+):[0-9]+$/\1/')
        type=${type%% *}
    fi
    if [ "$type" = stack-overflow ]; then
        loc=
    fi
    echo "$1|$2|$type|$loc"
}

# fuzz_stack <summary file>: the crash report from its first sanitizer or
# libFuzzer marker, so the start-up log does not use up the size limit. Without
# a marker the whole file is used.
fuzz_stack() {
    local line
    line=$(grep -n -m 1 -E '^==([0-9]+==)? ?(ERROR|WARNING):|runtime error:' "$1" | cut -d: -f1 || true)
    tail -n "+${line:-1}" "$1" | head -c 30000
}

fuzz_marker() {
    echo "<!-- fuzz-key: $1 -->"
}

# fuzz_write <gh args...>: gh, or a printout in dry-run mode.
fuzz_write() {
    if [ -n "${DRY_RUN:-}" ]; then
        echo "DRY-RUN: gh $*"
    else
        gh "$@"
    fi
}

# fuzz_find <state> <key> <title>: number of the first fuzz issue in that state
# with the key marker in the body or the same title.
fuzz_find() {
    # shellcheck disable=SC2016  # $ENV is jq, not shell
    MARKER=$(fuzz_marker "$2") TITLE=$3 gh issue list -R "$GITHUB_REPOSITORY" --label fuzz --state "$1" \
        --limit 200 --json number,title,body \
        --jq '.[] | select((.body | contains($ENV.MARKER)) or .title == $ENV.TITLE) | .number' | head -n 1
}

# fuzz_report <target> <sanitizer> <testcase or empty> <summary file> <headline>
# Files an issue with the labels bug and fuzz, or comments on the open one with
# the same key. A closed issue is never reopened: a crash that comes back after
# a fix gets a new issue that links the closed one.
fuzz_report() {
    local target=$1 sanitizer=$2 testcase=$3 summaryfile=$4 headline=$5
    local summary detail='' title key body existing closed
    summary=$(fuzz_summary "$summaryfile" "$(basename "${testcase:-crash}")")
    if [[ $summary == *libFuzzer:* ]]; then
        detail=$(fuzz_detail "$summaryfile")
    fi
    title="Fuzzing: $target: ${summary#SUMMARY: }${detail:+ [$detail]}"
    title=${title:0:200}
    key=$(fuzz_key "$target" "$sanitizer" "$summaryfile")

    existing=$(fuzz_find open "$key" "$title")
    closed=
    if [ -z "$existing" ]; then
        closed=$(fuzz_find closed "$key" "$title")
    fi

    body="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/finding.md"
    {
        echo "$headline"
        echo
        echo "Run and artifacts: $RUN_URL"
        echo
        if [ -n "$testcase" ] && [ -f "$testcase" ]; then
            # shellcheck disable=SC2094  # the testcase is only read
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
            head -c 8000 "$testcase" | base64 | fold -w 76
            echo '```'
            echo
        else
            echo "The reproducer could not be recovered."
            echo
        fi
        echo "Stack:"
        echo
        echo '`````'
        fuzz_stack "$summaryfile"
        echo
        echo '`````'
        echo
        if [ -z "$existing" ]; then
            if [ -n "$closed" ]; then
                echo "The same crash was reported in #$closed, which is closed."
                echo
            fi
            echo "The fuzz label pauses the nightly fuzzing until this issue is closed."
            echo
            fuzz_marker "$key"
        fi
    } > "$body"

    echo "finding: $key"
    if [ -n "$existing" ]; then
        fuzz_write issue comment "$existing" -R "$GITHUB_REPOSITORY" --body-file "$body"
    else
        fuzz_write issue create -R "$GITHUB_REPOSITORY" --label bug --label fuzz --title "$title" --body-file "$body"
    fi
}
