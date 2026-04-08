import argparse
import itertools
import subprocess
import pandas
import re

from pandas import DataFrame
from math import ceil
from pathlib import Path

KIBI = 1_024
MEBI = 1_024 * KIBI
GIBI = 1_024 * MEBI

project_dir = Path(__file__).parent.parent

def ensure_cacti():
    cacti_path = project_dir / 'cacti'
    if not cacti_path.exists():
        print('cacti was missing, cloning from github')
        res = subprocess.run(['git', 'clone', 'https://github.com/HewlettPackard/cacti'])
        res.check_returncode()

    cacti_executable_path = cacti_path / 'cacti'
    if not cacti_executable_path.exists():
        print('cacti executable was missing, compiling')
        res = subprocess.run(['make'], cwd=cacti_path)
        res.check_returncode()

    return cacti_executable_path

def prepare_cacti_cfg(bits, bits_per_cycle):
    template_path = project_dir / 'scripts' / 'scrooge_tb.cfg.template'
    with open(template_path, 'rb') as f:
        template_bytes = f.read()

    capacity_bytes = str(int(ceil(bits/8))).encode('ascii')
    bandwidth_bits = str(bits_per_cycle).encode('ascii')
    cfg = template_bytes\
        .replace(b'<COLUMN_CAPACITY_BYTES>', capacity_bytes)\
        .replace(b'<BITS_PER_CYCLE>', bandwidth_bits)

    cfg_path = project_dir / 'cacti' / 'scrooge_tb.cfg'
    with open(cfg_path, 'wb') as f:
        f.write(cfg)

    return cfg_path

def cacti_area(bits, bits_per_cycle):
    cacti_path = project_dir / 'cacti'
    cfg_path = prepare_cacti_cfg(bits, bits_per_cycle)
    res = subprocess.run(['./cacti', '-infile', cfg_path], cwd=cacti_path, capture_output=True)
    res.check_returncode()

    match = re.search(b'Data array: Area \(mm2\): (\d+\.\d+)', res.stdout)
    if match:
        area_bytes = match.group(1)
        return float(area_bytes.decode('ascii'))
    else:
        raise Exception(f'cacti failed to find a valid configuration for bits={bits} bits_per_cycle={bits_per_cycle}')

def cacti_energy(bits, bits_per_cycle):
    cacti_path = project_dir / 'cacti'
    cfg_path = prepare_cacti_cfg(bits, bits_per_cycle)
    res = subprocess.run(['./cacti', '-infile', cfg_path], cwd=cacti_path, capture_output=True)
    res.check_returncode()

    match = re.search(b'Data array: Total dynamic read energy/access\s+\(nJ\): (\d+\.\d+)', res.stdout)
    if match:
        energy_bytes = match.group(1)
        return float(energy_bytes.decode('ascii'))
    else:
        raise Exception(f'cacti failed to find a valid configuration for bits={bits} bits_per_cycle={bits_per_cycle}')

def single_window_latency(W, O, processing_elements):
    #a window consists of multiple 'blocks' if W>processing_elements
    dc_cycles_per_block = W*2 + 1
    tb_cycles = W - O
    blocks_per_window = int(ceil(W/processing_elements))
    cycles_per_window = dc_cycles_per_block * blocks_per_window + tb_cycles
    
    return cycles_per_window

def latency(sequence_length, W, O, processing_elements):
    #all numbers are for a single vault
    progress_per_window = W - O
    windows_per_sequence = int(ceil(sequence_length/(progress_per_window)))
    cycles_per_sequence = single_window_latency(W, O, processing_elements) * windows_per_sequence

    return cycles_per_sequence

def throughput(sequence_length, W, O, processing_elements, frequency):
    #all numbers are for a single vault
    cycles = latency(sequence_length, W, O, processing_elements)
    alns_per_cycle = 1/cycles
    alns_per_second = alns_per_cycle * frequency
    return alns_per_second

def full_original_genasm_throughput(sequence_length):
    W, O = 64, 24
    PEs, vaults, frequency = 64, 32, 1_000_000_000
    return vaults*throughput(sequence_length, W, O, PEs, frequency)

def dc_bytes(W):
    #all numbers are for a single vault
    genasm_W = 64
    genasm_dc_SRAM = 8 * KIBI #ask Damla why so much
    dc_SRAM_per_char = genasm_dc_SRAM / genasm_W
    return dc_SRAM_per_char * W

def tb_memory(W, O, sene, dent):
    #all numbers are for a single vault
    if not sene and not dent:
        bits_per_bitvector = W
        bitvectors_per_entry = 3
        rows = W
        columns = W
    if sene and not dent:
        bits_per_bitvector = W
        bitvectors_per_entry = 1
        rows = W+1
        columns = W
    if dent and not sene:
        bits_per_bitvector = W-O
        bitvectors_per_entry = 3
        rows = W
        columns = W-O
    if sene and dent:
        bits_per_bitvector = min(W-O+1, W)
        bitvectors_per_entry = 1
        rows = W+1
        columns = min(W-O+1, W)

    bandwidth_per_column = bits_per_bitvector * bitvectors_per_entry
    bits_per_column = bits_per_bitvector * bitvectors_per_entry * rows
    return (columns, bits_per_column, bandwidth_per_column)

