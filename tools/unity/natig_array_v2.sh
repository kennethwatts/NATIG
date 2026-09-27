#!/bin/bash
#SBATCH --job-name=natig_v2
#SBATCH --partition=cpu
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=24
#SBATCH --mem=96G
#SBATCH --time=19:00:00
#SBATCH --array=0-1199%50
#SBATCH --output=/project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/logs/natig_v2_%A_%a.log
#SBATCH --error=/project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig/logs/natig_v2_%A_%a.err

# ── Array index → scenario + run ID mapping ───────────────────────────────────
# 12 scenarios × 100 runs each = 1200 tasks (indices 0-1199)
#   0-99:    normal-dnp3       100-199:  fdi-dnp3          200-299:  replay-dnp3
#   300-399: normal-modbus     400-499:  fdi-modbus        500-599:  slowddos-modbus
#   600-699: normal-mms        700-799:  fdi-mms           800-899:  slowddos-mms
#   900-999: normal-goose      1000-1099: fdi-goose        1100-1199: replay-goose
#
# Config source for each scenario: runs/3G-conf-123-<TAG>/config/
# (generated + verified separately — see grid.json isolation check before using)

RUNS_PER_SCENARIO=100

# tag:protocol pairs, in fixed order matching the index ranges above
SCENARIOS=(
    "normal-dnp3:dnp3"
    "fdi-dnp3:dnp3"
    "replay-dnp3:dnp3"
    "normal-modbus:modbus"
    "fdi-modbus:modbus"
    "slowddos-modbus:modbus"
    "normal-mms:mms"
    "fdi-mms:mms"
    "slowddos-mms:mms"
    "normal-goose:goose"
    "fdi-goose:goose"
    "replay-goose:goose"
)

IDX=${SLURM_ARRAY_TASK_ID}
SCENARIO_IDX=$(( IDX / RUNS_PER_SCENARIO ))
RUN_IDX=$(( IDX % RUNS_PER_SCENARIO ))

ENTRY="${SCENARIOS[$SCENARIO_IDX]}"
TAG="${ENTRY%%:*}"
PROTOCOL="${ENTRY##*:}"
RUN_ID=$(printf "%02d" ${RUN_IDX})

# ── Binary + protocol-specific flags ──────────────────────────────────────────
case ${PROTOCOL} in
    dnp3)
        BINARY="ns3-helics-grid-dnp3"
        # NOTE (2026-08-20 correction): the 2026-08-07 fix removing --tpFileDir
        # was based on Docker/Ryzen's binary, which genuinely does not register
        # this flag (confirmed via --PrintHelp there). Unity's ns-3-dev-unity
        # build DOES register it (see scratch/ns3-helics-grid-dnp3.cc:525,
        # cmd.AddValue("tpFileDir", ...)), defaulting to "./" if omitted — i.e.
        # the process's cwd at launch, which is /rd2c/ns-3-dev (per this script's
        # own `cd` before the binary call), NOT RUN_DIR. Omitting the flag on
        # Unity caused TP.txt to be silently written to ns-3-dev-unity/TP.txt on
        # the HOST side (outside RUN_DIR entirely) rather than being missing —
        # confirmed via a real full-duration run (175,748 rows, t=299.95s) found
        # sitting in the wrong location. Docker and Unity are separate compiled
        # binaries from what should be the same source — do not assume their
        # CommandLine surfaces match without checking --PrintHelp on each one
        # directly before relying on a flag's presence or absence.
        EXTRA_FLAGS="--tpFileDir=/rd2c/integration/control/"
        RESULT_FILE="TP.txt"
        ;;
    modbus)
        BINARY="ns3-modbus-helics-grid"
        EXTRA_FLAGS=""
        RESULT_FILE="TP.txt"
        ;;
    mms)
        BINARY="ns3-iec61850-helics-grid"
        EXTRA_FLAGS=""
        RESULT_FILE="TP.txt"
        ;;
    goose)
        BINARY="ns3-iec61850-goose-helics-grid"
        EXTRA_FLAGS=""
        # CORRECTED 2026-08-23: perf.txt does not exist anywhere in this binary's
        # source. GOOSE uses the identical RD2C-env-var-based Throughput()/TP.txt
        # mechanism as DNP3/Modbus/MMS (confirmed via grep on ns-3-dev-unity's
        # scratch/ns3-iec61850-goose-helics-grid.cc). The prior perf.txt
        # assumption was a misapplied recollection of a *different*, pcap-based
        # GOOSE validation context (slow-DDoS analysis) that doesn't apply to
        # ordinary TP.txt-based scenarios.
        RESULT_FILE="TP.txt"
        ;;
    *)
        echo "FATAL: unknown protocol '${PROTOCOL}' for scenario '${TAG}'"
        exit 1
        ;;
