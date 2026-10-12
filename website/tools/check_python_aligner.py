#!/usr/bin/env python3
"""Export and verify the Python aligner tutorial's executable examples.

The ordered ``python id=py_aligner_*`` fences form one standard-library Python
session. The downloadable script is generated from those fences, so changing
the tutorial cannot silently leave its examples behind. NumPy discussion
snippets use a different ID and are not required to run this tutorial.
"""
from __future__ import annotations

import argparse
import random
import re
import runpy
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


WEBSITE = Path(__file__).resolve().parents[1]
OUTPUT = WEBSITE / "static/examples/python-aligner.py"
FENCES = re.compile(r"^```(?P<info>[^\n]*)\n(?P<code>.*?)^```[ \t]*$", re.M | re.S)
PREFIX = "py_aligner_"


def tutorial_fences(post: Path) -> list[tuple[str, str]]:
    examples = []
    seen = set()
    for match in FENCES.finditer(post.read_text()):
        info = shlex.split(match["info"])
        ids = [part[3:] for part in info if part.startswith("id=")]
        if not ids or not ids[0].startswith(PREFIX):
            continue
        if len(ids) != 1 or info[0] != "python":
            raise ValueError(f"{post.name}: executable examples need one Python fence ID")
        identifier = ids[0]
        if identifier in seen:
            raise ValueError(f"{post.name}: duplicate example ID {identifier}")
        seen.add(identifier)
        examples.append((identifier, match["code"].rstrip() + "\n"))
    if not examples:
        raise ValueError(f"{post.name}: no {PREFIX} Python examples found")
    return examples


def discover_post() -> Path:
    posts = [post for post in sorted((WEBSITE / "blog").glob("*.md"))
             if re.search(r"^```python\b[^\n]*\bid=" + PREFIX, post.read_text(), re.M)]
    if len(posts) != 1:
        raise ValueError(f"Expected one Python aligner tutorial; found {len(posts)}")
    return posts[0]


def export_source(post: Path, examples: list[tuple[str, str]]) -> str:
    try:
        source = post.relative_to(WEBSITE.parent).as_posix()
    except ValueError:
        source = post.name
    header = (
        "#!/usr/bin/env python3\n"
        '"""Small Python sequence-mapping examples from the BioseqDB tutorial.\n\n'
        f"Generated from {source}.\n"
        "Run with Python 3; only the standard library is required.\n"
        "This teaching implementation is not the historical course submission.\n"
        '"""\n'
    )
    return header + "".join(f"\n# Example: {identifier}\n{code}"
                            for identifier, code in examples)


def edit_distance(left: str, right: str) -> int:
    """Independent linear-space global edit-distance oracle."""
    previous = list(range(len(right) + 1))
    for i, base in enumerate(left, start=1):
        current = [i]
        for j, other in enumerate(right, start=1):
            current.append(min(previous[j] + 1, current[-1] + 1,
                               previous[j - 1] + (base != other)))
        previous = current
    return previous[-1]


def expected_kmers(text: str, k: int) -> list[tuple[int, int]]:
    """Encode fresh slices without using the tutorial's rolling recurrence."""
    return [(int("".join(str("ACGT".index(base)) for base in text[i:i + k]), 4), i)
            for i in range(len(text) - k + 1)]


def expected_minimizers(text: str, k: int, width: int) -> list[tuple[int, int]]:
    kmers = expected_kmers(text, k)
    selected = {min(kmers[start:start + width])
                for start in range(len(kmers) - width + 1)}
    return sorted(selected, key=lambda pair: pair[1])


def best_chain_length(anchors: list[tuple[int, int]]) -> int:
    """Quadratic strict two-coordinate DP, independent of patience sorting."""
    anchors = sorted(set(anchors))
    lengths = []
    for i, (r, q) in enumerate(anchors):
        lengths.append(1 + max((lengths[j] for j in range(i)
                               if anchors[j][0] < r and anchors[j][1] < q), default=0))
    return max(lengths, default=0)


