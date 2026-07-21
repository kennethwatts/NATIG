# Session Summary: Slow DDoS (DNP3, Modbus, MMS, GOOSE)

**Date:** 2026-07-21
**Branch:** `feature/slow-ddos` (off `feature/replay`)
**Status:** Modbus and MMS fully validated end-to-end in Docker (real TCP
connection exhaustion). GOOSE fully validated end-to-end (sustained
CSMA-segment congestion), with a real tuning finding along the way. DNP3
is blocked on a pre-existing, unrelated bug in the vendored DNP3 library's
TCP transmit path -- scaffolding exists but is not validated. Not yet
validated on Unity.

---

## What this branch adds

A "slow DDoS" threat, complementing the existing flat-rate flood DDoS
(topology-level, `Ipv4RawSocketFactory` + `OnOffHelper` burst against a
raw IP victim). "Slow" needed a different definition per protocol family:

- **DNP3/Modbus/MMS** (TCP protocols): real Slowloris-style connection
  exhaustion. A new bot opens many genuine TCP sockets against the
  target's actual listening port and holds them open, sending nothing.
  Verified beforehand (this session) that none of the three servers cap
  accepted connections or enforce an idle-connection timeout, so a
  silently-held-open socket is sufficient to exhaust a real resource --
  no handshake/timeout trickery needed.
- **GOOSE**: connectionless UDP multicast, so connection exhaustion
  doesn't apply. An earlier candidate (rogue publisher publishes less
  often to starve a subscriber timeout) was investigated and rejected --
  there's no subscriber-side staleness timer in this codebase at all, and
  the real publisher runs independently of any rogue instance, so
  slowing a rogue down starves nothing. Implemented instead as sustained
  low-rate congestion of the actual CSMA segment GOOSE frames traverse.

New config: top-level `"SlowDDoS"` array, sibling to `"DDoS"` (same
category -- a topology-level bot swarm, not a per-instance `AttackConf`
attack). Documented in `ATTACKEXAMPLE.md`. Field split by protocol:
`NumberOfBots`/`threadsPerAttacker`/`Active`/`Start`/`End`/`NodeType`/
`NodeID`/`endPoint` reuse the `DDoS` convention (for GOOSE, `NodeID`
instead lists microgrid indices to jam); `ConnectionsPerBot`/
`ConnectRate`/`TrickleBytes`/`TrickleInterval` are new, DNP3/Modbus/MMS
only; `PacketSize`/`Rate`/`TimeOn`/`TimeOff` reuse `DDoS`'s field names,
GOOSE only, with very different guidance values (see tuning finding
below).

## Implementation pattern

- **DNP3/Modbus/MMS**: new shared `SlowlorisBotApplication`/
  `SlowlorisBotApplicationHelper` (protocol-agnostic -- it has zero
  protocol framing, just opens a `TcpSocketFactory` socket and holds it).
  Self-reschedules `OpenNextConnection()` at a fixed low rate
  (`ConnectRate` connections/sec) rather than opening all connections in
  one tick, which is what actually makes connection *opening* slow
  (a same-tick burst would just be a SYN-flood variant of the existing
  DDoS). Wired into each topology file the same way flood-DDoS's own
  `botNodes` are: node/link/address setup happens unconditionally,
  *before* the file's single `PopulateRoutingTables()` call (a second
  call anywhere later in these files segfaults -- confirmed by
  instrumented testing, and each file's own pre-existing commented-out
  second calls are a record of the same landmine); actual bot
  application install is gated on `SlowDDoS.Active` and happens later,
  once the target's real port variable is in scope.
