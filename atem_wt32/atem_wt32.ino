/*
 * ATEM Tally Light — WT32-ETH01 Edition, with multi-unit relay
 * ------------------------------------------------------------------------
 * Targets the WT32-ETH01 module specifically: a classic ESP32 (not S2)
 * with a built-in LAN8720 Ethernet PHY wired via RMII to the chip's
 * internal EMAC. This is a DIFFERENT networking foundation from the
 * W5500-based version of this sketch — no SPI module, no Ethernet_Generic
 * / EthernetWebServer libraries. Instead it uses ETH.h, built into the
 * ESP32 Arduino core, plus the standard WebServer/WiFiUDP classes (which
 * work transparently over the Ethernet interface once it's up, since the
 * ESP32 core shares one network stack across WiFi/Ethernet/PPP).
 *
 * ============================================================
 * UNVERIFIED ON REAL HARDWARE — please read
 * ============================================================
 * This file was adapted from a W5500-based sketch that went through
 * extensive real-hardware debugging over many iterations. This WT32-ETH01
 * rewrite has NOT been through that process — the ETH.h API, pin
 * assignments, and event-handling pattern below are based on Espressif's
 * own current documentation and multiple cross-referenced community
 * sources (Tasmota, ESPHome, ESP-IDF, and a dedicated WT32-ETH01
 * reference repo all agree on the fixed PHY pins used below), but I
 * cannot compile or run this myself. Please treat first boot as a real
 * test, and share any compiler errors or unexpected serial output —
 * that's exactly the kind of thing that took many rounds to nail down
 * on the W5500 version too.
 *
 * Config webpage, persisted settings, tally number selection, and the
 * multi-unit relay concept are inspired by the general feature set of
 * AronHetLam's WiFi-based ATEM_tally_light_with_ESP8266 project (which
 * uses a "Tally Server" relay to work around the ATEM's 5–8 simultaneous
 * client connection limit). Reimplemented from scratch — no code from
 * that repo is reused.
 *
 * Libraries required:
 *   - ETH             (bundled with the ESP32 Arduino core — no install needed)
 *   - WebServer       (bundled with the ESP32 Arduino core)
 *   - WiFiUdp         (bundled with the ESP32 Arduino core, despite the name —
 *                       it works over any active network interface, not just WiFi)
 *   - Preferences     (bundled with the ESP32 core)
 *   - Adafruit NeoPixel   by Adafruit (RGBW addressable strip support)
 *   - ATEMbase / ATEMstd   by Kasper Skårhøj (SKAARHOJ)
 *       https://github.com/kasperskaarhoj/SKAARHOJ-Open-Engineering
 *
 * Board setup: Tools -> Board -> "WT32-ETH01" (or "ESP32 Dev Module" if
 * your core version doesn't list it by name). Upload speed 115200. This
 * module has no onboard USB — you'll need a USB-to-serial adapter wired
 * to its TXD0/RXD0/GND/3V3 pins, with GPIO0 pulled low during flashing
 * to enter bootloader mode (consistent with how this board is documented
 * everywhere — see the WT32-ETH01 datasheet for the exact procedure).
 *
 * ============================================================
 * WIRING — fixed by the module's own hardware, not user-configurable
 * ============================================================
 * The LAN8720 PHY is already wired to the ESP32 inside the module itself
 * — there's nothing to connect for Ethernet. These GPIOs are permanently
 * committed to that PHY and MUST NOT be reused for anything else:
 *   GPIO 0  - RMII clock input (50MHz from the PHY's oscillator)
 *   GPIO 16 - PHY power/enable
 *   GPIO 18 - MDIO
 *   GPIO 19 - RMII TXD0   (fixed in ESP32 silicon, not board-specific)
 *   GPIO 21 - RMII TX_EN  (fixed in ESP32 silicon, not board-specific)
 *   GPIO 22 - RMII TXD1   (fixed in ESP32 silicon, not board-specific)
 *   GPIO 23 - MDC
 *   GPIO 25 - RMII RXD0   (fixed in ESP32 silicon, not board-specific)
 *   GPIO 26 - RMII RXD1   (fixed in ESP32 silicon, not board-specific)
 *   GPIO 27 - RMII CRS_DV (fixed in ESP32 silicon, not board-specific)
 * That's 10 of the module's GPIOs permanently spoken for. Because GPIO 0
 * is committed to the Ethernet clock, the usual "hold BOOT low" trick
 * doesn't apply the same way here — use the datasheet's documented
 * programming procedure instead.
 *
 * What's left, per the WT32-ETH01 datasheet's own header pinout, is:
 *   IO2, IO4, IO5, IO12, IO14, IO15, IO17, IO32, IO33  (general I/O)
 *   IO35, IO36, IO39                                   (INPUT ONLY)
 * A few of those have boot-time caveats (IO2 must float/be low at boot,
 * IO12 must float/be low at boot or the chip won't start, IO5/IO15
 * affect boot debug output). IO4, IO14, IO17, IO32, IO33 have no such
 * caveats, so this sketch's LED defaults are drawn from that clean set:
 *
 *   LED output — pick ONE, set via the config webpage:
 *     Addressable RGBW strip (e.g. SK6812): data -> GPIO 32
 *       (clean, caveat-free WT32-ETH01 pin), driven via Adafruit_NeoPixel
 *       — see "LED OUTPUT" note below for why not FastLED. Pixel count
 *       configurable on the webpage (1-60). First half of the strip
 *       shows green for PREVIEW, second half shows red for PROGRAM —
 *       independent halves, not a single whole-strip color.
 *     Simple red/green LEDs: default GPIO 4 (red) and GPIO 14 (green),
 *       each through a current-limiting resistor to the LED, other leg
 *       to GND — pins are editable on the webpage if you need different
 *       ones (IO17 and IO33 are the other clean, caveat-free options).
 *       Red and green are driven independently here too (PROGRAM and
 *       PREVIEW respectively), matching the strip's behavior.
 *
 * ============================================================
 * LED OUTPUT — why Adafruit_NeoPixel instead of FastLED
 * ============================================================
 * FastLED has no official support for true 4-channel RGBW LEDs like the
 * SK6812 — CRGB is a 3-byte struct, and full RGBW support has been an
 * open FastLED feature request since 2016 without landing. The only way
 * to get RGBW out of FastLED is an unofficial third-party workaround (a
 * separate, non-bundled header, a custom CRGBW struct, and documented
 * rough edges: broken color-order handling for the 4th channel, and
 * stray "junk" data bleeding onto neighboring pixels for LED counts
 * that aren't multiples of 12). Adafruit_NeoPixel has mature, native
 * RGBW support with none of that — genuinely runtime-configurable pixel
 * counts included — so this sketch uses it instead for the addressable-
 * strip LED mode. The simple red/green GPIO mode is unaffected either
 * way, since it never used either LED library.
 *
 * ============================================================
 * TALLY LOGIC — proper DSK/keyer-aware tally-by-index
 * ============================================================
 * Comparing against the raw program/preview bus number alone misses
 * sources fed to air only through a DSK (downstream keyer) or upstream
 * keyer: a lower-third graphic or an overlay can genuinely be live on
 * air without ever being "on program" in the raw-bus sense.
 *
 * ATEMstd exposes getTallyByIndexTallyFlags(videoSource), which mirrors
 * the ATEM protocol's own "Tally by Index" data — the same mechanism
 * Blackmagic's control panels and official tally systems use. It
 * returns a per-source bitmask (bit 0 = program, bit 1 = preview) that
 * correctly accounts for DSKs and keyers, not just the raw program bus.
 * This sketch reads that flag for every source up to MAX_TALLY_SOURCES
 * once per loop and uses it — not the raw bus number — to decide the
 * LED color and the status page's ON AIR / ARMED pills. The raw M/E1
 * bus numbers are still shown on the status page, but purely as a
 * diagnostic, decoupled from the actual tally decision.
 *
 * Note getTallyByIndexTallyFlags() takes a 0-based array index (0-20),
 * not the 1-based physical input number — see the comment in loop()
 * where it's called for why this matters.
 *
 * ============================================================
 * MULTI-UNIT SUPPORT (relay mode)
 * ============================================================
 * An ATEM switcher only accepts a handful of simultaneous client
 * connections (typically 5–8 depending on model). To run more tally
 * lights than that, only ONE unit connects to the switcher directly
 * ("Direct" mode); the rest run in "Relay Client" mode and get their
 * tally data from another unit over a lightweight UDP broadcast
 * instead of talking to the switcher at all.
 *
 * Every unit — Direct or Relay Client — also rebroadcasts whatever
 * tally state it currently has, so you can chain units into a tree:
 *   Unit A (Direct, talks to ATEM)
 *     -> Unit B (Relay Client of A)
 *     -> Unit C (Relay Client of A)
 *          -> Unit D (Relay Client of C)
 * There's no hard limit on chain depth or fan-out beyond your network.
 * A WT32-ETH01 direct unit can freely mix with Lolin-S2-Mini/W5500 units
 * from the other version of this sketch in the same relay tree — the
 * UDP relay protocol is just raw bytes on the wire, independent of
 * which hardware sent them.
 *
 * Each unit's mode, upstream relay IP, and tally number are all set
 * through the same config webpage. Relay packets are small UDP
 * broadcasts on the local subnet (port 9910 by default) — nothing is
 * sent over the internet and no pairing/discovery step is required
 * beyond typing in the upstream unit's IP.
 *
 * First boot / reconfiguring:
 *   The unit tries DHCP first. Once it has an address (check Serial
 *   Monitor at 115200 baud for the IP), open http://<that-ip>/ in a
 *   browser to configure mode, tally number, switcher/upstream IP,
 *   and optionally a static IP for the unit itself. Settings are
 *   saved to flash (NVS) and survive reboots/power loss.
 */

