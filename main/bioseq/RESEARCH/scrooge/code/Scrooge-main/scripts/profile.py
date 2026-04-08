import argparse
import subprocess
import re
import sys
import platform
from datetime import datetime
from itertools import product
from pathlib import Path

def csv_write(path, data, header=None):
    with open(path, "w+") as f:
        if header:
            line = ",".join([str(x) for x in header])
            f.write(line + "\n")
        for row in data:
            line = ",".join([str(x) for x in row])
            f.write(line + "\n")

def run_cpu(W, O, sene, dent, early_termination, threads, reference, reads, seeds, res_data):
    if type(threads) == list:
        threads= ",".join([str(t) for t in threads])
    else:
        threads = str(threads)

    subprocess.run(["mkdir", "-p", "baseline_algorithms/wfa/build"])
    subprocess.run(["make", "-C", "baseline_algorithms/wfa/", "lib_wfa"], capture_output=True)

    compile_base = ["g++", "-o", "cpu_baseline", "-DLIB_WFA", "-Ibaseline_algorithms/wfa", "-Lbaseline_algorithms/wfa/build", "src/cpu_baseline.cpp", "src/genasm_cpu.cpp", "src/util.cpp", "baseline_algorithms/ksw2/ksw2_extz.c", "baseline_algorithms/ksw2/ksw2_extz2_sse.c", "baseline_algorithms/edlib/edlib.cpp", "baseline_algorithms/gact/gact.cpp", "-std=c++17", "-O3", "-lpthread", "-lstdc++fs", "-lwfa", "-fopenmp"]
    compile_config = ["-DCLI_KNOBS", f"-DCLI_W={W}", f"-DCLI_K={W}", f"-DCLI_O={O}"]
    if sene: compile_config += ["-DCLI_STORE_ENTRIES_NOT_EDGES"]
    if dent: compile_config += ["-DCLI_DISCARD_ENTRIES_NOT_USED_BY_TRACEBACK"]
    if early_termination: compile_config += ["-DCLI_EARLY_TERMINATION"]
    compile_res = subprocess.run(compile_base + compile_config, capture_output=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(W, O, sene, dent, early_termination, threads, reference, reads, seeds)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    run_base = ["./cpu_baseline", "--algorithms=genasm_cpu", "--verbose"]
    run_config = [f"--threads={threads}", f"--reference={reference}", f"--reads={reads}", f"--seeds={seeds}"]
    run_res = subprocess.run(run_base + run_config, capture_output=True)
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(W, O, sene, dent, early_termination, threads, reference, reads, seeds)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    threads = None
    lines = run_res.stdout.split(b"\n")
    for line in lines:
        if b"threads" in line:
            threads = int(line.split(b" ")[0])
        if b"genasm_cpu:" in line:
            params = [W, O, sene, dent, early_termination]
            aligns_per_sec = line.split(b" ")[1]
            res_data.append(params + [threads, float(aligns_per_sec.decode("ascii"))])

def cpu_sweep_wo(reference, reads, seeds, outpath, repetitions):
    data = []
    threads = 48

    max_W = 256
    granularity = max(1, max_W//max_experiments)

    Ws = range(granularity, max_W+1, granularity)
    bls = [False, True]
    configs = list(product(Ws, bls, bls, bls))

    for i, (W, sene, dent, early_termination) in enumerate(configs):
        print(f"[{datetime.now()}] cpu_sweep_wo {i}/{len(configs)}")
        O = min(W//2+1, W-1)
        for rep in range(repetitions):
            run_cpu(W, O, sene, dent, early_termination, threads, reference, reads, seeds, data)

    csv_write(outpath, data, header=["W", "O", "SENE", "DENT", "early termination", "threads", "aligns/second"])

def cpu_sweep_o(reference, reads, seeds, outpath, repetitions):
    data = []
    threads = 48

    W = 64
    if override_W:
        W = override_W

    granularity = max(1, W//max_experiments)
    Os = range(granularity-1, W, granularity)
    bls = [False, True]
    configs = list(product(Os, bls, bls, bls))

    for i, (O, sene, dent, early_termination) in enumerate(configs):
        print(f"[{datetime.now()}] cpu_sweep_o {i}/{len(configs)}")
        for rep in range(repetitions):
            run_cpu(W, O, sene, dent, early_termination, threads, reference, reads, seeds, data)

    csv_write(outpath, data, header=["W", "O", "SENE", "DENT", "early termination", "threads", "aligns/second"])

def cpu_sweep_threads(reference, reads, seeds, outpath, repetitions):
    max_threads = 64
    granularity = max(1, max_threads//max_experiments)
    threads = list(range(granularity, max_threads+1, granularity))
    W = 64
    if override_W:
        W = override_W
    O = W//2+1
    
    bls = [False, True]
    configs = list(product(bls, bls, bls))

    data = []
    for i, (sene, dent, early_termination) in enumerate(configs):
        print(f"[{datetime.now()}] cpu_sweep_threads {i}/{len(configs)}")
        for rep in range(repetitions):
            run_cpu(W, O, sene, dent, early_termination, threads, reference, reads, seeds, data)

    csv_write(outpath, data, header=["W", "O", "SENE", "DENT", "early termination", "threads", "aligns/second"])

def profile_cpu(reference, reads, seeds, dataset_name, repetitions):
    override_name = f"_W={override_W}" if override_W else ""
    cpu_sweep_wo(reference, reads, seeds, out_dir/f"{dataset_name}_cpu_sweep_WO.csv", repetitions)
    cpu_sweep_o(reference, reads, seeds, out_dir/f"{dataset_name}_cpu_sweep_O{override_name}.csv", repetitions)
    cpu_sweep_threads(reference, reads, seeds, out_dir/f"{dataset_name}_cpu_sweep_threads{override_name}.csv", repetitions)

def run_gpu(W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_memory_type, smem_carveout_percent, arch, reference, reads, seeds, res_data):
    compile_base = ["nvcc", "-lineinfo", "-rdc=true", "src/bitvector_test.cu", "src/genasm_gpu.cu", "src/tests.cu", "src/util.cpp", "src/genasm_cpu.cpp", "-o", "tests", "-O3", "-std=c++17", "-lstdc++fs", "-Xcompiler", "-fopenmp"]
    compile_config = [f"-arch={arch}", "-DCLI_KNOBS", f"-DCLI_W={W}", f"-DCLI_K={W}", f"-DCLI_O={O}", f"-DCLI_THREAD_BLOCKS_PER_SM={tb_per_sm}", f"-DCLI_CIGAR_SUBLIST_SIZE={cigar_sublist_size}"]
    if dp_memory_type == "shared":
        compile_config += ["-DCLI_DP_MEMORY_SHARED"]
    elif dp_memory_type == "global":
        compile_config += ["-DCLI_DP_MEMORY_GLOBAL"]
    if smem_carveout_percent != None:
        compile_config += [f"-DCLI_SMEM_CARVEOUT_PERCENT={smem_carveout_percent}"]
    if sene: compile_config += ["-DCLI_STORE_ENTRIES_NOT_EDGES"]
    if dent: compile_config += ["-DCLI_DISCARD_ENTRIES_NOT_USED_BY_TRACEBACK"]
    if early_termination: compile_config += ["-DCLI_EARLY_TERMINATION"]
    compile_res = subprocess.run(compile_base + compile_config, capture_output=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_memory_type, smem_carveout_percent, arch)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    run_base = ["./tests", "--verbose"]
    run_config = [f"--reference={reference}", f"--reads={reads}", f"--seeds={seeds}"]
    run_res = subprocess.run(run_base + run_config, capture_output=True)
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_memory_type, smem_carveout_percent, arch)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    lines = run_res.stdout.split(b"\n") + run_res.stderr.split(b"\n")
    for line in lines:
        if b"idx=0" in line and b"name=" in line:
            gpu=re.search(b'name="(.+)"', line).group(1)
            sm=int(re.search(b"SMs=(\d+)", line).group(1))
            smem=int(re.search(b"smem=(\d+)kiB", line).group(1))
        if b"DP memory per thread block" in line:
            dp_memory_per_tb = int(re.search(b"using (\d+)B DP memory per thread block", line).group(1))
        if b"core algorithm" in line:
            params = [W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_memory_type, smem_carveout_percent, arch]
            aligns_per_sec = float(re.search(b"core algorithm ran at (\d+) ", line).group(1).decode("ascii"))
            res_data.append(params + [gpu, sm, smem, dp_memory_per_tb, aligns_per_sec])

def gpu_sweep_wo(reference, reads, seeds, outpath, arch, repetitions):
    data = []

    max_W = 256
    granularity = max(1, max_W//max_experiments)

    tb_per_sm = 20
    cigar_sublist_size = 64
    Ws = range(granularity, max_W+1, granularity)
    toggle = [False, True]
    dp_mems = ["shared", "global"]
    configs = list(product(Ws, toggle, toggle, toggle, dp_mems))

    for i, (W, sene, dent, early_termination, dp_mem) in enumerate(configs):
        print(f"[{datetime.now()}] gpu_sweep_wo {i}/{len(configs)}")
        O = min(W//2+1, W-1)
        smem_carveout_percent = 100 if dp_mem=="shared" else 0
        for rep in range(repetitions):
            run_gpu(W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_mem, smem_carveout_percent, arch, reference, reads, seeds, data)
    csv_write(outpath, data, header=["W", "O", "sene", "dent", "early termination", "threadblocks/sm", "cigar sublist size", "dp memory type", "smem carveout percent", "arch", "gpu", "sm count", "available smem per sm (kiB)", "used smem per threadblock (B)", "throughput (aligns/s)"])

def gpu_sweep_o(reference, reads, seeds, outpath, arch, repetitions):
    data = []

    W = 64
    if override_W:
        W = override_W

    granularity = max(1, W//max_experiments)
    tb_per_sm = 20
    cigar_sublist_size = 64
    Os = range(granularity-1, W, granularity)
    toggle = [False, True]
    dp_mems = ["shared", "global"]
    configs = list(product(Os, toggle, toggle, toggle, dp_mems))

    for i, (O, sene, dent, early_termination, dp_mem) in enumerate(configs):
        print(f"[{datetime.now()}] gpu_sweep_o {i}/{len(configs)}")
        smem_carveout_percent = 100 if dp_mem=="shared" else 0
        for rep in range(repetitions):
            run_gpu(W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_mem, smem_carveout_percent, arch, reference, reads, seeds, data)

    csv_write(outpath, data, header=["W", "O", "sene", "dent", "early termination", "threadblocks/sm", "cigar sublist size", "dp memory type", "smem carveout percent", "arch", "gpu", "sm count", "available smem per sm (kiB)", "used smem per threadblock (B)", "throughput (aligns/s)"])

def gpu_sweep_threadblocks(reference, reads, seeds, outpath, arch, repetitions):
    data = []

    W = 64
    if override_W:
        W = override_W
    O = W//2+1
    cigar_sublist_size = 64

    max_tb = 32
    granularity = max(1, max_tb//max_experiments)

    tb_per_sms = list(range(granularity, max_tb+1, granularity))
    toggle = [False, True]
    dp_mems = ["shared", "global"]
    configs = list(product(tb_per_sms, toggle, toggle, toggle, dp_mems))
    for i, (tb_per_sm, sene, dent, early_termination, dp_mem) in enumerate(configs):
        print(f"[{datetime.now()}] gpu_sweep_threadblocks {i}/{len(configs)}")
        smem_carveout_percent = 100 if dp_mem=="shared" else 0
        for rep in range(repetitions):
            run_gpu(W, O, sene, dent, early_termination, tb_per_sm, cigar_sublist_size, dp_mem, smem_carveout_percent, arch, reference, reads, seeds, data)
    csv_write(outpath, data, header=["W", "O", "sene", "dent", "early termination", "threadblocks/sm", "cigar sublist size", "dp memory type", "smem carveout percent", "arch", "gpu", "sm count", "available smem per sm (kiB)", "used smem per threadblock (B)", "throughput (aligns/s)"])

def profile_gpu(reference, reads, seeds, dataset_name, arch, repetitions):
    override_name = f"_W={override_W}" if override_W else ""
    gpu_sweep_threadblocks(reference, reads, seeds, out_dir/f"{dataset_name}_gpu_sweep_threadblocks{override_name}.csv", arch, repetitions)
    gpu_sweep_wo(reference, reads, seeds, out_dir/f"{dataset_name}_gpu_sweep_WO.csv", arch, repetitions)
    gpu_sweep_o(reference, reads, seeds, out_dir/f"{dataset_name}_gpu_sweep_O{override_name}.csv", arch, repetitions)

def run_cpu_baselines(threads, algorithms, reference, reads, seeds, res_data):
    if type(threads) == list:
        threads= ",".join([str(t) for t in threads])
    else:
        threads = str(threads)

    if type(algorithms) == list:
        algorithms = ",".join(algorithms)

    subprocess.run(["mkdir", "-p", "baseline_algorithms/wfa/build"])
    subprocess.run(["make", "-C", "baseline_algorithms/wfa/", "lib_wfa"], capture_output=True)

    compile_base = ["g++", "-o", "cpu_baseline", "-DLIB_WFA", "-Ibaseline_algorithms/wfa", "-Lbaseline_algorithms/wfa/build", "src/cpu_baseline.cpp", "src/genasm_cpu.cpp", "baseline_algorithms/ksw2/ksw2_extz.c", "baseline_algorithms/ksw2/ksw2_extz2_sse.c", "baseline_algorithms/edlib/edlib.cpp", "baseline_algorithms/gact/gact.cpp", "src/util.cpp", "-std=c++17", "-O3", "-lpthread", "-lstdc++fs", "-lwfa", "-fopenmp"]
    compile_config = []

    compile_res = subprocess.run(compile_base + compile_config, capture_output=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(threads, reference, reads, seeds)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    run_base = ["./cpu_baseline", "--verbose"]
    run_config = [f"--algorithms={algorithms}", f"--threads={threads}", f"--algorithms={algorithms}", f"--reference={reference}", f"--reads={reads}", f"--seeds={seeds}"]
    run_res = subprocess.run(run_base + run_config, capture_output=True)
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(threads, reference, reads, seeds)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    threads = None
    lines = run_res.stdout.split(b"\n")
    for line in lines:
        if b"threads" in line:
            threads = int(line.split(b" ")[0])

        match = re.search(b"(.+): (\d+.\d+) aligns/second", line)
        if match:
            algorithm = match.group(1)
            aligns_per_sec = match.group(2)
            res_data.append([algorithm.decode("ascii"), threads, float(aligns_per_sec.decode("ascii"))])

def cpu_baselines_sweep_threads(algorithms, reference, reads, seeds, outpath, repetitions):
    max_threads = 64
    granularity = max(1, max_threads//max_experiments)
    threads = list(range(granularity, max_threads+1, granularity))
    configs = algorithms

    data = []
    for i, algorithm in enumerate(configs):
        print(f"[{datetime.now()}] baselines_sweep_threads {i}/{len(configs)}")
        for rep in range(repetitions):
            run_cpu_baselines(threads, algorithm, reference, reads, seeds, data)
    csv_write(outpath, data, header=["algorithm", "threads", "aligns/second"])

def profile_cpu_baselines(algorithms, reference, reads, seeds, dataset_name, repetitions):
    cpu_baselines_sweep_threads(algorithms, reference, reads, seeds, out_dir/f"{dataset_name}_baselines_sweep_threads.csv", repetitions)

def run_darwin_gpu(cpu_threads, thread_blocks, threads_per_block, arch, reference, reads, seeds, tmp_dir, res_data):
    #prepare data
    tmp_dir = Path(args.tmp_dir).resolve()
    tmp_dir.mkdir(parents=True, exist_ok=True)
    tmp_reads = tmp_dir / f"{(str(reads).replace('/', '_').replace('.', '_'))}.fasta"
    tmp_reference = tmp_dir / f"{(str(reference).replace('/', '_').replace('.', '_'))}.fasta"
    if not tmp_reads.exists():
        subprocess.run([sys.executable, "scripts/GenConverter.py", reads, str(tmp_reads), "--fasta_line_size", "70"])
    if not tmp_reference.exists():
        subprocess.run([sys.executable, "scripts/GenConverter.py", reference, str(tmp_reference), "--fasta_line_size", "70"])
    reads = tmp_reads
    reference = tmp_reference

    #compile
    options = "-D GPU -D TIME -D Z_COMPILE_USED -D DISABLE_FIRST_TILE_THRESHOLD"
    gpu_options = f"{options} --maxrregcount=128"
    nvccinstance = "/usr/local/cuda-11.1/bin/nvcc"
    vars = f'options="{options}" gpu_options="{gpu_options}" nvccinstance="{nvccinstance}" GPU_ARCH="{arch}"'
    compile_res = subprocess.run(f"make clean && {vars} make", capture_output=True, cwd="baseline_algorithms/darwin-gpu/", shell=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(arch)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    #run
    run_base = ["./darwin"]
    run_config = [reference, reads, str(cpu_threads), str(thread_blocks), str(threads_per_block)]
    run_res = subprocess.run(run_base + run_config, capture_output=True, cwd="baseline_algorithms/darwin-gpu/")
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(cpu_threads, thread_blocks, threads_per_block, arch, reference, reads, seeds)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    lines = run_res.stdout.split(b"\n")
    for line in lines:
        match = re.search(b"At (\d+\.\d+) alignments/second", line)
        if match:
            aligns_per_sec = match.group(1)
    #last alignments/second print contains the correct throughput when the last thread terminates
    res_data.append(["darwin_gpu", cpu_threads, thread_blocks, threads_per_block, arch, float(aligns_per_sec.decode("ascii"))])

def sweep_darwin_gpu(reference, reads, seeds, outpath, arch, tmp_dir, repetitions):
    data = []

    cpu_threadss = list(range(1, 17))
    threadblockss = list(range(84, 4*84, 84))
    threads_per_blocks = [32, 64]
    configs = list(product(cpu_threadss, threadblockss, threads_per_blocks))

    for i, (cpu_threads, threadblocks, threads_per_block) in enumerate(configs):
        print(f"[{datetime.now()}] sweep_darwin_gpu {i}/{len(configs)}")
        for rep in range(repetitions):
            run_darwin_gpu(cpu_threads, threadblocks, threads_per_block, arch, reference, reads, seeds, tmp_dir, data)

    csv_write(outpath, data, header=["algorithm", "threads", "thread_blocks", "threads_per_block", "arch", "aligns/second"])

def run_cudaswpp3(sequence_length_limit, arch, reference, reads, seeds, tmp_dir, res_data):
    #prepare data
    tmp_dir = Path(args.tmp_dir).resolve()
    tmp_dir.mkdir(parents=True, exist_ok=True)
    tmp_reads = tmp_dir / f"{(str(reads).replace('/', '_').replace('.', '_'))}.fasta"
    if not tmp_reads.exists():
        subprocess.run([sys.executable, "scripts/GenConverter.py", reads, str(tmp_reads), "--fasta_line_size", "70"])
    reads = tmp_reads

    #compile
    nvccinstance = "nvcc"
    vars = f'sequence_length_limit={sequence_length_limit} GPU_ARCH="{arch}" nvccinstance="{nvccinstance}"'
    compile_res = subprocess.run(f"make clean && {vars} make", capture_output=True, cwd="baseline_algorithms/cudasw++v3.1.2/", shell=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(sequence_length_limit, arch)
        print(arch)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    #run
    run_base = ["./cudasw"]
    run_config = ["-db", str(reads), "-query", str(reads)]
    run_res = subprocess.run(run_base + run_config, capture_output=True, cwd="baseline_algorithms/cudasw++v3.1.2/")
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(sequence_length_limit, arch, reference, reads, seeds)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    gcups_list = []
    length_list = []
    throughput_list = []
    for line in run_res.stderr.split(b"\n"):
        if b'GCUPS' in line and b'Length' in line:
            gcups = float(re.search(b"GCUPS: ((?:\d+\.)?\d+)", line).group(1).decode("ascii"))
            gcups_list.append(gcups)

            length = int(re.search(b"Length: (\d+)", line).group(1).decode("ascii"))
            length_list.append(length)

            cells_per_pair = length**2
            throughput = gcups/cells_per_pair*1_000_000_000
            throughput_list.append(throughput)

    avg_gcups = sum(gcups_list)/len(gcups_list)
    avg_read_length = sum(length_list)/len(length_list)
    avg_alignments_per_second = sum(throughput_list)/len(throughput_list)

    res_data.append(["cudasw++3.0", arch, avg_read_length, avg_gcups, avg_alignments_per_second])

def sweep_cudaswpp3(reference, reads, seeds, outpath, sequence_length_limit, arch, tmp_dir, repetitions):
    cudaswpp_data = []
    for rep in range(repetitions):
        run_cudaswpp3(sequence_length_limit, arch, reference, reads, seeds, tmp_dir, cudaswpp_data)
    csv_write(outpath, cudaswpp_data, header=["algorithm", "arch", "avg_read_length", "avg_gcups", "aligns/second"])

def profile_gpu_baselines(reference, reads, seeds, dataset_name, arch, tmp_dir, repetitions):
    sweep_darwin_gpu(reference, reads, seeds, out_dir/f"{dataset_name}_darwin_gpu_sweep.csv", arch, tmp_dir, repetitions)
    sweep_cudaswpp3(reference, reads, seeds, out_dir/f"{dataset_name}_cudaswpp.csv", 20000, arch, tmp_dir, repetitions)

def run_accuracy(threads, algorithms, scoring, reference, reads, seeds, cigar, res_data):
    if type(threads) == list:
        threads= ",".join([str(t) for t in threads])
    else:
        threads = str(threads)

    if type(algorithms) == list:
        algorithms = ",".join(algorithms)

    if type(scoring) == dict:
        scoring = f"{scoring['mat']},{scoring['sub']},{scoring['gapo']},{scoring['gape']}"

    subprocess.run(["mkdir", "-p", "baseline_algorithms/wfa/build"])
    subprocess.run(["make", "-C", "baseline_algorithms/wfa/", "lib_wfa"], capture_output=True)

    compile_base = ["g++", "-o", "cpu_baseline", "-DLIB_WFA", "-Ibaseline_algorithms/wfa", "-Lbaseline_algorithms/wfa/build", "src/cpu_baseline.cpp", "src/genasm_cpu.cpp", "baseline_algorithms/ksw2/ksw2_extz.c", "baseline_algorithms/ksw2/ksw2_extz2_sse.c", "baseline_algorithms/edlib/edlib.cpp", "baseline_algorithms/gact/gact.cpp", "src/util.cpp", "-std=c++17", "-O3", "-lpthread", "-lstdc++fs", "-lwfa", "-fopenmp"]
    compile_config = []

    compile_res = subprocess.run(compile_base + compile_config, capture_output=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(threads, reference, reads, seeds)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    run_base = ["./cpu_baseline", "--verbose", "--accuracy"] + (["--cigar"] if cigar else [])
    run_config = [f"--algorithms={algorithms}", f"--threads={threads}", f"--scoring={scoring}", f"--reference={reference}", f"--reads={reads}", f"--seeds={seeds}"]
    print(" ".join(run_base+run_config))
    run_res = subprocess.run(run_base + run_config, capture_output=True)
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(threads, reference, reads, seeds)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    threads = None
    lines = run_res.stdout.split(b"\n")
    for line in lines:
        #if b"threads" in line:
        #    threads = int(line.split(b" ")[0])

        match = re.search(b"^(.+): (\d+.\d+) aligns/second$", line)
        if match:
            algorithm = match.group(1)
            aligns_per_sec = match.group(2)

        if cigar:
            match = re.search(b"^pair_idx=(\d+) score=(-?\d+) cigar=((?:\d+[=XID])*) read=([acgtnACGTN]*) reference=([acgtnACGTN]*)$", line)
            if match:
                pair_idx = match.group(1)
                score = match.group(2)
                cigar_str = match.group(3) or b""
                read_bases = match.group(4)
                reference_bases = match.group(5)
                res_data.append([algorithm.decode("ascii"), int(pair_idx.decode("ascii")), int(score.decode("ascii")), cigar_str.decode("ascii"), read_bases.decode("ascii"), reference_bases.decode("ascii")])
        else:
            match = re.search(b"^pair_idx=(\d+) score=(-?\d+)$", line)
            if match:
                pair_idx = match.group(1)
                score = match.group(2)
                res_data.append([algorithm.decode("ascii"), int(pair_idx.decode("ascii")), int(score.decode("ascii"))])

def all_accuracy(algorithms, reference, reads, seeds, cigar, outpath):
    threads = 48
    scoring = {
        'mat': 2,
        'sub': 4,
        'gapo': 4,
        'gape': 2
    }

    data = []
    print(f"[{datetime.now()}] all_accuracy {0}/{1}")
    run_accuracy(threads, algorithms, scoring, reference, reads, seeds, cigar, data)
    csv_header = ["algorithm", "pair_idx", "score"] + (["cigar", "read", "reference"] if cigar else [])
    csv_write(outpath, data, header=csv_header)

def run_accuracy_cpu(scoring, W, O, threads, reference, reads, seeds, cigar, res_data):
    if type(threads) == list:
        threads= ",".join([str(t) for t in threads])
    else:
        threads = str(threads)

    if type(scoring) == dict:
        scoring = f"{scoring['mat']},{scoring['sub']},{scoring['gapo']},{scoring['gape']}"

    subprocess.run(["mkdir", "-p", "baseline_algorithms/wfa/build"])
    subprocess.run(["make", "-C", "baseline_algorithms/wfa/", "lib_wfa"], capture_output=True)

    compile_base = ["g++", "-o", "cpu_baseline", "-DLIB_WFA", "-Ibaseline_algorithms/wfa", "-Lbaseline_algorithms/wfa/build", "src/cpu_baseline.cpp", "src/genasm_cpu.cpp", "baseline_algorithms/ksw2/ksw2_extz.c", "baseline_algorithms/ksw2/ksw2_extz2_sse.c", "baseline_algorithms/edlib/edlib.cpp", "baseline_algorithms/gact/gact.cpp", "src/util.cpp", "-std=c++17", "-O3", "-lpthread", "-lstdc++fs", "-lwfa", "-fopenmp"]
    compile_config = ["-DCLI_KNOBS", f"-DCLI_W={W}", f"-DCLI_K={W}", f"-DCLI_O={O}"]
    compile_config += ["-DCLI_STORE_ENTRIES_NOT_EDGES"]
    compile_config += ["-DCLI_DISCARD_ENTRIES_NOT_USED_BY_TRACEBACK"]
    compile_config += ["-DCLI_EARLY_TERMINATION"]

    compile_res = subprocess.run(compile_base + compile_config, capture_output=True)
    if compile_res.returncode != 0:
        print("*"*80)
        print("compilation failed")
        print(threads, reference, reads, seeds)
        print("*"*80)
        print(compile_res.stderr)
        print(compile_res.stdout)
        return

    run_base = ["./cpu_baseline", "--verbose", "--accuracy", "--algorithms=genasm_cpu"] + (["--cigar"] if cigar else [])
    run_config = [f"--threads={threads}", f"--scoring={scoring}", f"--reference={reference}", f"--reads={reads}", f"--seeds={seeds}"]
    run_res = subprocess.run(run_base + run_config, capture_output=True)
    if run_res.returncode != 0:
        print("*"*80)
        print("run failed")
        print(threads, reference, reads, seeds)
        print("*"*80)
        print(run_res.stderr)
        print(run_res.stdout)
        return

    threads = None
    lines = run_res.stdout.split(b"\n")
    for line in lines:
        if cigar:
            match = re.search(b"^pair_idx=(\d+) score=(-?\d+) cigar=((?:\d+[=XID])*) read=([acgtnACGTN]*) reference=([acgtnACGTN]*)$", line)
            if match:
                pair_idx = match.group(1)
                score = match.group(2)
                cigar_str = match.group(3) or b""
                read_bases = match.group(4)
                reference_bases = match.group(5)
                res_data.append([W, O, int(pair_idx.decode("ascii")), int(score.decode("ascii")), cigar_str.decode("ascii"), read_bases.decode("ascii"), reference_bases.decode("ascii")])
        else:
            match = re.search(b"pair_idx=(\d+) score=(-?\d+)", line)
            if match:
                pair_idx = match.group(1)
                score = match.group(2)
                res_data.append([W, O, int(pair_idx.decode("ascii")), int(score.decode("ascii"))])

def cpu_accuracy_sweep_wo(reference, reads, seeds, cigar, outpath):
    threads = 48

    scoring = {
        'mat': 2,
        'sub': 4,
        'gapo': 4,
        'gape': 2
    }

    max_W = 256
    granularity = max(1, max_W//max_experiments)

    Ws = list(range(granularity, max_W+1, granularity))
    configs = Ws

    data = []
    for i, W in enumerate(configs):
        print(f"[{datetime.now()}] accuracy_cpu_sweep_wo {i}/{len(configs)}")
        O = min(W//2+1, W-1)
        run_accuracy_cpu(scoring, W, O, threads, reference, reads, seeds, cigar, data)

    csv_header = ["W", "O", "pair_idx", "score"] + (["cigar", "read", "reference"] if cigar else [])
    csv_write(outpath, data, header=csv_header)

def cpu_accuracy_sweep_o(reference, reads, seeds, cigar, outpath):
    threads = 48

    scoring = {
        'mat': 2,
        'sub': 4,
        'gapo': 4,
        'gape': 2
    }

    Ws = [32, 64, 96, 128]

    max_O = 128 #exclusive
    granularity = max(1, max_O//max_experiments)

    Os = list(range(0, max_O, granularity))

    configs = [(W, O) for (W, O) in product(Ws, Os) if W > O]

    data = []
    for i, (W, O) in enumerate(configs):
        print(f"[{datetime.now()}] accuracy_cpu_sweep_o {i}/{len(configs)}")
        run_accuracy_cpu(scoring, W, O, threads, reference, reads, seeds, cigar, data)

    csv_header = ["W", "O", "pair_idx", "score"] + (["cigar", "read", "reference"] if cigar else [])
    csv_write(outpath, data, header=csv_header)

def profile_accuracy_cpu(algorithms, reference, reads, seeds, dataset_name, cigar=False):
    cigar_filename_str = "_cigar" if cigar else ""
    all_accuracy(algorithms, reference, reads, seeds, cigar, out_dir/f"{dataset_name}_all_accuracy{cigar_filename_str}.csv")
    if 'genasm_cpu' in algorithms:
        cpu_accuracy_sweep_wo(reference, reads, seeds, cigar, out_dir/f"{dataset_name}_cpu_accuracy_sweep_wo{cigar_filename_str}.csv")
        cpu_accuracy_sweep_o(reference, reads, seeds, cigar, out_dir/f"{dataset_name}_cpu_accuracy_sweep_o{cigar_filename_str}.csv")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Profile gpu/cpu implementation or cpu baselines")
    parser.add_argument("target", type=str, choices=["cpu", "gpu", "cpu_baselines", "gpu_baselines", "accuracy_cpu"])
    parser.add_argument("dataset", type=str, help="Name of a subdirectory of datasets_dir. Must contain reference.fasta, reads.fastq and candidates.[paf,maf]")
    parser.add_argument("--arch", type=str, help="gpu architecture, e.g. sm_70, mandatory when 'gpu' is specified")
    parser.add_argument("--tmp_dir", type=str, help="directory to temporarily store inputs for baseline algorithms that require custom input formats, e.g. darwin_gpu")
    cpu_algs = ["edlib", "ksw2_extz", "ksw2_extz2_sse", "genasm_cpu", "wfa_lm", "wfa_adaptive", "wfa_exact", "custom_gact"]
    parser.add_argument("--algorithms", type=str, nargs='+', choices=cpu_algs, default=cpu_algs, help="cpu baseline algorithms to run (required if target is 'cpu_baselines')")
    parser.add_argument("--cigar", action="store_true", help="get cigar strings in accuracy measurement")
    parser.add_argument("--override_W", type=int, default=None)
    parser.add_argument("--datasets_dir", type=Path, default=Path("datasets"))
    parser.add_argument("--profile_dir", type=Path, default=Path("profile"))
    parser.add_argument("--repetitions", type=int, default=1)
    args = parser.parse_args()

    dataset_dirs = [d for d in args.datasets_dir.iterdir() if d.is_dir()] if args.datasets_dir.is_dir() else []
    datasets = [d.name for d in dataset_dirs]
    if not datasets:
        print(f"datasets_dir='{args.datasets_dir}' is missing or empty, please refer to README.md on how to obtain datasets")
        exit(1)

    if args.dataset not in datasets:
        print(f"could not find dataset '{args.dataset}' in datasets_dir='{args.datasets_dir}'")
        print(f"available are:")
        for dataset in datasets:
            print(f"'{dataset}'")
        exit(1)

    dataset_path = args.datasets_dir / args.dataset

    reference_path:Path = dataset_path / 'reference.fasta'
    if not reference_path.exists():
        print(f"could not find 'reference.fasta' in dataset '{args.dataset}'")
        exit(1)

    reads_path:Path = dataset_path / 'reads.fastq'
    if not reads_path.exists():
        print(f"could not find 'reads.fastq' in dataset '{args.dataset}'")
        exit(1)

    candidates_path_paf:Path = dataset_path / 'candidates.paf'
    candidates_path_maf:Path = dataset_path / 'candidates.maf'
    if candidates_path_paf.exists():
        candidates_path = candidates_path_paf
    elif candidates_path_maf.exists():
        candidates_path = candidates_path_maf
    else:
        print(f"could not find 'candidates.[maf,paf]' in dataset '{args.dataset}'")
        exit(1)

    print('server:', platform.node())
    print('cpu:', platform.processor())

    out_dir = args.profile_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    max_experiments = 16
    override_W = args.override_W

    if args.target=="cpu":
        profile_cpu(reference_path, reads_path, candidates_path, args.dataset, args.repetitions)
    if args.target=="gpu":
        profile_gpu(reference_path, reads_path, candidates_path, args.dataset, args.arch, args.repetitions)
    if args.target=="cpu_baselines":
        profile_cpu_baselines(args.algorithms, reference_path, reads_path, candidates_path, args.dataset, args.repetitions)
    if args.target=="gpu_baselines":
        assert(args.tmp_dir != None)
        profile_gpu_baselines(reference_path, reads_path, candidates_path, args.dataset, args.arch, args.tmp_dir, args.repetitions)
    if args.target=="accuracy_cpu":
        profile_accuracy_cpu(args.algorithms, reference_path, reads_path, candidates_path, args.dataset, cigar=args.cigar)
