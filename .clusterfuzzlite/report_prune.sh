#!/bin/bash -eu
# Reports the crashes that the prune job hit while merging the corpus. Prune
# drops the crashing inputs from the corpus and run_fuzzers returns no crash and
# no .summary for them, but the job log has the sanitizer report and the name of
# each input (the crash-<sha1> file is the corpus file corpus/<target>/<sha1>).
# Crashes with one key (fuzz_issue.sh) make one issue.
# Usage: report_prune.sh <prune job log>
# Environment: GH_TOKEN, RUN_URL, FUZZ_CORPUS_URL (corpus repository, with the
# token), the GitHub Actions variables; DRY_RUN=1; FUZZ_CORPUS_DIR uses an
# existing clone of the corpus repository.

source "$(dirname "$0")/fuzz_issue.sh"

work=$(mktemp -d)
tab=$(printf '\t')
# Drop the "job<TAB>step<TAB>" prefix of `gh run view --log` and the timestamp.
sed -E "s/^([^${tab}]*${tab}[^${tab}]*${tab})?[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z ?//" "$1" > "$work/log"

# One <target>__<sanitizer>__<sha1>.summary per crash: the lines from the
# sanitizer ERROR line to the "Test unit written" line.
awk -v dir="$work" '
    /^(==[0-9]+== ?)?ERROR: / { buf = "" }
    { buf = buf $0 "\n" }
    /Test unit written to .*\/artifacts\/[^\/]+\/[^\/]+\/(crash|timeout|oom|leak|slow-unit)-[0-9a-f]+$/ {
        n = split($NF, p, "/")
        sub(/^[a-z-]+-/, "", p[n])
        printf "%s", buf > (dir "/" p[n-2] "__" p[n-1] "__" p[n] ".summary")
        buf = ""
    }' "$work/log"

index="$work/index"
: > "$index"
for summaryfile in "$work"/*.summary; do
    [ -e "$summaryfile" ] || continue
    name=$(basename "$summaryfile" .summary)
    target=${name%%__*}
    rest=${name#*__}
    printf '%s\t%s\n' "$(fuzz_key "$target" "${rest%%__*}" "$summaryfile")" "$summaryfile" >> "$index"
done
if [ ! -s "$index" ]; then
    echo "no crashes in the prune log: nothing to report"
    exit 0
fi

corpus=${FUZZ_CORPUS_DIR:-$work/corpus}
if [ -z "${FUZZ_CORPUS_DIR:-}" ]; then
    git clone --quiet --filter=blob:none --no-checkout "$FUZZ_CORPUS_URL" "$corpus"
fi
# The prune job uploads the pruned corpus before it fails, so the crashing
# input is gone from HEAD: take it from the commit before the one that deleted it.
fetch_input() {
    local path=corpus/$1/$2 commit
    git -C "$corpus" show "HEAD:$path" 2> /dev/null && return 0
    commit=$(git -C "$corpus" rev-list -n 1 HEAD -- "$path")
    [ -n "$commit" ] && git -C "$corpus" show "$commit^:$path" 2> /dev/null
}

cut -f1 "$index" | sort -u | while IFS= read -r key; do
    summaryfile=$(grep -F -m 1 "${key}${tab}" "$index" | cut -f2)
    count=$(grep -c -F "${key}${tab}" "$index")
    name=$(basename "$summaryfile" .summary)
    target=${name%%__*}
    sanitizer=${name#*__}
    sha=${sanitizer#*__}
    sanitizer=${sanitizer%%__*}
    testcase="$work/$sha"
    fetch_input "$target" "$sha" > "$testcase" || rm -f "$testcase"
    fuzz_report "$target" "$sanitizer" "$testcase" "$summaryfile" \
        "ClusterFuzzLite corpus pruning ($sanitizer) of $GITHUB_SHA hit this in $count corpus input(s) of $target; this one is $sha. Prune dropped them from the corpus."
done