#include <Arduino.h>

// These must be defined BEFORE #include <ETH.h> — the library picks
// them up to configure the built-in EMAC/PHY driver. Values below are
// the WT32-ETH01's fixed hardware wiring (see WIRING note above),
// cross-referenced against Tasmota, ESPHome, ESP-IDF, and the dedicated
// egnor/wt32-eth01 reference repo, which all agree on these exact pins.
#ifndef ETH_PHY_MDC
#define ETH_PHY_TYPE  ETH_PHY_LAN8720
#define ETH_PHY_ADDR  1
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
#define ETH_PHY_POWER 16
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN
#endif

#include <ETH.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ATEMbase.h>
#include <ATEMstd.h>              // See "TALLY LOGIC" note above — this has full tally-by-index support
#include <Adafruit_NeoPixel.h>    // NOT FastLED — see "LED OUTPUT" note above for why

// ---------- LED output ----------
// Two mutually exclusive ways to drive the tally indicator:
//   LED_MODE_FASTLED   - an RGBW addressable strip (SK6812 etc.) via
//                         Adafruit_NeoPixel, with a configurable pixel
//                         count. Named LED_MODE_FASTLED for backward
//                         compatibility with saved config values from
//                         earlier versions of this sketch — it no
//                         longer actually uses the FastLED library.
//   LED_MODE_SIMPLE_RG - two plain LEDs (just red + green) wired
//                         directly to GPIO pins, driven with digitalWrite.
// Only one is active at a time, chosen via the config webpage.
//
// Display scheme for LED_MODE_FASTLED: the strip is split into two
// halves. The first half shows green when this unit's source is on
// PREVIEW; the second half shows red when it's on PROGRAM. These are
// independent, not mutually exclusive — if a source is somehow both
// (e.g. certain DSK/keyer configurations), both halves light
// simultaneously rather than one taking priority.
enum LedMode : uint8_t { LED_MODE_FASTLED = 0, LED_MODE_SIMPLE_RG = 1 };

#define LED_PIN     32          // NeoPixel data pin. GPIO32 is a clean,
                                 // caveat-free WT32-ETH01 pin.
// SK6812 RGBW strips are almost always GRBW wire order at 800kHz — the
// same timing as WS2812B, just 4 bytes/pixel instead of 3. If colors
// come out wrong on real hardware (e.g. red shows as green), this is
// the first thing to try changing, along with the channel order in
// setStatusColor()/updateLed() below.
#define NEOPIXEL_TYPE (NEO_GRBW + NEO_KHZ800)
#define BRIGHTNESS  128

#define MAX_FASTLED_COUNT 60     // upper bound accepted by the config page

// Constructed in setup() once cfg.fastLedCount is known — Adafruit_NeoPixel
// (unlike FastLED) accepts a genuine runtime pixel count, so there's no
// need for a fixed-size backing array here.
Adafruit_NeoPixel *strip = nullptr;

// Default GPIO pins for simple red/green LEDs. GPIO4 and GPIO14 are on
// the WT32-ETH01's clean, caveat-free general-I/O list (see WIRING note
// above) — no boot sequence, Ethernet PHY, or flash involvement. If you
// need different ones, GPIO17 and GPIO33 are the other clean options;
// all four are editable on the config page regardless.
#define DEFAULT_SIMPLE_RED_PIN   4
#define DEFAULT_SIMPLE_GREEN_PIN 14

// ---------- Watchdog / diagnostics timing ----------
// (No hardware watchdog in this sketch — an earlier attempt at one on
// the W5500 version caused a worse failure mode, a silent boot loop,
// than the hang it was meant to prevent. See the diagnostic serial
// output below instead if you run into a stuck connection.)

// ---------- Relay protocol ----------
#define RELAY_PORT 9910
const unsigned long RELAY_BROADCAST_INTERVAL_MS = 250; // heartbeat even if unchanged
const unsigned long RELAY_TIMEOUT_MS             = 3000; // upstream considered lost after this

// How many ATEM sources (physical inputs, color bars, media players, etc.)
// to track tally-by-index data for. ATEMstd's internal tally-by-index
// array holds 21 entries (sources 1-21) — this covers every current
// ATEM model's inputs plus internal sources (color bars, media players,
// etc). Don't raise this past 21; ATEMstd won't track more than that.
#define MAX_TALLY_SOURCES 21

// getTallyByIndexTallyFlags() bit meaning, per the ATEM protocol's "Tally by Index"
// command — this is the flag Blackmagic's own control panels and tally
// systems use, and it correctly reflects sources live via a DSK or
// upstream keyer, not just whatever's on the main program bus.
#define TALLY_FLAG_PROGRAM 0x01
#define TALLY_FLAG_PREVIEW 0x02

