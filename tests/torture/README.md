# Fluxa — Torture Test (IoT Runtime Simulation)

Simulates executing Fluxa on a resource-constrained device (ESP32/RP2040-class).

## Design

```
HOST (full CPU)                    CONTAINER (throttled)
─────────────────                  ──────────────────────────────
make build → ./fluxa    ──────→    cpus: 0.1  (10% of 1 core)
                                   mem:  128MB (no swap)
                                   runs: run_torture.sh
                                         ↳ test suites with real binary
```

The binary is compiled on the host with full CPU. Only the **test execution**
runs inside the throttled container — this correctly simulates an IoT device
running Fluxa, not an IoT device compiling it.

## Constraints

| Parameter | Value | Simulates |
|---|---|---|
| `cpus: 0.1` | 10% of 1 core | IoT CPU sharing |
| `mem_limit: 128m` | 128 MB | ESP32-class heap |
| `memswap_limit: 128m` | no swap | Embedded (no swap) |

## Usage

```bash
# Run torture test (compiles on host, tests run throttled)
make test-torture

# Clean Docker state if needed
docker compose -f tests/torture/docker-compose.torture.yml down --rmi local
```

## What it validates

- IPC server responds within timeout under CPU starvation
- GC sweep completes without OOM under 128MB
- `fluxa apply` / hot reload works under resource pressure
- `flxthread` mailbox processing survives CPU starvation
- All test suites pass under combined CPU + memory pressure

## When to run

`test-torture` is separate from `make test-all` — it requires Docker and
takes longer. Run before production releases:

```bash
make test-all      # normal CI
make test-torture  # pre-release validation
```
