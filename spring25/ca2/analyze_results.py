#!/usr/bin/env python3
"""
analyze_results.py

Parses procsim output files in ./outputs, computes IPC, generates plots
(IPC vs each parameter) with a fixed baseline, and selects the minimal
hardware configuration (k0+k1+k2+R) achieving ≥95% of max IPC per trace & F.

Requires:
  - Python 3
  - pandas
  - matplotlib

Usage:
  $ python3 analyze_results.py
"""

import os
import re
import glob
import pandas as pd
import matplotlib.pyplot as plt

OUTPUT_DIR = "./outputs"
FIGURE_DIR = "./figures"
os.makedirs(FIGURE_DIR, exist_ok=True)

# Baseline configuration for sweeping parameters
BASELINE = {
    'k0': 2,
    'k1': 2,
    'k2': 2,
    'R':  2,
    'F':  4
}

def parse_output_file(path):
    """
    Extract parameters and IPC from a single output file.
    Expected filename: <trace>_j<k0>_k<k1>_l<k2>_r<R>_f<F>.out
    """
    fname = os.path.basename(path)
    m = re.match(r'(?P<trace>.+)_j(?P<k0>\d+)_k(?P<k1>\d+)_l(?P<k2>\d+)'
                 r'_r(?P<R>\d+)_f(?P<F>\d+)\.out', fname)
    if not m:
        return None
    params = m.groupdict()
    # Convert numeric fields
    for key in ['k0','k1','k2','R','F']:
        params[key] = int(params[key])
    # Extract IPC from the end of the file
    ipc = None
    with open(path, 'r') as f:
        for line in f:
            if 'Avg inst retired per cycle' in line:
                ipc = float(line.split(':')[1].strip())
                break
    if ipc is None:
        return None
    params['IPC'] = ipc
    return params

def load_data(output_dir):
    """Load all .out files into a pandas DataFrame."""
    records = []
    for filepath in glob.glob(os.path.join(output_dir, "*.out")):
        rec = parse_output_file(filepath)
        if rec:
            records.append(rec)
    df = pd.DataFrame(records)
    return df

def plot_vs_param_fixed_baseline(df, param):
    """
    Plot IPC vs 'param', holding other parameters fixed at BASELINE values.
    One line per trace, saved to figures/ipc_vs_<param>.png.
    """
    others = [p for p in BASELINE if p != param]
    plt.figure()
    for trace, sub in df.groupby('trace'):
        # For each value of param, filter on baseline of others
        xs = sorted(sub[param].unique())
        ys = []
        for x in xs:
            cond = (sub[param] == x)
            for o in others:
                cond &= (sub[o] == BASELINE[o])
            row = sub[cond]
            if not row.empty:
                ys.append(row.iloc[0]['IPC'])
            else:
                ys.append(float('nan'))
        plt.plot(xs, ys, marker='o', label=trace)
    plt.xlabel(param)
    plt.ylabel("IPC")
    base_desc = ', '.join(f"{o}={BASELINE[o]}" for o in others)
    plt.title(f"IPC vs {param}  (baseline: {base_desc})")
    plt.legend(title="Trace")
    plt.tight_layout()
    plt.savefig(os.path.join(FIGURE_DIR, f"ipc_vs_{param}.png"))
    plt.close()

def find_minimal_configs(df):
    """
    For each trace and each fetch rate F, find the minimal hardware cost
    achieving ≥95% of max IPC for that (trace, F).
    Hardware cost = k0 + k1 + k2 + R.
    """
    results = []
    for (trace, F), sub in df.groupby(['trace', 'F']):
        max_ipc = sub['IPC'].max()
        threshold = 0.95 * max_ipc
        # Candidates meeting threshold
        cand = sub[sub['IPC'] >= threshold].copy()
        # Compute hardware cost
        cand['cost'] = cand['k0'] + cand['k1'] + cand['k2'] + cand['R']
        # Choose minimal cost; tie-break by lower R then lower k0, k1, k2
        cand_sorted = cand.sort_values(['cost', 'R', 'k0', 'k1', 'k2'])
        best = cand_sorted.iloc[0].to_dict()
        best['max_ipc'] = max_ipc
        results.append(best)
    return pd.DataFrame(results)

def main():
    df = load_data(OUTPUT_DIR)
    if df.empty:
        print("No output files found in", OUTPUT_DIR)
        return
    print(f"Loaded {len(df)} records.")

    # Generate plots for each parameter
    for param in ['k0','k1','k2','R','F']:
        print(f"Plotting IPC vs {param}...")
        plot_vs_param_fixed_baseline(df, param)

    # Find and print minimal hardware configs for ≥95% IPC
    best_df = find_minimal_configs(df)
    print("\nRecommended minimal hardware per trace & fetch rate (≥95% of max IPC):")
    for _, row in best_df.iterrows():
        print(f"Trace={row['trace']}, F={row['F']}:")
        print(f"  Max IPC = {row['max_ipc']:.3f}")
        print(f"  Chosen config: k0={row['k0']}, k1={row['k1']}, "
              f"k2={row['k2']}, R={row['R']} (cost={row['cost']}) "
              f"→ IPC={row['IPC']:.3f}")
    # Save the recommendation table
    best_df.to_csv(os.path.join(FIGURE_DIR, "minimal_configs.csv"), index=False)

if __name__ == "__main__":
    main()