enum TallyMode : uint8_t { MODE_DIRECT = 0, MODE_RELAY_CLIENT = 1 };

struct TallyState {
  // Tally-by-index flags for sources 1..MAX_TALLY_SOURCES, stored at
  // index (source - 1). Bit 0 = program, bit 1 = preview. This is what
  // actually drives the LED and the ON AIR / ARMED pills.
  uint8_t flags[MAX_TALLY_SOURCES];
  // Raw M/E 1 program/preview bus numbers — diagnostic only, shown on
  // the status page so you can confirm live data is flowing even for a
  // unit whose own tally number isn't currently active.
  uint16_t rawProgramInput;
  uint16_t rawPreviewInput;
};

// ---------- Persisted config ----------
Preferences prefs;

// How many comma-separated tally numbers a single unit can monitor at
// once. This is a practical cap, not a protocol limit — raise it if you
// genuinely need more than this many inputs OR'd onto one tally light.
#define MAX_MONITORED_TALLY_INPUTS 8

struct TallyConfig {
  // Which ATEM input(s) this unit represents. Parsed from a comma-
  // separated string like "7,8" — this unit shows ON AIR if ANY listed
  // input is on program, ARMED if ANY is on preview (independently, per
  // the usual bit-OR of tally flags — see getMyTallyFlags()).
  // tallyNumbersStr is kept around verbatim so the config webpage can
  // redisplay exactly what was typed, rather than a reconstructed list.
  String    tallyNumbersStr;
  uint8_t   tallyNumbers[MAX_MONITORED_TALLY_INPUTS];
  uint8_t   tallyNumberCount;
  TallyMode mode;          // MODE_DIRECT or MODE_RELAY_CLIENT
  bool      useDHCP;
  IPAddress staticIP;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress switcherIP;    // used in MODE_DIRECT
  IPAddress upstreamIP;    // used in MODE_RELAY_CLIENT
  LedMode   ledMode;       // LED_MODE_FASTLED or LED_MODE_SIMPLE_RG
  uint8_t   fastLedCount;  // used in LED_MODE_FASTLED, 1..MAX_FASTLED_COUNT
  uint8_t   simpleRedPin;  // used in LED_MODE_SIMPLE_RG
  uint8_t   simpleGreenPin; // used in LED_MODE_SIMPLE_RG
};

TallyConfig cfg;

// Parses a comma-separated tally number string (e.g. "7,8", "7, 8",
// "3") into cfg.tallyNumbers/tallyNumberCount. Out-of-range tokens
// (outside 1..MAX_TALLY_SOURCES) and duplicates are silently skipped;
// if nothing valid was found, falls back to a single "1" so the unit
// always monitors something rather than nothing.
void parseTallyNumbers(const String &input) {
  cfg.tallyNumberCount = 0;
  int start = 0;
  int len = input.length();
  while (start < len && cfg.tallyNumberCount < MAX_MONITORED_TALLY_INPUTS) {
    int comma = input.indexOf(',', start);
    String token = (comma == -1) ? input.substring(start) : input.substring(start, comma);
    token.trim();
    if (token.length() > 0) {
      int val = token.toInt();
      if (val >= 1 && val <= MAX_TALLY_SOURCES) {
        bool dup = false;
        for (uint8_t i = 0; i < cfg.tallyNumberCount; i++) {
          if (cfg.tallyNumbers[i] == val) { dup = true; break; }
        }
        if (!dup) {
          cfg.tallyNumbers[cfg.tallyNumberCount++] = (uint8_t)val;
        }
      }
    }
    if (comma == -1) break;
    start = comma + 1;
  }
  if (cfg.tallyNumberCount == 0) {
    cfg.tallyNumbers[0] = 1;
    cfg.tallyNumberCount = 1;
  }
}

void loadConfig() {
  prefs.begin("tally", true); // read-only
  cfg.tallyNumbersStr = prefs.getString("tallyNos", "1");
  cfg.mode           = (TallyMode)prefs.getUChar("mode", MODE_DIRECT);
  cfg.useDHCP        = prefs.getBool("dhcp", true);
  cfg.staticIP       = IPAddress(prefs.getUInt("ip", 0));
  cfg.gateway        = IPAddress(prefs.getUInt("gw", 0));
  cfg.subnet         = IPAddress(prefs.getUInt("sn", (uint32_t)IPAddress(255,255,255,0)));
  cfg.switcherIP     = IPAddress(prefs.getUInt("atem", (uint32_t)IPAddress(192,168,1,240)));
  cfg.upstreamIP     = IPAddress(prefs.getUInt("upstream", (uint32_t)IPAddress(192,168,1,177)));
  cfg.ledMode        = (LedMode)prefs.getUChar("ledMode", LED_MODE_FASTLED);
  cfg.fastLedCount   = prefs.getUChar("fastLedN", 1);
  cfg.simpleRedPin   = prefs.getUChar("redPin", DEFAULT_SIMPLE_RED_PIN);
  cfg.simpleGreenPin = prefs.getUChar("grnPin", DEFAULT_SIMPLE_GREEN_PIN);
  prefs.end();

  parseTallyNumbers(cfg.tallyNumbersStr);

  // Guard against a stray/corrupt 0 or out-of-range value bricking the
  // NeoPixel strip construction/indexing.
  if (cfg.fastLedCount < 1 || cfg.fastLedCount > MAX_FASTLED_COUNT) {
    cfg.fastLedCount = 1;
  }
}

void saveConfig() {
  prefs.begin("tally", false); // read-write
  prefs.putString("tallyNos", cfg.tallyNumbersStr);
  prefs.putUChar("mode", (uint8_t)cfg.mode);
  prefs.putBool("dhcp", cfg.useDHCP);
  prefs.putUInt("ip", (uint32_t)cfg.staticIP);
  prefs.putUInt("gw", (uint32_t)cfg.gateway);
  prefs.putUInt("sn", (uint32_t)cfg.subnet);
  prefs.putUInt("atem", (uint32_t)cfg.switcherIP);
  prefs.putUInt("upstream", (uint32_t)cfg.upstreamIP);
  prefs.putUChar("ledMode", (uint8_t)cfg.ledMode);
  prefs.putUChar("fastLedN", cfg.fastLedCount);
  prefs.putUChar("redPin", cfg.simpleRedPin);
  prefs.putUChar("grnPin", cfg.simpleGreenPin);
  prefs.end();
}

// ---------- Web config server ----------
WebServer server(80);

String ipToStr(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

IPAddress strToIp(const String &s) {
  IPAddress ip;
  ip.fromString(s);
  return ip;
}

// ---------- Ethernet link state (event-driven) ----------
// ETH.h reports link/IP status via events rather than the polling-style
// linkStatus()/hardwareStatus() the W5500 Ethernet library used. These
// flags are updated from onEthEvent() and read everywhere else.
volatile bool ethLinkUp = false;
volatile bool ethGotIP  = false;

void onEthEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH started");
      // Hostname must be set after the interface starts but before DHCP.
      ETH.setHostname("atem-tally");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH link up");
      ethLinkUp = true;
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("ETH got IP: ");
      Serial.println(ETH.localIP());
      ethGotIP = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH lost IP");
      ethGotIP = false;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH link down");
      ethLinkUp = false;
      ethGotIP = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH stopped");
      ethLinkUp = false;
      ethGotIP = false;
      break;
    default:
      break;
  }
}

