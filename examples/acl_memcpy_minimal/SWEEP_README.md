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
-n 10, 50, 100, 300, 500, 1000, 2000, 3000, 5000, 7500, 10000
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
TEST_TYPE=batch ./run_n_sweep.sh
TEST_TYPE=all8_single_stream ./run_n_sweep.sh
TEST_TYPE=all8_process ./run_n_sweep.sh
TEST_TYPE=all8_process DEVICES=1,2 ./run_n_sweep.sh
TEST_TYPE=multi_stream STREAMS=8 ./run_n_sweep.sh
TEST_TYPE=all8_process STREAMS=8 ./run_n_sweep.sh
IO_SIZE=32K ITERS=256 ./run_n_sweep.sh
BIN=/path/to/h2d_d2h_async_memcpy ./run_n_sweep.sh
LOG_DIR=/tmp/h2d-n-sweep ./run_n_sweep.sh
```

Supported `TEST_TYPE` values are:

```text
single_stream
batch
multi_stream
all8_single_stream
all8_process
all
```

Logs are written under:

```text
logs/n-sweep-<test-type>-<timestamp>/
```

## Multi-Stream Scaling Compare

`run_multistream_scale_compare.sh` compares single-device multi-stream H2D
against multi-process multi-stream H2D with controlled device groups. It is meant
for checking why 8 devices do not scale to exactly 8 times the single-device
bandwidth. It prints a terminal table at the end and keeps the full per-case logs
under `logs/`.

Default fixed parameters:

```text
-s 32K
-n 10000
-m 4
-i 10
```

Run:

```bash
./run_multistream_scale_compare.sh
```

The default device groups include single-device process baselines, anchored
pairs such as `0,1` and `0,2`, adjacent pairs, several 4-device groups, and all
8 devices. The table includes bandwidth, scale versus process device 0, and
efficiency versus ideal linear scaling.

Useful overrides:

```bash
STREAMS=8 ./run_multistream_scale_compare.sh
ITERS=32 ./run_multistream_scale_compare.sh
DEVICE_GROUPS='0;1;0,1;0,2;0,1,2,3;0,1,2,3,4,5,6,7' ./run_multistream_scale_compare.sh
LOG_DIR=/tmp/h2d-scale ./run_multistream_scale_compare.sh
```

## Fixed IO Multi-Stream Compare

`run_io_multistream_compare.sh` compares one selected device against all 8
devices for three fixed IO sizes. It uses the same multi-process multi-stream
path for both rows so that the comparison isolates scale-out behavior.

Default fixed parameters:

```text
IO sizes: 64K, 37K, 2K
-n 10000
-m 4
-i 10
repeats: 3
single device: 0
all devices: 0,1,2,3,4,5,6,7
```

Run:

```bash
./run_io_multistream_compare.sh
```

Useful overrides:

```bash
REPEATS=5 ./run_io_multistream_compare.sh
SINGLE_DEVICE=2 ./run_io_multistream_compare.sh
ALL_DEVICES=0,2,4,6 ./run_io_multistream_compare.sh
IO_SIZES='64K;37K;2K;128K' ./run_io_multistream_compare.sh
LOG_DIR=/tmp/h2d-io-compare ./run_io_multistream_compare.sh
```
