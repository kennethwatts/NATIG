# Follow-up: `SS_0`–`SS_9` Points Scheme Is Disconnected From Live GridLAB-D Data

**Date:** 2026-07-20
**Found during:** MMS (IEC 61850) production validation, `feature/iec61850-mms`
**Affects:** All protocols (DNP3, Modbus, MMS) — this is shared topology/config infrastructure, not protocol-specific
**Status:** Root cause confirmed via live testing; workaround confirmed working; no decision made yet on the production fix

---

## Summary

While generating real points files for MMS's 123-bus validation, two real issues surfaced in the shared (non-protocol-specific) topology code and config:

1. **A real off-by-one bug** in the microgrid-name-to-points-file lookup, present in all three protocols' topology files. **Fixed** (see below).
2. **A much bigger finding**: the `SS_0`–`SS_9` microgrid scheme — the one `integration/control/config/grid.json` currently defines, with its associated ~15,890-point `points_SS_*.csv` files — appears to have **never actually received live telemetry from GridLAB-D**, for any protocol, possibly since it was introduced. This is not fixed; it needs a decision (see "Open question" below).

---

## 1. The off-by-one bug (fixed)

`ns3-helics-grid-dnp3.cc`, `ns3-modbus-helics-grid.cc`, and `ns3-iec61850-helics-grid.cc` all contain this pattern (twice each — once for the master/outstation install loop, once for MIM install):

```cpp
std::string IDx = "SS_";
if (std::string(ep_name).find(IDx) != std::string::npos){
    ep_name = "SS_"+std::to_string(i+1);   // BUG: assumes 1-indexed names
}
```

The real config names its microgrids `SS_0`..`SS_9` (0-indexed), matching the loop's own 0-indexed `i` directly. Since every one of those names already contains `"SS_"`, this line unconditionally *renamed* them to `SS_1`..`SS_10` before building the points-file path — meaning the last substation (`SS_9`) would look for `points_SS_10.csv`, which doesn't exist anywhere in the config directory. `initConfig()`'s `exit(-1)` on a missing points file would crash that node's load, for any protocol using this shared loop.

**Fixed** in all three files: the substitution now uses `i` (loop) / `MIM_ID-1` (MIM block) instead of `i+1`/`MIM_ID`, matching what's actually on disk. Confirmed this doesn't affect the `mg1`/`mg2`/`mg3`/`substation` scheme below, since those names don't contain `"SS_"` and never hit this substitution at all.

## 2. The disconnected `SS_N` scheme (open question)

### What was tested

Ran the real production pipeline (real `helics_broker` + real `gridlabd` against `IEEE_123_Dynamic.glm` + the real `ns3-modbus-helics-grid`/`ns3-iec61850-helics-grid` binaries) in the `natig-modbus-test` Docker container, with temporary `std::cerr` tracing added to `DoEndpoint`/`store_points` (removed afterward — no instrumentation left in the codebase).

- **Against the current `SS_0`–`SS_9` config**: over 90 seconds and 5.6 million trace lines, the three variables GridLAB-D's `gridlabd_config.json` declares 10,607 receiving endpoints for (`phases`, `nominal_voltage`, `bustype`) **never appeared once** — not as a successful store, not as a miss. They simply never arrive via HELICS in a real run.
- **Against `RC/code/3G-conf-123/`'s config** (`mg1`/`mg2`/`mg3`/`substation` microgrids, `points_mg1.csv`/`points_mg2.csv`/`points_mg3.csv`/`points_substation.csv`): confirmed **live and correctly wired** — 1,374,571 successful point updates in a 25-second run, real values like `voltage_A`/`Pref`/`Qref`/`tap_A` flowing and being stored correctly by the existing `DoEndpoint` dispatch table.
- **MMS's own production topology** (`ns3-iec61850-helics-grid`) run against the `mg`-scheme config: completed cleanly, no crashes, 17,754 real packet records logged in `perf.txt`. This confirms MMS works correctly end-to-end against genuinely live 123-bus telemetry.

### What this means

- `DoEndpoint`'s explicit dispatch table (`voltage_A`, `Pref`, `Qref`, `tap_A`, `capacitor_A`, `status`, `phase_A_state`, etc.) is correct and working — it was written for the `mg`-scheme's variable names, and those are exactly what GridLAB-D's model actually publishes.
- The `SS_0`–`SS_9` scheme's variable names (`phases`, `nominal_voltage`, `bustype`) don't match any dispatch branch — but that gap turned out not to matter in practice, because those variables never arrive at all. The real problem is one level up: GridLAB-D's `.glm` model doesn't appear to be publishing under that naming scheme, despite `gridlabd_config.json` being configured to receive it.
- This means the `SS_0`–`SS_9` points files (the larger, more detailed ~15,890-point scheme, matching real substation-level meter names) have likely been sitting at their initial CSV-loaded values for the entire lifetime of any run that used them — for DNP3 and Modbus too, not just MMS.

### Scope / why this matters beyond MMS

`natig_array.sh` (the production Unity SLURM pipeline) is built around `integration/control/config/`, which is the `SS_0`–`SS_9` config. If this finding is accurate, **any prior production dataset generated through that pipeline may have "live" analog telemetry that never actually updated** — a data-quality question for whatever's already been collected, not just a blocker for future work.

### Open question — needs a decision, not more digging (for now)

Two ways forward, deliberately not chosen here:

1. **Migrate production configs to the `mg1`/`mg2`/`mg3`/`substation` scheme** — known-live, smaller (909 points vs. ~15,890), already confirmed working end-to-end for Modbus and MMS in this session.
2. **Fix the `SS_N` scheme's GridLAB-D wiring** — would require digging into `IEEE_123_Dynamic.glm`'s (or whichever `.glm` file is authoritative) `helics_msg` publish configuration to figure out why it doesn't publish under the `SS_N`/`phases`/`nominal_voltage`/`bustype` naming at all. A different problem domain (GridLAB-D model configuration) than the ns-3/protocol work this session covered, and potentially a bigger scope-wise investment for more monitoring detail per substation.

This affects the shared pipeline used by every protocol, so it's worth Sumit's or Dr. Vokkarane's input before committing to either path.
