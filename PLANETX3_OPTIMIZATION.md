# Planet X3 performance and audio plan

## Implementation progress (2026-09-23)

- Phase 1 counters are in the firmware. Each completed Planet X3 session appends
  a compact report to `/kefyros/saves/planetx3/PERF.TXT` on the SD card. It
  records PWM/BT underruns, audio ring minimum, DMA IRQ gap, OPL queue pressure,
  render and packing peaks, tick backlog, and display costs.
- The OPL mono signal now uses a mono audio write path, avoiding a duplicate PWM
  noise-shaper pass and halving the temporary render buffer. Duplicate synth
  initialization and the unused 8 KiB audio buffer were removed. The 16 KiB
  audio ring shares the idle KAPI arena with the display chunk when available;
  heap allocation remains a fallback and failure is handled explicitly.
- Hardware measurements are still needed before changing ring target, timer
  policy, OPL implementation, or display transport. The first profile should
  include about a minute of title music and a minute of active gameplay, then
  exit normally to flush `PERF.TXT`.
- Both Pimoroni and stock Pico 2 W firmware builds pass. The stock target's
  Planet X3 runtime remains constrained by its existing direct-QMI assumption.

## Scope and baseline (2026-09-23)

This is a plan for the Pimoroni Pico Plus 2 W build. It is based on code inspection,
the existing firmware ELF, the RP2350 datasheet, and the Planet X3 Open Source
Edition repository. The game has not been profiled on hardware for this plan, so
the suspected bottlenecks below are hypotheses until counters confirm them.

The checked-in `port/clock.c` currently sets normal to 300 MHz and boost to
350 MHz. The supplied AGENTS.md describes 400/420 MHz. Use the actual runtime
clock reading in every benchmark, and reconcile these configurations before
attempting higher clocks. Do not change voltage or QMI/SPI timing speculatively.

The current `build-pimoroni/kefyros.elf` has `__end__ = 0x20050b84` and
`__HeapLimit = 0x20074000`, leaving about 144 KiB of linker heap before runtime
allocations. The KAPI arena occupies the final 48 KiB and is not ordinary heap.

## Findings

| Priority | Evidence | Likely effect / question |
| --- | --- | --- |
| P0 | `apps/planetx3.c:54-76` drains the entire OPL register FIFO before checking audio space; FIFO overflow at `:35-42` is silent. | A write burst can postpone synthesis, and dropped writes can corrupt notes. Measure both. |
| P0 | `port/audio.c:100-117` fills underruns with silence and records no counter; `apps/planetx3.c:64-70` only synthesizes one 256-frame block per pass. | Choppiness cannot currently be attributed to synth speed, IRQ latency, or queue starvation. At 44.1 kHz a block is 5.80 ms. |
| P0 | Core 1 loops immediately and fills nearly the entire 4,096-frame ring; `port/audio.c` also has two 256-frame DMA halves. | The full ring alone represents 92.9 ms of queued audio, so OPL writes arriving now can be heard noticeably later. Increasing the ring trades latency for underrun tolerance. |
| P0 | `apps/planetx3.c:675-711` schedules 12,000 interpreter iterations per 13,731 us tick, catches up to eight ticks, then converts and sends a full 320x200 frame. | When execution falls behind, catch-up work increases latency; display can delay the next CPU tick and Bluetooth polling. `px3_cpu_exec()` counts iterations, not 8086 clock cycles. |
| P1 | `apps/planetx3.c:696-707` always reads 64,000 VGA bytes, converts 64,000 pixels, and sends 128,000 bytes; `port/lcdspi/lcdspi.c:40-50` feeds each pixel as two byte writes in a blocking loop. | 30 FPS needs 3.84 MB/s of panel payload. The 87.5 MHz SPI wire-time floor is about 11.7 ms per full frame, before CPU conversion and setup. |
| P1 | `apps/planetx3.c:573-574` uses direct cached QMI PSRAM for the whole 1 MiB DOS machine while `port/psram_qmi.c:149-155` deliberately exposes a block-only contract. | Game reads/writes and display traffic compete with flash fetches in the 16 KiB XIP cache. Reset/coherency safety needs review before preserving this shortcut. |
| P1 | `apps/planetx3.c:634-636` starts DMA audio before Core 1 synthesis, without priming the ring; the second start result is ignored. `:626` and `:632` reset the synth twice. | Initial silence and hidden start failure; cleanup is straightforward after measurement. |
| P2 | `apps/planetx3.c:202-220` fakes AdLib status and `:655` hardcodes the PIT rate. | Timer emulation may alter game/music pacing. Confirm against the actual DOS executable and upstream engine before changing it. |
| P2 | `apps/planetx3.c:103-104` keeps an unused 8 KiB static stereo buffer. | Easy memory recovery, but not a likely direct cause of choppy sound. |

The RP2350 datasheet says the XIP cache is 16 KiB and QMI transactions from the
two XIP cache ports and streaming DMA arbitrate for the external interface. This
supports measuring contention, not assuming it is the main cause.

## Step 1: make the failure measurable

Add small counters with a low-rate summary after exit or via an optional diagnostic
overlay. Avoid serial printing, allocation, and formatting in the audio or DMA
hot paths. Record:

- Audio: DMA underrun frames/events, ring low-water mark, peak producer write
  time, peak 256-frame OPL render time, OPL FIFO high-water and dropped writes;
  record when writes were queued and applied to expose music timing jitter.
  Also count DMA IRQ intervals and time spent packing PWM samples; the DMA IRQ
  runs on Core 0 while synthesis and ring writes run on Core 1.
