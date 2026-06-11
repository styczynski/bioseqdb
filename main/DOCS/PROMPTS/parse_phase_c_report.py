#!/usr/bin/env python3
"""DOC0310 — extract XXL_HIFI per-algorithm wall clock + Phase C region timings."""
import json, sys, glob, os

path = sys.argv[1] if len(sys.argv) > 1 else sorted(
    glob.glob("reporting/bioseq_bench/report_*.json"),
    key=os.path.getmtime)[-1]
d = json.load(open(path))
print(f"# report: {os.path.basename(path)}")
cur = d["benchmarks"]["bench_aligners_curated"]
print("\n## XXL_HIFI wall clock (P50, seconds) by algorithm")
for t in cur["execution_tuples"]:
    algo = list(t.get("tuple", {}).keys())
    algo = algo[0] if algo else "?"
    xxl = t.get("test_cases", {}).get("XXL_HIFI", {})
    wc = xxl.get("all_regions", {}).get("wall_clock_ns", {})
    p50 = wc.get("percentiles", {}).get("P50")
    mean = wc.get("stats", {}).get("mean")
    print(f"  {algo:46s}  P50={p50/1e9 if p50 else None:>8} s  mean={mean/1e9 if mean else None:.3f} s")
    if "phase_c" in algo:
        regs = ["phase_c_gpu_index_build", "target_index_build", "mm_sketch",
                "minimap_port", "minimap2_port", "minimap2_port_with_index",
                "collect_seed_hits", "collect_seed_hits_sort",
                "backtrack_chains", "chain_anchors_vulkan", "target_index_lookup"]
        print(f"    -- region P50 (s) for {algo} --")
        for r in regs:
            node = xxl.get(r, {})
            rp = node.get("region_total_ns", node.get("wall_clock_ns", {}))
            pp = rp.get("percentiles", {}).get("P50") if isinstance(rp, dict) else None
            if pp is not None:
                print(f"       {r:34s} {pp/1e9:.4f}")
print("\n## correctness (primary_correct_fraction P50) by algorithm")
for t in cur["execution_tuples"]:
    algo = list(t.get("tuple", {}).keys())
    algo = algo[0] if algo else "?"
    xxl = t.get("test_cases", {}).get("XXL_HIFI", {})
    pcf = xxl.get("primary_correct_fraction", {})
    val = pcf.get("percentiles", {}).get("P50") if isinstance(pcf, dict) else pcf
    print(f"  {algo:46s}  correct_frac={val}")
print("\n## rule_violations:", d.get("rule_violations"))
print("## regressions:", d.get("regressions"))