// ---------- ATEM (Direct mode) ----------
// Runs in its own FreeRTOS task (see atemTask() and setup()) rather than
// inline in loop(). AtemSwitcher.connect() has turned out to block for a
// long time — possibly indefinitely — when the switcher is unreachable:
// unlike the W5500 sketch's EthernetUDP (which offloads ARP resolution
// to dedicated chip hardware, non-blocking from the ESP32's
// perspective), this sketch's hand-patched WiFiUDP-based ATEMbase relies
// on lwIP's software ARP resolution, which can stall the calling thread.
// Isolating it in its own task means the web server (serviced from
// loop() on the main task) stays responsive no matter what the ATEM
// connection is doing.
ATEMstd AtemSwitcher;
unsigned long lastConnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL_MS = 5000;

// Tracks when the current Direct-mode connection was established, so we
// can give the switcher a brief grace period to stream its first few
// state commands before trusting the raw bus numbers (see
// rawBusDataReady() below). Deliberately NOT based on
// AtemSwitcher.hasInitialized() — that flag only latches once *every*
// piece of initial state has arrived (multiview, DSK, media pool,
// macros, etc.), and on some switcher models/firmware it never fully
// completes, which would leave the diagnostic display stuck forever.
// Only ever touched from atemTask.
unsigned long directConnectedSince = 0;
bool wasDirectConnectedLastLoop = false;
const unsigned long RAW_BUS_GRACE_PERIOD_MS = 1500;

// Periodic diagnostic serial dump — see the [diag] print in atemTask().
unsigned long lastDiagnosticPrint = 0;
const unsigned long DIAGNOSTIC_INTERVAL_MS = 3000;

// ---------- Cross-task shared state ----------
// Everything below is written by atemTask (Direct mode only) and read
// from the main task (loop(), the web server, updateLed(), etc.) — or
// vice versa for the Relay-Client-mode writes to currentState. Protect
// currentState with stateMutex since it's a multi-field struct that
// could tear if read mid-write; the plain bools are simple enough that
// a torn read just means "stale by one cycle," not corrupted data, so
// they're left as volatile rather than mutex-protected for simplicity.
SemaphoreHandle_t stateMutex;
WiFiUDP relayUdp;
TallyState currentState       = {}; // zero-initialized: all flags 0, raw inputs 0
TallyState lastBroadcastState = {}; // deliberately equal to currentState at boot —
                                     // see forceInitialBroadcast below for why that's fine
bool forceInitialBroadcast = true;  // ensures we still send one packet even though
                                     // currentState == lastBroadcastState at startup
unsigned long lastBroadcastTime = 0;
unsigned long lastRelayRxTime   = 0;

// True once real tally-by-index data (TlIn) has been seen from the
// switcher at least once since the last (re)connect. Some switcher
// firmware only sends TlIn on the first state change rather than as
// part of the initial connection dump — so until this flips true, the
// LED shows the animated "waiting for switcher action" indicator (see
// showWaitingTallyData()) instead of the real tally display.
volatile bool atemTallyDataSeen = false;
volatile bool atemIsConnected   = false; // mirrors AtemSwitcher.isConnected(), safe to read cross-task
volatile bool atemRawBusReady   = false; // mirrors rawBusDataReady()'s grace-period check

// True if this unit currently has a live source of tally data —
// an active ATEM connection in Direct mode, or a recent packet from
// the upstream unit in Relay Client mode. Direct mode reads the shared
// flag written by atemTask rather than calling AtemSwitcher.isConnected()
// directly — AtemSwitcher itself is only ever touched from atemTask.
bool isConnected() {
  if (cfg.mode == MODE_DIRECT) {
    return atemIsConnected;
  } else {
    return (millis() - lastRelayRxTime) <= RELAY_TIMEOUT_MS;
  }
}

// This unit's own tally-by-index flags — bit 0 = program, bit 1 = preview
// — combined (bit-OR'd) across every source in cfg.tallyNumbers, with
// program taking priority: if ANY monitored source is on program, the
// preview bit is suppressed even if another monitored source is
// simultaneously on preview. Multiple monitored sources are meant to
// represent one PC/feed — showing both ON AIR and ARMED at once for
// what's really a single source would be confusing to the operator.
// Using the real tally-by-index data (not just the raw program bus)
// means this correctly reflects a source being live via a DSK/keyer
// too. Reads currentState under the mutex since it's written from
// atemTask (Direct mode) or the main task (Relay Client mode's
// receiveRelayUpdates()).
uint8_t getMyTallyFlags() {
  uint8_t combined = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    for (uint8_t i = 0; i < cfg.tallyNumberCount; i++) {
      uint8_t num = cfg.tallyNumbers[i];
      if (num >= 1 && num <= MAX_TALLY_SOURCES) {
        combined |= currentState.flags[num - 1];
      }
    }
    xSemaphoreGive(stateMutex);
  }
  if (combined & TALLY_FLAG_PROGRAM) {
    combined &= ~TALLY_FLAG_PREVIEW;
  }
  return combined;
}

// True once the raw M/E1 program/preview bus numbers are trustworthy
// enough to display. This matters because 0 is a legitimate ATEM
// source ID (commonly "Black"), so the raw number alone can't tell
// "genuinely Black" apart from "haven't received this yet". Uses a
// short grace period after connecting rather than waiting for the
// program/preview commands specifically, which arrive very early —
// well before hasInitialized(). This is only for the diagnostic
// display; the LED/pills use their own more responsive path and don't
// wait for this.
bool rawBusDataReady() {
  if (cfg.mode == MODE_DIRECT) {
    return atemRawBusReady;
  } else {
    return isConnected(); // relay clients trust whatever their upstream already sent
  }
}

// 1-R 2-G 3-B 4-Y 5-LR 6-LG 7-LB 8-OR 9-PU 0-W

// True once real tally-by-index data is trustworthy for THIS unit's
// configured source. Shown on the status page as "Tally data: Live" vs
// "Waiting for switcher action" so the pre-first-cut window reads as an
// expected startup state, not a fault — matches the animated LED
// indicator for the same state.
bool tallyReady() {
  if (cfg.mode == MODE_DIRECT) {
    return atemTallyDataSeen;
  } else {
    return isConnected(); // relay clients trust whatever their upstream already sent
  }
}


