# demo-ce

A Zephyr UART command console using the [CEVO command engine](https://github.com/nidops/cevo).
The qualified hardware target is the STM32WB55 Nucleo with ST-LINK virtual COM port.
The firmware and native receiver tests run independently of any host test framework.

## Design

```text
UART RX DMA → async callback → bounded line assembler → message queue
                                                        ↓
                                              application thread
                                                        ↓
                                               CEVO → handler → TX
```

Each receiver owns two aligned 128-byte DMA buffers in a Zephyr `k_mem_slab`.
The driver returns ownership through `UART_RX_BUF_RELEASED`; received-byte events
alone never make a buffer reusable. Recovery waits for `UART_RX_DISABLED` before
restarting. Teardown waits for release, stops recovery work and detaches the callback.
If teardown fails, the caller retains the context and retries.

The callback assembles newline-terminated commands. Idle notifications preserve
partial lines. Oversized lines are discarded through newline, including any valid
command suffix. The queue holds eight lines of 128 bytes each; commands have at
most 127 characters. A full queue drops a complete line and reports a warning.
CEVO dispatch and polling TX run in the application thread.

The buffers and queue payloads reserve 1,280 bytes, plus the line assembly buffer,
receiver/kernel metadata and thread stacks. No heap allocation is used for RX.
The Nucleo overlay enables USART1 RX DMA through DMAMUX1 request 14. TX uses polling.

## Build and flash

First prepare a Zephyr SDK and west environment using the
[Zephyr getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
Install `ruamel.yaml` in the Python environment used for the build. Clone these
repositories as siblings:

```bash
git clone https://github.com/nidops/cevo.git
git clone https://github.com/nidops/demo-ce.git
cd demo-ce
python -m pip install ruamel.yaml
west build -b nucleo_wb55rg/stm32wb55xx -d build -p always . -- \
  -DINPUT_YAML="$PWD/inputs/demo.yaml" \
  -DDTC_OVERLAY_FILE="$PWD/boards/nucleo_wb55rg.overlay" \
  -DPython_EXECUTABLE="$(command -v python)"
```

CEVO defaults to `../cevo`; override `CEVO_PATH` for another checkout layout.
Select your own ST-LINK serial locally before flashing:

```bash
export PROBE_SERIAL=YOUR_STLINK_SERIAL
west flash -d build -r openocd --no-erase --verify \
  --cmd-pre-init="adapter serial $PROBE_SERIAL"
```

Connect to your board's virtual COM port at 115200 baud, 8N1. Startup reports
`CEVO Demo`, the board name and the `>> ` prompt. Example commands:

| Command | Result |
| --- | --- |
| `add 10 20` | `Sum: 30` |
| `div 1000000000 4` | `Quotient: 250000000` |
| `reverse hello` | `olleh` |
| `upper helloworld` | `HELLOWORLD` |
| `sort FF01A0 3` | `01 A0 FF` |
| `foobar` | `ERR` |

After malformed or oversized input, a subsequent valid command should still work.
`sort` callers must keep the length within the decoded byte count; CEVO does not
associate that scalar with the pointer's extent. Addition uses unsigned arithmetic.

## Native receiver tests

The Zephyr ztest suite exercises the actual receiver and memory slab against a
controlled async UART driver on `native_sim`. It does not need CEVO or a board:

```bash
west build -b native_sim/native/64 -d build-tests -p always tests/uart_line -- \
  -DEXTRA_CFLAGS=-Werror=implicit-function-declaration
timeout 30 build-tests/zephyr/zephyr.exe
```

The 16 tests cover partial ranges, CR/LF, DMA turnover, line and queue overflow,
stop/release ordering, failed buffer responses and restarts, asynchronous teardown,
timeouts, cleanup retries, repeated lifecycle operations, and rejection of truncated
or unterminated queue messages. CI runs this suite with the host compiler and a
pinned Zephyr revision.

## Qualification

The firmware has been exercised on the STM32WB55 Nucleo, including DMA buffer
turnover, bursts, idle fragments, oversized lines, MCU reset and debugger resume.
The native suite covers controlled driver failures; it does not establish timing
or driver behavior on arbitrary hardware. Physical USB unplug/replug and target
fatal-error injection were not qualified.

The verified toolchain uses Zephyr revision
`b3ccee473ed64ac9ba7260beafb4fd175eee13ab` and SDK 0.17.1. Other Zephyr revisions
and boards require their own validation; generic QEMU UART compatibility is not
claimed for this DMA application.

Keep toolchain paths, probe identities, local bench configs, firmware outputs and
raw test evidence outside Git. `*.local.yaml`, `*.local.yml`, `build/`, `build-*/`
and `artifacts/` are ignored.

Licensed under Apache-2.0; see [LICENSE](LICENSE).
