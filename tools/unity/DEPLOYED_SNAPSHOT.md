# Deployed Unity build-tree snapshot — slow-DDoS / Slowloris

**Date:** 2026-09-28. **Provenance only — NOT deployed from here, NOT rebuilt.**

These files are a verbatim snapshot of what is actually compiled and run on the
Unity cluster for the Modbus/MMS slow-DDoS scenarios. They were developed and
deployed directly in the Unity build tree, out of band, and had **diverged from
the branch copies under `RC/code/`** (which predate the SlowDDoS feature: the
`RC/code/` topology files are 1367/1385 lines and contain no `SlowDDoS`/
`SlowlorisBotApplication` code). This snapshot captures the running code so the
branch has a faithful record of it.

`$BUILD = /project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/ns-3-dev-unity`

## Topology sources (`tools/unity/scratch/`)
| file | source in build tree | deployed mtime | size | sha256 |
|---|---|---|---|---|
| `ns3-modbus-helics-grid.cc` | `$BUILD/scratch/` | 2026-07-24 00:57 UTC | 77001 | `f8db0d862c992b58b2ef51cc0dea38a7e1da4e072e98470c4613e77be0bf1561` |
| `ns3-iec61850-helics-grid.cc` | `$BUILD/scratch/` | 2026-07-24 00:57 UTC | 78034 | `cdab3d84b35192a5e5f736e46df8895a083b9f0958867387299ac98cd56ddb5a` |

## Slowloris bot application (`tools/unity/model/`)
| file | source in build tree | deployed mtime | size | sha256 |
|---|---|---|---|---|
| `slowloris-bot-application.cc` | `$BUILD/contrib/helics/model/` | 2026-07-24 14:48 UTC | 7499 | `c61d9539e8687fbff99a3b5a6e34a94ec4ef9f60f210cd028cdfa2c06b8aa829` |
| `slowloris-bot-application.h` | `$BUILD/contrib/helics/model/` | 2026-07-24 14:48 UTC | 3496 | `998e7efc78e0bdee521e3118299e37b2b75c046fb69620c132866629353ee1cf` |

## Scenario configs (`tools/unity/configs/`)
Copies of the two slow-DDoS `grid.json` files **after** the 2026-09-28 fix
(`SlowDDoS.endPoint`: `"MIM"` → `"subNode"`; flood-`DDoS.endPoint` left `"MIM"`).
Both files are byte-identical (grid.json does not vary by protocol). sha256
`2c8c2b761a610b017e9b1438d3d1be85b48f79d11386cdb3314c6f0ab1a02e8b`.
- `grid.slowddos-modbus.json` ← `$PROJECT/runs/3G-conf-123-slowddos-modbus/config/grid.json`
- `grid.slowddos-mms.json`    ← `$PROJECT/runs/3G-conf-123-slowddos-mms/config/grid.json`
On Unity the pre-edit originals are backed up as `grid.json.bak-20260928_035504`
in each scenario's `config/` dir.

## The fix (why)
`SlowlorisBotApplicationHelper(victimAddr, port)` dials `port = 20000`, which is
the Modbus **outstation** / MMS **server** listener port — and those listeners
run only on the **Microgrid (subNode)** nodes. With `endPoint="MIM"` and
`includeMIM=0`, the bots aimed at a populated-but-idle MIM ring node (no listener
on 20000) → every connection RST'd. `endPoint="subNode"` (NodeID 2 = `mg3`)
points them at a real listener on the exact port. Config-only fix keeps the
compiled binary unchanged, so the `normal-modbus` / `normal-mms` regression
anchors stay byte-identical.
