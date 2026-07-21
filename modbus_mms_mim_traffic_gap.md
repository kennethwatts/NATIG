# Finding: Modbus/MMS on-path MIM node never receives traffic

**Date:** 2026-07-20
**Found during:** Implementation and Docker validation of the new replay attack
(`attack_type 5`), branch `feature/replay`. Not caused by that work — replay's
validation is what happened to exercise this path for the first time.

**Status:** Confirmed via direct instrumentation. Root cause not yet isolated;
needs input on whether `Ipv4L3ProtocolMIM` was ever intended/validated to work
under TCP, or whether Modbus/MMS interception needs a different mechanism.

---

## TL;DR

The on-path MIM node's `handle_MIM` is never invoked — **zero packets** —
for Modbus or MMS, across a full 20s run against the real production topology
(`3G-conf-123/grid.json`), with `includeMIM: 1`. Same run, same config,
compromised-endpoint FDI fires correctly (15,259 times, matching DNP3/GOOSE's
counts exactly). DNP3's on-path MIM node, by contrast, is confirmed working
(5,701 packets intercepted in the same test).

This means: **any MIM-family attack (types 2/3/4, and now 5) targeting
Modbus or MMS should be treated as unvalidated against live traffic**, not
just the new replay attack. Only compromised-endpoint FDI has been proven to
actually fire on those two protocols. DNP3 and GOOSE are unaffected — GOOSE's
rogue-publisher attack doesn't use this mechanism at all (see below), and
DNP3's MIM path is directly confirmed working.

A DNP3-based result (LSTM blind to MIM attacks, Random Forest detects them)
predates and is unrelated to this finding, since DNP3's MIM traffic is
confirmed real.

---

## How it was found

While validating the new replay attack (captures a real point value pre-window,
replays it frozen once the window opens — see the four per-protocol commits on
`feature/replay`), DNP3 validated cleanly end-to-end against live HELICS/GridLAB-D
telemetry (900 captures, 626 successful replays). Porting the identical pattern to
Modbus and MMS, both compiled cleanly but showed **zero** replay activity — no
captures, no fires, no "no value captured yet" warnings either. That last part
was the tell: warnings require reaching the code at all.

`NS_LOG_INFO`/`NS_LOG_WARN` calls are silent in this container's optimized
build (`libns3.35-helics-optimized.so`) regardless of the `NS_LOG` env var, so
absence of those log lines alone wasn't conclusive — but instrumenting the
literal first line of `handle_MIM`'s receive loop with `std::cout` (guaranteed
visible, same reason FDI's own logging already uses `std::cout` instead of
`NS_LOG_INFO`) confirmed it directly: **0 hits** for both Modbus and MMS,
across the full 20s run, while FDI logged its usual 15,259 lines in the same
log file.

## What's ruled out

Compared Modbus's topology wiring (`ns3-modbus-helics-grid.cc`) against DNP3's
(`ns3-helics-grid-dnp3.cc`) line by line for the MIM node:

- **`Ipv4L3ProtocolMIM`/`victimAddr`** — the protocol-agnostic IP-layer
  redirect mechanism that makes the MIM node's L3 layer claim packets destined
  for the real victim's address. Wired identically in all three topology files.
- **Physical topology** — same `p2p.Install(NodeContainer(tempNode, MIMNode.Get(i)))`
  wiring, same static routes through the MIM node as gateway.
- **Socket role** — Modbus's MIM node has `isMaster=false`, correctly
  configured as a two-socket proxy (`m_socket` listens for the redirected
  master traffic; `mim_socket` connects out to the real outstation via
  `RemoteAddress2`) — this looks like the right shape for a TCP MITM.
- **Socket binding** — uses `Ipv4Address::GetAny()` (wildcard), not an exact
  address, so it's not simply failing an address-match check.

No pcap capture is currently enabled on the MIM node's NetDevice in this
topology, so there's no packet-level evidence yet of whether the redirected
SYN physically reaches the MIM node's interface at all, or arrives there and
is dropped by ns-3's TCP stack specifically.

## Working hypothesis

DNP3 uses UDP for its MIM node's socket; Modbus and MMS are TCP by protocol
definition. UDP's per-datagram dispatch in ns-3 is comparatively permissive.
TCP is connection-oriented, and `TcpL4Protocol`'s own endpoint demux likely
does additional matching/consistency checks on top of whatever
`Ipv4L3ProtocolMIM` does at L3 — it's plausible the promiscuously-redirected
SYN (still addressed, at the IP header level, to the real victim, not the MIM
node) never survives that TCP-specific demux, even though the identical
redirect works for UDP. This is not confirmed — would need pcap-level tracing
or a read of this fork's `Ipv4L3ProtocolMIM`/`TcpL4Protocol` interaction to
verify.

## Impact / scope

| Attack family | DNP3 | Modbus | MMS | GOOSE |
|---|---|---|---|---|
| FDI (compromised-endpoint) | validated | validated | validated | validated |
| DDoS (topology-level flood, separate mechanism) | unaffected | unaffected | unaffected | unaffected |
| MIM (types 2/3/4) | validated | **unvalidated — likely never fires** | **unvalidated — likely never fires** | validated (doesn't use this mechanism) |
| Replay (type 5, new) | validated (900 captures, 626 replays) | code-complete, blocked by this gap | code-complete, blocked by this gap | code-complete, not yet run end-to-end |

GOOSE's rogue-publisher attack (`handle_rogue_publish`) doesn't route through
`Ipv4L3ProtocolMIM` at all — it's UDP multicast pub/sub with no in-path
position, so the rogue role just publishes a competing frame over the same
group from a node that's already a legitimate subscriber. It's structurally
immune to whatever this gap turns out to be.

## Questions for Sumit

1. Was `Ipv4L3ProtocolMIM` ever validated against live TCP traffic, or only
   UDP (DNP3)? If it was designed/tested UDP-only, Modbus/MMS interception may
   need a different mechanism entirely (e.g., an actual NAT-style rewrite, or
   terminating two independent TCP connections rather than relying on L3
   promiscuous capture).
2. Is there existing pcap tooling/config for this topology that would let us
   directly confirm whether the redirected SYN reaches the MIM node's
   NetDevice, without adding new capture code from scratch?
3. Should any prior Modbus/MMS MIM-attack results (if such a dataset run
   happened before this session) be treated as suspect pending this gap being
   resolved?

## What's committed / where things stand

- `feature/replay` branch has replay (`attack_type 5`) implemented across all
  four protocols, compiles cleanly, and DNP3 is fully validated end-to-end.
- Modbus and MMS replay code is complete but unvalidated pending this gap.
- GOOSE replay (baseline case — replay a captured real frame verbatim,
  expect the subscriber's `stNum`/`sqNum` freshness check to reject it) is
  implemented and compiles, not yet run end-to-end.
- Dataset generation can proceed now on what's validated (FDI across all four
  protocols; MIM/replay on DNP3 and GOOSE), with Modbus/MMS MIM-family attacks
  scoped out until this is resolved.