esac

# ── Paths ─────────────────────────────────────────────────────────────────────
PROJECT=/project/pi_vinod_vokkarane_uml_edu/kenneth_watts/natig
SANDBOX=${PROJECT}/natigfixed.sif
NATIG_DIR=${PROJECT}/NATIG
CTRL_TEMPLATE=${PROJECT}/natigfixed_sandbox/rd2c/integration/control
SCENARIO_CONFIG=${PROJECT}/runs/3G-conf-123-${TAG}/config
NS3_UNITY=${PROJECT}/ns-3-dev-unity
LIB_UNITY=${PROJECT}/lib-unity
HELICS_DIR=${PROJECT}/natigfixed_sandbox/rd2c/HELICS
OUTPUT_BASE=${OUTPUT_BASE:-${PROJECT}/runs}
LOG_DIR=${PROJECT}/logs

# Flat naming to match build_master_dataset.py's glob('run_*') — do not nest under a scenario subdir
RUN_DIR=${OUTPUT_BASE}/run_${TAG}_${RUN_ID}

# Unique broker port + federate tag across the whole array (0-1199 → ports 9000-20999, 10 ports/task)
#
# UPDATED 2026-08-31: PORT_STRIDE=2 was insufficient. Confirmed via a real
# failure (run_fdi-mms_00, task IDX=700): BROKER_PORT was 10400, but the
# actual bind failure was on port 10406 -- SIX ports above base, not base+1.
# With --federates=2, the HELICS zmq broker apparently needs a wider port
# range than just push/pull (likely one or more ports per federate
# connection on top of the broker's own two). PORT_STRIDE=2 let adjacent
# tasks' real footprints overlap (task IDX's true range 10400-10406
# overlapped task IDX+1's reserved 10402-10403), reproducing the same class
# of collision the original stride-1->2 fix was meant to close, just at a
# wider radius. PORT_STRIDE=10 gives real margin above the confirmed
# 7-port footprint.
PORT_STRIDE=10
BROKER_PORT=$((9000 + IDX * PORT_STRIDE))
FED_TAG=${IDX}

# ── Verify the computed port is actually free before committing to it ─────────
# Added 2026-08-23 after fdi-dnp3's broker failed to bind its computed port
# (9000+task_id) even under --exclusive, with no collision possible among this
# batch's own jobs (each task's port is unique by construction). Rather than
# just killing whatever's on the port (see the pkill sweep below, which still
# runs for general hygiene), check availability directly and fall back to a
# scan range outside the array's own reserved port band if busy — avoids
# needing to know or assume anything about what's occupying the original port.
#
# UPDATED 2026-08-26: check_port_free() now checks the base port AND base+1,
# since HELICS zmq brokers bind both (push on base, pull on base+1) — the
# single-port check previously let two tasks whose base ports differed by
# exactly 1 collide on one task's pull port matching the other's base port.
#
# UPDATED 2026-08-31: check_port_free() now checks the ENTIRE PORT_STRIDE
# block (not just base/base+1), matching the wider real footprint confirmed
# above. Also: the fallback scan itself was found to have its own check-then-
# act race, independent of the primary-port fix -- confirmed via two tasks
# (slowddos-mms_96 and _97) that started on the same node in the same second,
# each independently found port 20000 free via check_port_free, and both
# then tried to bind it, both failing. Adding a randomized starting offset
# into the fallback range means simultaneous fallback attempts are unlikely
# to probe candidates in the same order, making a repeat collision far less
# likely (though not impossible -- a real fix would need a lock file or
# atomic reservation, not just randomization; this is a mitigation, not a
# guarantee).
check_port_free() {
    local port=$1
    for offset in $(seq 0 $((PORT_STRIDE - 1))); do
        (exec 3<>/dev/tcp/127.0.0.1/$((port+offset))) 2>/dev/null && { exec 3>&-; return 1; }
    done
    return 0
}