- Timing: per-second interpreter iterations, timer ticks due/run/skipped, largest
  tick lateness, frame conversion time, SPI time, missed frame deadlines.
- Memory and clock: actual `clk_sys`, heap free/high-water if available, QMI bus
  clock; separate PWM and Bluetooth runs.

Run the same 60-second title/music and active gameplay scenes at the same clock
and SD image, with PWM first and Bluetooth second. Save baseline counters and a
short audio capture. Profile hardware; a successful WSL build alone cannot prove
real-time behavior.

## Step 2: stop audio starvation first

1. In Core 1, check the audio ring before draining OPL writes. Limit register
   processing per pass, but preserve write order and bound latency for key-on/off
   events. Add FIFO overflow reporting; do not silently discard events. If writes
   arrive in bursts, attach a tick or sample timestamp so they are applied at
   the intended point in the synthesized stream, rather than all at once.
2. Maintain a measured target fill instead of continually filling the full ring.
   Start by comparing 1,024 and 2,048 queued frames (23 and 46 ms at 44.1 kHz),
   then choose the lowest level with zero steady-state underruns. Keep the
   4,096-frame capacity as burst headroom. Prime before enabling DMA so the
   first two halves are not silence; this needs an audio-start API change because
   the current API starts DMA immediately. Check both start attempts and fail
   visibly if neither succeeds.
3. Compare OPL render plus PWM packing time with the 5.80 ms block budget. If
   synthesis is the limit, benchmark a faster OPL2 implementation against
   captured register traces. A 32 kHz output rate is worth measuring, but
   `OPL2_GenerateResampled()` continues advancing its roughly 49.7 kHz internal
   generator, so a lower output rate mainly reduces output/packing work. Check
   pitch, percussion, note-off timing, and CPU cost. Keep Nuked OPL2 as the
   reference for sound.
4. For Bluetooth, measure the SBC encoder and `cyw43_arch_poll()` time separately.
   A local PWM improvement does not establish that A2DP is fixed.

Acceptance: no steady-state DMA underruns or FIFO drops during the test scenes;
audio pace and notes match the reference capture. Do not hide sustained overload
only by increasing the ring: a 4096-frame ring already represents about 93 ms.

## Step 3: remove unnecessary display work

1. Establish frame cost from counters. First try a lower presentation cap only
   if it releases meaningful CPU time without damaging gameplay feel.
2. Track VGA writes through the 8086 memory accessors. Accumulate dirty rows or
   tiles and send only changed regions. A palette write must invalidate every
   affected pixel (initially the entire screen). Check whether DOS file reads or
   bulk/string instructions can write into the VGA aperture without using those
   accessors, then cover those paths too.
3. Compare the current CPU-driven SPI loop with a DMA transfer from the SRAM
   conversion chunk. Reuse the existing display DMA only after verifying channel,
   IRQ, and Core 1 ownership. Keep buffers alive until DMA completion and avoid
   stalling audio IRQs. Batch adjacent dirty rows to reduce panel commands.

Acceptance: identical screenshots, palette effects, and input response; lower
95th-percentile display time and no rise in audio underruns.

## Step 4: reduce emulation and memory cost

1. Use measurements to set an interpreter budget that fits the real 13.731 ms
   interval. Replace runaway catch-up with an explicit backlog policy; preserve
   emulated PIT time and music tempo. Benchmark representative scenes before
   deciding whether to skip presentation frames or cap emulated work.
2. Profile interpreter opcode frequencies and XIP/QMI stalls. Optimize the hot
   handlers and address calculation first. Consider moving the VGA aperture, or
   its most-used tiles, into SRAM only after a memory map shows room (about
   144 KiB linker heap before runtime allocations). Keep the rest of the 1 MiB
   DOS address space in PSRAM with clear read/write coherence rules.
3. Treat direct cached PSRAM writes as a separate correctness risk. Test repeated
   launch/exit, saves, warm reset, and QMI cache clean/invalidate. Prefer a
   bounded cache design that follows the firmware's block-oriented PSRAM API.
4. If the interpreter remains the dominant cost after these changes, evaluate a
   native port of the upstream 8086 assembly game engine as a separate project.
   It is a much larger compatibility and asset/licensing effort, not a quick
   replacement for the shipped DOS executable.

Acceptance: timer backlog stays bounded, median and worst-case input latency
fall, gameplay remains correct through load/save, and audio criteria still pass.

## Verification order

Implement in small revisions: counters -> audio scheduling -> display -> CPU and
memory. Build with `wsl.exe -d Ubuntu -- bash -lc 'bash
/home/legitbox/kefyros-pico/build.sh pimoroni'`, then flash hardware and repeat
the exact baseline scenes. There are no repository unit tests. Keep the stock
Pico 2 W target building too, but the current direct-QMI Planet X3 path requires
board-specific handling before it can run there.

## Sources

- Local: `apps/planetx3.c`, `apps/px3_cpu.c`, `apps/nuked_opl2.c`,
  `apps/px3_opl2.c`, `port/audio.c`, `port/psram_qmi.c`, `port/lcdspi/lcdspi.c`,
  `port/clock.c`, `port/bt_audio.c`.
- RP2350 datasheet, sections 4.4 and 4.4.1:
  https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf
- Planet X3 Open Source Edition engine and manual:
  https://github.com/planet-x3/px3_ose
  https://github.com/planet-x3/px3_ose/blob/master/assets/manual.txt