const char PAGE_STYLE[] PROGMEM =
  "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:960px;"
  "margin:24px auto;padding:0 16px;color:#222;background:#f7f7f8}"
  "h2{margin-bottom:4px}"
  ".sub{color:#777;font-size:0.9em;margin-top:0;margin-bottom:20px}"
  ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));"
  "gap:16px;align-items:start}"
  ".card{background:#fff;border-radius:10px;padding:16px 18px;"
  "box-shadow:0 1px 3px rgba(0,0,0,0.08)}"
  ".card h3{margin:0 0 10px;font-size:0.95em;font-weight:700;color:#111;"
  "text-transform:uppercase;letter-spacing:0.03em;border-bottom:1px solid #eee;"
  "padding-bottom:8px}"
  ".status-row{display:flex;justify-content:space-between;align-items:center;"
  "padding:6px 0;border-bottom:1px solid #f1f1f1;gap:8px}"
  ".status-row:last-child{border-bottom:none}"
  ".status-label{color:#666;font-size:0.88em}"
  ".status-value{font-weight:600;font-size:0.92em;text-align:right}"
  ".pill{display:inline-block;padding:3px 10px;border-radius:999px;font-size:0.82em;"
  "font-weight:600;color:#fff;white-space:nowrap}"
  ".pill.ok{background:#2ea043}"
  ".pill.bad{background:#cf222e}"
  ".pill.idle{background:#8c959f}"
  ".pill.program{background:#cf222e}"
  ".pill.preview{background:#2ea043}"
  "label{display:block;margin:10px 0 3px;font-size:0.88em;color:#333}"
  "label:first-child{margin-top:0}"
  "input[type=text],input[type=number]{width:100%;padding:7px;border:1px solid #d0d7de;"
  "border-radius:6px;font-size:0.95em;box-sizing:border-box}"
  ".radio-row{display:flex;align-items:center;gap:8px;margin:6px 0;font-weight:normal;font-size:0.92em}"
  ".radio-row input{width:auto}"
  ".save-bar{margin-top:16px}"
  "input[type=submit]{width:100%;padding:14px;border:none;border-radius:8px;"
  "background:#2563eb;color:#fff;font-size:1.05em;font-weight:600;cursor:pointer}"
  "input[type=submit]:hover{background:#1d4ed8}"
  ".note{color:#777;font-size:0.85em;line-height:1.4}"
  ".checkbox-row{display:flex;align-items:center;gap:8px;font-weight:normal;margin:10px 0;font-size:0.92em}"
  ".checkbox-row input{width:auto}";

const char PAGE_SCRIPT[] PROGMEM =
  "async function refreshStatus(){"
  "try{"
  "const r=await fetch('/status');const s=await r.json();"
  "const c=document.getElementById('conn');"
  "c.textContent=s.connected?'Connected':'Disconnected';"
  "c.className='pill '+(s.connected?'ok':'bad');"
  "const t=document.getElementById('tallyDataStatus');"
  "t.textContent=s.tallyReady?'Live':'Waiting for switcher action';"
  "t.className='pill '+(s.tallyReady?'ok':'idle');"
  "const p=document.getElementById('prog');"
  "p.textContent=s.onProgram?'ON AIR':'-';"
  "p.className='pill '+(s.onProgram?'program':'idle');"
  "const v=document.getElementById('prev');"
  "v.textContent=s.onPreview?'ARMED':'-';"
  "v.className='pill '+(s.onPreview?'preview':'idle');"
  "document.getElementById('progNo').textContent=s.rawBusReady?s.rawProgram:'-';"
  "document.getElementById('prevNo').textContent=s.rawBusReady?s.rawPreview:'-';"
  "document.getElementById('modeVal').textContent=s.mode;"
  "}catch(e){}"
  "}"
  "refreshStatus();setInterval(refreshStatus,1000);";

