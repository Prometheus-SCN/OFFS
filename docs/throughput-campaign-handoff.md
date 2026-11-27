# Throughput campaign handoff (2026-11-27)

Read this before touching the throughput/perf work. Companion docs:
`docs/msquic-bump-design.md` (msquic bump scope, committed), Harmony
OFFS-254/271/272/273/274/275 and the auto-memory
`project_throughput_campaign.md`.

## 1. Where the campaign stands (all committed + pushed)

Upstream (both remotes): liboffs master `a13710c`, OFFS master `6c6d5ae`,
pins: deps/liboffs `a4a0218`, deps/poll-dancer `0ccb488`. Runtime junk
(cert droppings, offs_cache, peer_store.cbor, deps/WaveDB WIP) is
deliberately untracked; the norelay docker deploy trio is committed.

Accepted best transfer state, measured 3x consistent on a converged
multi-holder fleet: **10MB cold load in 4s, 0s warm, sha256-exact**
(fresh10.bin, sha 95833f55af...99755b). Stack: per-message streams
(a3daee9-era OFFS-254 phase 1) + raced-StreamOpen retry (51a0bda) +
hop-resolvability filter (e307a99) + PeerUnidiStreamCount 65535 (0fad109)
+ receive fall-through fix (0b966df) + mute-layer repairs
(1672fcd/440e9b9/b7daacd/4deffcf/cd594b2) + offer-list top-K (dec3347) +
slot reconciliation (1723fc7) + **consumer fetch window 64 (6dba65e)** +
**find fanout 2 (43872fa)** + **stream recv window 256KB (8f9b2f2)** +
**priority lane (a4a0218)** + harness ADD_PEER reachability (99ea0f9).
msquic itself is UNCHANGED: vendored v2.5.0-402 (931047700, May 2026).

## 2. The one open fix: relay-endpoint staleness after relay reconnection

Traced with a full log run (RecallBlockRoundTrip, OFFS-254 comment
2026-11-27). Mechanism: a relay connection dies by transport
(status 0x6e = ETIMEDOUT ≈ 20 min in, DESPITE idle 120s + keepalive 15s on
both ends — suspected spurious timeout at the v2.5.0-402 vintage; the
v2.6.4 runs showed ZERO SHUTDOWN_BY_TRANSPORT). After the death, peers
keep sending to the dead endpoint id; the relay drops them
("dest endpoint 1 not found" every 10s). The receipt-time id refresh
(network.c RELAY_RECEIVED) heals this only when the re-registered node
sends before the in-flight reply leg — mid-flow deaths poison the response
leg. Candidate fixes (design review, then pick): (a) relay error envelope
"dest endpoint X not found" → sender refreshes/retries; (b) client
endpoint-update broadcast on re-registration; (c) connection-generation
counters on peer entries. NOTE: v2.6.4 killing the spurious 0x6e death
would remove the main production trigger for the staleness — that couples
this fix to the bump work below.

## 3. The open diagnosis: msquic 2.6.4 throughput regression (CONTAMINATED so far)

What was measured across four runs, none clean:

| mesh | result |
|---|---|
| v2.5.0-402 (both), multi-holder, converged | 4s/82 tuples, 363 blocks (1.48x dup) |
| v2.5.0-402 (both), fresh fleet, 1 holder (u40) | 4s failed 0/82 (mesh not converged), then 60s/63 tuples, then 30s/81 |
| 2.6.4 requester + 2.5.0-402 holders | 21s, 490 blocks |
| 2.6.4 uniform (norelay14 fleet + home 2.6.4 build) | 21s/490, then 45s/295 (after StreamRecvBufferDefault 256KB), second run variance |
| 2.6.4 with 0x6e deaths | NONE — zero SHUTDOWN_BY_TRANSPORT on 2.6.4 |

**Critical confound discovered: replication spread / holder count.** A
freshly redeployed fleet has fresh /data (only the seeding node holds the
file); anti-entropy spreads it over 10-60+ min. The single-holder state is
slow at BOTH vintages (~10 blocks/s), the multi-holder state fast at the
old vintage (90 blocks/s). So today's "2.6.4 regression" numbers
(21s/45s) are CONTAMINATED — they were all measured on unreplicated or
mixed-churn states. The 4s baseline was a multi-holder state.

**The clean experiment to run first (do not skip):** identical fleet,
identical replication state, both vintages. Protocol: deploy fleet → seed
file on ONE node → wait fixed intervals (e.g. measure t+5min, t+30min,
t+60min after put — anti-entropy replication state evolves) → run the
same single load each interval → compare per-interval between vintages.
Also instrument holders: count `find: hash` receipts per holder
(azexec on each node's /proc/1/fd/1 does NOT work — use az container logs
with a fresh tail per run instead) to distinguish "find never arrived"
from "holder answered slowly".

If the regression holds at identical state: bisect msquic between
931047700 (May 2026) and v2.6.4 (candidates: all releases v2.5.11,
v2.5.12, v2.5.13, v2.6.1..2 — release line tags exist). Suspects: 2.6.x
flow-control/auto-tune changes and the per-stream receive buffer
rework; our config sets StreamRecvWindowDefault/StreamRecvBufferDefault
256KB — try removing those two settings on 2.6.4 (defaults there may be
better than our 2.5.0-era guesses). Also try QuicParam congestion control
explicitly (2.6.x BBR vs cubic differences on this RTT profile).

If the multi-holder state fixes everything at both vintages: no
regression — the bump is green pending only the coordination step.

