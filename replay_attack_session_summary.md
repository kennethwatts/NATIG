# Session Summary: Replay Attack (DNP3, Modbus, MMS, GOOSE)

**Date:** 2026-07-21
**Branch:** `feature/replay` (off `feature/fdi`)
**Status:** DNP3 and GOOSE fully validated end-to-end in Docker. Modbus and MMS
are code-complete but blocked on a pre-existing environment gap, written up
for Sumit. Not yet validated on Unity.

---

## What this branch adds

A new "replay" threat: `attack_type 5`. Where FDI (previous branch) fabricates
a value at the source and MIM (types 2/3/4) injects an attacker-chosen value
on-path, replay captures a **real** value observed in legitimate traffic
before a point's attack window opens, freezes it once the window opens, and
re-injects that frozen, aging observation instead of the live value or a
fabricated one.

**Why this is a distinct threat worth having**: a replayed value is real and
physically plausible (it *was* the true state at some point), so it defeats
the physical-consistency checks that would catch FDI. It's only catchable via
freshness/sequence/timestamp checks — the complementary end of the
detectability spectrum from FDI's fabricated-but-implausible values. See
`attack_model_reference.md` (written before implementation) for the design
survey this was built from.

## Implementation pattern

- **DNP3/Modbus/MMS**: on-path, extends the existing `handle_MIM` (same
  `mitmFlag`/`MIM_ID` node used by types 2/3/4 — unlike FDI, which needed its
  own `FdiFlag`/`FdiID` pair on the real endpoint since it's structurally
  different). Capture happens whenever a point's own attack window
  (`PointStart`/`PointStop`) isn't open; freezes once it is. DNP3 needed a
  raw byte-offset walk (`find_point_byte_indices`, factored out of the
  existing type-2 write logic) since its packets have no parsed-value
  abstraction; Modbus/MMS are much simpler, just reading/writing through
  the existing register-file/point accessors (`GetHoldingRegister`,
  `SetAnalogPoint`, etc.).
- **GOOSE**: structurally different, like FDI before it. No in-path position,
  so the rogue-publisher instance (reused per-microgrid subscriber, same as
  types 2/3/4) hooks its own receive path (`HandleRead`) to capture the most
  recent legitimate frame per goID, then resends it **verbatim** — including
  its original, now-stale `stNum`/`sqNum` — unlike the other attack types,
  which forge a strictly *newer* one. This is deliberately the detectability
  **baseline**: GOOSE's own "newest wins" acceptance rule should reject a
  verbatim-stale replay, a negative result documenting the one protocol here
  with a built-in defense against this threat.
- Topology wiring: DNP3/Modbus/MMS needed `NodeID`/`PointID` added to the
  `attack_type == 5` case (previously only wired for 2/3/4 — same bug class
  as the `AttackConf`/`ID` gap fixed in the FDI session, different attribute
  pair). GOOSE needed no topology change for replay itself.

## Commits (in order)

1. `4682e85` — DNP3: capture/freeze/replay logic, `find_point_byte_indices`
   helper, `NodeID`/`PointID` topology wiring, retargeted example config.
2. `5745959` — Modbus: same pattern, register-file based.
3. `cd1dfb0` — MMS: same pattern, point-accessor based.
4. `63e6ffb` — GOOSE: baseline-rejection case (initial implementation —
   see commit 6 below, this didn't actually fire until fixed).
5. `3e1bac8` — Write-up on the Modbus/MMS traffic gap (see below), for Sumit.
6. `9e33913` — **Unrelated bug fixes**, found while validating GOOSE:
   `handle_rogue_publish` had never actually been invoked by any code path in
   the production topology, for *any* attack type (2/3/4/5), not just
   replay — exactly the concern `iec61850_session_summary.md:164` had
   flagged as unverified before starting this work. Three compounding bugs
   (`isRogue` name-matching that never matched the reused subscriber, an
   unwired rogue send-target address, `handle_rogue_publish` never
   scheduled), all pre-existing and unrelated to replay's own code. Also
   fixed replay's GOOSE capture to freeze at window-open (was refreshing
   continuously), for consistency with the other three protocols.
7. `7e62413` — Added the GOOSE finding as related context to the write-up.

## Validation

Ran DNP3 and GOOSE production topologies end-to-end in the
`natig-modbus-test` Docker container, against the real `helics_broker` +
`gridlabd` + `IEEE_123_Dynamic.glm`, using `3G-conf-123/grid.json`'s `MIM2`
entry (retargeted from `attack_type 4` to `5`, `PointStart`/`PointStop`
widened to 5/15 within the attacker's 1/20 `Start`/`End` window so there's
real time both before and after the window to capture from and resume to).

- **DNP3**: 900 real-value captures, 626 successful frozen-value replays
  during the window, correct fallback to unmodified forwarding when a
  point's byte offset wasn't resolvable in a given packet's shape (1,222
  such skips — these are bulk poll-response packets that don't carry this
  particular point, as opposed to the direct-operate acks that do).
- **GOOSE**: 19 replay fires across the 20s window (once per second), each
  resending the identical frozen frame (`stNum=1`, `sqNum=3`) captured right
  before the window opened, while the real subscriber's own tracking
  organically climbed to `stNum=1036` by the run's end — clear evidence the
  "newest wins" check correctly rejects the stale replay throughout. Normal
  GOOSE traffic (57,581 accepted frames) and FDI (15,259 firings) unaffected.
- **Modbus/MMS**: blocked — see below.

### A note on log visibility

`NS_LOG_INFO`/`NS_LOG_WARN` are silently compiled out in this container's
optimized build (already known, see
`feedback_ns3_build_environment_gotchas.md`), but it cost real time twice
this session (once for Modbus, once for GOOSE) before being remembered —
absence of a warning line is not evidence a code path didn't run. FDI's own
logging already uses `std::cout` for exactly this reason; the new
replay-specific log lines added this session follow that precedent. If a
future session adds new logging to `handle_MIM`/`handle_rogue_publish`/etc.,
use `std::cout`, not `NS_LOG_*`, or it won't be visible in this environment.

## Open items for next session(s)

- **Modbus/MMS on-path MIM node never receives traffic** — confirmed via
  direct instrumentation (zero packets across a full 20s run, while FDI
  fires normally in the same run). Affects all MIM-family attack types
  (2/3/4/5), not just replay. Full write-up with evidence, ruled-out causes,
  and questions for Sumit: `modbus_mms_mim_traffic_gap.md` (repo root).
  **Waiting on Sumit's input before this can be resolved or worked around.**
- **Unity validation not done.** Same caveat as every protocol before this
  one — see `feedback_ns3_build_environment_gotchas.md`.
- **Slow DDoS is next**, per the branching decision (own branch/chat,
  implemented across all four protocols for cross-protocol comparability).
  Design notes already exist in `attack_model_reference.md` — the open
  question flagged there: existing DDoS is flat-rate flood only, and
  "slow" needs a protocol-agnostic definition, since GOOSE's connectionless
  UDP multicast has no TCP-style connection to exhaust the way
  DNP3/Modbus/MMS do (candidate: artificially low publish rate to starve
  subscribers' timeout logic instead).
- Dataset generation can proceed now on what's validated (FDI on all four
  protocols; MIM/replay on DNP3 and GOOSE), with Modbus/MMS MIM-family
  attacks scoped out pending the gap above.