if ! check_port_free ${BROKER_PORT}; then
    ORIGINAL_PORT=${BROKER_PORT}
    # Randomize the starting point within the fallback range so concurrent
    # tasks hitting fallback at the same moment don't all scan in the same
    # order and converge on the same first-free candidate. Fallback range
    # starts at 30000, not 20000 -- with PORT_STRIDE=10 across 1200 tasks,
    # the primary range now extends up to 9000+1199*10+9=20999, so 20000
    # would have overlapped the primary range itself.
    FALLBACK_START=$(( 30000 + (RANDOM % 100) * PORT_STRIDE ))
    for candidate in $(seq ${FALLBACK_START} ${PORT_STRIDE} $((FALLBACK_START + 990))); do
        if check_port_free ${candidate}; then
            BROKER_PORT=${candidate}
            break
        fi
    done
    if [ "${BROKER_PORT}" = "${ORIGINAL_PORT}" ]; then
        echo "[$(date)] FATAL: computed port ${ORIGINAL_PORT} busy and no fallback port free"
        exit 1
    fi
    echo "[$(date)] WARNING: computed port ${ORIGINAL_PORT} was already in use — falling back to ${BROKER_PORT}"
fi

SIM_TIMEOUT=64800   # 18 hour hard limit — raised again 2026-08-31 after the
                     # 12hr limit still left 14/63 MMS-family retries timing
                     # out even under a "soft exclusivity" resubmission
                     # (16c/64G requested vs 8c/32G used, %30 concurrency,
                     # specifically meant to reduce node contention). All 14
                     # showed genuine progress at the correct ~80 rows/sec
                     # rate, just cut off between t=73.55s and t=289.45s --
                     # not a crash, a real timeout. Since reducing contention
                     # didn't meaningfully change the timeout rate (14/63 ~=
                     # 22%, statistically similar to the prior round's 53/282
                     # ~= 19%), the evidence now points toward MMS's genuine
                     # worst-case runtime sometimes exceeding 12hr on its own,
                     # not primarily a contention artifact. 18hr gives real
                     # margin above 12hr while this theory gets one more
                     # confirming/disconfirming data point.

mkdir -p ${LOG_DIR} ${OUTPUT_BASE}
export APPTAINER_CACHEDIR=/project/pi_vinod_vokkarane_uml_edu/.apptainer/cache

echo "=== NATIG v2.0 Array Job ${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID} ==="
echo "Scenario: ${TAG} (protocol: ${PROTOCOL}), Run: ${RUN_ID}, Binary: ${BINARY}, Port: ${BROKER_PORT}, RngRun: ${RUN_IDX}"
echo "Node: $(hostname)"
echo "Started: $(date)"
echo ""

# ── Defensive pre-launch cleanup ────────────────────────────────────────────────
# REMOVED 2026-08-26: this block used to run `pkill -9 -f helics_broker` /
# `gridlabd` / the ns3 binaries unscoped, on the theory that it was only ever
# mopping up orphaned processes from old manual debugging sessions. In practice,
# on a shared node running up to 50 concurrent array tasks (--array=0-1199%50),
# ANY task starting up killed every OTHER co-located task's already-running
# broker/gridlabd/ns3 process the instant it reached this line — confirmed as
# the dominant cause of the majority of array failures (672/1200 tasks exiting
# 137/SIGKILL), independent of and in addition to the separate broker-port
# adjacency bug fixed above. killall.sh is now PID-scoped to only kill what a
# task itself launched (see HELICS_PID/GRIDLABD_PID/NS3_PID below), which should
# prevent the orphan-process buildup this cleanup was originally added to guard
# against. If orphaned processes reappear, scope any future cleanup by PID or
# job/session ownership — never by bare process name — on a shared node.
sleep 2   # give the OS a moment to actually release any freed ports from a prior attempt on this node

# ── Setup run directory ───────────────────────────────────────────────────────
mkdir -p ${RUN_DIR}/output
mkdir -p ${RUN_DIR}/config
mkdir -p ${RUN_DIR}/scratch

# Baseline: glm files, output structure, default configs — from the standard control template
cp -r ${CTRL_TEMPLATE}/. ${RUN_DIR}/
cp -r ${NS3_UNITY}/scratch/. ${RUN_DIR}/scratch/ 2>/dev/null || true
rm -f ${RUN_DIR}/TP*.txt ${RUN_DIR}/perf.txt

