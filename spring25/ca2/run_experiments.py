#!/usr/bin/env python3
import os
import subprocess
import itertools

# Directory containing your trace files
traces_dir = "./traces"
# Directory where outputs will be written
output_dir = "./outputs"
os.makedirs(output_dir, exist_ok=True)

# Parameter values to sweep
k0_values = [1, 2]         # Number of k0 FUs
k1_values = [1, 2]         # Number of k1 FUs
k2_values = [1, 2]         # Number of k2 FUs
fetch_rates = [4, 8]       # Fetch rate F
result_buses = [1, 2, 4, 8] # Number of result buses R; adjust as needed

# Path to your simulator executable
sim_executable = "./procsim"

# Find all .trace files in the traces directory
trace_files = [f for f in os.listdir(traces_dir) if f.endswith(".trace")]

# Loop over each trace and each combination of parameters
for trace_name in trace_files:
    trace_path = os.path.join(traces_dir, trace_name)
    base_name = os.path.splitext(trace_name)[0]
    for k0, k1, k2, R, F in itertools.product(k0_values, k1_values, k2_values, result_buses, fetch_rates):
        # Construct an output filename reflecting the configuration
        out_filename = f"{base_name}_j{k0}_k{k1}_l{k2}_r{R}_f{F}.out"
        out_path = os.path.join(output_dir, out_filename)

        # Build the command-line invocation
        cmd = [
            sim_executable,
            "-r", str(R),
            "-j", str(k0),
            "-k", str(k1),
            "-l", str(k2),
            "-f", str(F)
        ]

        # Run the simulator, feeding the trace file on stdin,
        # and capturing both stdout and stderr to the output file.
        with open(trace_path, "r") as trace_fh, open(out_path, "w") as out_fh:
            subprocess.run(cmd, stdin=trace_fh, stdout=out_fh, stderr=subprocess.STDOUT)

        print(f"Completed: {trace_name} [j={k0}, k={k1}, l={k2}, r={R}, f={F}] → {out_filename}")
