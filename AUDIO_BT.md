# Audio and Bluetooth integration

## Current audio clients

| Client | Entry points | Execution context |
| --- | --- | --- |
| Music | `kf_audio_start_buffered_external`, `kf_audio_space`, `kf_audio_write_s32`, `kf_audio_flush`, `kf_audio_stop` | Core 0; ring storage in the idle KAPI arena |
| Game Boy | `kf_audio_start_buffered`, `kf_audio_running`, `kf_audio_space`, `kf_audio_write`, `kf_audio_stop` | Core 0 modal loop |
| Planet X3 | `kf_audio_start_buffered`, `kf_audio_running`, `kf_audio_space`, `kf_audio_write`, `kf_audio_stop` | Core 1 OPL producer; Core 0 runs the radio |
| SD KAPI apps | KAPI audio wrappers over `kf_audio_*` | App execution context |
| OS boot sounds | No active sound implementation | N/A |

The shared `kf_audio_*` producer API is unchanged. Its ring contains PWM duty frames when the speaker is selected and signed stereo PCM when a Bluetooth A2DP stream is active. Route changes clear queued audio; the next frames from a running app go to the selected sink. Game Boy and Planet X3 pump the radio in their modal loops.

## Shipping scope

- Output starts on the speaker on every boot. Saved headsets are listed but never connected automatically, including after a dropped link.
- The Bluetooth Manager explicitly scans, connects, disconnects and forgets devices. Settings can select a saved headset or return to the speaker. The top bar shows Bluetooth state. The supplied icon is staged at `/kefyros/icons/bluetooth.png`.
- Bluetooth Classic A2DP source uses the SDK BTstack SBC encoder at the headset's negotiated 44.1 or 48 kHz rate. A linear PCM resampler handles other producer rates. Pairing keys use the SDK's flash bank; names and addresses use deskconf.
- aptX, AVRCP headphone button control, OS sound effects, mixing of multiple simultaneous producers, and a dedicated audio service are future work. The current API has one producer and one active output.

## Hardware checks before calling this stable

1. Boot with an SD card and confirm launcher, Settings and Music still fit in memory. Inspect the Memory app while Music plays and after exiting it.
2. Scan from Bluetooth Manager, connect a headset manually, and check that its negotiated stream reaches `connected`. Reboot: it must remain on the speaker until explicitly connected again.
3. While playing Music, switch speaker to Bluetooth and back, seek, skip tracks, and check dropouts. Repeat with Game Boy and Planet X3. Test a 44.1 kHz and 48 kHz headset if available.
4. Disconnect by turning the headset off. Confirm the speaker resumes, the status changes, and the saved device does not reconnect on its own. Forget it and verify its key and config entries are removed.
5. Check Wi-Fi with Bluetooth active, sleep and wake, and a fresh flash/reboot cycle.

The Pimoroni Pico Plus 2 W and stock Pico 2 W builds verify compilation only; radio pairing, throughput, CPU headroom and audio quality require the above device tests.