# CTRL_TEMPLATE's output/ dir carries stale logs/results from whenever it was last used manually —
# confirmed via a smoke test (May 26 timestamps, 343MB gridlabd.log) silently surviving into a
# fresh task's directory. Clear contents (not the dir itself — some binaries expect subdirs to
# pre-exist) before every run so a broken run can never masquerade as having real output.
find ${RUN_DIR}/output -mindepth 1 -delete 2>/dev/null || true

# Overlay this scenario's verified config (Simulation/MIM/FDI/SlowDDoS blocks, points files)
cp ${SCENARIO_CONFIG}/*.json ${RUN_DIR}/config/
cp ${SCENARIO_CONFIG}/*.csv ${RUN_DIR}/config/ 2>/dev/null || true

# ── Verify the overlay actually landed before launching anything ──────────────
echo "[$(date)] Verifying scenario config overlay..."
python3 -c "
import json
with open('${RUN_DIR}/config/grid.json') as f:
    d = json.load(f)
sim = d['Simulation'][0]
print(f\"  includeMIM={sim['includeMIM']} includeFDI={sim['includeFDI']} MonitorPerf={sim['MonitorPerf']}\")
print(f\"  Attack blocks present: {[k for k in ('MIM','FDI','SlowDDoS') if k in d]}\")
" || { echo "FATAL: grid.json overlay verification failed"; exit 1; }

# ── Patch broker port ──────────────────────────────────────────────────────────
find ${RUN_DIR} -name "gridlabd_config.json" | while read f; do
    sed -i "s/\"brokerPort\": [0-9]*/\"brokerPort\": ${BROKER_PORT}/" "$f"
    sed -i "s/\"broker\": \"127\.0\.0\.1:[0-9]*\"/\"broker\": \"127.0.0.1:${BROKER_PORT}\"/" "$f"
done

# ── Make federate names unique per task ────────────────────────────────────────
# NOTE: the original sed-based approach only matched GLD/ns3 tokens in specific
# quote-adjacent positions (e.g. exact `"GLD"` or `"GLD/`) and silently missed
# embedded references inside longer path strings (e.g. the "value": "ns3/GLD/..."
# fields in ns_config.json's reroute filters) — confirmed via a smoke-test crash
# where sourcetargets got renamed but the matching destination value didn't,
# producing inconsistent federate names. This replaces it with a single regex
# pass that matches GLD/ns3 as a full path segment (bounded by quote, slash, or
# underscore on either side) wherever it appears, in both files, in one pass.
echo "[$(date)] Patching federate names with FED_TAG=${FED_TAG}"
python3 <<PYEOF
import re

tag = "${FED_TAG}"
files = [
    "${RUN_DIR}/config/gridlabd_config.json",
    "${RUN_DIR}/ns_config.json",
]

pattern_gld = re.compile(r'(?<=["/_])GLD(?=["/\$])')
pattern_ns3 = re.compile(r'(?<=["/_])ns3(?=["/\$])')

for path in files:
    with open(path) as f:
        content = f.read()
    n_gld = len(pattern_gld.findall(content))
    n_ns3 = len(pattern_ns3.findall(content))
    patched = pattern_gld.sub(f'GLD_{tag}', content)
    patched = pattern_ns3.sub(f'ns3_{tag}', patched)
    with open(path, 'w') as f:
        f.write(patched)
    print(f"  {path}: patched {n_gld} GLD refs, {n_ns3} ns3 refs")
PYEOF

echo "[$(date)] Setup complete — launching simulation"

# ── Truncate the run log for this task ─────────────────────────────────────────
# Added 2026-08-20: this file was previously opened with `tee -a`/`>>` and never
# truncated, so it silently accumulated EVERY past invocation across sessions —
# confirmed at 268MB+ after a handful of manual test runs over two weeks, mixing
# content from unrelated dates/attempts in one file. Truncate here so each task
# starts clean; matches the "always `>` to truncate at the start of a fresh
# attempt, not just on the very first run" lesson already documented for
# gridlabd_stdout.log/helics_broker.log.
RUN_LOG=${OUTPUT_BASE}/run_${TAG}_${RUN_ID}.log
> ${RUN_LOG}

