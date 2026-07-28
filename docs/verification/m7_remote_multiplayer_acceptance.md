# M7 Remote Multiplayer Acceptance

Date: 2026-07-28  
Implementation commit: `f686d08f9d2d62f7722920074b0223870198b2a5`

## Automated acceptance

The deterministic impaired-runtime case runs 600 fixed ticks with:

- 100 ms round-trip latency and ±20 ms round-trip jitter;
- 2% unreliable-message loss with a fixed seed;
- shared client/server movement, client prediction/reconciliation, binary live codecs, and the
  normal runtime replication path.

Observed deterministic result:

| Measurement | Result | Gate |
| --- | ---: | ---: |
| Accepted movement inputs | 594 / 600 | > 90% |
| Hard corrections | 1 bootstrap collision-revision reset | ≤ 1 |
| Maximum correction | 0.825 m | < 1.0 m |
| Encoded server-to-client average | 21.0 KiB/s | < 64 KiB/s |
| Encoded server-to-client one-second peak | 26.6 KiB | < 256 KiB |

The host also enforces a separate fixed one-second 256 KiB encoded application-traffic ceiling
per client. Reliable FIFO traffic is deferred under that ceiling; replaceable unreliable state is
dropped. Both results are exposed through host-session inspection.

## Verification matrix

Machine: Intel Core Ultra 7 258V, 8 logical CPUs, Linux x86-64, kernel
`6.17.0-1028-oem`.

| Preset | Result |
| --- | --- |
| `default-debug-werror` | 93 / 93 tests passed |
| `linux-clang-asan` | 93 / 93 tests passed |
| `linux-clang-tsan` | 93 / 93 tests passed |

The matrix includes two-client true-socket integration, disconnect/timeout handling, the
deterministic 25,000-input codec mutation target, binary command/result/event/world-delta
round-trips, truncation/trailing-data rejection, and the impaired-runtime acceptance case.

## Operator-only LAN check

The repository exposes dedicated-server `--bind`, client `--connect`, and
`tools/netem_multiplayer.sh` latency/loss controls. A physical two-machine walkthrough was not
executed in this single-machine environment because it requires a second host and privileged
network configuration; it remains an explicit release-operator check.
