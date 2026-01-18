pf: (flame "par_iter_bp_bench" "par_iter_bp_buf_bench")
pt: (test "par_iter_bp_bench" "par_iter_bp_buf_bench")
df: (flame "par_iter_bp_delayed_bench" "par_iter_bp_delayed_buf_bench")
dt: (test "par_iter_bp_delayed_bench" "par_iter_bp_delayed_buf_bench")
default: (flame "par_iter_bp_bench" "par_iter_bp_delayed_bench")
t: (test "par_iter_bp_bench" "par_iter_bp_buf_bench" "par_iter_bp_delayed_bench" "par_iter_bp_delayed_buf_bench")
f: (flame "par_iter_bp_bench" "par_iter_bp_buf_bench" "par_iter_bp_delayed_bench" "par_iter_bp_delayed_buf_bench")

# Justfile for running tests with specific options
# Usage:
# just test TESTCASE
test *TEST:
    cargo test -r --lib -- --ignored --nocapture --test-threads 1 {{TEST}}
flame *TEST:
    cargo flamegraph --open --unit-test -- --ignored --nocapture --test-threads 1 {{TEST}}