def tb_memory_accesses(W, O, sene, dent):
    if not sene and not dent:
        rows = W
        columns = W
    if sene and not dent:
        rows = W+1
        columns = W
    if dent and not sene:
        rows = W
        columns = W-O
    if sene and dent:
        rows = W+1
        columns = min(W-O+1, W)
    dc_writes = rows * columns

    if sene:
        accesses_per_tb_step = 3
    else:
        accesses_per_tb_step = 1
    tb_steps = W - O
    tb_reads = tb_steps * accesses_per_tb_step

    return dc_writes + tb_reads

def area(W, O, processing_elements, sene, dent, tb_sram_model):
    #all numbers are for a single vault
    genasm_PEs = 64
    genasm_dc_SRAM = 8 * KIBI
    genasm_tb_SRAM = 96 * KIBI
    genasm_dc_logic_area = 0.049
    genasm_tb_logic_area = 0.016
    genasm_dc_SRAM_area  = 0.013
    genasm_tb_SRAM_area  = 0.256

    dc_logic_area_per_PE = genasm_dc_logic_area / genasm_PEs
    dc_SRAM_area_per_byte = genasm_dc_SRAM_area / genasm_dc_SRAM
    tb_SRAM_area_per_byte = genasm_tb_SRAM_area / genasm_tb_SRAM

    dc_logic_area = processing_elements * dc_logic_area_per_PE
    dc_SRAM_area = dc_bytes(W) * dc_SRAM_area_per_byte
    tb_logic_area = genasm_tb_logic_area + (dc_logic_area_per_PE if sene else 0)
    if tb_sram_model == 'cacti':
        tb_columns, tb_bits_per_column, tb_bandwidth_per_column = tb_memory(W, O, sene, dent)
        tb_SRAM_area = cacti_area(tb_bits_per_column, tb_bandwidth_per_column) * tb_columns
    else:
        tb_columns, tb_bits_per_column, tb_bandwidth_per_column = tb_memory(W, O, sene, dent)
        tb_bits =  tb_columns * tb_bits_per_column
        tb_bytes = int(ceil(tb_bits / 8))
        tb_SRAM_area = tb_bytes * tb_SRAM_area_per_byte

    return (dc_logic_area, tb_logic_area, dc_SRAM_area, tb_SRAM_area)

def power(W, O, processing_elements, sene, dent, tb_sram_model, frequency):
    #all numbers are for a single vault
    genasm_PEs = 64
    genasm_dc_SRAM = 8 * KIBI
    genasm_tb_SRAM = 96 * KIBI
    genasm_dc_logic_power = 0.033 #W
    genasm_tb_logic_power = 0.004 #W
    genasm_dc_SRAM_power  = 0.009 #W
    genasm_tb_SRAM_power  = 0.055 #W

    dc_logic_power_per_PE = genasm_dc_logic_power / genasm_PEs
    dc_SRAM_power_per_byte = genasm_dc_SRAM_power / genasm_dc_SRAM
    tb_SRAM_power_per_byte = genasm_tb_SRAM_power / genasm_tb_SRAM

    dc_logic_power = processing_elements * dc_logic_power_per_PE
    dc_SRAM_power = dc_bytes(W) * dc_SRAM_power_per_byte
    tb_logic_power = genasm_tb_logic_power + (dc_logic_power_per_PE if sene else 0)
    if tb_sram_model == 'cacti':
        tb_columns, tb_bits_per_column, tb_bandwidth_per_column = tb_memory(W, O, sene, dent)
        tb_SRAM_energy_per_access = cacti_energy(tb_bits_per_column, tb_bandwidth_per_column)
        tb_SRAM_energy_per_window = tb_memory_accesses(W, O, sene, dent) * tb_SRAM_energy_per_access #nano Joules, nJ
        cycles_per_window = single_window_latency(W, O, processing_elements)
        time_per_window = cycles_per_window / frequency #seconds
        tb_SRAM_power = tb_SRAM_energy_per_window / (time_per_window * 1_000_000_000) #nJ / ns = W
    else:
        tb_columns, tb_bits_per_column, tb_bandwidth_per_column = tb_memory(W, O, sene, dent)
        tb_bits =  tb_columns * tb_bits_per_column
        tb_bytes = int(ceil(tb_bits / 8))
        tb_SRAM_power = tb_bytes * tb_SRAM_power_per_byte

    return (dc_logic_power, tb_logic_power, dc_SRAM_power, tb_SRAM_power)

