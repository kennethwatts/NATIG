# IEC 61850 (MMS + GOOSE) Implementation — Session Summary
**Dates:** July 19–20, 2026
**Branch:** `feature/iec61850-mms`
**Scope:** Full implementation of two IEC 61850 sub-protocols — MMS (client-server, TCP) and GOOSE (publish/subscribe, UDP multicast) — following the two-class `ApplicationNew`/`ApplicationHelperNew` pattern established by DNP3/Modbus. Sampled Values was explicitly scoped out (deferred, not needed for the current detectability comparison). MMS was completed and validated first; GOOSE followed the same design/implement/validate cycle.

## Outcome

**MMS is complete and validated on both Docker and Unity.** GOOSE is complete and thoroughly validated on Docker; Unity validation is blocked by a pre-existing infrastructure gap unrelated to GOOSE's own correctness (see Part 4).

- **MMS:** Full client-server request/response (READ/WRITE/REPORT), name-keyed object references, production topology (`ns3-iec61850-helics-grid.cc`) validated against the real 123-bus config on both Docker and Unity.
- **GOOSE:** Full publish/subscribe over UDP multicast, burst-then-heartbeat retransmission, deadband-based analog change detection, rogue-publisher attack model. Production topology (`ns3-iec61850-goose-helics-grid.cc`) validated on Docker: **11,350 successful GOOSE receptions** across all 3 real microgrids (mg1/mg2/mg3), with real DDoS bots and MIM attack config simultaneously active.
- Three shared bugs (not IEC-61850-specific) were found and fixed across MMS, Modbus, and DNP3 during MMS work: an `AttackConf` substring-match bug and a microgrid-naming off-by-one.
- One real architectural finding — the 123-bus config's `SS_N` naming scheme is disconnected from the live GridLAB-D data, while the `mg1`/`mg2`/`mg3` scheme is confirmed live — is documented separately in `gridlabd_points_scheme_followup.md` as an open decision for Sumit/advisor, not a code bug.
- One infrastructure gap remains open: Unity's shared `ns-3-dev-unity` build tree needs a `./waf configure` to register GOOSE's new header files, and that reconfigure currently fails for reasons unrelated to GOOSE (see Part 4 and `~/Downloads/iec61850_handoff_notes.md` section 5 for full detail).

---

# Part 1 — MMS

## Design

Client-server over TCP, matching Modbus/DNp3's transport shape but with a custom application-layer framing: length header + service code (READ/WRITE/REPORT) + object reference string + value — no real ISO 8823/8327/COTP stack, since that OSI presentation-layer machinery isn't relevant to the detectability comparison. Object references are name-keyed strings (e.g. `Node1$MMXU1.TotW.mag.f`) rather than Modbus-style numeric addresses, which eliminates the address-space-collision bug class Modbus/DNP3 point-matching code has to guard against explicitly.

## Files

- `RC/code/helics/mms-application-new.h` / `.cc`, `mms-application-helper-new.h` / `.cc`
- `RC/code/ns3-iec61850-helics-grid.cc` (production topology, PointToPoint per-substation like Modbus/DNP3)
- `RC/code/ns3-mms-helics-test.cc`, `ns3-mms-mim-test.cc`, `ns3-mms-report-test.cc` (standalone scenarios)
- `RC/code/points_mms_mim_test.csv`, `RC/code/attack_config_mms_test.json`

## Bugs found and fixed

### 1. `resetToRealValue` signature mismatch
Header declared a new signature; the `.cc` still had the old stub (previously just a log statement, not real restore logic). Fixed by rewriting the `.cc` to match the header and actually implement the restore.

### 2. `AttackConf` substring bug (MMS, Modbus, DNP3)
`configFile.find("NA") == npos` false-matched any path containing the substring `"NA"` — including `"NATIG"` itself, meaning the "no attack config" sentinel check fired on real config paths. Fixed with exact equality (`configFile != "NA"`) in all three protocols' files, since Modbus and DNP3 shared the same bug pattern and the user asked for all three to be fixed together, not just MMS.