## 4. msquic bump state (corrected design doc: docs/msquic-bump-design.md)

- Premise corrected: vendored msquic is v2.5.0-402 (May 2026), ALREADY
  RFC9000 (advertises {v2, v1, ms1, draft-29}); the bump's value is
  upstream bug fixes, not a version flip. CONFIRMED upstream win: the
  loopback source-selection bug is fixed in v2.6.4 — two daemons both
  bound 127.0.0.1 dial+salute in seconds under v2.6.4 (impossible at
  our vintage; msquic egresses the LAN interface for 127.0.0.1 dials and
  127.0.0.1-bound listeners drop them). If you re-enable 127.0.0.1 test
  binding, only do so on the bumped vintage.
- Both msquic trees were rolled BACK to 931047700 (shipped vintage) after
  the mixed-version regression. Single-command repro of the 2.6.4
  scratch state (for continuing the diagnosis):
  `git -C deps/msquic checkout v2.6.4 && git -C deps/msquic submodule
  update --init submodules/openssl submodules/googletest
  submodules/xdp-for-windows` (same in BOTH trees: liboffs's own and
  OFFS/deps/liboffs/deps/msquic), then add `StreamRecvBufferDefault`
  256KB to listener settings (the 2.6.4 default is 4KB vs the 2.5.0
  vintage which inherited a larger path — see
  QUIC_DEFAULT_STREAM_RECV_BUFFER_SIZE).
- The OFFS/deps/liboffs/deps/msquic scratch checkout currently sits at
  **931047700** (rolled back); liboffs/deps/msquic same. Binaries:
  OFFS/build rebuilt last at the 931047700 vintage (post-rollback) —
  consistent with the fleet's norelay13 image. Note the fleet was
  REDEPLOYED on norelay13 with fresh identities — see section 6.

## 5. Environment recreation (persistent: /home/victor/Workspace/ops/offs-fleet/)

- `aci-test-node{1,2,3}.yaml` — ACI deploy manifests (image
  offsrelay030665.azurecr.io/offs-offsd:norelay13, api keys
  offstest1235/1236/1237, RELAY_URL env 4.157.23.229:14000, ports
  23401/udp + 23402/tcp with container-level exposedPorts — required
  after an ACI "ports not used" validation change). Contains the ACR
  admin password — gitignored territory; creds also refreshable via
  `az acr credential show -n offsrelay030665`.
- `azexec.sh` — run an inline shell string in a container (stdin pipe
  into sh; az 2.89 container exec has NO --command-line).
- Deploy: `az container delete -g offs-relay-rg2 -n offs-test-nodeN -y`
  → `az container create -g offs-relay-rg2 -f aci-test-nodeN.yaml`
  (delete wipes /data + identities; re-put test files after).
  /bootstrap: POST http://IP:23402/bootstrap, Bearer <api-key>,
  {"endpoint":"IP:23401"} for the non-self peers.
- The relay container (offs-test-relay, offs-relay:latest) is separate —
  leave it running; nodes get endpoint ids from it.
- Home daemon (offline): run OFFS/build/offsd with --host 0.0.0.0
  --port 29402 --quic-port 29401 --cache-dir /tmp/offs-home-node/cache
  --config-dir /tmp/offs-home-node/config --unix .../offs.sock --api-key
  homekey123 --bootstrap <fleet IPs> --relay-url 4.157.23.229:14000.
  Kill by PID; never pkill -f (the pattern matches your own command
  line — it bit twice this campaign even post-gotcha).
- Rebuild offsd after liboffs changes:
  `cmake --build /home/victor/Workspace/src/github.com/vijayee/OFFS/build
  -j --target offsd offs`. OFFS/build embeds OFFS/deps/liboffs (sync via:
  `git fetch <local liboffs> master && git checkout <sha>` inside
  OFFS/deps/liboffs, then rebuild). Note the OFFS-side cmake uses gcc;
  clang build fails (fixed by the quic_listener.h include in offsd
  main.c — present since 4c4ccdd).

## 6. Test-suite state

- The RpcIntegration diamond set now passes 3 of 4 stages per test
  (harness ADD_PEER reachability, liboffs 99ea0f9); the remaining recall-
  response leg failure is the relay-staleness bug of section 2, not
  OFFS-271. QuicIntegration.TeardownAfterPoolStopBounds hangs
  environmentally if ambient local offsd daemons are running — kill
  leftovers first (they hold the 25xxx fixture ports).
- NEVER run two test processes at once; never run a suite while local
  daemons hold fixture ports. Ambient-die-check:
  `pgrep -f "offsd --foreground"` + `pgrep -f offs_relay` +
  `pgrep -f "mode=node"` → kill leftovers by exact PID, then
  `rm -rf /tmp/test_rpc_*`.
- ctest suite grew to 1259; the four known OFFS-271 diamonds now pass
  with the harness fix (verify on a clean run before assuming known).
- Valgrind/DWARF5: rebuild with -gdwarf-4 (memory note) if you need
  vgcore evidence.

## 7. Do nots (bitten this campaign, some twice)

- pkill -f with the pattern in your own command line (killed my own
  shell twice even after knowing).
- Trusting suite runs with orphaned fixture processes (killed by
  timeout, children still hold ports — the 432-failure false alarm).
- Measuring a msquic regression without controlling replication state
  (the section-3 confound).
- /tmp for anything that survives a reboot (lost the corrupt-cache
  archive + all probes in one crash; persistence now lives in
  /home/victor/Workspace/ops/offs-fleet/ and this repo's docs/).