# ── Background progress logger ─────────────────────────────────────────────────
# Added 2026-08-18 after a smoke test hit the SIM_TIMEOUT wall with no way to
# tell, after the fact, whether it was genuinely still progressing throughout
# or stalled early. Apptainer shares the host PID namespace here (confirmed via
# `ps aux` from the host seeing the in-container ns-3 process directly), so this
# can poll from the host without needing to enter the container. Logs a CPU-time
# snapshot every 5 minutes; steadily climbing time = real progress, a plateau =
# likely stalled.
PROGRESS_LOG=${OUTPUT_BASE}/run_${TAG}_${RUN_ID}_progress.log
> ${PROGRESS_LOG}
(
    while true; do
        sleep 300
        SNAPSHOT=$(ps aux | grep "${BINARY}" | grep -v grep)
        if [ -z "${SNAPSHOT}" ]; then
            echo "[$(date)] process not found (finished or not yet started)" >> ${PROGRESS_LOG}
        else
            echo "[$(date)] ${SNAPSHOT}" >> ${PROGRESS_LOG}
        fi
    done
) &
PROGRESS_LOGGER_PID=$!

# ── Run simulation: direct binary invocation, bypassing run.sh entirely ───────
# (matches the manually-validated launch pattern from Modbus/MMS/GOOSE/DNP3 sessions)
echo "[$(date)] Launch command: ./build/scratch/${BINARY} --helicsConfig=... --RngRun=${RUN_IDX} ${EXTRA_FLAGS}" \
    | tee -a ${RUN_LOG}