void handleStatus() {
  bool connected = isConnected();
  uint8_t myFlags = getMyTallyFlags();
  uint16_t rawProg = 0, rawPrev = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    rawProg = currentState.rawProgramInput;
    rawPrev = currentState.rawPreviewInput;
    xSemaphoreGive(stateMutex);
  }
  String json = "{";
  json += "\"connected\":" + String(connected ? "true" : "false") + ",";
  json += "\"mode\":\"" + String(cfg.mode == MODE_DIRECT ? "Direct" : "Relay Client") + "\",";
  json += "\"tallyNumber\":\"" + cfg.tallyNumbersStr + "\",";
  json += "\"rawBusReady\":" + String(rawBusDataReady() ? "true" : "false") + ",";
  json += "\"tallyReady\":" + String(tallyReady() ? "true" : "false") + ",";
  json += "\"rawProgram\":" + String(rawProg) + ",";
  json += "\"rawPreview\":" + String(rawPrev) + ",";
  json += "\"onProgram\":" + String((myFlags & TALLY_FLAG_PROGRAM) ? "true" : "false") + ",";
  json += "\"onPreview\":" + String((myFlags & TALLY_FLAG_PREVIEW) ? "true" : "false") + ",";
  json += "\"ip\":\"" + ipToStr(ETH.localIP()) + "\"";
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleRoot() {
  bool isDirect = (cfg.mode == MODE_DIRECT);
  bool isFastLed = (cfg.ledMode == LED_MODE_FASTLED);

  String html = "<!DOCTYPE html><html><head><title>Tally Light Setup</title>"
                 "<meta charset='UTF-8'>"
                 "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                 "<style>" + String(PAGE_STYLE) + "</style></head><body>";

  html += "<h2>ATEM Tally Light</h2>"
          "<p class='sub'>Unit IP: " + ipToStr(ETH.localIP()) + " &middot; Tally #" + cfg.tallyNumbersStr + "</p>";

  html += "<form method='POST' action='/save'>"
          "<div class='grid'>";

  // ---- Status section ----
  html += "<div class='card'><h3>Status</h3>"
          "<div class='status-row'><span class='status-label'>Connectivity</span>"
          "<span id='conn' class='pill idle'>-</span></div>"
          "<div class='status-row'><span class='status-label'>Mode</span>"
          "<span id='modeVal' class='status-value'>" + String(isDirect ? "Direct" : "Relay Client") + "</span></div>"
          "<div class='status-row'><span class='status-label'>Tally data</span>"
          "<span id='tallyDataStatus' class='pill idle'>-</span></div>"
          "<div class='status-row'><span class='status-label'>Program</span>"
          "<span id='prog' class='pill idle'>-</span></div>"
          "<div class='status-row'><span class='status-label'>Preview</span>"
          "<span id='prev' class='pill idle'>-</span></div>"
          "<div class='status-row'><span class='status-label'>M/E1 program bus <span class='note'>(diagnostic)</span></span>"
          "<span id='progNo' class='status-value'>-</span></div>"
          "<div class='status-row'><span class='status-label'>M/E1 preview bus <span class='note'>(diagnostic)</span></span>"
          "<span id='prevNo' class='status-value'>-</span></div>"
          "</div>";

  // ---- Tally section ----
  html += "<div class='card'><h3>Tally</h3>"
          "<label>Tally / input number(s) <span class='note'>(comma-separated, e.g. 7,8 — ON AIR if any listed input is)</span></label>"
          "<input type='text' name='tallyNo' value='" + cfg.tallyNumbersStr + "' placeholder='e.g. 1 or 7,8'>"

          "<label>Mode</label>"
          "<div class='radio-row'><input type='radio' name='mode' value='direct' " + String(isDirect ? "checked" : "") + "> "
          "Direct - connect straight to the ATEM switcher</div>"
          "<div class='radio-row'><input type='radio' name='mode' value='relay' " + String(!isDirect ? "checked" : "") + "> "
          "Relay Client - get tally data from another unit</div>"

          "<label>ATEM switcher IP <span class='note'>(Direct mode)</span></label>"
          "<input type='text' name='atem' value='" + ipToStr(cfg.switcherIP) + "'>"

          "<label>Upstream unit IP <span class='note'>(Relay Client mode)</span></label>"
          "<input type='text' name='upstream' value='" + ipToStr(cfg.upstreamIP) + "'>"
          "</div>";

  // ---- Network section ----
  html += "<div class='card'><h3>Network</h3>"
          "<div class='checkbox-row'><input type='checkbox' name='dhcp' " + String(cfg.useDHCP ? "checked" : "") + "> Use DHCP for this unit</div>"

          "<label>Static IP <span class='note'>(used only if DHCP is unchecked)</span></label>"
          "<input type='text' name='ip' value='" + ipToStr(cfg.staticIP) + "'>"
          "<label>Gateway</label>"
          "<input type='text' name='gw' value='" + ipToStr(cfg.gateway) + "'>"
          "<label>Subnet mask</label>"
          "<input type='text' name='sn' value='" + ipToStr(cfg.subnet) + "'>"
          "</div>";

  // ---- LEDs section ----
  html += "<div class='card'><h3>LEDs</h3>"
          "<label>LED output</label>"
          "<div class='radio-row'><input type='radio' name='ledMode' value='fastled' " + String(isFastLed ? "checked" : "") + "> "
          "Addressable RGBW strip</div>"
          "<div class='radio-row'><input type='radio' name='ledMode' value='simple' " + String(!isFastLed ? "checked" : "") + "> "
          "Simple red/green LEDs on GPIO pins</div>"

          "<label>RGBW pixel count <span class='note'>(data pin GPIO " + String(LED_PIN) + ")</span></label>"
          "<input type='number' name='fastLedN' value='" + String(cfg.fastLedCount) + "' min='1' max='" + String(MAX_FASTLED_COUNT) + "'>"

          "<label>Red LED GPIO pin <span class='note'>(Simple mode)</span></label>"
          "<input type='number' name='redPin' value='" + String(cfg.simpleRedPin) + "' min='0' max='39'>"
          "<label>Green LED GPIO pin <span class='note'>(Simple mode)</span></label>"
          "<input type='number' name='grnPin' value='" + String(cfg.simpleGreenPin) + "' min='0' max='39'>"
          "</div>";

  html += "</div>"; // .grid

  html += "<div class='save-bar'><input type='submit' value='Save & Reboot'></div>"
          "</form>";

  html += "<p class='note' style='margin-top:16px'>Every unit - Direct or Relay Client - rebroadcasts its tally state "
          "on the local network, so you can chain units: a downstream unit can use any other "
          "unit (direct-connected or itself a relay client) as its upstream. Program/Preview "
          "here use the switcher's per-source tally data, so a source fed to air only through "
          "a DSK or upstream keyer still correctly shows ON AIR.</p>";

  html += "<script>" + String(PAGE_SCRIPT) + "</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  if (server.hasArg("tallyNo")) {
    cfg.tallyNumbersStr = server.arg("tallyNo");
    parseTallyNumbers(cfg.tallyNumbersStr);
    // Store back the parsed/normalized form (trimmed, deduped, invalid
    // tokens dropped) rather than the raw user input, so the page
    // reflects what's actually being monitored after a save+reboot.
    String normalized = "";
    for (uint8_t i = 0; i < cfg.tallyNumberCount; i++) {
      if (i > 0) normalized += ",";
      normalized += String(cfg.tallyNumbers[i]);
    }
    cfg.tallyNumbersStr = normalized;
  }
  if (server.hasArg("mode"))    cfg.mode = (server.arg("mode") == "relay") ? MODE_RELAY_CLIENT : MODE_DIRECT;
  if (server.hasArg("atem"))    cfg.switcherIP = strToIp(server.arg("atem"));
  if (server.hasArg("upstream")) cfg.upstreamIP = strToIp(server.arg("upstream"));
  cfg.useDHCP = server.hasArg("dhcp");
  if (server.hasArg("ip")) cfg.staticIP = strToIp(server.arg("ip"));
  if (server.hasArg("gw")) cfg.gateway  = strToIp(server.arg("gw"));
  if (server.hasArg("sn")) cfg.subnet   = strToIp(server.arg("sn"));

  if (server.hasArg("ledMode")) {
    cfg.ledMode = (server.arg("ledMode") == "simple") ? LED_MODE_SIMPLE_RG : LED_MODE_FASTLED;
  }
  if (server.hasArg("fastLedN")) {
    int n = server.arg("fastLedN").toInt();
    if (n >= 1 && n <= MAX_FASTLED_COUNT) cfg.fastLedCount = n;
  }
  if (server.hasArg("redPin")) {
    int p = server.arg("redPin").toInt();
    if (p >= 0 && p <= 39) cfg.simpleRedPin = p;
  }
  if (server.hasArg("grnPin")) {
    int p = server.arg("grnPin").toInt();
    if (p >= 0 && p <= 39) cfg.simpleGreenPin = p;
  }

  saveConfig();

  server.send(200, "text/html; charset=utf-8",
    "<html><body><h3>Saved. Rebooting...</h3></body></html>");
  delay(500);
  ESP.restart();
}

IPAddress calculateBroadcastAddress(IPAddress ip, IPAddress subnetMask) {
  IPAddress bcast;
  for (int i = 0; i < 4; i++) {
    bcast[i] = ip[i] | (~subnetMask[i] & 0xFF);
  }
  return bcast;
}

void receiveRelayUpdates() {
  int packetSize = relayUdp.parsePacket();
  if (packetSize >= (int)sizeof(TallyState)) {
    if (relayUdp.remoteIP() == cfg.upstreamIP) {
      TallyState incoming;
      relayUdp.read((uint8_t *)&incoming, sizeof(incoming));
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        currentState = incoming;
        xSemaphoreGive(stateMutex);
      }
      lastRelayRxTime = millis();
    } else {
      relayUdp.flush(); // packet from some other unit's broadcast — ignore
    }
  }
}

void broadcastRelayState() {
  TallyState snapshot;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    snapshot = currentState;
    xSemaphoreGive(stateMutex);
  }

  unsigned long now = millis();
  bool changed = forceInitialBroadcast ||
                 (memcmp(&snapshot, &lastBroadcastState, sizeof(TallyState)) != 0);

  if (changed || (now - lastBroadcastTime > RELAY_BROADCAST_INTERVAL_MS)) {
    IPAddress bcast = calculateBroadcastAddress(ETH.localIP(), ETH.subnetMask());
    relayUdp.beginPacket(bcast, RELAY_PORT);
    relayUdp.write((uint8_t *)&snapshot, sizeof(snapshot));
    relayUdp.endPacket();
    lastBroadcastState = snapshot;
    lastBroadcastTime = now;
    forceInitialBroadcast = false;
  }
}

// Solid whole-strip/both-channels color, used ONLY for non-tally status
// indicators (connecting, hardware fault, off at boot) — NOT used for
// normal tally display, which uses the independent-channel/split-strip
// logic in updateLed() below instead.
//
// Simple RG mode only has two channels, so it can't fully distinguish
// arbitrary colors: blue and purple both end up lighting both LEDs
// together here, since there's no physical blue channel to fall back
// on. That's an inherent hardware limit, not a bug — check the serial
// log for the exact status if you need to tell them apart.
void setStatusColor(uint8_t r, uint8_t g, uint8_t b) {
  if (cfg.ledMode == LED_MODE_FASTLED) {
    if (strip == nullptr) return;
    for (uint16_t i = 0; i < cfg.fastLedCount; i++) {
      strip->setPixelColor(i, strip->Color(r, g, b, 0));
    }
    strip->show();
  } else {
    digitalWrite(cfg.simpleRedPin, (r > 0 || b > 0) ? HIGH : LOW);
    digitalWrite(cfg.simpleGreenPin, (g > 0 || b > 0) ? HIGH : LOW);
  }
}

