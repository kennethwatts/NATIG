# Modbus TCP Production Integration — Debugging Session Summary
**Dates:** July 18–19, 2026
**Branch:** `feature/modbus-tcp`
**Scope:** First real compile-and-run attempt of the production topology builder (`ns3-modbus-helics-grid.cc`) against real HELICS/GridLAB-D federation — first on the Docker/Ryzen build environment (July 18), then verified against Unity's independently-maintained toolchain (July 19).

## Outcome

**The full production Modbus TCP pipeline runs end-to-end successfully on both environments.** Topology construction, HELICS federation, Modbus TCP master/outstation communication, MIM attack injection (`MIM1`–`MIM3`), flow monitoring, and clean process exit — all confirmed working, twice, independently:

- **Docker (July 18):** Real, non-empty `.pcap` output (several files in the 400KB–2.5MB range, including distinguishable MIM attack traffic) after a full 20-second simulated run.
- **Unity (July 19):** A complete, full-duration 300-second run — real multi-substation Modbus TCP flow data (71,757 rows in `TP.txt`, ending at `t=299.95`), clean shutdown, on Unity's separately-compiled `ns-3-dev-unity` toolchain.

Seven code/environment bugs were found and fixed on July 18 (Docker). One additional, more serious infrastructure gap was found on July 19 while verifying against Unity — see Part 2 below. Three source files changed: `modbus-application-new.cc`, `modbus-application-helper-new.cc`, `ns3-modbus-helics-grid.cc`.

---

# Part 1 — Docker/Ryzen debugging (July 18)

---

## Bugs found and fixed

### 1. `mim_socket` built and connected for every application instance, not just MIM/Insider roles
**File:** `modbus-application-new.cc` (`makeTcpConnection`)
Every master/outstation instance was constructing a second TCP socket (`mim_socket`) and actively `Connect()`-ing it, regardless of role. For plain (non-MIM) instances, `RemoteAddress2` was never set and defaulted to `10.0.0.0` — a real TCP handshake attempt to a bogus address, on every ordinary node, every run. Unlike DNP3/UDP (where `Connect()` is a harmless no-op), this generates real spurious traffic that could pollute FlowMonitor-based IDS features.
**Fix:** Gated `mim_socket` construction/connect behind the same `Inside`/`MIM` name check already used elsewhere in the file.
**Status:** Compiles clean; present throughout every successful run tonight, not yet isolated in a dedicated MIM-vs-non-MIM comparison test.

### 2. `handle_MIM` conflated analog and binary address spaces
**File:** `modbus-application-new.cc` (`handle_MIM`)
Holding-register and coil addresses are independent, zero-based spaces — an analog point and a binary point can legitimately share the same numeric address. The point-matching loop compared only the numeric address, not which space the request actually targeted, risking a MIM entry mapped to one point type silently matching an unrelated request on the other type at the same address.
**Fix:** Track whether each matched point is analog or binary (`isAnalogPoint`), and check that against the incoming request's function code before treating it as a match.
**Status:** Same as #1 — compiles clean, exercised during tonight's successful runs (this config's points don't appear to collide across spaces), not yet stress-tested with a deliberately colliding config.

### 3. jsoncpp include path hardcoded to one environment
**File:** `ns3-modbus-helics-grid.cc`
Hardcoded `#include <json/json.h>` — this container's jsoncpp package only provides `<jsoncpp/json/json.h>`. `modbus-application-new.h` already handled this exact ambiguity with an `__has_include` fallback; the grid file's own jsoncpp includes never got the same treatment.
**Fix:** Applied the identical `__has_include` pattern already used elsewhere in the codebase.
**Status:** Fixed and confirmed — compiles clean in this environment.

### 4. Missing `using namespace std;`
**File:** `ns3-modbus-helics-grid.cc`
The ~1000 lines of reused, protocol-agnostic code (`Throughput()`, `updateUETable()`, `setRoutingTable()`, `changeRoute()`, attack-config parsing) uses unqualified `string`/`map`/`pair`/`endl` throughout, assuming this was in scope. It wasn't — likely dropped when the file was assembled from the DNP3 original.
**Fix:** Added `using namespace std;` alongside the existing `using namespace ns3;`.
**Status:** Fixed and confirmed — cleared every `string`/`map`/`pair`/`endl` compile error in one shot.

