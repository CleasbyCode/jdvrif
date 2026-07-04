#!/bin/bash
# Build jdvrif and regenerate committed golden embedded JPG fixtures + manifest.
#
# Re-run after intentional format or crypto layout changes. Review diffs to
# golden/*.jpg and golden/manifest.tsv before committing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GOLDEN="$TESTS/golden"
MANIFEST="$GOLDEN/manifest.tsv"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
NO_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)
            NO_BUILD=1
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

bash "$TESTS/create_testdata.sh"

if [[ "$NO_BUILD" -eq 0 ]]; then
    (cd "$ROOT" && bash ./compile_jdvrif.sh)
fi

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found: $BIN" >&2
    exit 1
fi

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" JPG image: \([^ ]*\) (.*/\1/p' "$1" | tail -n 1
}

extract_pin() {
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

# case_id conceal_option cover_rel payload_rel min_icc_segments expect_pshop expect_xmp
# Use "." for the default (no) conceal option.
CASES=(
    $'default_single\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_text.txt\t0\t0\t0'
    $'default_multiseg\t.\ttestdata/covers/cover_default.jpg\ttestdata/payloads/payload_multi.bin\t2\t0\t0'
    $'reddit_zip\t-r\ttestdata/covers/cover_reddit.jpg\ttestdata/payloads/payload_archive.zip\t0\t0\t0'
    $'reddit_multiseg\t-r\ttestdata/covers/cover_reddit.jpg\ttestdata/payloads/payload_multi.bin\t2\t0\t0'
    $'bluesky_single\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bsingle.bin\t0\t0\t0'
    $'bluesky_split\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bsplit.bin\t0\t1\t0'
    $'bluesky_xmp\t-b\ttestdata/covers/cover_bluesky.jpg\ttestdata/payloads/bxmp.bin\t0\t1\t1'
)

rm -rf "$GOLDEN"
mkdir -p "$GOLDEN"

{
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        case_id conceal_option cover_rel payload_rel golden_rel pin min_icc_segments expect_pshop expect_xmp
    for row in "${CASES[@]}"; do
        IFS=$'\t' read -r case_id option cover_rel payload_rel min_icc expect_pshop expect_xmp <<<"$row"
        if [[ "$option" == "." ]]; then
            option=""
        fi

        case_dir="$GOLDEN/$case_id"
        mkdir -p "$case_dir"
        work="$(mktemp -d)"

        cover="$TESTS/$cover_rel"
        payload="$TESTS/$payload_rel"
        if [[ ! -f "$cover" || ! -f "$payload" ]]; then
            echo "Missing fixture for $case_id" >&2
            rm -rf "$work"
            exit 1
        fi

        pushd "$work" >/dev/null
        if [[ -n "$option" ]]; then
            "$BIN" conceal "$option" "$cover" "$payload" > conceal.log 2>&1
        else
            "$BIN" conceal "$cover" "$payload" > conceal.log 2>&1
        fi

        embedded="$(extract_embedded_image conceal.log)"
        pin="$(extract_pin conceal.log)"
        if [[ -z "$embedded" || -z "$pin" || ! -f "$embedded" ]]; then
            echo "Conceal failed for $case_id:" >&2
            cat conceal.log >&2
            popd >/dev/null
            rm -rf "$work"
            exit 1
        fi
        popd >/dev/null

        golden_rel="golden/$case_id/embedded.jpg"
        cp "$work/$embedded" "$TESTS/$golden_rel"
        rm -rf "$work"

        manifest_option="$option"
        if [[ -z "$manifest_option" ]]; then
            manifest_option="."
        fi

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$case_id" "$manifest_option" "$cover_rel" "$payload_rel" "$golden_rel" "$pin" "$min_icc" "$expect_pshop" "$expect_xmp"
    done
} > "$MANIFEST"

echo "Golden fixtures written to: $GOLDEN"
echo "Manifest: $MANIFEST"
wc -l "$MANIFEST"
