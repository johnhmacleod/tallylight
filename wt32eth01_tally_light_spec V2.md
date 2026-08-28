# ATEM Tally Light — WT32-ETH01 Edition — Specification

Reflects the sketch as of this document's generation. Source of truth is always
`atem_tally_light_wt32eth01.ino` — this file is a summary for reference, not a
replacement for reading the code's own comments where precision matters.

## 1. Target hardware

- **Board**: WT32-ETH01 — a classic ESP32 (ESP32-S2FN4R2 variant) with a built-in
  LAN8720 Ethernet PHY wired via RMII to the chip's internal EMAC.
- **No onboard USB.** Requires a USB-to-serial adapter wired to TXD0/RXD0/GND/3V3,
  with GPIO0 pulled low during flashing to enter bootloader mode.
- **Arduino IDE board setting**: "WT32-ETH01" if listed, otherwise "ESP32 Dev Module".
- **Status: unverified on real hardware in several respects.** The ETH.h API, pin
  assignments, and event-handling pattern are based on Espressif's documentation and
  cross-referenced community sources, but portions of this sketch (particularly the
  networking/threading architecture) evolved through live debugging against real
  hardware over many iterations — see §9 for what's been confirmed working vs. what
  hasn't been exercised.

## 2. Pin assignments

### Fixed by the module's own hardware (not configurable, do not reuse)

| GPIO | Function |
|---|---|
| 0 | RMII clock input (50MHz from PHY oscillator) |
| 16 | PHY power/enable |
| 18 | MDIO |
| 19 | RMII TXD0 (fixed in ESP32 silicon) |
| 21 | RMII TX_EN (fixed in ESP32 silicon) |
| 22 | RMII TXD1 (fixed in ESP32 silicon) |
| 23 | MDC |
| 25 | RMII RXD0 (fixed in ESP32 silicon) |
| 26 | RMII RXD1 (fixed in ESP32 silicon) |
| 27 | RMII CRS_DV (fixed in ESP32 silicon) |

10 GPIOs permanently committed to Ethernet. Because GPIO0 is the Ethernet clock, the
usual "hold BOOT low" trick doesn't apply the same way — use the datasheet's
documented programming procedure.

### Available general-purpose I/O

Per the WT32-ETH01 datasheet's header pinout: `IO2, IO4, IO5, IO12, IO14, IO15, IO17,
IO32, IO33` (general I/O), plus `IO35, IO36, IO39` (**input only**).

Caveats: IO2 must float/be low at boot; IO12 must float/be low at boot or the chip
won't start; IO5/IO15 affect boot debug output. **IO4, IO14, IO17, IO32, IO33 have no
such caveats** — this sketch's LED defaults are drawn from that clean set.

### LED output pins (configurable via web UI, defaults shown)

| Purpose | Default GPIO | Notes |
|---|---|---|
| NeoPixel/RGBW strip data | 32 | Clean, caveat-free |
| Simple mode — red LED | 4 | Clean, caveat-free |
| Simple mode — green LED | 14 | Clean, caveat-free |

IO17 and IO33 are the other clean options if the defaults don't suit a given wiring
layout.

## 3. Libraries required

| Library | Source | Notes |
|---|---|---|
| `ETH` | Bundled with ESP32 Arduino core | No install needed |
| `WebServer` | Bundled with ESP32 Arduino core | |
| `WiFiUdp` | Bundled with ESP32 Arduino core | Works over any active interface, not WiFi-specific |
| `Preferences` | Bundled with ESP32 core | NVS-backed config persistence |
| `Adafruit_NeoPixel` | Adafruit | RGBW addressable strip support |
| `ATEMbase` / `ATEMstd` | Kasper Skårhøj (SKAARHOJ) | **Requires a manual local patch — see §3.1** |

### 3.1 Required ATEMbase library patch

`ATEMbase.h`/`.cpp` hardcode their UDP class via `#ifdef ESP8266` — any other
platform, including all ESP32 variants, falls into an `#else` branch that expects
`EthernetUDP` (classic Arduino Ethernet shield library), regardless of what
networking the sketch actually uses. This causes linker errors
(`undefined reference to EthernetUDP::...`) when building for WT32-ETH01, which uses
`WiFiUDP`.