### 3. Microgrid-naming off-by-one (MMS, Modbus, DNP3-original topology files)
`ep_name = "SS_" + std::to_string(i+1)` assumed the config's microgrid names are 1-indexed (`SS_1`..`SS_N`); the real 123-bus config (`RC/code/3G-conf-123/`) names them 0-indexed (`SS_0`..`SS_9`), matching the loop's own 0-indexed `i` directly. The `+1` silently pointed every substation at the wrong points file (`SS_0` → looked for `points_SS_1.csv`, ..., `SS_9` → looked for the nonexistent `points_SS_10.csv`, crashing the last substation's config load). Fixed to use `i` directly (and `MIM_ID-1` in the MIM block) in `ns3-modbus-helics-grid.cc` and `ns3-helics-grid-dnp3.cc`.

## Architectural finding (not a bug): SS_N vs mg-scheme points disconnection

The 123-bus config directory used for the fixes above (`integration/control/config/`, `SS_0`-`SS_9` naming) turned out to be **disconnected from the live GridLAB-D simulation data** — confirmed by digging into how points get populated at runtime. A separate config directory (`RC/code/3G-conf-123/`, `mg1`/`mg2`/`mg3`/substation naming) is confirmed live. The real production validation runs (both MMS and GOOSE) were done against the `mg`-scheme config, not `SS_N`, per the user's explicit direction after this was found. Full detail and the open decision (which scheme the actual v2.0 dataset generation should use) is in `gridlabd_points_scheme_followup.md` — this is a decision for Sumit/advisor, not something resolved in code.

## Validation

- **Docker:** Full production topology run against the real `mg`-scheme config, confirmed working.
- **Unity:** Verified against Unity's independently-compiled `ns-3-dev-unity` toolchain, confirmed working (this predates the Unity infrastructure gap found during GOOSE work — MMS's build didn't require registering brand-new headers into an existing configured tree the same way, or was validated before that gap was hit).

## Commits

- `da9d960` — Complete MMS application, wire build, add topology file and standalone tests
- `69981b9` — Fix microgrid-name off-by-one; document disconnected SS_N points scheme

---

# Part 2 — GOOSE design and implementation

## Design decisions

GOOSE is structurally different from every other protocol in this codebase: single message type (no service codes, no request/response), multicast at the transport layer, and a real deployment never leaves the substation LAN. Two design questions came up and were resolved with the user before implementation:

1. **Transport: UDP multicast, not raw L2.** Real GOOSE is often raw Ethernet multicast in actual substations, but ns-3's socket/application-layer abstractions (which every other protocol here builds on) assume IP. UDP multicast was chosen as the pragmatic middle ground — multicast semantics preserved, without requiring a from-scratch raw-socket application class.
2. **Full go-ahead on the fuller design** (burst-then-heartbeat retransmission, rogue-publisher attack model, deadband change detection) after presenting the concrete plan.

## Key mechanics

- **Burst-then-heartbeat retransmission**: on a real state change, publish `BurstCount` times at `BurstIntervalMs` spacing (default 3× at 4ms), then fall back to `HeartbeatIntervalMs` (default 2000ms) until the next change — matches real GOOSE's re-transmission behavior on state change vs. steady-state.
- **Deadband-based analog change detection** (`DeadbandPct`/`DeadbandAbs`, defaults 0.5%/0.01): see Part 3, bug #1 — required because real grid telemetry never holds an exact floating-point value long enough for naive equality comparison to work.
- **Rogue-publisher attack model**: unlike MITM interception (Modbus/DNP3/MMS's `handle_MIM`, which sits in-path and rewrites intercepted traffic), multicast has no "in-path" position to intercept from — the attack model instead is a second, unauthorized publisher forging a higher `stNum` to win acceptance over the legitimate publisher's heartbeat, a structurally different and arguably more realistic GOOSE threat model.
- **CSMA topology, not PointToPoint**: multicast requires a shared/broadcast-capable medium; every other protocol's production topology uses point-to-point links per substation, but GOOSE's per-microgrid segment is CSMA specifically to allow multiple subscribers (including the rogue) on the same physical segment as the publisher.
- **Multicast routing**: this ns-3 version has no generic IPv4 `Socket::MulticastJoinGroup` (only `Ipv6JoinGroup` exists). The real, working pattern — found in `src/csma/examples/csma-multicast.cc` — is `Ipv4StaticRoutingHelper::SetDefaultMulticastRoute` on the **sender's** device only, with the receiver just doing a plain `Bind(Ipv4Address::GetAny())`. No explicit "join" call is needed on a single shared CSMA segment.

## Files

- `RC/code/helics/goose-application-new.h` / `.cc`, `goose-application-helper-new.h` / `.cc`
- `RC/code/ns3-iec61850-goose-helics-grid.cc` (production topology, CSMA per microgrid)
- `RC/code/ns3-goose-mim-test.cc` (standalone 3-node test: publisher, legitimate subscriber, rogue publisher)
- `RC/code/points_goose_mim_test.csv`, `RC/code/attack_config_goose_test.json`
- `RC/code/helics/wscript`, `build_ns3.sh`, `build_helics.sh` — updated for both MMS and GOOSE entries

## Standalone test bugs found and fixed (before production topology work)

- **`freq=0` infinite reschedule loop**: the rogue's re-forge interval was passed as `0`, causing a 0ms reschedule loop that burned real CPU without simulated time advancing (had to `kill -9` the stuck process). Fixed by using a real interval (500ms).
- **Missing `AttackStartTime`/`AttackEndTime`**: both default to `"0"` (falsy), so `set_attack(true)` was never scheduled and the rogue never actually attacked. Fixed by setting both attributes explicitly in the test scenario.
- **`GooseApplicationHelperNew::InstallPriv` const mismatch**: declared non-const in the header, mistakenly written `const` in the `.cc` — fixed to match.
- **`Ipv4Address` constructor needs `const char*`, not `std::string`** — compile fix, added `.c_str()`.

## Standalone test result

Fully validated: subscriber sees the real value (100) via the legitimate publisher's burst-then-heartbeat traffic, sees it forged to 9999 once the rogue's attack window opens (rogue wins acceptance by advancing `stNum` faster than the real publisher's heartbeat), then sees it return to 100 after the attack window closes and the real publisher's next heartbeat re-asserts genuine state.

## Commit

- `603752c` — Implement IEC 61850 GOOSE (publish/subscribe over UDP multicast)

---

# Part 3 — GOOSE production-topology debugging (Docker)

Two distinct, serious bugs were found integrating GOOSE into the real production topology against the real 123-bus config — neither was visible in the standalone test, which used a trivial 1-2 point dataset.

## Bug 1: Permanent burst mode (deadband fix)

**Symptom:** every publisher stayed in permanent burst mode (constant ~4ms retransmission) instead of settling into the 2000ms heartbeat cadence.
**Root cause:** `datasetChangedSinceLastPublish()` used exact floating-point equality to detect analog changes. Real GridLAB-D telemetry is continuously varying at the bit level, so it never matched exactly, and every publish cycle registered as "changed."
**Fix:** Replaced exact equality with a deadband comparison — `DeadbandPct` (0.5% of magnitude) with a `DeadbandAbs` floor (0.01, for values near zero) — added as new attributes on `GooseApplicationNew`.
**Verified:** before/after `std::cerr` tracing showed real ~1s gaps between publish events after the fix, vs. constant 4ms bursts before.

## Bug 2: Zero packet reception in the production topology (the big one)

**Symptom:** `HandleRead` never fired — zero successful receptions — despite the identical multicast mechanism working perfectly in the standalone test, and `Socket::Send()` confirmed firing repeatedly via tracing.

**Investigation, in order:**
1. Ruled out `Ipv4GlobalRoutingHelper::PopulateRoutingTables()` interference (disabled it, no effect, reverted).
2. Ruled out missing a subscriber-side multicast route (added `SetDefaultMulticastRoute` on the subscriber's own device too, no effect, reverted).
3. Found a real structural fact via tracing: the subscriber node (`MIMNode.Get(i)`) has 10 pre-existing IP interfaces from other shared topology code (DDoS bot links etc.), vs. the publisher's 2 — meaning the code's hardcoded `GetAddress(1,0)` used to build the subscriber's `LocalAddress` attribute grabs an unrelated address. This turned out to be a red herring for the actual bind, since the real `Bind()` call uses `Ipv4Address::GetAny()` regardless (same as the working standalone test) — but was suspicious enough to chase down first.
4. **Isolated to a single microgrid** (reduced the 4-microgrid config to 1) to test whether concurrent multicast groups was the trigger. This first hit two unrelated crashes — shared DDoS bot-install code and the MIM attack-list config both indexed out of bounds against the artificially shrunk microgrid count. Both were artifacts of the test config, not real bugs (trimmed `NumberOfBots` to 0 and `listMIM` to match, to get a valid isolated test). With those config issues cleared, **the single-microgrid case still failed identically** — ruling out concurrent multicast groups as the cause.
5. **Enabled pcap on the GOOSE CSMA segments directly** (no tshark/scapy available in the container — wrote a small raw pcap parser in Python instead). This showed packets genuinely arriving at the subscriber's device at L2 (frames visible on both the publisher's and subscriber's captures) — so the failure was upstream of the physical layer.
6. **Found it**: the captured packets showed IP fragmentation (`MF=1, MF=1, MF=0` across 3 fragments per GOOSE message). The real substation's dataset (`points_mg1.csv`, 129 points) serializes to ~3.7KB, well over the standard 1500-byte Ethernet MTU. **Confirmed via a controlled swap**: replacing the real 129-point file with a synthetic 2-point file (no fragmentation needed) in the same topology produced 3,168 successful receptions in the same run — proving fragmented multicast reassembly, not anything else, was the failure.

**Fix:** Raised the GOOSE CSMA segments' device MTU to 9000 bytes (`gooseCsma.SetDeviceAttribute("Mtu", UintegerValue(9000))`), so a full substation's point set never needs to fragment. This ns-3 version's fragment reassembly appears not to deliver reassembled *multicast* datagrams to the UDP socket (unicast reassembly wasn't tested and may be unaffected) — raising the MTU sidesteps the bug rather than fixing ns-3 core, matching how real GOOSE deployments size datasets to avoid fragmentation in the first place rather than relying on it.

**Verified at full scale:** full 4-microgrid production topology, real DDoS bots enabled, real MIM attack config enabled — **11,350 successful GOOSE receptions** across all 3 real microgrids (mg1: 3,859; mg2: 3,622; mg3: 3,869).

All temporary `std::cerr`/pcap debug instrumentation added during this investigation was removed afterward; the pcap-enable call was folded into the existing `MonitorPerf`-gated block alongside the other protocols' pcap tracing, for consistency.

---

# Part 4 — GOOSE Unity validation (blocked, unresolved)

## Goal

Verify GOOSE builds and runs correctly against Unity's independently-compiled `ns-3-dev-unity` toolchain, matching the testing pattern established for Modbus and MMS.

## What happened, in order

1. Copied the GOOSE files to Unity's `NATIG` checkout and the actual `ns-3-dev-unity` build tree (`contrib/helics/model/`, `contrib/helics/helper/`, `scratch/`), patching `contrib/helics/wscript` to add the same GOOSE model/helper entries as the local branch. Verified all copies with marker greps before building.
2. **First build attempt failed**: `waf: invalid lock file` / "project was not configured" — running `./waf build` directly on the bare Unity compute node doesn't work. `ns-3-dev-unity` was never configured against the raw filesystem; it's configured *inside* an Apptainer container (`natigfixed.sif`) with the tree bind-mounted to `/rd2c/ns-3-dev`. Found the real invocation pattern by reading `~/natig_array.sh` (the actual production SLURM script) rather than guessing.
3. **Second build attempt, correctly inside the Apptainer container, still failed**: `fatal error: ns3/goose-application-new.h: No such file or directory`. Adding brand-new header files to `contrib/helics/wscript` requires a fresh `./waf configure` to register them — an incremental `./waf build` alone doesn't (Docker didn't hit this because a full `build_ns3.sh`, which calls `configure`, had already run once earlier this session before any incremental builds there).
4. **Before reconfiguring**: checked `squeue` for any other jobs on the shared PI account first (none found relevant — only unrelated GPU jobs from a labmate), given `ns-3-dev-unity` is the shared production tree `natig_array.sh`'s SLURM array jobs depend on.
5. **First reconfigure attempt failed hard**: `./waf configure` reported *every* header/library check as "not found" (`stdint.h`, `pthread.h`, `zmq`, etc.), and the subsequent build failed with fundamental errors (`Cannot find definitions for fixed-width integral types`). **No permanent damage** — confirmed the failed configure did not overwrite the lock file (waf only commits config state on success) and all existing production binaries (`ns3-modbus-helics-grid`, `ns3-iec61850-helics-grid`, `ns3-helics-grid-dnp3`, etc.) kept their original timestamps.
6. Found the real bind-mount pattern via Unity shell history: the working configure additionally needs `--bind natigfixed_sandbox/rd2c/{lib,include,bin}` and `HELICS` bound in, which the first attempt was missing.
7. **Second reconfigure attempt, with the corrected binds, failed identically.** Ruled out: the sandbox's own `include/` doesn't even contain `stdint.h`, yet direct `gcc -E`/compile-and-execute tests in the *exact same* container (no extra binds at all) proved the container's own compiler and system headers work completely fine on their own — including a full compile-and-run round-trip.
8. Found and tested a specific hypothesis: `/tmp` inside this apptainer session is `noexec` (confirmed directly — a trivially compiled test binary got `Permission denied` when executed from `/tmp`), while the bind-mounted `ns-3-dev-unity` path allows execution fine. Since waf's `configure` checks compile *and run* small test programs (typically under `TMPDIR`, defaulting to `/tmp`), this looked like the likely cause.
9. **Third reconfigure attempt, with `TMPDIR` redirected to a location under the bind-mounted tree, failed identically** — same exhaustive "not found" on every check. This was the agreed stopping point for this session.

## Status: unresolved, not a GOOSE code issue

Nothing in this investigation points to GOOSE's actual code being incompatible with Unity's toolchain — the failure is entirely in `waf`'s own `configure`-time checks, which fail even for headers/compiler behavior proven to work directly in the same container. The last known-good configure of this tree was 2026-07-06; the exact interactive steps used then aren't remembered. This needs either reproducing that original session, or help from Sumit/Unity support — not further blind trial-and-error against the shared production tree.

**This is a one-time gate, not a per-run cost**: `natig_array.sh`'s actual production runs only execute already-built binaries, they don't reconfigure. Once a single successful configure+build gets through (by whatever means), GOOSE should run fine in Unity's production array jobs going forward, the same way Modbus/DNP3/MMS already do.

Full detail (exact commands, ruled-out hypotheses, bind-mount lists) is recorded in `~/Downloads/iec61850_handoff_notes.md` section 5, for whoever picks this up next.

---

## Open items

1. **Unity's `ns-3-dev-unity` configure failure (Part 4) is unresolved.** Blocks building *any* protocol with new headers on Unity until fixed, not just GOOSE. Needs Ken to reproduce the 2026-07-06 working session, or Sumit/Unity support.
2. **The `SS_N` vs `mg`-scheme points-file disconnection** (Part 1) is an open decision for Sumit/advisor — which scheme the v2.0 dataset generation should actually use. See `gridlabd_points_scheme_followup.md`.
3. **ns-3's multicast fragment-reassembly gap** (Part 3, bug 2) was sidestepped via MTU, not fixed at the ns-3 core level. If a future config ever produces a GOOSE dataset large enough to exceed even the 9000-byte MTU (unlikely at current scale, but worth flagging), the same failure mode would recur.
4. **GOOSE's rogue-publisher attack model has only been validated in the standalone test and the "no attack triggered" production run** — the production topology's MIM/attack-config-gated block (setting `mitmFlag`/attack attributes on `gooseSubscriberByMicrogrid[MIM_ID-1]`) compiles and runs without crashing, but a production run with an actual attack window active and its detection characteristics hasn't been separately confirmed the way the standalone test's forged-then-reverted value was.
5. **Sampled Values remains out of scope**, per the original scope decision this session — MMS + GOOSE were judged sufficient for the current detectability comparison.

## Files changed

- `RC/code/helics/mms-application-new.h` / `.cc`, `mms-application-helper-new.h` / `.cc` — MMS implementation
- `RC/code/ns3-iec61850-helics-grid.cc` — MMS production topology
- `RC/code/helics/goose-application-new.h` / `.cc`, `goose-application-helper-new.h` / `.cc` — GOOSE implementation
- `RC/code/ns3-iec61850-goose-helics-grid.cc` — GOOSE production topology
- `RC/code/ns3-goose-mim-test.cc`, `points_goose_mim_test.csv`, `attack_config_goose_test.json` — GOOSE standalone test
- `RC/code/helics/modbus-application-new.cc`, `dnp3-application-new.cc`, `dnp3-application-new-Docker.cc` — `AttackConf` bug fix
- `RC/code/ns3-modbus-helics-grid.cc`, `ns3-helics-grid-dnp3.cc` — off-by-one fix
- `RC/code/helics/wscript`, `build_ns3.sh`, `build_helics.sh` — build wiring for both MMS and GOOSE
- `gridlabd_points_scheme_followup.md` — SS_N vs mg-scheme writeup

Commits: `da9d960`, `69981b9`, `603752c` on `feature/iec61850-mms`.
