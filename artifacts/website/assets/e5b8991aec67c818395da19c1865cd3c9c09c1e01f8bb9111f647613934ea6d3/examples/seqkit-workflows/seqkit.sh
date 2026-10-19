#!/usr/bin/env bash
# SeqKit side of the BioseqDB sequence-workflows tutorial.
# Run the SQL/Python export first. It creates windows.fa (ordered by ID) and
# genomes.fa (IDs hg38, mm39 and danRer11). No sequence data is bundled here.
# Usage: bash seqkit.sh [windows.fa] [genomes.fa] [results/seqkit]
# Tested with SeqKit v2.14.0. Install: https://bioinf.shenwei.me/seqkit/download/
# Set SEQKIT=/path/to/seqkit if it is not on PATH.
set -euo pipefail

seqkit_bin="${SEQKIT:-seqkit}"
windows="${1:-windows.fa}"
genomes="${2:-genomes.fa}"
out="${3:-results/seqkit}"

command -v "$seqkit_bin" >/dev/null || {
    printf 'SeqKit was not found. Install it or set SEQKIT.\n' >&2
    exit 1
}
for input in "$windows" "$genomes"; do
    test -r "$input" || {
        printf 'Missing input: %s. Run the tutorial export first.\n' "$input" >&2
        exit 1
    }
done
mkdir -p "$out"
# Keep the two inputs beside the derived fixtures. After this script finishes,
# `cd results/seqkit` lets you run the article's short commands literally.
# cmp also avoids copying a file onto itself when rerunning from this directory.
cmp -s "$windows" "$out/windows.fa" || cp "$windows" "$out/windows.fa"
cmp -s "$genomes" "$out/genomes.fa" || cp "$genomes" "$out/genomes.fa"
windows="$out/windows.fa"
genomes="$out/genomes.fa"
"$seqkit_bin" version > "$out/version.txt"

# Basic sequence counts and lengths. The 51 windows total 49,464 bases.
"$seqkit_bin" stats -T "$windows" > "$out/stats.tsv"
"$seqkit_bin" head -n 6 "$windows" |
    "$seqkit_bin" subseq -r 1:12 |
    "$seqkit_bin" fx2tab -Q -i > "$out/prefixes.tsv"
"$seqkit_bin" seq --max-len 999 "$windows" |
    "$seqkit_bin" fx2tab -n -i -l > "$out/short-windows.tsv"

# GC excludes N from the denominator. Base counts preserve N separately.
"$seqkit_bin" fx2tab -n -i -l -g -C N "$genomes" > "$out/genome-gc.tsv"
"$seqkit_bin" fx2tab -n -i -C A -C C -C G -C T -C N "$genomes" > "$out/base-counts.tsv"
"$seqkit_bin" fx2tab -n -i -g "$windows" > "$out/window-gc.tsv"
awk -F '\t' '$2 >= 45' "$out/window-gc.tsv" > "$out/gc-at-least-45.tsv"

# IDs are exact matches by default. -r enables regex matching on the ID.
printf 'human_01\nmouse_02\n' > "$out/wanted.txt"
"$seqkit_bin" grep -f "$out/wanted.txt" "$windows" > "$out/wanted.fa"
"$seqkit_bin" grep -r -p '^human_' "$windows" > "$out/human.fa"
"$seqkit_bin" grep -v -f "$out/wanted.txt" "$windows" > "$out/without-wanted.fa"

# -s searches sequence content. -P limits the search to the stored strand,
# matching the SQL text predicates. These queries do not wrap across chrM's
# origin or across window boundaries.
"$seqkit_bin" grep -s -P -r -p '[^ACGT]' "$windows" > "$out/ambiguous.fa"
"$seqkit_bin" grep -s -P -p GATC "$windows" > "$out/gatc.fa"
"$seqkit_bin" grep -s -P -p GAATTC "$windows" > "$out/gaattc.fa"
"$seqkit_bin" grep -s -P -r -p 'TATA[AT]AA' "$windows" > "$out/tata-pattern.fa"
"$seqkit_bin" locate -P -p GAATTC "$windows" > "$out/gaattc-positions.tsv"

# SeqKit ranges are one-based and inclusive. This selects three whole records
# before extracting bases. The SQL equivalents use zero-based half-open slices.
"$seqkit_bin" grep -r -p '_01$' "$windows" > "$out/first-per-species.fa"
"$seqkit_bin" subseq -r 10:20 "$out/first-per-species.fa" |
    "$seqkit_bin" fx2tab -Q -i > "$out/bases-10-to-20.tsv"
"$seqkit_bin" subseq -r -10:-1 "$out/first-per-species.fa" |
    "$seqkit_bin" fx2tab -Q -i > "$out/last-10.tsv"