// ---------- Animated waiting-state indicators ----------
// Blink timing shared by both waiting animations below.
unsigned long lastBlinkToggle = 0;
bool blinkPhase = false;
const unsigned long BLINK_INTERVAL_MS = 500;

void updateBlinkPhase() {
  unsigned long now = millis();
  if (now - lastBlinkToggle >= BLINK_INTERVAL_MS) {
    lastBlinkToggle = now;
    blinkPhase = !blinkPhase;
  }
}

// Shown while waiting for a live connection — the ATEM switcher itself
// in Direct mode, or the upstream unit in Relay Client mode.
void showWaitingConnection() {
  updateBlinkPhase();
  if (cfg.ledMode == LED_MODE_FASTLED) {
    if (strip == nullptr) return;
    uint32_t color = blinkPhase ? strip->Color(0, 0, 255, 0) : 0;
    for (uint16_t i = 0; i < cfg.fastLedCount; i++) {
      strip->setPixelColor(i, color);
    }
    strip->show();
  } else {
    // No blue channel available — flash both red & green together instead.
    digitalWrite(cfg.simpleRedPin, blinkPhase ? HIGH : LOW);
    digitalWrite(cfg.simpleGreenPin, blinkPhase ? HIGH : LOW);
  }
}

// Shown once connected, but before real tally-by-index data has arrived
// for this unit's configured source (see atemTallyDataSeen/tallyReady()).
// Deliberately distinct from the normal tally display: rather than
// guessing at real state from the raw bus numbers (which could be
// wrong for a DSK/keyer source), this makes the "don't know yet" window
// visually unambiguous — both halves/LEDs are always lit, but which one
// is red vs green keeps swapping.
void showWaitingTallyData() {
  updateBlinkPhase();
  if (cfg.ledMode == LED_MODE_FASTLED) {
    if (strip == nullptr) return;
    uint16_t previewCount = cfg.fastLedCount / 2; // same split as updateLed()
    for (uint16_t i = 0; i < cfg.fastLedCount; i++) {
      bool firstHalf = (i < previewCount);
      bool showRedHere = firstHalf ? !blinkPhase : blinkPhase;
      strip->setPixelColor(i, showRedHere ? strip->Color(255, 0, 0, 0) : strip->Color(0, 255, 0, 0));
    }
    strip->show();
  } else {
    digitalWrite(cfg.simpleRedPin, blinkPhase ? HIGH : LOW);
    digitalWrite(cfg.simpleGreenPin, blinkPhase ? LOW : HIGH);
  }
}

void updateLed() {
  uint8_t flags = getMyTallyFlags();
  bool onProgram = flags & TALLY_FLAG_PROGRAM;
  bool onPreview = flags & TALLY_FLAG_PREVIEW;

  if (cfg.ledMode == LED_MODE_FASTLED) {
    if (strip == nullptr) return;
    // First half of the strip = PREVIEW (green), second half = PROGRAM
    // (red). Independent, not mutually exclusive: if a source is
    // genuinely both at once (some DSK/keyer setups), both halves light
    // simultaneously rather than one taking priority over the other.
    uint16_t previewCount = cfg.fastLedCount / 2; // remainder goes to program half
    for (uint16_t i = 0; i < cfg.fastLedCount; i++) {
      if (i < previewCount) {
        strip->setPixelColor(i, onPreview ? strip->Color(0, 255, 0, 0) : 0);
      } else {
        strip->setPixelColor(i, onProgram ? strip->Color(255, 0, 0, 0) : 0);
      }
    }
    strip->show();
  } else {
    digitalWrite(cfg.simpleRedPin, onProgram ? HIGH : LOW);
    digitalWrite(cfg.simpleGreenPin, onPreview ? HIGH : LOW);
  }
}

void setupEthernet() {
  setStatusColor(0, 0, 255); // "connecting" indicator

  Network.onEvent(onEthEvent);

  // For static IP, config() must be called BEFORE begin() — calling it
  // after has caused connection failures in some core versions (this is
  // the same order WiFi.config()/WiFi.begin() use).
  if (!cfg.useDHCP) {
    ETH.config(cfg.staticIP, cfg.gateway, cfg.subnet);
  }

  // Argument order changed in esp32-arduino core v3.x — see the
  // "UNVERIFIED ON REAL HARDWARE" note at the top of this file.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
#else
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_LAN8720, ETH_CLK_MODE);
#endif

  Serial.print("Waiting for Ethernet...");
  unsigned long waitStart = millis();
  while (!ethGotIP && (millis() - waitStart) < 15000) {
    Serial.print(".");
    delay(200);
  }
  Serial.println();
  if (!ethGotIP) {
    Serial.println("WARNING: no IP after 15s — check the cable/link. "
                    "Will keep retrying in the background via ETH events.");
  } else {
    Serial.print("Tally unit IP: ");
    Serial.println(ETH.localIP());
  }
}

