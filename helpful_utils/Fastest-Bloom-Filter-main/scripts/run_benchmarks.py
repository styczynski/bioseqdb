#!/usr/bin/env python3

import subprocess
import os
import argparse
from typing import List

def run_benchmark(executable: str, key_power: int, bits_per_key: int, lookup_power: int, output_dir: str) -> None:
    """Run a single benchmark with the given parameters and save the output."""
    output_file = f"{output_dir}/keys_{key_power}_bits_{bits_per_key}_lookups_{lookup_power}.txt"
    
    print(f"Running benchmark with: keys=2^{key_power}, bits={bits_per_key}, lookups=2^{lookup_power}")
    
    # Run the benchmark
    cmd = [executable, str(key_power), str(bits_per_key), str(lookup_power)]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        
        # Write output to file
        with open(output_file, 'w') as f:
            f.write(result.stdout)
        
        print(f"Results saved to {output_file}")
    except subprocess.CalledProcessError as e:
        print(f"Error running benchmark: {e}")
        print(f"Command output: {e.stdout}")
        print(f"Error output: {e.stderr}")

def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description="Run Bloom Filter benchmarks with various parameters")
    parser.add_argument("--executable", default="./build/main_benchmark", help="Path to the benchmark executable")
    parser.add_argument("--output-dir", default="benchmark_results", help="Directory to save results")
    args = parser.parse_args()
    
    # Create output directory if it doesn't exist
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Define parameter ranges
    num_keys_powers = [12, 14, 16, 18]   
    bits_per_key = [32]         
    lookup_powers = [24]
    
    print("Starting benchmarks...")
    
    # Run benchmarks with all parameter combinations
    for key_power in num_keys_powers:
        for bits in bits_per_key:
            for lookup_power in lookup_powers:
                run_benchmark(args.executable, key_power, bits, lookup_power, args.output_dir)
    
    print("All benchmarks completed!")

if __name__ == "__main__":
    main()