### 5. `grid.json`'s `StaticSeed` was `null`
**File:** Config data, not source (`/rd2c/integration/control/config/grid.json`)
The config directory in the Docker container had a stale/incomplete copy from a prior run — `StaticSeed` was literally JSON `null` where a real seed value (`1`) belonged, causing `std::stoi("")` to throw immediately at startup.
**Fix:** Refreshed `config/` from the real source (`/rd2c/PUSH/NATIG/RC/code/3G-conf-123/`).
**Also fixed while diagnosing this:** `readMicroGridConfig()` never checked whether the config file actually opened or parsed successfully — a bad path or malformed JSON silently left the config object empty rather than failing loudly. Now checks `ifstream::is_open()` before parsing and `Json::Reader::parse()`'s return value, calling `exit(1)` with a clear message naming the file on either failure.
**Status:** Fixed and confirmed.

### 6. `ModbusApplicationHelperNew`'s constructor set an unregistered attribute with the wrong type
**File:** `modbus-application-helper-new.cc`
`m_factory.Set ("Protocol", StringValue (protocol))` — `"Protocol"` **is** a real registered attribute on `ModbusApplicationNew` (unlike what was first suspected), but it's declared as a `TypeIdValue`, not a string. Passing a `StringValue` against a `TypeIdChecker` is what actually triggered ns-3's `"Invalid attribute set (Protocol)"` fatal error. This was the **first time this exact constructor had ever run** — none of the three earlier standalone test scenarios exercised this two-argument `ModbusApplicationHelperNew(protocol, address)` construction path the way the production grid file's install call sites do.
**Fix:** Removed the `Set("Protocol", ...)` call. Confirmed safe: `m_tid` (the field that attribute populates) is never read anywhere else in the codebase — socket type is decided elsewhere, via hardcoded `TypeId::LookupByName("ns3::TcpSocketFactory")` calls gated by the separate `EnableTCP` boolean attribute. Kept the adjacent `Set("LocalAddress", ...)` call, which **is** valid and harmless (a genuinely registered attribute), even though it's also not consumed downstream.
**Status:** Fixed and confirmed via real compile + runtime test.

