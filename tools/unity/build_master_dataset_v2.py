#!/usr/bin/env python3
"""
build_master_dataset.py
Assembles a master_dataset.csv from NATIG Unity run outputs.

v2.0 UPDATE (2026-08-24): extended for the 12-scenario, 4-protocol production
array (natig_array_v2.sh). Key changes from the v1.0-era version:
  - Directory-name parsing now uses an explicit whitelist of known v2.0
    scenario tags instead of a regex guess. This is deliberate: a generic
    regex would either fail on hyphenated tags (e.g. "normal-dnp3") or,
    if loosened, risk silently pulling in stale non-v2.0 debug directories
    left over from earlier smoke-testing (run_ddos_0, run_mim_0, etc.) with
    mismatched semantics. Only directories matching a known v2.0 tag exactly
    are included.
  - New `protocol` column (dnp3/modbus/mms/goose) — v1.0 was DNP3-only, so
    this dimension didn't previously exist.
  - New attack-type label scheme: normal=0, fdi=1, replay=2, slowddos=3.
    This is intentionally DIFFERENT from v1.0's old ddos=1/mim=2/combined=3
    scheme — the attack taxonomy itself changed, and reusing the same
    integer codes for different meanings would be a silent correctness trap
    for anyone comparing v1.0 and v2.0 label columns later.
  - PORT_MAP required NO changes: verified directly against real captured
    TP.txt data that DNP3, Modbus, and MMS all use the same port scheme
    (mg0_out-mg3_out at 40000-40003; DNP3 additionally uses sub_out/20000
    and dnp3_0-2/49153-49155 for its extra Monitor* endpoints, which
    Modbus/MMS don't have equivalents of — those columns are legitimately
    zero for Modbus/MMS rows, not a bug).
  - GOOSE is explicitly EXCLUDED from TP.txt-based processing. Confirmed via
    a real full-duration run (normal-goose_00: 459 accepted application-layer
    frames, reached t=299.018s) that TP.txt stays empty (0 rows) regardless
    of genuine traffic volume — ns-3's FlowMonitor/Ipv4FlowClassifier appears
    unable to track flows routed via SetDefaultMulticastRoute (multicast),
    even though both publisher and subscriber nodes are correctly covered by
    flowHelper.Install(). This is a real ns-3 limitation, not a bug in our
    topology or config. GOOSE scenarios need a separate, future processing
    path (e.g. parsing "accepted" log lines from the run log directly) —
    out of scope for this script's TP.txt-based pipeline for now.

v2.1 UPDATE (2026-09-27): multi-column labels + robust path mapping.
  - PATH MAPPING is now PER-PROTOCOL and keyed on the (src,dst) address PAIR,
    built from each protocol's own normal run. The old PORT-based map was
    wrong: ns-3 reassigns the outstation source port non-deterministically, so
    49153/49155 swap between 10.1.8.2 and 10.1.16.2 across runs, misattributing
    dnp3_0/dnp3_2. Source ADDRESS is stable. Per-protocol role maps (FLOW_MAPS):
      dnp3   (8 flows): sub_out=(10.1.4.2,11.2.0.1); mgN_out=(11.{2,3,4,5}.0.1,10.1.4.2);
                        dnp3_0/1/2=((10.1.8.2,11.3.0.1),(10.1.12.2,11.4.0.1),(10.1.16.2,11.5.0.1))
      modbus (4 flows): mgN_out=((11.2.0.1,10.1.4.2),(11.3.0.1,10.1.8.2),
                        (11.4.0.1,10.1.12.2),(11.5.0.1,10.1.16.2))
      mms    (4 flows): same flow set as modbus (from run_normal-mms_00).
                        NOTE: current-build MMS TP.txt differs from the older
                        production MMS runs (normal/fdi both 5e6989ca old vs
                        c1b815e1 new); MMS FDI is invisible in TP.txt (normal==fdi)
                        in both builds. Old MMS runs need rerunning to match this
                        build before MMS rows are mixed into the dataset.
    Cross-protocol analysis should use the aggregate columns, not per-path.
  - New label columns: `infra_present` (run-level: network-altering attack
    infrastructure) and `attack_active` (per-row: row time inside the attack
    window read from THIS run's own grid.json). Window source: MIM if
    includeMIM, else FDI if includeFDI, else SlowDDoS if SlowDDoS.Active.
    infra_present = 1 if includeMIM OR SlowDDoS.Active, else 0 (FDI's
    compromised endpoint adds no network infra). The slow-DDoS =1 is empirical:
    normal-modbus vs slowddos-modbus diverge in packet/byte COUNTS well before
    the 60s window (flow 40000: 484 vs 322 tx_pkts by t=60), i.e. the bot fleet
    (present from t=0) perturbs the network, not just RNG-level delay/jitter.
  - New per-interval delta features alongside the cumulative counters:
    <path>_{tx_bytes,rx_bytes,tx_pkts,rx_pkts}_delta, plus <path>_delivery_ratio
    (rx_pkts_delta/tx_pkts_delta, 0 when no tx) and <path>_interval_mean_delay
    (interval mean recovered from the cumulative-mean delay column).

KNOWN ISSUES (not fixed here):
  - Slow-DDoS attack does not currently land: with endPoint="MIM" the Slowloris
    bots target an idle MIM node (ns3-modbus-helics-grid.cc ~908-911 resolve the
    victim to MIMNode.Get(NodeID); ~1384 connects to it on the service `port`),
    which has no listener when includeMIM=0 — so all bot connections fail (log:
    "SlowlorisBotApplication: connection attempt failed", 8 bots x 20 conns).
    slowddos runs therefore carry the infra_present signal but no effective
    flood; attack_active marks the 60-180 window but in-window traffic impact is
    minimal. Fix/rerun is a separate decision.
  - GOOSE still excluded (empty TP.txt); infra_present/attack_active apply to
    dnp3/modbus/mms rows only.

Usage:
    python3 build_master_dataset.py \
        --runs_dir /project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/runs \
        --out /project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/master_dataset_v2.csv \
        --seed 42
"""