// Runs entirely on its own FreeRTOS task, started from setup() only in
// MODE_DIRECT. AtemSwitcher is ONLY ever touched from this task — never
// from loop() or the web server handlers — so there's no risk of two
// tasks calling into the (almost certainly not thread-safe) ATEMstd
// object concurrently. Results are published to the main task via the
// mutex-protected currentState struct and the volatile atemXxx flags
// above.
void atemTask(void *pvParameters) {
  for (;;) {
    AtemSwitcher.runLoop();

    if (!AtemSwitcher.isConnected()) {
      if (wasDirectConnectedLastLoop) {
        Serial.println("ATEM connection lost.");
      }
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        memset(&currentState, 0, sizeof(currentState));
        xSemaphoreGive(stateMutex);
      }
      atemTallyDataSeen = false; // re-arm the waiting-for-tally-data indicator for the next connection
      atemRawBusReady = false;
      atemIsConnected = false;
      // Clear the tally array here - avoids retaining it from the last connection and appearing to be valid!
      for (uint8_t i = 0; i < MAX_TALLY_SOURCES; i++) {
        newState.flags[i] = 0;
      }
      wasDirectConnectedLastLoop = false; // re-arm the raw-bus grace period too

      unsigned long now = millis();
      if (now - lastConnectAttempt > RECONNECT_INTERVAL_MS) {
        lastConnectAttempt = now;
        Serial.print("Reconnecting to ATEM... (free heap: ");
        Serial.print(ESP.getFreeHeap());
        Serial.println(" bytes)");
        AtemSwitcher.connect(); // may block for a long time if unreachable —
                                 // that's fine, it only stalls this task now
      }
    } else {
      atemIsConnected = true;
      if (!wasDirectConnectedLastLoop) {
        directConnectedSince = millis();
        wasDirectConnectedLastLoop = true;
        Serial.println("ATEM connected.");
      }

      // Don't gate this on AtemSwitcher.hasInitialized(): that flag only
      // latches once the switcher has sent its *entire* initial state
      // payload (multiview layout, DSK config, media pool, etc. — not
      // just tally), which can take a while, and on some switcher
      // models/firmware never fully completes. Program/preview bus data
      // populates as soon as the relevant commands arrive, early in the
      // handshake — but some switcher firmware only sends the per-source
      // Tally by Index data (TlIn) on the *first state change* rather
      // than as part of the initial dump, so we track whether we've
      // ever seen real tally data for this unit's own source.
      // Until atemTallyDataSeen flips true, the LED shows the animated
      // "waiting for switcher action" indicator (see showWaitingTallyData()
      // and the state machine at the end of loop()) instead of guessing
      // at real state from the raw bus numbers — a guess could be wrong
      // for a source that's only live via a DSK/keyer.
      TallyState newState;
      for (uint8_t i = 0; i < MAX_TALLY_SOURCES; i++) {
        // getTallyByIndexTallyFlags() takes a 0-based array index (valid
        // range 0-20 for ATEMstd's 21-entry table), NOT the 1-based
        // physical input number — confirmed from the library source:
        // "uint8_t ATEMbase::getTallyByIndexTallyFlags(uint16_t sources)
        // { return atemTallyByIndexTallyFlags[sources]; }". Passing i+1
        // here would both misalign every real reading by one slot AND
        // read one past the end of the array on the last iteration
        // (undefined behavior). newState.flags[i] still represents
        // physical input (i+1) — see getMyTallyFlags() above.
        newState.flags[i] = AtemSwitcher.getTallyByIndexTallyFlags(i);
      }
      // M/E 1 (index 0) raw bus numbers — shown on the status page as a
      // diagnostic only; no longer used to approximate tally state.
      newState.rawProgramInput = AtemSwitcher.getProgramInputVideoSource(0);
      newState.rawPreviewInput = AtemSwitcher.getPreviewInputVideoSource(0);

      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        currentState = newState;
        xSemaphoreGive(stateMutex);
      }

      // Only trust real tally data once ANY of the 21 tracked sources
      // has shown non-zero flags — not just this unit's own monitored
      // source(s). A monitored source can legitimately sit at zero for
      // a long time (genuinely off-air/off-preview) even after the
      // switcher's TlIn stream has started flowing; checking only the
      // monitored source(s) can't tell "genuinely zero" apart from
      // "haven't received anything yet" in that case. Any other source
      // going non-zero is still valid evidence real tally data has
      // started arriving, so it's safe to trust this unit's own
      // (possibly still-zero) flags from that point on.
      for (uint8_t i = 0; i < MAX_TALLY_SOURCES; i++) {
        if (newState.flags[i] != 0) {
          atemTallyDataSeen = true;
          break;
        }
      }

      atemRawBusReady = (millis() - directConnectedSince >= RAW_BUS_GRACE_PERIOD_MS);

      // Periodic diagnostic dump — prints every DIAGNOSTIC_INTERVAL_MS
      // while connected, so we can see exactly what the library is
      // reporting over time without flooding the serial log every loop.
      unsigned long nowDiag = millis();
      if (nowDiag - lastDiagnosticPrint > DIAGNOSTIC_INTERVAL_MS) {
        lastDiagnosticPrint = nowDiag;
        Serial.print("[diag] ethGotIP=");
        Serial.print(ethGotIP);
        Serial.print(" isConnected=");
        Serial.print(AtemSwitcher.isConnected());
        Serial.print(" hasInitialized=");
        Serial.print(AtemSwitcher.hasInitialized());
        Serial.print(" rawProgram=");
        Serial.print(newState.rawProgramInput);
        Serial.print(" rawPreview=");
        Serial.print(newState.rawPreviewInput);
        Serial.print(" tallyDataSeen=");
        Serial.print(atemTallyDataSeen);
        Serial.print(" myTallyFlags(sources=");
        Serial.print(cfg.tallyNumbersStr);
        Serial.print(")=");
        Serial.print(getMyTallyFlags());
        Serial.print(" freeHeap=");
        Serial.println(ESP.getFreeHeap());
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // yield — this task should never busy-loop
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  stateMutex = xSemaphoreCreateMutex();
  if (stateMutex == NULL) {
    // Every mutex-protected function downstream (getMyTallyFlags,
    // handleStatus, broadcastRelayState, receiveRelayUpdates, atemTask)
    // assumes this succeeded. Rather than silently proceeding into
    // undefined behavior on every xSemaphoreTake() call, fail loudly and
    // stop here — this should be extremely rare (essentially only on
    // heap exhaustion this early in boot).
    Serial.println("FATAL: failed to create state mutex. Halting.");
    while (true) delay(1000);
  }

  loadConfig();

  // Initialize whichever LED hardware this unit is configured for. This
  // has to happen AFTER loadConfig() since both the pixel count and the
  // simple-LED GPIO pins are user-configurable.
  if (cfg.ledMode == LED_MODE_FASTLED) {
    strip = new Adafruit_NeoPixel(cfg.fastLedCount, LED_PIN, NEOPIXEL_TYPE);
    if (strip == nullptr) {
      Serial.println("FATAL: failed to allocate NeoPixel strip. Halting.");
      while (true) delay(1000);
    }
    strip->begin();
    strip->setBrightness(BRIGHTNESS);
  } else {
    pinMode(cfg.simpleRedPin, OUTPUT);
    pinMode(cfg.simpleGreenPin, OUTPUT);
  }
  setStatusColor(0, 0, 0);

  setupEthernet();

  relayUdp.begin(RELAY_PORT);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Config page ready.");

  if (cfg.mode == MODE_DIRECT) {
    Serial.println("Mode: Direct (ATEM connection runs in its own background task)");
    AtemSwitcher.begin(cfg.switcherIP);
    AtemSwitcher.serialOutput(0x80);
    // Runs on its own task specifically so a blocking AtemSwitcher.connect()
    // call (which has been observed to hang for a long time — possibly
    // indefinitely — when the switcher is unreachable) can never take
    // the web server down with it. Stack size is a reasonable starting
    // guess for this task's needs (mostly UDP I/O, minimal string work);
    // if you see a stack-overflow panic in the serial log, raise it.
    xTaskCreate(atemTask, "atemTask", 8192, NULL, 1, NULL);
  } else {
    Serial.print("Mode: Relay Client (upstream = ");
    Serial.print(ipToStr(cfg.upstreamIP));
    Serial.println(")");
  }
}

void loop() {
  server.handleClient();

  if (cfg.mode == MODE_RELAY_CLIENT) {
    receiveRelayUpdates();
    if (millis() - lastRelayRxTime > RELAY_TIMEOUT_MS) {
      // Upstream unit hasn't been heard from recently — treat as unknown.
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        memset(&currentState, 0, sizeof(currentState));
        xSemaphoreGive(stateMutex);
      }
    }
  }
  // Direct mode's ATEM polling happens entirely in atemTask (started
  // from setup()) — this keeps loop()/server.handleClient() responsive
  // no matter how long AtemSwitcher.connect() blocks for.

  // LED state machine: waiting for connection takes priority over
  // waiting for tally data, which takes priority over the normal tally
  // display. isConnected()/tallyReady() already abstract over Direct vs
  // Relay Client mode, so this reads the same regardless of cfg.mode.
  if (!isConnected()) {
    showWaitingConnection();
  } else if (!tallyReady()) {
    showWaitingTallyData();
  } else {
    updateLed();
  }
  broadcastRelayState(); // always relay onward, so chains work in either mode

  delay(1); // small cooperative yield
}