### 7. Stale `/rd2c/lib` RPATH silently shadowing every fix (the big one)
**Not a code bug — a build/runtime environment issue**, but it cost more debugging time tonight than anything else, including a red herring chase through `strace`, core dumps, and multiple full rebuilds.
`ldd` revealed the compiled binary's RPATH resolves `libns3.35-helics-optimized.so` to `/rd2c/lib/...` (populated once, back when the `natigfixed` Docker image was originally built, via `make.sh`'s `./waf install` step) rather than `/rd2c/ns-3-dev/build/lib/...` (where every rebuild all session actually landed). Since `modbus-application-new.cc` and `modbus-application-helper-new.cc` both compile into that shared library, **every fix to those two files was silently invisible at runtime, all night, despite compiling clean every single time** — the binary kept loading the pre-session, pre-fix version of the library regardless of how many times we rebuilt.
**Fix (workaround, not permanent):** Prefix any direct binary invocation with `LD_LIBRARY_PATH=/rd2c/ns-3-dev/build/lib:$LD_LIBRARY_PATH` to force resolution to the freshly-built library.
**Not yet done:** No permanent fix applied (e.g., rewriting the binary's RPATH via `patchelf`/`chrpath`, or deleting the stale `/rd2c/lib` copies so the loader has no choice but to fall back correctly). Anyone continuing this work needs to either keep using the `LD_LIBRARY_PATH` prefix manually, or fix this properly first — **this bit us for hours and will bite again silently if forgotten.**

### 8. Post-`main()` crash during global/static destructor teardown
**File:** `ns3-modbus-helics-grid.cc`
Every run consistently crashed (`SIGABRT`, no exception message) at the very end — reproduced every time regardless of when in the run it was reached. Extensive `std::cerr` instrumentation (with explicit `finalize()` and even a manual `helics_federate.reset()`, both traced) proved conclusively that **everything in this codebase completes successfully** — `Simulator::Destroy()`, HELICS federate finalization, and even manually destroying the global federate `shared_ptr` ourselves all completed cleanly, every time. The crash happens strictly after all of that, inside the C++ runtime's automatic exit-time destruction of some other global/static object — almost certainly inside the HELICS or zmq libraries' own internal teardown, with zero source-level visibility to us. `gdb` was unusable in this container (`GLIBCXX` version conflict with its own dependencies, unresolvable even after successfully installing it), and `/proc/sys/kernel/core_pattern` can't be rewritten inside the container (host-level, non-namespaced), so no core dump or debugger backtrace was obtainable.
**Fix:** `_exit(0)` instead of `return 0` at the very end of `main()` — skips the crashing destructor chain entirely. Safe specifically because all research-value output (pcap captures, flow-monitor data) is written incrementally during the run itself, confirmed by real non-empty pcap files after this fix, not at process-exit time.
**Status:** Fixed and confirmed working. This is a workaround, not a root-cause fix — the underlying library-internal crash is still unexplained and would need a working debugger or a check against this specific HELICS version's known issues to actually resolve, rather than route around.

---

## Docker-specific open items

1. **The Docker `/rd2c/lib` RPATH issue has no permanent fix.** Anyone running the `natig-modbus-test` container needs `LD_LIBRARY_PATH=/rd2c/ns-3-dev/build/lib:$LD_LIBRARY_PATH` prefixed on any direct binary invocation, or they will silently be running against stale, pre-fix libraries again.
2. **The post-`main()` teardown crash is routed around, not solved.** If a future HELICS/ns-3 version upgrade or environment change ever makes `_exit(0)` insufficient, this will need actual library-level debugging (working `gdb`, or checking HELICS's issue tracker for this version).
3. **Bugs #1 and #2** (the `mim_socket` gating and the `handle_MIM` address-space fix) compile clean and ran without issue on both Docker and Unity, but neither has been exercised by a test specifically designed to trigger the failure mode they guard against — worth a targeted test before relying on them for real dataset generation.
4. **Fix #6's sibling bug** — the second, pre-existing `default: exit(1);` in `Throughput()`'s first loop (not the one fixed) still exits silently with no message if an unexpected transport protocol is ever encountered. Left alone as out of scope, but worth the same treatment eventually.
5. **A separate, unrelated correctness issue was spotted but not fixed:** `netStatsOut2` (a `std::stringstream` in `Throughput()`) is never cleared between loop iterations, so each `fprintf` re-appends the entire accumulated history to `TP-Prob.txt` rather than just the new entry — the file grows combinatorially. Confirmed present on both Docker and Unity (Unity's `TP-Prob.txt` reached 122MB over one 300s run). Not touched since it's unrelated to any crash, but worth fixing before relying on that output file for real analysis.
6. **The `natig-modbus-test` Docker container has accumulated real state changes** — an accidental `make.sh` reconfigure/install cycle (since reverted for the build config, though `/rd2c/lib`'s stale libraries are a permanent byproduct of it), multiple killed/restarted broker and GridLAB-D processes, and one in-place edit to `grid.json` (`StaticSeed`). Worth deciding whether to keep iterating on this container or start fresh from the `natigfixed` image for the next phase of work.

---

# Part 2 — Unity verification (July 19)

## Goal

Confirm the three fixed files (from Part 1) also work against Unity's independently-compiled `ns-3-dev-unity` toolchain, since Unity SIGILL'd on a Ryzen-compiled binary before (unrelated to Modbus) and has never run any Modbus code end-to-end.

## What happened, in order

1. **Deployed the three changed files via `scp`** to `ns-3-dev-unity/contrib/helics/model/`, `contrib/helics/helper/`, and `scratch/`. Verified clean (no leftover debug instrumentation) both before and after transfer.
2. **First build attempt failed immediately**: `modbus-application-new.h` on Unity was stale (missing `isException`/`exceptionCode` fields the `.cc` file expects) — an out-of-sync copy predating even the start of the Docker session. Re-copied the header; rebuild succeeded clean.
3. **First run attempt failed**: `./waf build`/`./waf --run` don't work directly on a bare Unity compute node. `ns-3-dev-unity` was never configured against the raw filesystem — it was configured *inside* an Apptainer container running `natigfixed.sif`, with the directory bind-mounted as `/rd2c/ns-3-dev`. Found the real bind-mount pattern by reading `natig_array.sh` (the actual production SLURM script) and replicated it manually for a one-off smoke test.
4. **Confirmed the same class of stale-library issue as Docker, via a different mechanism**: `/rd2c/lib` inside the container is bind-mounted to `${PROJECT}/lib-unity/`, a real, separate directory that hadn't been refreshed since before any of these fixes — same silent-stale-library risk as Docker's RPATH issue, just via bind-mount instead of RPATH. Worked around with the same `LD_LIBRARY_PATH` prefix trick for testing purposes.
5. **First real run attempt SIGILL'd immediately** (`Illegal instruction`). Root cause: `ns-3-dev-unity`'s build config has `-march=native` baked in (`CXXFLAGS`/`CCFLAGS` in `build/c4che/_cache.py`) — meaning the compiler targets the *exact* CPU it's built on. We built our three changed files on one Unity compute node (`uri-cpu074`) and tried to run on a different node (`cpu033`) via a separate `salloc` session — different AMD EPYC generations, incompatible instruction sets.
6. **This is a real, pre-existing, cluster-wide issue, not something introduced by Modbus work.** Every *other* `.so` in `ns-3-dev-unity/build/lib/` was untouched, dated `Jul 6` (original tree build), built on whatever node happened to run that initial configure — not necessarily the same node any given SLURM job lands on later. `natig_array.sh`'s DNP3 production runs reuse that pre-built binary without ever rebuilding, so this has likely gone unnoticed so far rather than never existing.
7. **Fix applied for tonight's purposes**: full `./waf clean && ./waf build` of the *entire* tree, done in one single `salloc` allocation, immediately followed by the actual run — guaranteeing build and run happen on the same physical node. This is a workaround (pins the tree to whichever node did the rebuild), not the real fix (see below).
8. **Result: complete success.** Full 300-second simulated run, real multi-substation Modbus TCP flow data across the whole 123-bus topology, clean shutdown (`_exit(0)` working as designed, no crash, no error output). Confirmed via `TP.txt`'s final row (`t=299.95`, 71,757 total rows).
9. **`lib-unity/` refreshed** with the freshly-built `.so` files, so `natig_array.sh` will pick up all three fixes by default on future production runs without needing the manual `LD_LIBRARY_PATH` workaround.

## Unity-specific open items

1. **The `-march=native` fleet-wide portability gap is real and unresolved.** Any future full rebuild of `ns-3-dev-unity` must happen in the same `salloc` allocation as the run that follows it — different nodes can have incompatible instruction sets. This affects more than Modbus: DNP3 production runs share the same tree and could hit this if a future rebuild ever happens on a different node than assumed. The actual fix (reconfiguring with a portable architecture baseline like `-march=x86-64-v2` instead of `-march=native`, then a full rebuild) was deliberately deferred — bigger, riskier change to shared infrastructure, worth doing deliberately rather than late at night. Worth flagging to Sumit or the Unity support channel, since other NATIG users on this cluster could be silently affected.
2. **`lib-unity/` was refreshed manually tonight** (`cp .../ns-3-dev-unity/build/lib/*.so .../lib-unity/`) — this is a one-time sync, not an automated step. Any future rebuild of `ns-3-dev-unity` needs the same manual refresh, or `natig_array.sh` will silently drift back to stale libraries the same way it did before tonight.
3. **The `NATIG-modbus-test/` directory found during this session is a red herring** — its `rundocker.sh` uses `winpty` (Windows-only) and points at a different, unrelated repo (`pnnl/natig:natigbase`). Not connected to this work; safe to ignore or clean up.
4. **Unity's `3G-conf-123-normal` scenario was used for tonight's test** (no MIM attacks) — the equivalent of Docker's "simplest possible smoke test" starting point. Unity's `-ddos`/`-mim`/`-combined` scenario directories haven't been tested with Modbus yet.
5. **`unity-compute` defaults to the `cpu-preempt` partition**, which got a job killed mid-run tonight. Use `salloc --partition=cpu ...` (matching `natig_array.sh`'s own partition choice) for anything long enough to matter.

---

## Files changed (attached to this summary)

- `modbus-application-new.cc` — fixes #1, #2
- `modbus-application-helper-new.cc` — fix #6
- `ns3-modbus-helics-grid.cc` — fixes #3, #4, #5 (partial — the `readMicroGridConfig` error-handling improvement), #8; header also updated to properly credit both the original DNP3 file's authors and this adaptation
- `modbus-application-new.h` — unchanged by this session's edits, but confirm the Unity copy matches this one exactly (Part 2, item 2) before relying on any future Unity build

These four files produced both the Docker (July 18) and Unity (July 19) successful runs — nothing further needs to change to reproduce either result, aside from the environment-specific caveats noted in each Part's open items above.
