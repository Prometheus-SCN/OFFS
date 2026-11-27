# msquic modernization — corrected design (OFFS epic: per-message streams phase 2)

## Corrected premise

The epic originally read "vendored msquic is draft-29 / 2021 vintage; bump
to RFC9000". Both halves are wrong as stated:

- The vendored tree is microsoft/msquic `v2.5.0-402` (commit 931047700,
  May 2026 vintage), already speaking RFC9000: its version negotiation
  advertises `{ v2, v1, ms1, draft-29 }` — draft-29 is the legacy fallback,
  not the negotiated version.
- The real argument for bumping is **upstream bug fixes** in the same
  protocol versions. Several failure modes we root-caused by hand at
  v2.5.0-402 are known-and-fixed upstream: loopback source-address
  selection (dialing a 127.0.0.1-bound listener egressed the LAN interface
  and timed out — OFFS-271's environmental dial failure; confirmed fixed
  in v2.6.4, see measurements), receive-completion contract handling, and
  continued flow-control auto-tuning work.

## Scratch-build evidence (this bump: v2.5.0-402 → v2.6.4)

- Builds clean: liboffs + offsd + full test suite compile unmodified
  (API stable: MsQuicOpen2 / ConfigurationOpen / Stream* / the config
  fields we set — PeerUnidiStreamCount, StreamRecvWindowDefault,
  KeepAliveIntervalMs — all present).
- Suite: 129/129 on the fast actor/stream/cache/index gate.
- Loopback: two daemons BOTH bound 127.0.0.1 dial + salute each other in
  seconds on v2.6.4 (fails at v2.5.0-402) — the upstream bug fix confirmed.
- e2e integrity: 10MB load + get, sha256-exact through mixed-version mesh
  (requester 2.6.4 ↔ holders 2.5.0-402).
- **WARNING — mixed-version mesh performance regressed**: the same
  home↔norelay13-transfer that ran 4–6s at v2.5.0-402 on both ends ran
  **21s** with the requester alone on 2.6.4 (490 blocks ≈ same dup ratio;
  no transport deaths — 0 SHUTDOWN_BY_TRANSPORT vs recurring 0x6e deaths
  at the old version; purely throughput). The fleet must move together:
  this bump ships ONLY as a coordinated fleet upgrade, and the mixed
  configuration must be re-measured (per-holder 2.6.4 sidecar test) before
  any production rollout.

## Rollout plan

1. Build a norelay image with v2.6.4 (the dockerfile's msquic COPY path
   flows from this same pin) and redeploy the three-node fleet.
2. Re-run the 10MB timing set fleet-wide-2.6.4. If ≤ the v2.5.0-402
   baseline (4–8s cold): wire the scratch pins into
   liboffs/deps/msquic + OFFS submodule commit + image tag bump.
3. If still degraded: file the per-pair perf delta against msquic upstream
   (2.6.4 unidi stream throughput regression vs 2.5.0) and stay on the
   shipped vintage; the scratch trees (documented below) reproduce this
   state in one command.

## Scratch state reproduction

- liboffs tree: `deps/msquic` checked out at `v2.6.4` with its three
  submodules materialized (`submodules/openssl`, `submodules/googletest`,
  `submodules/xdp-for-windows` — uncommitted, working-tree only).
- OFFS tree: `deps/liboffs/deps/msquic` same — uncommitted.
- Restore: `git -C deps/msquic checkout 931047700` (+ submodule update)
  in both trees.

## What stays regardless of the bump

All campaign fixes are msquic-version-independent app-layer work:
per-message streams, fanout/window/priority-lane tuning, mute-layer
repairs, slot reconciliation. The v2.5.0-402 config defaults that bit us
(PeerUnidiStreamCount=1, 64KB stream window) remain fixed at both vintages.