import argparse
import os
import glob
import json
import random
import numpy as np
import pandas as pd

# ── Column schema ──────────────────────────────────────────────────────────
PATH_NAMES = ['sub_out', 'mg0_out', 'mg1_out', 'mg2_out', 'mg3_out',
              'dnp3_0', 'dnp3_1', 'dnp3_2']

# Per-protocol (src_addr, dst_addr) -> role name. Built from each protocol's own
# normal run. Keyed on the address PAIR (stable), NOT source port (ns-3 reassigns
# it non-deterministically across runs). See module docstring (v2.1). MMS TBD.
FLOW_MAPS = {
    'dnp3': {
        ('10.1.4.2', '11.2.0.1'): 'sub_out',
        ('11.2.0.1', '10.1.4.2'): 'mg0_out',
        ('11.3.0.1', '10.1.4.2'): 'mg1_out',
        ('11.4.0.1', '10.1.4.2'): 'mg2_out',
        ('11.5.0.1', '10.1.4.2'): 'mg3_out',
        ('10.1.8.2', '11.3.0.1'): 'dnp3_0',
        ('10.1.12.2', '11.4.0.1'): 'dnp3_1',
        ('10.1.16.2', '11.5.0.1'): 'dnp3_2',
    },
    'modbus': {
        ('11.2.0.1', '10.1.4.2'): 'mg0_out',
        ('11.3.0.1', '10.1.8.2'): 'mg1_out',
        ('11.4.0.1', '10.1.12.2'): 'mg2_out',
        ('11.5.0.1', '10.1.16.2'): 'mg3_out',
    },
    'mms': {  # from run_normal-mms_00 (matched pair) — identical flow set to modbus
        ('11.2.0.1', '10.1.4.2'): 'mg0_out',
        ('11.3.0.1', '10.1.8.2'): 'mg1_out',
        ('11.4.0.1', '10.1.12.2'): 'mg2_out',
        ('11.5.0.1', '10.1.16.2'): 'mg3_out',
    },
}

