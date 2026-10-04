# DRAFT email to Sumit — on-path MIM over TCP (Modbus/MMS) doesn't intercept

**Status:** Draft for Ken to review and send himself — not sent.
**Date drafted:** 2026-10-04
**Continuation of:** the earlier NATIG v2.x thread (the Store()/DoEndpoint gap write-up from Sep 21).

---

**Subject:** NATIG v2.x — on-path MIM doesn't intercept over TCP (Modbus/MMS); a design question before I scope the next dataset

Hi Sumit,

Another NATIG finding in the same vein as the Store()/DoEndpoint one I sent last month — this is
again a case where a mechanism looks wired correctly but silently never engages, and it affects
what I can put in the next dataset. I wanted to lay it out and get your (or your lead expert's)
read before I commit to a fix direction, since I think it needs a different shape than the fixes
I've landed so far.

**1. What's broken.** No on-path MIM-driven attack fires on Modbus or MMS over TCP — and this is
not just the replay attack (type 5) I was validating; it's the whole MIM family (types 2/3/4/5).
I confirmed it today on both protocols with a dedicated config that has *zero* type-5 entries
(MIM nodes set to attack types 3 and 4 only, matching what the production `3G-conf-123` grid.json
actually uses — e.g. MIM2 at type 4, MIM4 at type 3). In a full 20 s run against the real
123-bus co-simulation:

- `handle_MIM` gets **0 hits** and the MIM proxy application accepts **0 TCP connections** — only
  the real outstation ever accepts a connection.
- Meanwhile FDI (the compromised-endpoint path) fires normally — 15,244 fabrications on MMS in the
  same run — so the simulation, HELICS federation, and the non-MIM attack path are all healthy.
  The on-path MIM/TCP mechanism is the only thing dead.

Same result on Modbus and MMS, byte-identical whether or not a type-5 entry is present.

**2. Root cause (precise).** This is structural, not a per-attack-type bug. The MIM proxy's
`victimAddr` now resolves correctly — the loopback regression from the earlier `feature/fdi` work
is genuinely fixed, and it resolves to the real hub-side address, not 127.0.0.1. The problem is
*which* address it is: `victimAddr` is the **reply-direction** address (the address the outstation
sends its replies back to), while a TCP master opens its connection **toward the outstation's own
address** directly. So the L3 transparent-redirect model — which works for DNP3 because DNP3 is
UDP and every datagram is matched and redirected individually — never captures the TCP **SYN** in
the first place: the SYN's destination is the outstation, not `victimAddr`, so it's forwarded to
the real outstation, which accepts the connection directly. The MIM is bypassed as a TCP party
entirely. `attack_type` is only read *inside* `handle_MIM`, which never runs, so the behavior is
identical for types 3, 4, and 5 — which is exactly what the type-3/4-only run confirmed
(0 accepts, same as the type-5 run).

**3. Why I think this needs a different fix shape.** The UDP-style `victimAddr` interception
simply doesn't translate to TCP's connection-oriented model — there's no per-packet redirect that
makes an established master↔outstation TCP connection route through a third party after the fact.
My current read is that a working TCP MIM needs to be a **real proxy**: the master connects to the
MIM node's *own* address, the MIM terminates that connection and independently opens a second
connection onward to the outstation, and relays (and tampers) between the two — rather than an
L3-level address rewrite that tries to be transparent. I haven't committed to that; I'm flagging
it as the open design question rather than a fix I've already built, because it touches the shared
`Ipv4L3ProtocolMIM` path that DNP3 depends on and I don't want to destabilize the working UDP case.
This feels like the kind of thing your lead expert (Oceane?) would have a quick opinion on, if the
original MIM design intent for TCP is written down somewhere.

**4. Scope / impact for the dataset.** To be clear about what this does and doesn't touch:

- **Unaffected and already validated:** DNP3 MIM (all attack types — its UDP interception works;
  `handle_MIM` fired ~5,400 times in the same test round). FDI / compromised-endpoint injection on
  all four protocols. And the new slow-DDoS (Slowloris) connection-exhaustion attack on Modbus and
  MMS, which I validated today — 401 accepted connections, 400 held open with 0 failures at the
  retargeted mg1 outstation, i.e. clean exhaustion against a real listener.
- **Affected:** only Modbus/MMS's *on-path MIM* family (which is what the replay attack rides on).
  Those scenarios are currently silent no-ops, so I'll be excluding Modbus/MMS MIM rows from the
  dataset until this is resolved.

**5. The ask (same kind as last time).** Is this worth a proxy-model redesign of the TCP MIM now,
or should the next dataset (v2.2) proceed with what's validated — DNP3 MIM, FDI on all four
protocols, and Modbus/MMS slow-DDoS — and leave a working Modbus/MMS on-path MIM to be scoped as a
separate piece of work? Either way I'll keep the design modular and merge-friendly as we discussed;
I mostly want to avoid half-building the TCP MIM one way if there's a known-right approach I should
follow instead.

Happy to share the repro config and the exact log evidence if that's useful.

Thanks,
Ken
