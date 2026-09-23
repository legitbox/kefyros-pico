# WiFi

The network manager: a live status header, a scan list of nearby networks,
password entry, Disconnect and Forget actions, and a speed test. Joined
networks are saved (up to 8) in `config.txt`.

## Status

The top lines show the connection (network name, state, IP) plus diagnostic
telemetry and the current clock speed.

## List actions

- **Rescan** - scan for networks again.
- **Speed test** - measure download throughput; progress shows on the status line.
- **Disconnect** - drop the connection but keep the network saved.
- **Forget network** - disconnect and delete the saved password.
- **Network rows** - each shows signal strength and `[*]` secured / `[o]` open.

The last two only show while a network is connected or being joined.

## Connecting

- An **open** network connects immediately.
- A **saved** network connects with its stored password.
- Any other **secured** network opens a password box - type it and press **ENTER**.

## When WiFi turns on

The radio stays off at boot. The browser (Spineko), the terminal and SD apps
that use the network turn it on and join the strongest saved network. The WiFi
app and chat (DeepSeek) turn the radio on but don't join by themselves; pick a
network in WiFi. The connection stays up after you leave the app.

## Idle timeout

WiFi disconnects by itself after **10 minutes** with no key pressed and no
open connection (an SSH session keeps it up). The first key you press inside
an app afterwards reconnects it; a key on the desktop does not. A manual
**Disconnect** is not undone by a key press.

Change the time with `wifi_timeout=<minutes>` in `/kefyros/config.txt`;
`wifi_timeout=0` keeps WiFi connected until you disconnect or restart.

## Keys

- **Up / Down** - move; **ENTER** - activate a row; **ESC** - back to desktop.
- In the password box: type the key, **ENTER** to connect, **ESC** to cancel.

## Clock

The radio only joins at a lower clock, so the CPU drops to about 250 MHz while
it connects (watch the top-bar MHz show `250M`). Once joined, the connection
keeps working at the normal clock.