TP_COLS = ['throughput', 'lost_pkts', 'tx_bytes', 'rx_bytes',
           'loss_rate', 'delay', 'tx_pkts', 'rx_pkts', 'jitter']

# ── v2.0 scenario whitelist ───────────────────────────────────────────────
# Explicit mapping from directory-name tag -> (attack_type, protocol, label).
# Only directories whose tag appears here are processed; everything else
# (including old v1.0-era debug directories like run_ddos_0, run_normal_1,
# etc.) is skipped with a clear message, not guessed at.
#
# Label scheme (v2.0, distinct from v1.0's ddos/mim/combined scheme):
#   normal=0, fdi=1, replay=2, slowddos=3
#
# GOOSE entries are listed for completeness/traceability but are always
# skipped at the TP.txt-processing stage — see PROCESS_GOOSE_VIA_TP below.
SCENARIO_INFO = {
    'normal-dnp3':     {'attack_type': 'normal',   'protocol': 'dnp3',   'label': 0},
    'fdi-dnp3':        {'attack_type': 'fdi',      'protocol': 'dnp3',   'label': 1},
    'replay-dnp3':     {'attack_type': 'replay',   'protocol': 'dnp3',   'label': 2},
    'normal-modbus':   {'attack_type': 'normal',   'protocol': 'modbus', 'label': 0},
    'fdi-modbus':      {'attack_type': 'fdi',      'protocol': 'modbus', 'label': 1},
    'slowddos-modbus': {'attack_type': 'slowddos', 'protocol': 'modbus', 'label': 3},
    'normal-mms':      {'attack_type': 'normal',   'protocol': 'mms',    'label': 0},
    'fdi-mms':         {'attack_type': 'fdi',      'protocol': 'mms',    'label': 1},
    'slowddos-mms':    {'attack_type': 'slowddos', 'protocol': 'mms',    'label': 3},
    'normal-goose':    {'attack_type': 'normal',   'protocol': 'goose',  'label': 0},
    'fdi-goose':       {'attack_type': 'fdi',      'protocol': 'goose',  'label': 1},
    'replay-goose':    {'attack_type': 'replay',   'protocol': 'goose',  'label': 2},
}

# Set to True only once a separate GOOSE processing path (log-based, not
# TP.txt-based) has been built and wired in. Until then, GOOSE directories
# are always skipped with an explanatory message.
PROCESS_GOOSE_VIA_TP = False