"$seqkit_bin" subseq -r 1:20 "$out/first-per-species.fa" > "$out/prefix-20.fa"
"$seqkit_bin" seq -r -p "$out/prefix-20.fa" |
    "$seqkit_bin" fx2tab -Q -i > "$out/reverse-complement.tsv"

# Group by the species prefix in each ID. SeqKit writes one file per group.
# The SQL side keeps one table and uses WHERE or GROUP BY instead.
"$seqkit_bin" split --by-id --id-regexp '^([^_]+)_' \
    --out-dir "$out/by-species" --force "$windows"
"$seqkit_bin" stats -T "$out"/by-species/*.fa > "$out/species-stats.tsv"

# range selects RECORDS, not nucleotide positions. This is SQL's ordered
# LIMIT 5 OFFSET 3 on the exported input. The sort below is shortest first.
"$seqkit_bin" range -r 4:8 "$windows" |
    "$seqkit_bin" fx2tab -n -i -l > "$out/records-4-to-8.tsv"
"$seqkit_bin" sort -l "$windows" > "$out/sorted.fa"
"$seqkit_bin" head -n 3 "$out/sorted.fa" |
    "$seqkit_bin" fx2tab -n -i -l > "$out/shortest-three.tsv"

# These are record permutations/subsets, not shuffled bases. Seeds make the
# SeqKit runs reproducible, but PostgreSQL uses a different random generator.
# sample -p gives a variable-size sample; sample -n is not an exact-size promise.
"$seqkit_bin" shuffle -s 23 "$windows" > "$out/shuffled.fa"
"$seqkit_bin" sample -p 0.1 -s 11 "$windows" > "$out/sampled.fa"

# Arbitrary header editing uses replace. rename, shown later, resolves repeated
# IDs. Point editing below changes position 5 to T in the first three prefixes.
"$seqkit_bin" head -n 3 "$windows" |
    "$seqkit_bin" replace -p '^' -r 'mito_' |
    "$seqkit_bin" seq -n -i > "$out/replaced-ids.txt"
"$seqkit_bin" head -n 3 "$windows" |
    "$seqkit_bin" subseq -r 1:20 > "$out/first-three-prefixes.fa"
"$seqkit_bin" mutate -p 5:T "$out/first-three-prefixes.fa" |
    "$seqkit_bin" fx2tab -Q -i > "$out/mutated-prefixes.tsv"

# concat matches IDs, regardless of input order. Make two teaching files from
# the FIRST and LAST ten bases of the first three human windows. Their concatenation
# joins separated ends; it does not reconstruct the original continuous prefix.
"$seqkit_bin" head -n 3 "$windows" > "$out/first-three.fa"
"$seqkit_bin" subseq -r 1:10 "$out/first-three.fa" > "$out/left.fa"
"$seqkit_bin" subseq -r -10:-1 "$out/first-three.fa" |
    "$seqkit_bin" sort -r > "$out/right.fa"
"$seqkit_bin" concat "$out/left.fa" "$out/right.fa" |
    "$seqkit_bin" sort |
    "$seqkit_bin" fx2tab -Q -i > "$out/concatenated-ends.tsv"

# A three-record teaching fixture derived from human_01's first 20 bases.
# Records 1 and 3 have the same bases and IDs copy/other. Record 2 also has ID
# copy, but its last base was deliberately set to A. No biological duplication
# or mutation is inferred. rename resolves repeated IDs; rmdup compares bases.
"$seqkit_bin" head -n 1 "$windows" |
    "$seqkit_bin" subseq -r 1:20 |
    "$seqkit_bin" replace -p '^.*$' -r copy > "$out/copy.fa"
"$seqkit_bin" mutate -p 20:A "$out/copy.fa" > "$out/edited-copy.fa"
"$seqkit_bin" replace -p '^copy$' -r other "$out/copy.fa" > "$out/other.fa"
cat "$out/copy.fa" "$out/edited-copy.fa" "$out/other.fa" > "$out/duplicates.fa"
"$seqkit_bin" rename "$out/duplicates.fa" |
    "$seqkit_bin" seq -n -i > "$out/unique-ids.txt"
"$seqkit_bin" rmdup -s -P -i "$out/duplicates.fa" \
    -D "$out/duplicate-groups.tsv" -o "$out/deduplicated.fa"
"$seqkit_bin" fx2tab -Q -i "$out/deduplicated.fa" > "$out/deduplicated.tsv"

# A deliberately gapped string explains cleaning an alignment representation.
# This is a teaching string, not a UCSC record or a newly computed alignment.
printf '>teaching_gap\nAC-G.TN\n' > "$out/teaching-gaps.fa"
"$seqkit_bin" seq -g "$out/teaching-gaps.fa" |
    "$seqkit_bin" fx2tab -Q -i > "$out/ungapped.tsv"

printf 'SeqKit examples completed. Results are in %s\n' "$out"