def verify_script(script: Path) -> None:
    namespace = runpy.run_path(str(script), run_name="__main__")
    required = ("dna", "reverse_complement", "rolling_kmers", "minimizers",
                "shared_anchors", "lis_chain", "historical_cutoff_chain",
                "window_score", "candidate_region", "align_substring",
                "toy_map", "bwt_exact_positions")
    missing = [name for name in required if not callable(namespace.get(name))]
    if missing:
        raise AssertionError("Missing teaching functions: " + ", ".join(missing))
    rng = random.Random(917_203)
    check = namespace

    assert check["dna"]("acgt") == "ACGT"
    assert check["reverse_complement"]("AACGT") == "ACGTT"
    for invalid in ("N", "AC-GT", "AC GT"):
        try:
            check["dna"](invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Invalid DNA silently accepted: {invalid!r}")

    for text in ("", "A", "AAAAAA", "ACGTACGT", "G" * 40):
        for k in (1, 3, 17):
            assert check["rolling_kmers"](text, k) == expected_kmers(text, k)
            for width in (1, 2, 8):
                assert check["minimizers"](text, k, width) == expected_minimizers(text, k, width)
    for _ in range(80):
        text = "".join(rng.choices("ACGT", k=rng.randrange(2, 55)))
        k, width = rng.randrange(1, 19), rng.randrange(1, 10)
        assert check["rolling_kmers"](text, k) == expected_kmers(text, k)
        assert check["minimizers"](text, k, width) == expected_minimizers(text, k, width)
    assert max(key for key, _ in check["rolling_kmers"]("G" * 17, 17)) > 2**32

    reference, query = "ACGTACGTACGTACGT", "ACGTACGT"
    ref = expected_minimizers(reference, 3, 2)
    qry = expected_minimizers(query, 3, 2)
    expected = {(r, q) for rk, r in ref for qk, q in qry if rk == qk}
    assert set(check["shared_anchors"](reference, query, 3, 2)) == expected
    for _ in range(100):
        anchors = [(rng.randrange(10), rng.randrange(10)) for _ in range(rng.randrange(15))]
        chain = check["lis_chain"](anchors)
        assert len(chain) == best_chain_length(anchors), (anchors, chain)
        assert all(anchor in anchors for anchor in chain)
        assert all(r0 < r1 and q0 < q1 for (r0, q0), (r1, q1) in zip(chain, chain[1:]))
    assert check["historical_cutoff_chain"]([], 130) == []
    simple = [(10, 2), (20, 12), (30, 22), (40, 32)]
    assert check["historical_cutoff_chain"](simple, 130) == simple
    # The cutoff can lose an optimal LIS while its parent chain stays monotone.
    distant = [(0, 0), (100, 10)]
    assert check["lis_chain"](distant) == distant
    assert check["historical_cutoff_chain"](distant, 30) == [(100, 10)]
    for _ in range(40):
        anchors = [(rng.randrange(500), rng.randrange(100)) for _ in range(20)]
        chain = check["historical_cutoff_chain"](anchors, 30)
        assert all(r0 < r1 and q0 < q1 for (r0, q0), (r1, q1) in zip(chain, chain[1:]))

    dense = [(100, 0), (110, 10), (120, 20), (130, 30)]
    sparse = [(100, 0), (130, 10), (160, 20), (190, 30)]
    assert check["window_score"](dense, 100, 17) == 0.3
    assert check["window_score"](sparse, 100, 17) == 0.105
    assert check["candidate_region"](dense, 500, 100, 17) == (91, 209)
    assert check["candidate_region"]([(2, 0), (20, 18)], 70, 70, 7) == (0, 70)

    for _ in range(65):
        target = "".join(rng.choices("ACGT", k=rng.randrange(9)))
        query = "".join(rng.choices("ACGT", k=rng.randrange(6)))
        result = check["align_substring"](query, target)
        score, left, right, q_line, t_line = result
        oracle = min(edit_distance(query, target[start:end])
                     for start in range(len(target) + 1)
                     for end in range(start, len(target) + 1))
        assert score == oracle, (query, target, result, oracle)
        assert 0 <= left <= right <= len(target)
        assert q_line.replace("-", "") == query
        assert t_line.replace("-", "") == target[left:right]
        assert len(q_line) == len(t_line)
        assert all(a != "-" or b != "-" for a, b in zip(q_line, t_line))
        assert sum(a != b for a, b in zip(q_line, t_line)) == score

    fixture_rng = random.Random(41)
    reference = "".join(fixture_rng.choices("ACGT", k=500))
    query = reference[180:260]
    assert reference.count(query) == 1
    for changed, expected_score in ((query, 0), (query[:35] + query[36:], 1),
                                    (query[:35] + "A" + query[35:], 1)):
        result = check["toy_map"](reference, changed)
        assert result is not None and result[:3] == (expected_score, 180, 260), result
        assert result[3].replace("-", "") == changed
        assert result[4].replace("-", "") == reference[180:260]
    assert check["toy_map"]("ACGT", "ACGT") is None

    for _ in range(45):
        text = "".join(rng.choices("ACGT", k=rng.randrange(1, 22)))
        query = "".join(rng.choices("ACGT", k=rng.randrange(1, 6)))
        expected = [i for i in range(len(text)) if text.startswith(query, i)]
        assert check["bwt_exact_positions"](text, query) == expected
    assert check["bwt_exact_positions"]("GATTACAGATTACA", "TACA") == [3, 10]
    print("Independent checks: PASS (encoding, window ties, repeated seeds, strict chains, "
          "substring alignment, indels and BWT search)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--post", type=Path, help="Override automatic blog post discovery")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true", help="Fail if the committed download has drifted")
    parser.add_argument("--worker", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        verify_script(args.worker)
        return 0
    post = args.post.resolve() if args.post else discover_post()
    examples = tutorial_fences(post)
    source = export_source(post, examples)
    compile(source, str(args.output), "exec")
    if args.check and (not args.output.is_file() or args.output.read_text() != source):
        raise ValueError("Python tutorial download has drifted; run website/tools/check_python_aligner.py")
    with tempfile.TemporaryDirectory(prefix="bioseq-python-aligner-") as temporary:
        script = Path(temporary) / "python-aligner.py"
        script.write_text(source)
        result = subprocess.run([sys.executable, "-I", str(Path(__file__).resolve()), "--worker", str(script)],
                                cwd=temporary, text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise ValueError("Python tutorial checks failed:\n" + result.stdout + result.stderr)
        print(result.stdout.rstrip())
    if not args.check:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(source)
    print(f"Python aligner tutorial: PASS ({len(examples)} executable fences, download matches)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error)) from error