def print_improvements(tb_sram_model):
    frequency = 1_000_000_000
    genasm_area = area(64, 33, 64, False, False, tb_sram_model)
    scrooge_area =area(64, 33, 64, True, True, tb_sram_model)
    print(f'GenASM Area: {sum(genasm_area):.3f}mm^2')
    print(f' - DC Logic: {genasm_area[0]:.3f}mm^2')
    print(f' - TB Logic: {genasm_area[1]:.3f}mm^2')
    print(f' - DC SRAM:  {genasm_area[2]:.3f}mm^2')
    print(f' - TB SRAM:  {genasm_area[3]:.3f}mm^2')
    print(f'Scrooge Area: {sum(scrooge_area):.3f}mm^2')
    print(f' - DC Logic: {scrooge_area[0]:.3f}mm^2')
    print(f' - TB Logic: {scrooge_area[1]:.3f}mm^2')
    print(f' - DC SRAM:  {scrooge_area[2]:.3f}mm^2')
    print(f' - TB SRAM:  {scrooge_area[3]:.3f}mm^2')
    print(f'Area Improvement: {sum(genasm_area)/sum(scrooge_area):.3f}x')
    print()

    genasm_power =  power(64, 33, 64, False, False, tb_sram_model, frequency)
    scrooge_power = power(64, 33, 64, True, True, tb_sram_model, frequency)
    print(f'GenASM Power: {sum(genasm_power):.3f}W')
    print(f' - DC Logic: {genasm_power[0]:.3f}W')
    print(f' - TB Logic: {genasm_power[1]:.3f}W')
    print(f' - DC SRAM:  {genasm_power[2]:.3f}W')
    print(f' - TB SRAM:  {genasm_power[3]:.3f}W')
    print(f'Scrooge Power: {sum(scrooge_power):.3f}W')
    print(f' - DC Logic: {scrooge_power[0]:.3f}W')
    print(f' - TB Logic: {scrooge_power[1]:.3f}W')
    print(f' - DC SRAM:  {scrooge_power[2]:.3f}W')
    print(f' - TB SRAM:  {scrooge_power[3]:.3f}W')
    print(f'Power Improvement: {sum(genasm_power)/sum(scrooge_power):.3f}x')
    print()

def sweep(tb_sram_model):
    sequence_length = 10_000
    frequency = 1_000_000_000
    Ws = [64] #list(range(1, 128+1))
    Os = list(range(0, 128))
    bools = [False, True]
    configs = list(itertools.product(Ws, Os, bools, bools))

    df = DataFrame(columns=['sequence_length', 'W', 'O', 'processing_elements', 'frequency', 'vaults', 'sene', 'dent', 'throughput', 'area', 'dc_logic_area', 'tb_logic_area', 'dc_sram_area', 'tb_sram_area', 'power', 'dc_logic_power', 'tb_logic_power', 'dc_sram_power', 'tb_sram_power', 'tb_sram_capacity', 'tb_sram_columns', 'tb_sram_bits_per_column', 'tb_sram_bandwidth_per_column'])
    for W, O, sene, dent in configs:
        if O >= W: continue
        processing_elements = W
        try:
            t = throughput(10_000, W, O, processing_elements, frequency)
            a = area(W, O, processing_elements, sene, dent, tb_sram_model)
            p = power(W, O, processing_elements, sene, dent, tb_sram_model, frequency)
            dc_logic_area, tb_logic_area, dc_sram_area, tb_sram_area = a
            dc_logic_power, tb_logic_power, dc_sram_power, tb_sram_power = p
            tb_sram_columns, tb_sram_bits_per_column, tb_sram_bandwidth_per_column = tb_memory(W, O, sene, dent)
            tb_sram_capacity = tb_sram_columns * int(ceil(tb_sram_bits_per_column / 8))
        except Exception as error:
            print(f'W={W} O={O} sene={sene} dent={dent} failed: {str(error)}')
            continue

        entry = [
            sequence_length,
            W,
            O,
            processing_elements,
            frequency,
            1,
            sene,
            dent,
            t,
            sum(a), dc_logic_area, tb_logic_area, dc_sram_area, tb_sram_area,
            sum(p), dc_logic_power, tb_logic_power, dc_sram_power, tb_sram_power,
            tb_sram_capacity, tb_sram_columns, tb_sram_bits_per_column, tb_sram_bandwidth_per_column
        ]

        df = pandas.concat([df, DataFrame([entry], columns=df.columns)])

    df.to_csv('profile/asic_numbers.csv', index=False)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    modes = ['improvements', 'sweep']
    parser.add_argument('mode', type=str, nargs='?', default='improvements', choices=modes)
    parser.add_argument('--cacti', action='store_true', help='use cacti to obtain TB SRAM area and power, instead of back-of-the-envelope model')
    args = parser.parse_args()

    tb_sram_model = 'cacti' if args.cacti else 'back-of-the-envelope'
    if tb_sram_model == 'cacti':
        cacti_executable_path = ensure_cacti()

        print(cacti_executable_path)

    if args.mode == 'improvements':
        print_improvements(tb_sram_model)
    if args.mode == 'sweep':
        sweep(tb_sram_model)