# ── Parse one TP.txt file ─────────────────────────────────────────────────
def parse_tp_file(tp_path, protocol):
    """Parse a TP.txt file into a DataFrame with one row per timestep.
    Flows are mapped to path columns by their (src,dst) address pair using this
    protocol's FLOW_MAPS entry (source port is unstable across runs)."""
    flow_map = FLOW_MAPS.get(protocol, {})
    records = {}  # time -> {path_col: value}

    with open(tp_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 12:
                continue
            try:
                t = float(parts[0])
                src, dst = parts[3], parts[7]  # (PROTO <src> / sp --> <dst> / dp)
                path = flow_map.get((src, dst))
                if path is None:
                    continue
                desc_end = line.index(')') + 1
                vals = line[desc_end:].strip().split()
                if len(vals) < 9:
                    continue
                if t not in records:
                    records[t] = {}
                for i, col in enumerate(TP_COLS):
                    records[t][f'{path}_{col}'] = float(vals[i])
            except (ValueError, IndexError):
                continue

    if not records:
        return None

    df = pd.DataFrame.from_dict(records, orient='index')
    df.index.name = 'time'
    df = df.reset_index().sort_values('time')
    return df


# ── Derive aggregate columns ──────────────────────────────────────────────
def add_derived_columns(df):
    """Add roll10, delta, delivery-ratio, interval-mean-delay, total, max columns.
    All new columns are accumulated in one dict and concatenated in a single
    pd.concat, avoiding the DataFrame fragmentation from many single inserts.
    (fill_missing_columns has already ensured every path column exists.)"""
    new = {}
    for path in PATH_NAMES:
        col = f'{path}_throughput'
        new[f'{col}_roll10'] = (df[col].rolling(10, min_periods=1).mean()
                                if col in df.columns else 0.0)

    for path in PATH_NAMES:
        col = f'{path}_lost_pkts'
        new[f'{path}_lost_pkts_delta'] = (df[col].diff().fillna(0).clip(lower=0)
                                          if col in df.columns else 0.0)

    # Per-interval deltas for the cumulative byte/packet counters (alongside the
    # cumulative columns). Rates (throughput/delay/jitter) keep their cumulative
    # form; delay additionally gets an interval-mean recovery below.
    for path in PATH_NAMES:
        for c in ['tx_bytes', 'rx_bytes', 'tx_pkts', 'rx_pkts']:
            col = f'{path}_{c}'
            new[f'{col}_delta'] = (df[col].diff().fillna(0).clip(lower=0)
                                   if col in df.columns else 0.0)

    for path in PATH_NAMES:
        txd = new[f'{path}_tx_pkts_delta']
        rxd = new[f'{path}_rx_pkts_delta']
        # interval delivery ratio: rx_pkts_delta / tx_pkts_delta (0 when no tx)
        ratio = np.where(txd > 0, rxd / txd.replace(0, np.nan), 0.0)
        new[f'{path}_delivery_ratio'] = pd.Series(ratio, index=df.index).fillna(0.0)
        # interval mean delay recovered from the cumulative-mean delay column:
        # (d_t*R_t - d_{t-1}*R_{t-1}) / (R_t - R_{t-1}), R = cumulative rx_pkts;
        # NaN -> 0 when no packets arrived in the interval (denominator 0).
        d, R = f'{path}_delay', f'{path}_rx_pkts'
        if d in df.columns and R in df.columns:
            num = (df[d] * df[R]).diff()
            den = df[R].diff()
            new[f'{path}_interval_mean_delay'] = (num / den).where(den != 0).fillna(0.0)
        else:
            new[f'{path}_interval_mean_delay'] = 0.0

    throughput_cols = [f'{p}_throughput' for p in PATH_NAMES if f'{p}_throughput' in df.columns]
    lost_cols = [f'{p}_lost_pkts' for p in PATH_NAMES if f'{p}_lost_pkts' in df.columns]
    delay_cols = [f'{p}_delay' for p in PATH_NAMES if f'{p}_delay' in df.columns]
    jitter_cols = [f'{p}_jitter' for p in PATH_NAMES if f'{p}_jitter' in df.columns]
    lost_delta = {k: new[k] for k in (f'{p}_lost_pkts_delta' for p in PATH_NAMES)}

    new['total_throughput'] = df[throughput_cols].sum(axis=1) if throughput_cols else 0.0
    new['total_lost_pkts'] = df[lost_cols].sum(axis=1) if lost_cols else 0.0
    new['total_lost_delta'] = (pd.DataFrame(lost_delta, index=df.index).sum(axis=1)
                               if lost_delta else 0.0)
    new['max_delay'] = df[delay_cols].max(axis=1) if delay_cols else 0.0
    new['max_jitter'] = df[jitter_cols].max(axis=1) if jitter_cols else 0.0

    return pd.concat([df, pd.DataFrame(new, index=df.index)], axis=1)


def fill_missing_columns(df):
    """Ensure all expected path columns exist, fill missing with 0.
    Legitimately zero for protocols that don't use certain paths (e.g.
    Modbus/MMS never populate sub_out/dnp3_0-2 — see module docstring)."""
    for path in PATH_NAMES:
        for col in TP_COLS:
            full_col = f'{path}_{col}'
            if full_col not in df.columns:
                df[full_col] = 0.0
    return df


def get_column_order():
    cols = ['time']
    for path in PATH_NAMES:
        for col in TP_COLS:
            cols.append(f'{path}_{col}')
    for path in PATH_NAMES:
        cols.append(f'{path}_throughput_roll10')
    for path in PATH_NAMES:
        cols.append(f'{path}_lost_pkts_delta')
    for path in PATH_NAMES:
        for c in ['tx_bytes', 'rx_bytes', 'tx_pkts', 'rx_pkts']:
            cols.append(f'{path}_{c}_delta')
    for path in PATH_NAMES:
        cols.append(f'{path}_delivery_ratio')
    for path in PATH_NAMES:
        cols.append(f'{path}_interval_mean_delay')
    cols += ['total_throughput', 'total_lost_pkts', 'total_lost_delta',
             'max_delay', 'max_jitter',
             'run_id', 'scenario_tag', 'protocol', 'attack_type', 'label',
             'random_seed', 'fdi_active', 'replay_active', 'slowddos_active',
             'infra_present', 'attack_active']
    return cols


# ── Attack context from a run's own grid.json ──────────────────────────────
def read_attack_context(run_dir):
    """Return (infra_present, win_start, win_end) read from THIS run's config.
    Window source: MIM Start/End if includeMIM, else FDI if includeFDI, else
    SlowDDoS if SlowDDoS.Active. infra_present = network-altering attack infra:
    1 if includeMIM (MIM reroutes) OR SlowDDoS.Active (bot fleet up from t=0,
    empirically perturbs pre-window counts); FDI (compromised endpoint) = 0."""
    try:
        with open(os.path.join(run_dir, 'config', 'grid.json')) as f:
            g = json.load(f)
    except (OSError, ValueError):
        return 0, None, None
    sim = (g.get('Simulation') or [{}])[0]
    inc_mim = int(sim.get('includeMIM', 0) or 0)
    inc_fdi = int(sim.get('includeFDI', 0) or 0)
    sd = (g.get('SlowDDoS') or [{}])[0]
    sd_active = int(sd.get('Active', 0) or 0)

    def window(entries):
        starts, ends = [], []
        for e in entries or []:
            if isinstance(e, dict) and 'Start' in e and 'End' in e:
                starts += [float(v) for v in str(e['Start']).split(',')]
                ends += [float(v) for v in str(e['End']).split(',')]
        return (min(starts), max(ends)) if starts else (None, None)

    if inc_mim:
        ws, we = window(g.get('MIM'))
    elif inc_fdi:
        ws, we = window(g.get('FDI'))
    elif sd_active:
        ws, we = float(sd['Start']), float(sd['End'])
    else:
        ws, we = None, None
    infra = 1 if (inc_mim or sd_active) else 0
    return infra, ws, we


# ── Process one run directory ──────────────────────────────────────────────
def process_run(run_dir, run_id, scenario_tag, info, random_seed):
    tp_path = os.path.join(run_dir, 'TP.txt')
    if not os.path.exists(tp_path):
        print(f"  SKIP {os.path.basename(run_dir)} — no TP.txt")
        return None

    df = parse_tp_file(tp_path, info['protocol'])
    if df is None or len(df) == 0:
        print(f"  SKIP {os.path.basename(run_dir)} — empty TP.txt")
        return None

    df = fill_missing_columns(df)
    df = add_derived_columns(df)

    attack_type = info['attack_type']
    infra_present, win_s, win_e = read_attack_context(run_dir)
    if win_s is not None and attack_type != 'normal':
        attack_active = ((df['time'] >= win_s) & (df['time'] <= win_e)).astype(int)
    else:
        attack_active = 0

    # Add all run-level label columns in one assign (avoids re-fragmenting the
    # frame that add_derived_columns just returned contiguous).
    df = df.assign(
        run_id=run_id,
        scenario_tag=scenario_tag,
        protocol=info['protocol'],
        attack_type=attack_type,
        label=info['label'],
        random_seed=random_seed,
        fdi_active=1 if attack_type == 'fdi' else 0,
        replay_active=1 if attack_type == 'replay' else 0,
        slowddos_active=1 if attack_type == 'slowddos' else 0,
        infra_present=infra_present,            # run-level (includeMIM / SlowDDoS.Active)
        attack_active=attack_active,            # per-row (time in this run's attack window)
    )

    print(f"  OK  {os.path.basename(run_dir)}: {len(df)} rows, "
          f"t={df['time'].min():.2f}-{df['time'].max():.2f}s")
    return df


# ── Main ─────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runs_dir',
        default='/project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/runs')
    parser.add_argument('--out',
        default='/project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/master_dataset_v2.csv')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    random.seed(args.seed)
    np.random.seed(args.seed)

    run_dirs = sorted(glob.glob(os.path.join(args.runs_dir, 'run_*')))
    print(f"Found {len(run_dirs)} directories matching run_*")

    all_dfs = []
    run_id = 0
    skipped_unrecognized = 0
    skipped_goose = 0

    for run_dir in run_dirs:
        name = os.path.basename(run_dir)

        # Directory name shape: run_<scenario_tag>_<NN>. Split off the
        # trailing _NN run index, everything before it is the scenario tag.
        if '_' not in name:
            skipped_unrecognized += 1
            continue
        prefix, _, suffix = name.rpartition('_')
        if not suffix.isdigit() or not prefix.startswith('run_'):
            skipped_unrecognized += 1
            continue
        scenario_tag = prefix[len('run_'):]

        info = SCENARIO_INFO.get(scenario_tag)
        if info is None:
            # Not a recognized v2.0 scenario — likely a leftover debug
            # directory from earlier smoke-testing (run_ddos_0, run_mim_0,
            # etc.). Deliberately excluded rather than guessed at.
            skipped_unrecognized += 1
            continue

        if info['protocol'] == 'goose' and not PROCESS_GOOSE_VIA_TP:
            print(f"  SKIP {name} — GOOSE excluded from TP.txt processing "
                  f"(FlowMonitor cannot track multicast flows; see module docstring)")
            skipped_goose += 1
            continue

        random_seed = random.randint(0, 99999)
        df = process_run(run_dir, run_id, scenario_tag, info, random_seed)
        if df is not None:
            all_dfs.append(df)
            run_id += 1

    print(f"\nSkipped (unrecognized/legacy): {skipped_unrecognized}")
    print(f"Skipped (GOOSE, excluded by design): {skipped_goose}")

    if not all_dfs:
        print("ERROR: No valid runs found")
        return

    print(f"\nAssembling {len(all_dfs)} runs...")
    master = pd.concat(all_dfs, ignore_index=True)

    col_order = get_column_order()
    for col in col_order:
        if col not in master.columns:
            master[col] = 0
    master = master[col_order]

    print(f"Total rows: {len(master):,}")
    print(f"Protocols: {master['protocol'].value_counts().to_dict()}")
    print(f"Attack types: {master['attack_type'].value_counts().to_dict()}")
    print(f"Writing to {args.out}...")
    master.to_csv(args.out, index=False)
    print(f"Done — {os.path.getsize(args.out)/1e6:.1f} MB")

    print("\n=== Summary (by scenario tag) ===")
    for tag in sorted(master['scenario_tag'].unique()):
        sub = master[master['scenario_tag'] == tag]
        n_runs = sub['run_id'].nunique()
        n_rows = len(sub)
        print(f"  {tag}: {n_runs} runs, {n_rows:,} rows (label={sub['label'].iloc[0]})")


if __name__ == '__main__':
    main()
