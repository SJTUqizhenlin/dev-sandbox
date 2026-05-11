# N Sweep Script

`run_n_sweep.sh` runs the already-built H2D demo repeatedly while sweeping the
buffer count for one selected test type.

It does not compile the demo. Build `h2d_d2h_async_memcpy` first, then run:

```bash
cd dev-sandbox/examples/acl_memcpy_minimal
./run_n_sweep.sh
```

Default sweep:

```text
-n 1K, 3K, 5K, 7K, 9K, 11K, 13K, 15K
```

Default fixed parameters:

```text
-t single_stream
-s 64K
-i 128
```

Useful overrides:

```bash
TEST_TYPE=multi_stream ./run_n_sweep.sh
TEST_TYPE=all8_single_stream ./run_n_sweep.sh
IO_SIZE=32K ITERS=256 ./run_n_sweep.sh
BIN=/path/to/h2d_d2h_async_memcpy ./run_n_sweep.sh
LOG_DIR=/tmp/h2d-n-sweep ./run_n_sweep.sh
```

Supported `TEST_TYPE` values are:

```text
single_stream
multi_stream
all8_single_stream
all
```

Logs are written under:

```text
logs/n-sweep-<test-type>-<timestamp>/
```