- **GOOSE**: no bot application class needed. A jammer node is attached
  directly onto the existing per-microgrid `CsmaChannel` object (the
  standard ns-3 "additional device on an existing channel" pattern,
  already referenced in this file's own comments) and sends a
  subnet-directed broadcast UDP hum -- not the real GOOSE multicast
  group/port, so there's no risk of it being mistaken for a real frame,
  and no `PopulateRoutingTables()` ordering concern since the jammer only
  ever broadcasts within its own directly-connected segment.
- Topology wiring touched: `ns3-modbus-helics-grid.cc`,
  `ns3-iec61850-helics-grid.cc` (MMS), `ns3-iec61850-goose-helics-grid.cc`,
  and a new dedicated `ns3-helics-grid-dnp3-slowddos.cc` (see DNP3 section
  below for why this is a separate file rather than a modification of
  `ns3-helics-grid-dnp3.cc`).

## Commits (in order)

1. `cecc06e` -- Fix DNP3 `ConnectToPeer`'s null `mim_socket` deref
   (prerequisite bugfix, unrelated to slow-DDoS's own code -- found while
   scoping DNP3's `EnableTCP` path, same "unrelated bug found while
   validating X" pattern as replay's `9e33913`). Modbus had already hit
   and fixed the identical bug in a prior session.
2. `72fbdb3` -- `SlowlorisBotApplication`/-Helper, no topology wiring yet.
3. `7e0887f` -- Fix: missing `NS_OBJECT_ENSURE_REGISTERED` on the new
   class (found via runtime test: `ObjectFactory::Set` failed with
   "Invalid attribute set" -- the exact same gotcha Modbus's own file
   already has a comment about, and missed again here for the same
   reason).
4. `0b873c7` -- `SlowDDoS` config section (`Active: 0`) added to all
   seven example `grid.json` configs that already have a `DDoS` block.
5. `66ec856` -- Modbus wiring, validated.
6. `909d061` -- MMS wiring, validated.
7. `45f7ba2` -- DNP3 scaffolding (`ns3-helics-grid-dnp3-slowddos.cc`),
   blocked -- see below.
8. `1b417b2` -- `ATTACKEXAMPLE.md` documentation.
9. `5d690f2` -- GOOSE wiring, validated, plus a follow-up tuning fix to
   the example `Rate`/`PacketSize` values (see tuning finding below).

## Validation

Ran Modbus, MMS, and GOOSE production topologies end-to-end in the
`natig-modbus-test` Docker container, against the real `helics_broker` +
`gridlabd` + `IEEE_123_Dynamic.glm`, using `3G-conf-123/grid.json`'s
`SlowDDoS` entry (`NumberOfBots: 8`, `Start`/`End` widened to 5/15 within
a 20s `SimTime` so there's real time both before and after the window).

- **Modbus**: 8 bots, `ConnectionsPerBot: 20` at `ConnectRate: 2.0`/sec
  each. `m_socketList.size()` on the outstation grew monotonically to 72
  concurrent held-open connections by the end of the attack window, only
  dropping at `StopApplication`'s bulk close -- no crash, no cap hit.
  Confirmed the `Active: 0` default path also still runs cleanly (only
  the legitimate master's own connection accepted, no bot traffic).
- **MMS**: identical setup and identical result (72 connections, clean
  growth, no crash) -- MMS already runs `EnableTCP=true` everywhere, so
  no prerequisite fix was needed.
- **GOOSE**: jammer attached to microgrid index 2 (`mg3`)'s CSMA segment.
  **First attempt failed to show any effect**: `Rate: "8kb/s"`,
  `PacketSize: 64` (a literal "low absolute bandwidth" reading of "slow")
  produced no measurable difference in `mg3`'s frame inter-arrival timing
  vs. an unjammed control microgrid, against this topology's 100Mbps CSMA
  segments -- the hum was simply too small relative to the channel to
  ever contend for it. **Re-tested at `Rate: "70Mb/s"`, `PacketSize:
  1400`** (a large fraction of the channel's own capacity, sustained
  continuously): `mg3`'s worst-case inter-frame gap during the attack
  window was 0.62s vs. 0.008s for the control microgrid in the same run
  (~77x), with elevated gaps still visible for several seconds after the
  attack window closed (residual queued traffic draining). Updated
  `ATTACKEXAMPLE.md`'s guidance and all seven example `grid.json`
  `SlowDDoS.Rate`/`PacketSize` defaults to the validated values, with an
  explicit note: "slow" here means sustained/continuous duty cycle
  (`TimeOn` covering the whole window, `TimeOff: 0`), not small absolute
  rate -- tune `Rate` relative to the deployment's own CSMA `DataRate`.
- **DNP3**: not validated -- see below.

### A note on log visibility

Same gotcha as every prior session on this branch family: `NS_LOG_INFO`/
`NS_LOG_WARN` are compiled out in this container's optimized build. All
new instrumentation this session (`m_socketList.size()` tracking on
Modbus/MMS/DNP3's `HandleAccept`, a timestamp added to GOOSE's existing
subscriber accept log) uses `std::cout`, per the established precedent.

### A note on debugging without gdb

`gdb` is present in this container but non-functional -- it fails to
even launch (`GLIBCXX_3.4.26 not found`, a stale system `libstdc++`
mismatch with `gdb`'s own dependencies), independent of any
`LD_LIBRARY_PATH` set for the target process. Confirmed by trying both
with and without `LD_LIBRARY_PATH` set for `gdb` itself (`set environment`
inside a `gdb -batch` invocation doesn't help, since `gdb`'s own dynamic
linking already happened before that). The DNP3 root cause below was
found by reading the vendored library source directly (tracing
`mim_socket`'s construction sites) after a silent-crash pattern (process
exits with zero output, no exception text) repeated across several
instrumented re-runs -- the same "absence of an error message is not
evidence of no error" lesson `feedback_ns3_build_environment_gotchas.md`
already documents for `NS_LOG_*`, applying here to segfaults instead.

## DNP3: blocked, deferred

Two distinct bugs were found, only one of which is fixed:

1. **Fixed** (`cecc06e`): `ConnectToPeer`'s unconditional
   `mim_socket->Connect(...)` null-derefs under TCP mode, since
   `makeTcpConnection()` never constructs `mim_socket` (only
   `makeUdpConnection()` does). One-line guard, mirrors Modbus's own
   pre-existing fix for the identical issue.
2. **Not fixed, deeper**: `startMaster()`
   (`dnp3-application-new-Docker.cc:1025`) constructs the DNP3 library's
   `Endpoint` object with `mim_socket` as its transmit socket. Under UDP
   mode this worked because `mim_socket` was always a real, usable socket
   regardless of node role (`makeUdpConnection` builds it unconditionally
   for every instance). Under TCP mode `mim_socket` is *never*
   constructed for *any* node, so `Endpoint::send()`
   (`RC/code/dnp3/dnplib/endpoint.cpp:149-151`) schedules `Socket::Send`
   on a null socket via `Simulator::Schedule` whenever `tcp=true` --
   which doesn't crash at setup (the schedule call itself succeeds), only
   later when that scheduled event actually fires during
   `Simulator::Run()`, i.e. whenever the master genuinely tries to
   transmit. This is why it surfaced as a silent process exit with zero
   output rather than an immediate, traceable failure.

**This means any DNP3 master under `EnableTCP=true` is broken today, with
or without slow-DDoS in the picture** -- not something scoped to the new
bot's own traffic. A real fix requires auditing how `mim_socket` vs. the
primary `m_socket` are used across the vendored `dnplib`
`Endpoint`/`startMaster` integration: specifically, whether an ordinary
(non-MIM) master-role node can safely alias `mim_socket` to the primary
socket under TCP, and whether MIM/Inside-role instances (which
legitimately use `RemoteAddress2` as a second relay target) need their
own separate, real TCP connection instead of just an alias. That's a
substantially larger, riskier change than a one-line guard, sitting
inside vendored library integration code neither this nor the prior
session fully audited -- deferred as its own follow-up rather than
attempted here.

`ns3-helics-grid-dnp3-slowddos.cc` exists as scaffolding (a copy of
`ns3-helics-grid-dnp3.cc` with `EnableTCP=true` and the same
`SlowlorisBotApplication` wiring already validated on Modbus/MMS) for
whenever that fix lands -- it compiles cleanly but is **not** validated
end-to-end; running it crashes once the DNP3 master's own periodic poll
first tries to transmit. `ns3-helics-grid-dnp3.cc` itself was
deliberately left untouched (stays UDP-only) so replay/FDI/MIM's already-
validated results on that file aren't put at risk by an unrelated
transport change.

## Open items for next session(s)

- **DNP3's `mim_socket`/`Endpoint` TCP bug** (see above) -- needs a design
  decision on the aliasing question before it can be fixed, then
  DNP3 slow-DDoS validation on `ns3-helics-grid-dnp3-slowddos.cc`.
- **Unity validation not done.** Same caveat as every protocol before
  this one -- see `feedback_ns3_build_environment_gotchas.md`.
- **4G/5G/Docker topology-file parity for DNP3/Modbus/MMS/GOOSE slow-DDoS**
  not done -- only the primary (non-4G/5G) topology file per protocol was
  wired this session, matching the precedent replay's own branch set
  (`4682e85` only touched the same primary files first).
- Dataset generation can proceed now on what's validated (Modbus/MMS
  connection exhaustion, GOOSE congestion), with DNP3 slow-DDoS scoped
  out pending the `mim_socket`/`Endpoint` fix above.
