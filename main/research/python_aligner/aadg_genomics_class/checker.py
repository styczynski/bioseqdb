import sys
import csv
import os
import subprocess
from threading import Thread
import psutil
import time
import json
from collections import defaultdict

def process_watcher(pid, samples, samples_max_len):
    retries = 0
    i = 0
    while True:
        try:
            while not psutil.pid_exists(pid):
                time.sleep(0.2)
                retries += 1
                if retries > 5 and i > 0:
                    return
            process = psutil.Process(pid)
            mem_usage = process.memory_info().rss
            samples[i] = mem_usage
            i += 1
            if i >= samples_max_len:
                return
            else:
                time.sleep(0.009)
        except Exception:
            return

def compute_stats():
    stats = {'solution': {'small_reads2': {'name': 'read_lis', 't_samples_count': 100, 't_min': 1.18, 't_max': 14.0, 't_avg': 1.52, 't_total': 152.66}}}
    print(stats)

def run_aligners():
    track_data = defaultdict()
    for (program_label, program_name) in [
        ("bwt-fragment-02p01", "v10_bwt_params_02p01"),
        ("bwt-fragment-03p006", "v10_bwt_params_03p006"),
        ("bwt-fragment-03p008", "v10_bwt_params_03p008"),
        ("solution", "v10"),
        ("bwt-dp-numpy", "v10_bwt_dp_numpy"),
        ("bwt-dp-lists", "v10_bwt_lists"),
    ]:
        track_data[program_label] = defaultdict()
        for (case_name, reference_path, reads_path, expected_output_path) in [
            ("small_reads0", "./data2/reference.fasta", "data2/reads0.fasta", "data/reads0.txt"),
            ("small_reads1", "./data2/reference.fasta", "data2/reads1.fasta", "data/reads1.txt"),
            ("small_reads2", "./data2/reference.fasta", "data2/reads2.fasta", "data/reads2.txt"),
            ("20Ma", "./data_big/reference20M.fasta", "data_big/reads20Ma.fasta", "data_big/reads20Ma.txt"),
            ("20Mb", "./data_big/reference20M.fasta", "data_big/reads20Mb.fasta", "data_big/reads20Mb.txt"),
        ]:
            time.sleep(1)
            lines = []
            global_retry = 0
            while True:
                try:
                    track_data[program_label][case_name] = defaultdict()
                    #devnull = open(os.devnull, 'wb') # Use this in Python < 3.3
                    # Python >= 3.3 has subprocess.DEVNULL
                    print(f"{'' if global_retry == 0 else '[Retry='+str(global_retry)+'] '}Running [{program_label}] for case [{case_name}]")
                    lines = []
                    t = None
                    samples = [-1] * 100000
                    with subprocess.Popen(" ".join(["python3", f"aadg_genomics_class/{program_name}.py", reference_path, reads_path]), stdout=subprocess.PIPE, bufsize=1, universal_newlines=True, shell=True) as process:
                        t = Thread(target=process_watcher, args=(process.pid, samples, len(samples)), daemon=True).start()
                        while True:
                            line_chunk = process.stdout.readline()
                            if not line_chunk:
                                break
                            for line in line_chunk.split("\n"):
                                l = line.replace("\n", "").replace("\r", "").strip()
                                if len(l) > 0:
                                    if l.startswith("TRACK"):
                                        lines.append(l)
                                    else:
                                        print(f" |  {l}")
                    if t:
                        t.join()
                    
                    lines = [line.strip() for line in "\n".join(lines).split("\n") if len(line.strip()) > 0]
                    break
                except Exception as e:
                    global_retry += 1
                    if global_retry > 5:
                        raise e
                    continue
            for line in lines:
                if line.startswith("TRACK"):
                    track_data_part = json.loads(line.replace("TRACK", "").strip())
                    metric_name = track_data_part["name"]
                    del track_data_part["name"]
                    track_data[program_label][case_name][metric_name] = track_data_part
            track_data[program_label][case_name]["memory"] = dict() # TODO: FIX!!!
    track_data = {k : {k2 : {k3 : v3 for k3, v3 in v2.items()} for k2, v2 in v.items()} for k, v in track_data.items()}
    with open('./presentation/data1.json', 'w') as outf:
        json.dump(track_data, outf) 
        print(track_data)

if __name__ == '__main__':
    #run_aligners()
    run_aligners()