**Fix applied**: a **sketch-local copy** of `ATEMbase`/`ATEMstd` (placed in this
sketch's own `libraries/` subfolder, not the global Arduino libraries folder) has
been hand-patched:
- `ATEMbase.h`: the `#ifdef ESP8266 / WifiUDP / #else / EthernetUdp / #endif` block
  replaced with an unconditional `#include <WiFiUdp.h>`.
- `ATEMbase.cpp`: every `EthernetUDP` reference (the internal `Udp` object) replaced
  with `WiFiUDP`.

This is deliberately sketch-local rather than a global library edit, because the
companion Lolin S2 Mini / W5500 sketch genuinely needs the unpatched (`EthernetUDP`)
version of the same library, sharing one Arduino installation.

## 4. Software architecture

### 4.1 Two-task concurrency model

`AtemSwitcher.connect()` has been observed to block for a long time — possibly
indefinitely — when the ATEM switcher is unreachable. Root cause: unlike the W5500
sketch's `EthernetUDP` (which offloads ARP resolution to dedicated chip hardware,
non-blocking from the ESP32's perspective), this sketch's patched `WiFiUDP`-based
ATEMbase relies on lwIP's software ARP resolution, which can stall the calling
thread.

**Fix**: all `AtemSwitcher` interaction is isolated in its own FreeRTOS task
(`atemTask`), started from `setup()` only in Direct mode. `AtemSwitcher` is *never*
touched from `loop()` or the web server handlers — only from `atemTask` — avoiding
concurrent access to an object that is almost certainly not thread-safe.

| Task | Responsibilities |
|---|---|
| **Main (loop)** | `server.handleClient()`, relay UDP send/receive, LED state machine, reading shared state |
| **atemTask** (Direct mode only) | `AtemSwitcher.runLoop()`, connect/reconnect, reading tally-by-index flags, writing shared state |

### 4.2 Cross-task shared state

| Variable | Type | Protection | Written by | Read by |
|---|---|---|---|---|
| `currentState` (`TallyState`) | struct | Mutex (`stateMutex`) | `atemTask` (Direct) or `receiveRelayUpdates()` (Relay) | `getMyTallyFlags()`, `handleStatus()`, `broadcastRelayState()` |
| `atemIsConnected` | `volatile bool` | None (simple flag) | `atemTask` | `isConnected()` |
| `atemTallyDataSeen` | `volatile bool` | None (simple flag) | `atemTask` | `tallyReady()` |
| `atemRawBusReady` | `volatile bool` | None (simple flag) | `atemTask` | `rawBusDataReady()` |

Simple booleans are left as `volatile` rather than mutex-protected — a torn read
just means "stale by one cycle," not corrupted data. `currentState` is mutex-
protected since it's a multi-field struct that could tear if read mid-write.

**Startup safety**: `setup()` halts (`while(true) delay(1000)`) with a serial error
if mutex creation or NeoPixel strip allocation fails, rather than proceeding into
undefined behavior.

### 4.3 Boot sequence

1. `Serial.begin()`, create `stateMutex` (halt on failure)
2. `loadConfig()` — read persisted settings from NVS
3. Initialize LED hardware (NeoPixel strip *or* simple GPIO pins, per config)
4. `setupEthernet()` — register `Network.onEvent()`, call `ETH.config()` if static IP,
   call `ETH.begin()`, wait up to 15s for an IP (non-fatal timeout — continues via
   background events either way)
5. `relayUdp.begin()`
6. Register web routes, `server.begin()`
7. **Direct mode**: `AtemSwitcher.begin()` + `serialOutput()`, then spawn `atemTask`
   (does *not* call `connect()` inline — that's `atemTask`'s job, specifically so a
   blocking `connect()` call can never prevent `setup()` from returning)
   **Relay mode**: nothing further — `receiveRelayUpdates()` runs from `loop()`

## 5. Configuration (persisted via `Preferences`/NVS)

| Setting | Type | Default | Notes |
|---|---|---|---|
| Tally number(s) | comma-separated string | `"1"` | See §6 |
| Mode | Direct / Relay Client | Direct | |
| DHCP | bool | true | |
| Static IP / gateway / subnet | IPAddress | — | Used only if DHCP off |
| ATEM switcher IP | IPAddress | `192.168.1.240` | Direct mode |
| Upstream unit IP | IPAddress | `192.168.1.177` | Relay Client mode |
| LED mode | FastLED(name)/Simple | Addressable (RGBW) | See §7 |
| RGBW pixel count | 1–60 | 1 | |
| Red/Green GPIO pins | 0–39 | 4 / 14 | Simple mode only |

All settings are editable via the web UI and take effect after a save-triggered
reboot (`ESP.restart()`).

## 6. Tally logic

### 6.1 DSK/keyer-aware tally-by-index

Uses `AtemSwitcher.getTallyByIndexTallyFlags(index)` — mirrors the ATEM protocol's
own "Tally by Index" data (the same mechanism Blackmagic's control panels use), not
just the raw program-bus number. Correctly reflects a source live via a downstream
keyer (DSK) or upstream keyer, which a raw-bus comparison would miss.

**Critical implementation detail**: `getTallyByIndexTallyFlags()` takes a **0-based
array index** (valid range 0–20 for ATEMstd's 21-entry internal table), *not* the
1-based physical input number. Confirmed from the library source:
```cpp
uint8_t ATEMbase::getTallyByIndexTallyFlags(uint16_t sources) {
    return atemTallyByIndexTallyFlags[sources];
}
```
The sketch loops `i` from 0 to `MAX_TALLY_SOURCES - 1` and calls
`getTallyByIndexTallyFlags(i)` directly (no `+1`) — `currentState.flags[i]`
represents physical input `i+1`.

Bit meaning: bit 0 = program, bit 1 = preview.

### 6.2 Multi-source monitoring

A single unit can monitor multiple ATEM inputs at once via a comma-separated tally
number field (e.g. `7,8`). Parsed by `parseTallyNumbers()` into
`cfg.tallyNumbers[]`/`cfg.tallyNumberCount` (capped at
`MAX_MONITORED_TALLY_INPUTS = 8`). Invalid tokens and duplicates are silently
dropped; an empty/all-invalid result falls back to `"1"`.

`getMyTallyFlags()` bit-ORs the flags across every monitored source:
- **ON AIR** if *any* monitored input is on program
- **ARMED** if *any* monitored input is on preview
- **Program takes priority**: if the combined result has the program bit set, the
  preview bit is explicitly masked off, even if a different monitored source is
  simultaneously on preview. Rationale (per product decision): multiple monitored
  sources are meant to represent one PC/feed, and showing both ON AIR and ARMED at
  once for what's conceptually a single source would be confusing to the operator.

### 6.3 "Waiting for tally data" detection

`atemTallyDataSeen` (exposed as `tallyReady()`) latches `true` once **any of the 21
tracked sources** — not just the monitored ones — shows a non-zero flag. Rationale:
a monitored source can legitimately sit at zero for a long time (genuinely off-air)
even after the switcher's tally stream has started flowing; checking only the
monitored sources can't distinguish "genuinely zero" from "haven't received
anything yet." Any source going non-zero is valid evidence real data has started
arriving, at which point the unit's own (possibly still-zero) flags can be trusted.

This deliberately does **not** use `AtemSwitcher.hasInitialized()`, which only
latches once the switcher's *entire* initial payload (multiview, DSK, media pool,
macros, etc.) has arrived — observed to never complete on some switcher
models/firmware.

### 6.4 Raw bus diagnostic (not used for tally decisions)

`getProgramInputVideoSource(0)` / `getPreviewInputVideoSource(0)` (M/E 1) are read
and shown on the status page purely as a diagnostic, decoupled from the actual
tally logic. Gated by `rawBusDataReady()` (a 1.5s grace period after connecting)
rather than the raw value itself, since `0` is a legitimate ATEM source ID
("Black") and can't be distinguished from "not yet received" by value alone.

## 7. LED output

Two mutually exclusive modes, chosen via web UI (`cfg.ledMode`):

### 7.1 Addressable RGBW strip

- Driven via **Adafruit_NeoPixel**, not FastLED — FastLED has no official 4-channel
  RGBW support (open feature request since 2016; the only workaround is an
  unofficial third-party hack with documented rough edges). Adafruit_NeoPixel has
  mature native RGBW support and genuinely runtime-configurable pixel counts.
- Wire order: `NEO_GRBW + NEO_KHZ800` (typical for SK6812). If colors are wrong on
  real hardware, this is the first thing to check.
- **Display scheme**: strip split into two halves. First half = PREVIEW (green),
  second half = PROGRAM (red). Independent, not mutually exclusive at the per-half
  level (see §6.2 for the program-priority rule that applies before this stage).
  Split uses integer division (`count / 2`); the remainder goes to the program half.

### 7.2 Simple red/green GPIO

Two plain LEDs via `digitalWrite()` — no addressable-LED library involved. Red =
PROGRAM, green = PREVIEW, driven independently (same conceptual split as the strip).

### 7.3 Non-tally status indicators

`setStatusColor(r,g,b)` — used only for connecting (blue) and boot-time off (black),
never for tally display. Simple mode can't represent blue (no blue channel); it
lights both LEDs together as an approximation, making it indistinguishable from
other "both on" states — check the serial log for exact status if needed.

### 7.4 Animated waiting-state indicators

| State | Trigger | FastLED/RGBW behavior | Simple GPIO behavior |
|---|---|---|---|
| Waiting for connection | `!isConnected()` | Whole strip flashes blue, 500ms period | Both LEDs flash together |
| Waiting for tally data | Connected, `!tallyReady()` | Both halves lit, red/green sides swap every 500ms | Red/green alternate on/off |
| Normal tally display | Connected + tally ready | Split-strip per §7.1 | Independent red/green per §7.2 |

Priority order in `loop()`: waiting-for-connection > waiting-for-tally-data > normal
display.

## 8. Multi-unit relay protocol

- An ATEM switcher accepts only ~5–8 simultaneous client connections. One unit runs
  in **Direct** mode (talks to the switcher); others run in **Relay Client** mode,
  receiving tally data via UDP broadcast instead.
- Every unit — Direct or Relay Client — rebroadcasts its current `TallyState` via
  UDP broadcast on port 9910 (`RELAY_PORT`), enabling chained trees of arbitrary
  depth/fan-out (e.g. Unit A direct → Units B, C relay off A → Unit D relay off C).
- Relay clients filter incoming packets by source IP (`cfg.upstreamIP`) to avoid
  cross-talk between independent chains on the same network.
- Broadcast cadence: on change, or at minimum every 250ms (`RELAY_BROADCAST_INTERVAL_MS`)
  as a heartbeat.
- Relay Client mode considers the upstream lost after 3s
  (`RELAY_TIMEOUT_MS`) without a packet, clearing `currentState` to unknown.
- **Cross-hardware compatible**: the relay protocol is raw bytes on the wire — a
  WT32-ETH01 unit can freely mix with Lolin S2 Mini/W5500 units from the companion
  sketch in the same relay tree.

## 9. Known limitations / unverified areas

- **Networking foundation is real-hardware-tested; broader sketch is not.** The
  boot sequence, web server responsiveness under an unreachable switcher, and the
  two-task concurrency model have been debugged against actual WT32-ETH01 hardware
  through this project's development. However, this happened over a compressed
  iteration cycle — treat any new failure mode as a real possibility, not
  something already ruled out.
- **No hardware watchdog.** An earlier attempt at one (on the companion W5500
  sketch) caused a worse failure mode — a silent boot loop — than the hang it was
  meant to prevent, and was removed. Recovery from a genuine hang currently
  requires a manual power cycle.
- **Concurrency risk.** The FreeRTOS task + mutex model is new territory for this
  codebase. `atemTask`'s stack size (8192 bytes) is a reasonable starting guess, not
  a profiled value — a stack-overflow panic in the serial log would be the signal
  to raise it.
- **RGBW wire order and split-count rounding are unverified assumptions** — see §7.1.
- **The ATEMbase library patch (§3.1) is a manual, sketch-local edit** — it isn't
  tracked by any package manager and will need to be reapplied if the library is
  ever reinstalled or updated.

## 10. Web interface

Single-page config UI served from `/` (form), with live status polled from `/status`
(JSON) every second via `fetch()`. Layout: responsive CSS grid of cards — Status,
Tally, Network, LEDs — collapsing to a single column on narrow viewports. One
`<form>` wraps all editable fields; a single "Save & Reboot" button submits
everything and triggers `ESP.restart()`.