timeout ${SIM_TIMEOUT} \
apptainer exec \
    --bind ${RUN_DIR}:/rd2c/integration/control \
    --bind ${NATIG_DIR}:/rd2c/PUSH/NATIG \
    --bind ${NS3_UNITY}:/rd2c/ns-3-dev \
    --bind ${LIB_UNITY}:/rd2c/lib \
    --bind ${HELICS_DIR}:/rd2c/HELICS \
    --bind ${RUN_DIR}/scratch:/rd2c/ns-3-dev/scratch \
    ${SANDBOX} \
    bash -c "
        cd /rd2c/integration/control
        > helics_broker.log
        > gridlabd_stdout.log

        helics_broker --slowresponding --federates=2 --port=${BROKER_PORT} --loglevel=1 >> helics_broker.log 2>&1 &
        HELICS_PID=\$!

        # Wait for the broker to actually be listening before EITHER federate
        # tries to connect, instead of a blind sleep 3 before just GridLAB-D's
        # own startup check. Added 2026-08-29 after ~41 gridlabd_stdout.log
        # instances AND ~42 ns3-side run logs (proportional across dnp3/
        # modbus/goose/mms, not protocol-specific) failed with 'zmq broker
        # connection timed out ... receiver connection failure' -- a startup
        # race under heavy node contention, distinct from the earlier
        # port-adjacency collision bug. The old FATAL-only grep never caught
        # this because it only matched 'FATAL'/'unable to bind', not the
        # 'timed out' phrasing this race actually produces. A healthy broker
        # log is EMPTY on success (confirmed via a real passing run's
        # helics_broker.log), so there's no log message to poll for --
        # check the port itself instead. Both federates share this one wait
        # since both depend on the same broker being up, not on each other.
        BROKER_READY=0
        for i in \$(seq 1 30); do
            if (exec 3<>/dev/tcp/127.0.0.1/${BROKER_PORT}) 2>/dev/null; then
                exec 3>&-
                BROKER_READY=1
                break
            fi
            sleep 1
        done
        if [ \"\$BROKER_READY\" -eq 0 ]; then
            echo '[FATAL] helics_broker never became reachable on port ${BROKER_PORT} after 30s'
            kill -9 \$HELICS_PID 2>/dev/null || true
            exit 1
        fi

        gridlabd -D OUT_FOLDER=/rd2c/integration/control/output /rd2c/integration/control/IEEE_123_Dynamic.glm > gridlabd_stdout.log 2>&1 &
        GRIDLABD_PID=\$!
        sleep 3

        if grep -qi 'FATAL\|unable to bind\|timed out\|receiver connection failure' gridlabd_stdout.log; then
            echo '[FATAL] gridlabd failed to start — aborting task'
            cat gridlabd_stdout.log
            kill -9 \$HELICS_PID \$GRIDLABD_PID 2>/dev/null || true
            exit 1
        fi

        cd /rd2c/ns-3-dev
        LD_LIBRARY_PATH=/rd2c/lib:\$LD_LIBRARY_PATH ./build/scratch/${BINARY} \
            --helicsConfig=/rd2c/integration/control/config/gridlabd_config.json \
            --microGridConfig=/rd2c/integration/control/config/grid.json \
            --topologyConfig=/rd2c/integration/control/config/topology.json \
            --pointFileDir=/rd2c/integration/control/config \
            --RngRun=${RUN_IDX} \
            ${EXTRA_FLAGS} &
        NS3_PID=\$!
        wait \$NS3_PID
        SIM_EXIT=\$?

        cd /rd2c/integration/control
        HELICS_PID=\$HELICS_PID GRIDLABD_PID=\$GRIDLABD_PID NS3_PID=\$NS3_PID bash killall.sh 2>/dev/null || true

        # Preserve the SIMULATION's exit code, not killall.sh's — killall.sh is expected to
        # report nonzero/error when there's nothing left to kill, which previously masked
        # every real failure as a false success (confirmed: exit 0 on a run that never
        # produced TP.txt).
        exit \$SIM_EXIT
    " >> ${RUN_LOG} 2>&1

EXIT_CODE=$?

# Stop the background progress logger — no longer needed once the run has ended
kill ${PROGRESS_LOGGER_PID} 2>/dev/null || true

# ── Results ───────────────────────────────────────────────────────────────────
if [ ${EXIT_CODE} -eq 124 ]; then
    echo "[$(date)] TIMEOUT after ${SIM_TIMEOUT}s"
else
    echo "[$(date)] Simulation complete — exit code ${EXIT_CODE}"
fi

# NOTE (2026-08-23): GOOSE's TP.txt output is unreliable as a success signal —
# confirmed via normal-goose_00: real, sustained multicast traffic (459 accepted
# frames, reaching t=299.018s) with TP.txt staying at 0 rows the whole time.
# Root cause: ns-3's FlowMonitor/Ipv4FlowClassifier appears unable to properly
# track flows routed via SetDefaultMulticastRoute, even though both the
# publisher and subscriber nodes are correctly covered by flowHelper.Install()
# (traced through source — this is not a node-coverage bug in our topology
# code). Rather than rely on TP.txt for this one protocol, count real
# application-layer "accepted" log lines instead — a directly-confirmed,
# reliable signal of genuine GOOSE traffic.
if [ "${PROTOCOL}" = "goose" ]; then
    ACCEPTED_COUNT=$(grep -c "GooseApplication (subscriber): accepted" ${RUN_LOG} 2>/dev/null || echo 0)
    LAST_T=$(grep "GooseApplication (subscriber): accepted" ${RUN_LOG} | tail -1 | grep -oP 't=\K[0-9.]+' )
    if [ "${ACCEPTED_COUNT}" -gt 0 ]; then
        echo "[$(date)] GOOSE traffic: ${ACCEPTED_COUNT} accepted frames, last t=${LAST_T}s ✓"
    else
        echo "[$(date)] GOOSE traffic: 0 accepted frames ✗"
    fi
    # Still report TP.txt's status too, for visibility, but it does not gate success for GOOSE
    if [ -f ${RUN_DIR}/${RESULT_FILE} ]; then
        ROWS=$(wc -l < ${RUN_DIR}/${RESULT_FILE})
        echo "[$(date)] (info only, not authoritative for GOOSE) ${RESULT_FILE}: ${ROWS} rows"
    else
        echo "[$(date)] (info only, not authoritative for GOOSE) ${RESULT_FILE}: MISSING"
    fi
elif [ -f ${RUN_DIR}/${RESULT_FILE} ]; then
    ROWS=$(wc -l < ${RUN_DIR}/${RESULT_FILE})
    LAST_T=$(tail -1 ${RUN_DIR}/${RESULT_FILE} | awk '{print $1}')
    echo "[$(date)] ${RESULT_FILE}: ${ROWS} rows, last t=${LAST_T}s ✓"
else
    echo "[$(date)] ${RESULT_FILE}: MISSING ✗"
fi

echo ""
echo "=== Done: $(date) ==="

# Propagate the simulation's exit code to SLURM so an aborted/crashed run shows
# as FAILED in sacct instead of COMPLETED 0:0. EXIT_CODE was captured from the
# apptainer exec above; the inner "exit \$SIM_EXIT" only sets the container shell.
exit ${EXIT_CODE}
