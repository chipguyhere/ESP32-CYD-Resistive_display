# Resistive-touch CYD boards — engineering notes

The derivations behind the numbers quoted in [README-power.md](README-power.md),
[README-board.md](README-board.md), [README-sound.md](README-sound.md) and
[README-sdcard.md](README-sdcard.md). Those files state what the board does and what a
sketch must do about it; this one shows the schematic reading, the datasheet figures
and the arithmetic that got there, so anyone who doubts a figure can check it or
replace it with a measurement.

The power, sound and card-slot sections are the **ESP32-32E 2.8"**'s, the board whose
schematic was read. The other boards appear where the bench has said something about
them: [board wiring](#two-wirings-and-how-begin-tells-them-apart), the
[3.2" panel](#the-32-panel), and [GPIO4](#telling-gpio4s-two-jobs-apart).

**Sources.** This board's schematic as read for these notes; part datasheets (TP4054,
ME6217, SL2305 / SI2305, B5819W, SC8002B / LM4871, CH340C, XPT2046, ILI9341, ST7789); the ESP32
Arduino core 3.x / ESP-IDF 5.x driver sources; and the sibling board's write-up
(`../chipguy_cyd2432S028_display/README-power.md` and `README-sound.md`), where the
DAC path was worked out by ear on 2026-09-18. **Nothing on this board has been put on a
meter or a scope yet**; every current, voltage and corner frequency below is inferred,
and the topic files carry a `**TODO (bench):**` wherever that matters. The list at the
[end](#what-a-bench-session-should-measure) collects them.

---

## Identifying the board

Generic "ESP32-32E" 2.8" board, silkscreen *2.8" LCD Display, ESP32-32E 240x320,
Resistance Touch*, sold by several vendors. It is the ESP32-2432S028 CYD's display half
(panel on HSPI, XPT2046 bit-banged, identical pin map — but the panel may be an ST7789,
[below](#display-panel-variants)) with a redesigned power section:
single USB-C connector, 2-pin lithium battery connector, TP4054 charger, discrete
load-sharing path, an ME6217 LDO in place of the sibling's two AMS1117s, and an audio
amplifier moved to the 5 V rail with a shutdown pin. The light sensor is gone; its
GPIO34 is now the battery divider tap. GPIO4 moved from the red LED to the amplifier
enable, and the red LED moved to GPIO22; green/blue stay on GPIO16/17 (all three
confirmed on this board). The SPI header's chip-select is **IO27**, with **no pull-up**
on this board (the sibling's IO27 header pin had 10 kΩ). IO27 is otherwise
unconnected — RTC_GPIO17, touch pad 7, ADC2 channel 7.

**I²C on the SPI header.** With the card unused, IO23 and IO19 make the natural SDA /
SCL pair because the card's 10 kΩ pull-ups are already on them. 10 kΩ is on the weak
side for 400 kHz with any bus capacitance but is fine at 100 kHz, and most breakouts
parallel their own 4.7–10 kΩ, bringing the effective pull-up to 3–5 kΩ. A card left in
the slot with CS high (its own pull-up) sees I²C activity on CMD and DAT0 with no clock
on IO18, so it never interprets anything; still, "remove the card" is the advice that
needs no reasoning.

Every connector is 1.25 mm-pitch Molex PicoBlade or compatible ("MX1.25", "JST 1.25" —
not JST GH; the housings don't interchange). No battery, and so no battery pigtail,
ships with the board.

### Telling GPIO4's two jobs apart

[README-board-detection.md](README-board-detection.md#red-led-gpio4s-voltage) states
what `cydr_redLedPin()` does; this is why it works. Readings with
`analogReadMilliVolts()` (one unit each, 2026-10-13; the pull-down column was
re-measured on the 2432S028, 3.2" and 4.0" through the library's own probe — 1643,
2520 and 2521 mV):

| GPIO4 | internal pull-up | internal pull-down | floating |
|---|---|---|---|
| ESP32-32E 2.8" / 3.2" / 3.5" / 4.0" (amplifier shutdown) | 3.14 V | 2.50 V | 3.14 V |
| ESP32-2432S028 (red LED) | 3.14 V | 1.63 V | 2.47 V |
| ESP32-2432S032C (red LED; capacitive, not supported) | — | 1.62 V | — |

Both boards pull GPIO4 *up*; what differs is the element doing it.

- **ESP32-32E:** a resistor to 3.3 V holds the amplifier's shutdown input off at reset.
  The amplifier input itself draws next to nothing, so what is measured is that
  resistor against the internal pull-down, a plain divider:
  3.3 × R<sub>pd</sub> / (R<sub>pd</sub> + R) = 2.50 V gives R ≈ 14 kΩ at the nominal
  45 kΩ pull-down (inferred; likely a 10 kΩ part with the pull-down off nominal). A
  divider scales with its resistors, so the pull-down's spread (roughly 30–80 kΩ)
  moves this reading across about 2.2–2.9 V.
- **ESP32-2432S028:** the RGB LED is common-anode at 3.3 V, and the red cathode
  reaches GPIO4 through its series resistor. The pull-down draws a few tens of µA
  through the LED, and the pin sits at 3.3 V minus the forward drop. At that current
  a red LED drops about 1.6–1.7 V, and the drop grows only logarithmically with
  current, so the same 30–80 kΩ spread moves the reading by tens of mV, not hundreds.
  Floating, only leakage flows, the drop shrinks to about 0.8 V, and the pin rises to
  2.47 V — the same picture.

So the separation comes from the LED's diode drop pinning the pin near 1.6 V while the
resistor pull-up gives a ratio near 2.5 V. A 2.0 V threshold leaves roughly 0.4 V on
each side across the pull-down's spread (inferred from the one unit of each). The
classic ESP32 guarantees a digital LOW only below 0.25 × VDD (≈ 0.8 V) and HIGH only
above 0.75 × VDD (≈ 2.5 V); 1.6 V is in the undefined band and 2.5 V on its upper edge,
which is why the check has to be analog — and why it has to run before WiFi, since
GPIO4 is ADC2 channel 0.

**Implementation.** `cydr_redLedPin()` reads once (one throwaway read to attach the
channel, internal pull-down on, 2 ms, eight averaged `analogReadMilliVolts()` reads),
restores the pin to a floating input, and calls `perimanClearPinBus(4)` so the ADC2
oneshot unit is deleted before WiFi starts. The result is latched; `display.begin()`
makes the first call right after board detection. A failed ADC2 read (WiFi already
running) comes back as 0, so anything under 500 mV is treated as "no reading" and
returns -1 without latching. On a shared-touch board the measurement is skipped: the
2432S028 is the only red-LED-on-GPIO4 board in scope, and it has own-touch wiring.

### Two wirings, and how `begin()` tells them apart

The display half is the same on every board (panel on HSPI: MOSI 13, SCK 14, CS 15,
DC 2, SDO 12). What differs is the backlight pin and where the XPT2046 sits:

| | Own touch | Shared touch |
|---|---|---|
| Boards | 2432S028, 32E 2.8" | 32E 3.2", 3.5", 4.0" |
| Backlight | GPIO21, external pull-down | GPIO27, external pull-down |
| XPT2046 | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 — bit-banged | on HSPI (13 / 14 / 12), CS 33, IRQ 36 — second SPI device at 2 MHz |
| The other backlight pin | GPIO27: floats (32E 2.8", header CS) or 10 kΩ pull-up (2432S028) | GPIO21: floats (3.2") |

`cydr_detectBoardPassive()` enables the internal pull-up on GPIO21 and GPIO27 in turn;
the backlight driver's pull-down overpowers it and the pin reads 0.13 V. Exactly one
LOW decides the wiring; neither or both is `UNKNOWN`, and `begin()` then guesses from
the panel (ST7796 → shared, else own) and says so on Serial.

The library used to infer the wiring *from* the panel (ST7796 ⇒ 320×480 ⇒ shared). The
3.2" broke that: a probe on 2026-10-13 found GPIO27 pulled down and GPIO21 floating, an
XPT2046 answering on the display bus (temperature channel T0 = 725, `Y` = 4095 with no
touch) and nothing on 25 / 32 / 39 — the shared wiring — behind a 240×320 ST7789. Since
then wiring comes from the pull-downs and geometry from the panel, independently. The
same probe showed GPIO25, 32 and 33 held HIGH under the internal pull-down on the 3.2"
(external pull-ups; 33 is the touch CS, 25 and 32 unexplained — **TODO (bench)**), and
GPIO34 at 2.10 V on USB, as on the 32E 2.8" with no cell.

The touch calibration lives in one NVS namespace per wiring + panel size, so a sketch
moved between boards doesn't carry the wrong rectangle: `e32e28touch` (own touch),
`e32e32touch` (shared, 240×320), `e32etouch` (shared, 320×480 — also the earlier
`chipguy_ESP32-32E_CYD_4inch_display`'s). The names predate the `cydr_` prefix and
were kept so saved calibrations survive the rename.

**A capacitive board looks like a shared-touch one.** The ESP32-2432S032C probed on
2026-10-13 has the GPIO27 pull-down (GPIO21 floats), so it reads as shared touch. Its
panel left GPIO12 following the pull on every ID read (index `FF FF / 00 00`, `0xD3`
and RDDID `FF FF FF`), so detection fell back to ST7796. No XPT2046 answered on either
bus; a GT911 answered at I²C `0x5D` on SDA 33 / SCL 32 (product ID `911`, firmware
`0x1060`), with GPIO21 and 25 also externally pulled up (likely its INT and RST).
GPIO34 read 0.14 V (light sensor, as on the 2432S028), GPIO4 1.62 V (red LED). Out of
scope for this library; recorded so nobody mistakes the misdetection for a bug.

### USB-C CC wiring

**CC1 and CC2 are tied together and share a single 5.1 kΩ pull-down** — the original
Raspberry Pi 4 mistake. A compliant sink has one Rd (5.1 kΩ) per CC pin. A cable only
connects one CC line through; the other pin at the sink end is the cable's VCONN, which
an *e-marked* cable loads with its own Ra (≈1 kΩ). With the two pins shorted, the source
sees Rd ∥ Ra ≈ 5.1 k ∥ 1 k ≈ 0.84 kΩ on its CC line, which is the *audio adapter
accessory* signature, and supplies no VBUS.

Consequences: A-to-C cables (no CC at the A end, so no Ra) and plain non-e-marked C-to-C
cables work; 5 A / 100 W, Thunderbolt, USB4 and most laptop-charger cables are e-marked
and don't. The fix, for anyone inclined, is cutting the CC1–CC2 link and adding a second
5.1 kΩ to the freed pin.

---

## Display panel variants

### The 3.2" panel

An ST7789 at 240×320 that answers RDDID (so detection recognises it on shared-touch
wiring, where a silent panel would be taken for an ST7796). Same MADCTL as the 2.8"
ST7789, but it needs **INVON** (`0x21`): with the 2.8"'s INVOFF it showed inverse video
(white background, cyan where red belonged; observed 2026-10-13). `_initST7789()` picks
INVON on shared-touch wiring, INVOFF on own-touch. Touch runs opposite the panel on
**both** raw axes (the 2.8" boards mirror X only, the 320×480 ones Y only; found by
touch, 2026-10-13). First calibration through `TouchCalibrate`: x 208–3923,
y 399–3982, minimum pressure 58, maximum jump 120 px.

### The 2.8" panels

Two boards with identical silkscreen were probed on 2026-10-10 with a standalone sketch
(HSPI as in the library, MISO on GPIO12, 1 MHz reads; register dumps under GPIO12's
internal pull-up and then pull-down). Board `…6890C8` has an ILI9341, board `…8D3D10`
an ST7789.

**ILI9341 board.** It behaves like the 2432S028: GPIO12 is the panel's SDO. Each reply
was identical under both pulls, and only the bytes after the reply followed the pull,
so SDO is high-Z when not answering. RDDPM `0x0A` = `98` (after SLPOUT, before DISPON),
RDDCOLMOD `0x0C` = `06` (power-on 18-bit), and RDDST `0x09` = `C0 31 80 00`. RDID4 `0xD3`
read directly is undriven. The `0xD9` index trick (write `0x10+n`, then read one byte of
`0xD3`) returns `41 00 93 41` for n = 0..3. RDDID `0x04` and RDID1–3 `0xDA`–`0xDC`
return `00`: no module ID is programmed.

**ST7789 board.** On 2026-10-10 every read came back `FF` under pull-up and `00` under
pull-down, which was taken to mean SDO is not wired to GPIO12. **That was wrong.** A
bit-banged re-probe on 2026-10-11 (the one `begin()` uses, about 200 kHz, CS held across
command and reply) shows the panel does drive GPIO12. RDDID `0x04` returned `81 81 B3`, and
RDDST `0x09` returned `00 61 00 00`, the power-on default, both identical under both pulls.
Without the dummy clock RDDID reads `40 C0 D9`, the same bits one place late, so the
alignment is right. The datasheet ID is `85 85 52`. `81 81 B3` is either a different module
programming or an ST7789-compatible controller; which one is **TODO (bench)**. The panel
leaves `0xD3` and the `0xD9` index trick undriven, so those still read as the pull. Why
the first probe saw nothing was not pinned down; it used the hardware SPI at 1 MHz.
With no pull, GPIO12 idled LOW on both boards, and with the pull-up HIGH. So neither
board has an external pull on it.

The ILI9341 board's RDDST before init is also `00 61 00 00`, the power-on default, so its
panel is reset along with the ESP32. That supports the EN-tied reset inferred below.

**Reset test.** GPIO12 was driven LOW for 2 s, then HIGH, and the panel was left 3 s
before re-init. Neither picture changed (observed). On the ILI9341 board RDDPM read
`9C` before and after, so the controller kept its state. GPIO12 is therefore not
`TFT_RST` on either board. The library's earlier reset pulse on GPIO12 was harmless on
both boards (SDO is high-Z outside reads). Init now
starts with SWRESET `0x01`. That the panel reset is tied to EN, as on the 2432S028
(schematic-description §1.6 there), is **inferred**.

**Panel orientation and color**, chosen by touch from cycled variants on each board:

| | ILI9341 | ST7789 |
|---|---|---|
| Rotation-0 MADCTL | `0x48` (MX, BGR) | `0x00` |
| Inversion | off | off — unusual for an ST7789 module, most need INVON |
| MV for 240×320 | 0 | 0 |
| Touch raw at screen (20,20) / (220,300) | 3518,408 / 532,3437 | 3541,429 / 497,3564 |

Every ST7789 MADCTL is the ILI9341 one with BGR cleared and MX toggled, rotations 1–3
included. That the same toggle holds under MV was established on the bench rather than
from the datasheets. All four rotations and the touch mapping were confirmed on both boards.
The touch overlays read alike, so the touch driver needs no per-panel handling.

**Detection in `begin()`.** It runs before `spi_bus_initialize()` claims the pins, as a
bit-banged exchange at about 200 kHz. It reads index 2 and 3 of the `0xD9`/`0xD3` trick
under pull-up and then pull-down, and RDDID `0x04` (one dummy clock, three bytes) under
each. `93 41` under both pulls means ILI9341. `FF FF` / `00 00` means nothing answered
`0xD3`, so ST7789. Anything else is checked against RDDID bytes 2–3: `85 52` (datasheet)
or `81 B3` (the board above) under both pulls means ST7789. The ILI9341s return
`00 00 00` there, since no module ID is programmed. If neither matches, the bytes are
logged and detection falls back by wiring: ST7796 on shared touch, ILI9341 on own.
(The "nothing answered `0xD3` ⇒ ST7789" shortcut applies on own-touch wiring only.) Verified 2026-10-11 on `…6890C8` (ILI9341),
`…8D3D10` (ST7789) and an ESP32-2432S028 `…1F003C` (ILI9341, same `41 00 93 41` and
RDDID `00 00 00`). GPIO12 is left as an input with pull-down, its reset default. It is the MTDI
strapping pin, which must not be held HIGH at reset (1.8 V flash).

**ST7789 init.** SWRESET, SLPOUT, COLMOD `0x55`, INVOFF (INVON on the 3.2"), NORON and DISPON on power-on
defaults: porch, gate, VCOM and gamma registers are untouched. That was enough for
correct color in the test, but the image has not been compared closely against the
ILI9341 board. **TODO (bench):** check gamma and contrast against a vendor init, if one
turns up.

## Power

### The nets

Three nets matter, and the schematic names two of them confusingly alike:

```
USB VBUS ──┬── +5V ──────────────────┬── D  B5819W (Schottky) ──┬── VCC5V ── ME6217C33 ── VCC3V3 ── ESP32, display, CH340, ...
           │                         │        (arrow → VCC5V)   │
           │   TP4054 VCC ◄──────────┘                          │  S
           │      │ BAT                                 Q3 SL2305 (P-MOSFET), gate ── +5V
           │      │                                             │  D
BAT+ ──────┴──────┴─────────────────────────────────────────────┘
```

- **+5V** is USB VBUS, straight from the connector: it feeds the charger's VCC and Q3's
  gate, and reaches the load only through the diode. The UART header's 5V pin also lands
  here (below).
- **VCC5V** is what actually powers the board — the regulator's input, and the audio
  amplifier's VDD — and is the higher of "USB minus a Schottky drop" and "the cell".
- **BAT+** is the cell: the TP4054's BAT pin, Q3's drain, and the GPIO34 divider.
- **VCC3V3** is the ME6217's output and feeds everything else, including the CH340C.

### Load-sharing handover (inferred from the parts; not scoped)

- **USB present.** VCC5V ≈ 4.5–4.7 V (5 V less the B5819W's forward drop at the load
  current). Q3's gate is at 5 V and its source at ~4.6 V, so V<sub>GS</sub> is
  *positive* and the P-channel FET is off. Its body diode (drain → source, i.e. BAT+ →
  VCC5V) is reverse-biased because a cell never exceeds 4.2 V. The load draws nothing
  from the cell while USB is present, and the TP4054 charges it undisturbed.
- **USB absent.** The +5V net collapses (the charger's VCC pin and anything else on it
  pull it down), Q3's gate goes to ~0 V, V<sub>GS</sub> ≈ −V<sub>BAT</sub>, and the
  SL2305 (an SI2305-class part, tens of mΩ on) connects the cell to VCC5V with only tens
  of millivolts lost. The Schottky blocks VCC5V from feeding back into the dead +5V net.
  VCC5V is now the cell voltage, 3.0–4.2 V — the "5" in its name is aspirational.
- **USB plugged in while running on the cell.** VCC5V rises to ~4.6 V through the
  diode, Q3 turns off as its gate comes up — a make-before-break handover with no
  brown-out, which is the reason for this topology.

Nothing in the path is readable or settable from software: no GPIO sees VBUS or +5V,
and the TP4054's status pin is grounded (below). The only "is USB present" signal a
sketch has is the heuristic that a cell reading ≥ ~4.15 V on GPIO34 is being charged.

### The regulator: ME6217C33 vs the sibling's AMS1117

VCC5V feeds an **ME6217C33M5G** (Microne, 3.3 V fixed, SOT-23-5): an 800 mA CMOS LDO,
dropout of a few hundred millivolts at ESP32 currents, quiescent current in the tens of
µA (datasheet). It is the board's **only** regulator — the ESP32 module has no separate
one (the sibling has two AMS1117s). Compared with an AMS1117 (≈1.1 V dropout, ~5 mA
quiescent):

- The 3.3 V rail stays in regulation down to roughly a **3.5–3.6 V cell**; below that the
  ESP32 rides the sagging rail to its own ~3.0 V limit, i.e. to about a **3.2–3.3 V
  cell** — where a protected cell's own cutoff sits anyway. WiFi bursts (a few hundred
  mA) are within the part's rating and its dropout barely moves.
- The regulator's own consumption drops from milliamps to tens of µA, which is what
  makes deep sleep meaningful on this board and pointless on the sibling.

**Chip markings.** The two SOT-23-5 parts are told apart by laser mark: the regulator
carries the **MicrOne arc logo and `J2Ym`** (which resolves to ME6217C33M5G); the part
beside the battery connector marked **`LTH7`** is the TP4054 — `LTH7` being the
industry-standard marking of the LTC4054 family it clones.

### The charger: TP4054

Linear single-cell lithium charger, SOT-23-5, TP4056 / LTC4054 family. Autonomous —
no bus, no MCU connection, own state machine. Termination at **4.2 V** (fixed). Charge
current is set by the **3.3 kΩ** on PROG: `I_chg = 1000 V / R_prog` ≈ **300 mA**
(datasheet formula). Together with the board's own draw (backlight, WiFi-active ESP32)
that can exceed a 500 mA USB 2.0 port's budget; a weak port sags and the charger's
thermal / voltage fold-back slows the charge rather than anything breaking. Trickle
charge below ~2.9 V, automatic recharge when the cell drifts down to ~4.05 V, and
thermal fold-back are all in the chip (datasheet). Battery-side leakage with USB absent
is ~2 µA (datasheet).

The open-drain **`CHRG` status pin is tied to ground** — not to a GPIO, not to an LED —
so there is no charge indication of any kind. There is no discharge-side protection
anywhere on the board; the cell's own protection PCB is the only undervoltage cutoff.

### The UART header's 5V pin

Lands on the +5V net through a second **SL2305** P-MOSFET: drain on the header pin,
source on +5V, gate to GND via 10 kΩ — the classic reverse-polarity-protection hookup.
5 V of the right polarity flows through the body diode until the FET turns on, then
behaves exactly like USB, charger and load sharing included. Wrong polarity finds the
FET off and the body diode reverse-biased; nothing happens. Because the gate is simply
grounded the FET also conducts the other way, so **with USB plugged in this pin outputs
USB 5 V**. It is not VCC5V, so on battery alone it is dead. USB and this pin are
paralleled — don't feed both from separate supplies.

The header's **RXD** reaches GPIO3 through **100 Ω**, while the CH340's TXD drives GPIO3
directly; the bridge wins, and since the CH340 is on VCC3V3 it does so on battery too.
TXD (GPIO1) out is fine.

### Rails the peripherals sit on

- **Audio amplifier VDD = VCC5V** — ~4.6 V on USB, the cell voltage on battery. Sound
  works on battery at reduced headroom (see [Sound](#sound)).
- **CH340C on VCC3V3** — powered on battery as well as USB. Whatever it draws with no
  host present is part of the battery floor: likely its USB-suspend figure, ~0.1 mA
  (datasheet), possibly the largest single item in that floor. If it dominates, the
  only cure is lifting its supply pin.
- **Panel logic (ILI9341 or ST7789) on VCC3V3**, reset not on a GPIO — no power control. Sleep In (`0x10`) and
  Display Off (`0x28`) get it to a few tens of µA; not exposed by the library yet.
- **XPT2046** — already in power-down between conversions (µA). The touch driver sends
  control bytes `0xD0` / `0x90` with the power-down bits clear, which leaves the
  controller in its lowest-power mode with `PENIRQ` enabled after every read; `PENIRQ`
  reaches **GPIO36** (RTC_GPIO0, input-only, no internal pull; the XPT2046 drives it
  high when idle).
- **IO35 header pin** has **no pull-up on this board** (the sibling's equivalent had
  10 kΩ). IO35 is input-only with no internal pull, so a switch-to-GND there needs an
  external pull-up.

### Battery divider on GPIO34

**100 kΩ / 100 kΩ** from BAT+ to GND, tap on GPIO34 (ADC1 channel 6, input-only,
RTC_GPIO4). A 4.2 V cell presents 2.1 V — inside the ADC's 11 dB-attenuation range and
comfortably mid-scale, away from the ESP32 ADC's non-linear ends. There is a **100 nF
("104") from GPIO34 to ground** at the tap and another from BAT+ to ground at the top.

- The divider's Thevenin source impedance is 100 k ∥ 100 k = **50 kΩ**, which on its own
  would starve the ESP32's ADC sampling capacitor and make readings droop low; the 100 nF
  at the tap supplies the sample instead, so there is no droop and no "discard the
  first sample" ritual.
- 50 kΩ × 100 nF ≈ **5 ms** low-passes the node, so WiFi-burst sag and ADC noise are
  smoothed before the sketch averages anything. Averaging 16 samples is still worth it
  for the ADC's own noise.
- The divider draws 4.2 V / 200 kΩ = **21 µA**, always.

Practical resolution ≈ **±50 mV at the cell**: `analogReadMilliVolts()` applies the
eFuse calibration and is good to a few tens of millivolts at the pin, doubled by the
divider. A raw `analogRead()` scaled by 3300/4095 can be 10 % off on a classic ESP32.
Good for an icon and a cutoff; not for state of charge better than ~10 % on a lithium
curve that is nearly flat between 3.9 and 3.6 V, and not coulomb counting.

**No cell fitted, USB present:** GPIO34 reads the voltage the charger is trying to
impose on the missing cell — ~4.2 V, a convincing "full" that is a lie, and
indistinguishable from a genuinely full cell.

### Deep-sleep floor, by part (inferred)

| Item | On the cell, USB absent |
|---|---|
| ME6217 quiescent | tens of µA |
| ESP32 deep sleep | ~10 µA |
| GPIO34 divider | 21 µA |
| TP4054 battery-side leakage | ~2 µA |
| Schottky reverse leakage into the dead +5V net | µA |
| CH340C idle on VCC3V3 | ~0.1 mA if it suspends; more if it doesn't |
| ILI9341 logic, not slept | more than the above; tens of µA in Sleep In |
| XPT2046 in power-down | µA |
| SC8002B in shutdown | ~1 µA (LM4871 family) |
| RGB LED with all three pins HIGH | 0 |
| Idle SD card, if fitted | hundreds of µA to a few mA |

**On the order of 100–200 µA** before the panel, the touch controller and any card —
a months-of-deep-sleep number on a 1000 mAh cell, fifty to a hundred times better than
the sibling. The RGB LED pins float at reset, so the LED may glow dimly until the sketch
drives all three HIGH; each colour draws a few mA when lit.

### RTC-capable pins on this board

| GPIO | RTC pad | Wired to |
|---|---|---|
| 0 | RTC_GPIO11 | BOOT button (pulled up; strapping pin) |
| 4 | RTC_GPIO10 | amplifier enable, 10 kΩ pull-up — `rtc_gpio_hold_en` can pin it HIGH through deep sleep; unheld, the pull-up does the same |
| 27 | RTC_GPIO17 | SPI header CS — free as an `ext0` / `ext1` / touch-pad wake source when the header carries no SPI device |
| 34 | RTC_GPIO4 | battery divider tap — a wake-on-low-battery needs the ULP to sample it; `ext0` on a digital threshold is unreliable |
| 35 | RTC_GPIO5 | header IO35, no pull |
| 36 | RTC_GPIO0 | XPT2046 `PENIRQ` |

---

## Sound

### Amplifier

**SC8002B**, mono class-AB bridge-tied-load, LM4871 family, **VDD on VCC5V**. From
~4.6 V the LM4871 curves give roughly **1.2 W into 8 Ω / 2 W into 4 Ω** before 1 % THD.
Shutdown pin to **GPIO4, active LOW** (LOW = on), **10 kΩ pull-up** on the board, so the
part is in shutdown from reset until a sketch drives GPIO4 LOW. Shutdown current ~1 µA;
quiescent a few mA when enabled. The family soft-starts its internal bias on release
from shutdown, so its own turn-on transient should be a faint tick at most.

Only **IN−** is reachable from the ESP32. Gain is fixed by **Rf = 20 kΩ** (IN− to VO1)
against the **24.7 kΩ** series input path: single-ended −20 k / 24.7 k = −0.81, doubled
by the bridge to **≈1.62 V/V (+4.2 dB)** across the speaker. IN− sits at the
amplifier's internal VDD/2 bias — a virtual ground for the network feeding it. The
inversion is irrelevant for a single speaker. Output is bridge-tied: neither speaker
pin is ground.

### Input network

From GPIO26 to IN−, in order: **4.7 kΩ** → node with **10 nF ("103") and 100 pF** to
ground → **0.39 µF** coupling capacitor → **20 kΩ** → IN−.

- **Series resistance 24.7 kΩ** — sets the gain with Rf; a light load for the DAC.
- **Low-pass pole at ≈4 kHz** — 10.1 nF against the ~3.8 kΩ Thevenin resistance at the
  node (4.7 kΩ from the DAC in parallel with the 20 kΩ path to the virtual ground). That
  is *inside* the audio band, unlike the sibling's 36 kHz pole: first-order treble
  roll-off, −3 dB at ~4 kHz, −6 dB an octave above. Telephone-ish, but it suppresses the
  DAC's sample-rate stair-steps far better than the sibling, so lower sample rates sound
  less different from high ones. The 100 pF is negligible beside the 10 nF. **If the
  "103" turns out to read "102" (1 nF)** on the board, the corner is ≈41 kHz and this
  paragraph reverts to the sibling's — worth a second look at the part.
- **High-pass at ≈16 Hz** (0.39 µF against 24.7 kΩ) — DC blocking, time constant
  **≈9.6 ms**, a third of the sibling's, so whatever pops get through are shorter.

Passband ≈16 Hz to ≈4 kHz (−3 dB), first-order above. Nothing above ~8 kHz survives at a
useful level, so sample rates above ~22 kHz buy nothing beyond the 19.6 kHz floor the
default DAC clock imposes.

### Headroom arithmetic

The DAC's full 3.3 Vpp swing × 1.62 ≈ **5.3 Vpp differential** across the speaker. A
bridge amplifier on ~4.6 V can swing roughly 7 Vpp before clipping (LM4871 curves), so
on USB the DAC runs out first: full-scale playback ≈ **0.44 W into 8 Ω / 0.9 W into
4 Ω**, below the amplifier's 1 % THD ceiling. On battery the margin goes away: at a
3.7 V cell the swing is ≈5 Vpp, about what full scale asks for; at 3.3 V it is below it
— the sibling's situation, where full scale was nevertheless found clean by ear. Hence
the topic file's advice: use the full 8 bits on USB, and on battery apply a software
ceiling (`setMaxSwing()`, ~±100 counts) if peaks sound rough — it could be tied to the
GPIO34 reading. The ±80-count ceiling once inferred for the sibling was A/B'd against
±127 on real music: no audible clipping, just more volume.

### Pop physics

The DAC pin idles at 0 V when disabled or never written. The first sample (nominally
128) is a half-scale step of 1.65 V, which the amplifier turns into a ~2.7 V thump
decaying with the network's 9.6 ms time constant; `dacDisable()` /
`dac_continuous_disable()` steps back to 0 the same way. On this board the amplifier
shutdown hides all of it — enable only after the DAC is parked at 128, disable before
the DAC changes state — and the 0 → 128 ramps over ~200 ms are belt-and-braces for any
case where the amplifier is on while the DAC changes. Through reset and bootloader
entry the pull-up holds the amplifier off, so those shouldn't pop either.

Amplifier off, idle noise is not a question. If it is left on between sounds, GPIO26
must be a known level (`dacWrite(26, 128)`), never a floating input, which picks up WiFi
and the display bus and hisses or ticks.

### DAC driver internals (ESP-IDF 5.x `dac_continuous`)

Findings from the sibling board (listened, 2026-09-18); the DAC side is identical here.

- **DMA descriptor alignment.** Each `dac_continuous_write()` becomes an I2S DMA
  descriptor. After the driver's 8→16-bit expansion, a length that isn't a multiple of
  4 bytes — any odd sample count, e.g. a 1-sample write — makes a non-word-aligned
  descriptor, the DMA stalls, the EOF interrupt never returns descriptors to the pool,
  and the next write blocks forever. Pass a finite timeout so a stall returns
  `ESP_ERR_TIMEOUT` instead of wedging the task.
- **The halving quirk.** `s_dac_wait_to_load_dma_data()` halves any write smaller than
  `buf_size` on alternate calls, so small writes leave descriptors half-empty and shrink
  the effective queue. Write exactly `buf_size` samples, padded with 128.
- **Queue depth.** 4 × 256 samples (~23 ms at 44.1 kHz) is thinner than an SD read
  stall; 12 × 2048 bytes (~190 ms after the halving quirk) ran an MP3 with zero
  underruns. The `on_stop` callback (`dac_continuous_register_event_callback`, handler
  `IRAM_ATTR`) fires whenever the chain runs dry — a free underrun counter, if you count
  only the stops you didn't cause with a deliberate drain.
- **The APLL static.** With `DAC_DIGI_CLK_SRC_APLL` the output carries random,
  full-scale spikes whenever the code is changing: static independent of the software
  volume (so *relatively* louder as you turn down), absent in digital silence, and
  different from one play of the same file to the next — a mis-latched code, not a
  data artefact. Dither made no difference; switching the clock source made it vanish.
  Same fingerprint as [arduino-esp32 #8467](https://github.com/espressif/arduino-esp32/issues/8467)
  on the legacy I2S driver, also cured by `use_apll = false`. The underrun counter
  stayed at zero throughout, which is how the static was told apart from underruns.
- **Clock floor.** On `DAC_DIGI_CLK_SRC_DEFAULT` the driver reaches down to **19.6 kHz**
  (`mclk_div` hits 255 below that); 8 / 11.025 / 16 kHz would need the APLL (648 Hz and
  up) and get the static with it. `MP3Player` upsamples by the smallest integer that
  clears 19.6 kHz (8 kHz ×3, 11.025 / 16 kHz ×2) with linear interpolation — more than
  good enough for an 8-bit DAC, and the ~4 kHz filter hides the interpolation artefacts.
- **Sample format.** The hardware path is 16 bits wide with only the high byte used; the
  driver expands 8-bit unsigned input to that by default. Signed 16-bit PCM needs
  `(s16 >> 8) + 128` after any volume scaling. Full scale is roughly 0–3.3 V
  (VDD3P3_RTC) in 256 steps: ~48 dB dynamic range, so at 25 % software volume you are
  playing 6-bit audio. `MP3Player` has TPDF dither with first-order noise shaping behind a
  checkbox; whether it helps quiet passages is untested.
- **`dac_cosine`** (`driver/dac_cosine.h`) generates a sine in hardware with a
  selectable **0 / −6 / −12 / −18 dB** attenuation — the only hardware gain anywhere in
  the chain, and only for that generator. It is the tool for the clipping test.
- **Cores and priorities.** WiFi and LwIP tasks are pinned to core 0 at priorities 23
  and 18 and preempt anything there in bursts. Without WiFi the audio task lives on
  core 0 (LVGL and `loop()` keep core 1); with WiFi it moves to core 1 at priority 3,
  above `loopTask` (1), where it sleeps in the DMA write most of the time and takes about
  a third of the core for 44.1 kHz libmad when it wakes. Either way the deep queue is
  what actually tolerates the other core's business.

### The GPIO25 conflict

`DAC_CHAN_0` is GPIO25, which this library uses as the XPT2046 clock. Anything that
enables both channels — `DAC_CHANNEL_MASK_ALL`, `dacWrite(25, …)`, the legacy I²S
`I2S_DAC_CHANNEL_BOTH_EN` behaviour some `AudioOutput` classes default to — takes GPIO25
away from the touch driver, silently. **ESP8266Audio 2.4.1's `AudioOutputInternalDAC`**
uses `DAC_CHANNEL_MASK_CH0` for mono and both for stereo, and drives GPIO25 low in its
constructor; touch dies the moment audio starts. Its decoders and file sources are fine.
One more decoder quirk: `AudioGeneratorMP3::loop()` only returns when the output's
`ConsumeSample()` refuses a sample, so an output that always accepts decodes the whole
file inside one call and starves the task — return `false` once per chunk written; the
decoder re-offers the refused sample next call.

---

## Card slot

GPIO5 / 18 / 19 / 23 are the classic ESP32's **VSPI IOMUX pins**, so the card runs at
full SPI speed without the GPIO matrix. **10 kΩ pull-ups** on CS, MOSI, MISO and the two
unused data lines (DAT1, DAT2, not routed to the ESP32) — exactly what the SD spec asks
of unused lines in SPI mode, so a card sitting in the slot before SPI is initialised
sees a properly idle bus. No pull-up on CLK (correct). GPIO5 is a strapping pin (SDIO
slave timing); harmless here, and its pull-up keeps CS high through reset. No
card-detect switch is routed.

**SDMMC mode is not an option** regardless of how D1–D3 are wired: the classic ESP32's
SDMMC controller can only use fixed IOMUX pins, and its slot 1 set (GPIO2 / 4 / 12 / 13 /
14 / 15) is the TFT's bus. `SD`, `SdFat` or an IDF `sdspi` mount only.

The card and the display each hold a general-purpose SPI host — VSPI (`SPI3_HOST`) and
HSPI (`SPI2_HOST`) respectively — so both of the classic ESP32's hosts are spoken for.
Touch is bit-banged and uses none. Anything else that wants hardware SPI joins VSPI
through the SPI header (IO23 / 19 / 18, CS on IO27) or bit-bangs.

---

## What a bench session should measure

In roughly the order of how much a sketch author would gain from the number:

1. **Current draw, both columns** of the table in
   [README-power.md](README-power.md#power-consumption) — USB power meter for the USB
   column, meter in series with the cell for the battery column. The deep-sleep row with
   the panel in Sleep In and the card removed is the one that settles whether the
   ~100–200 µA floor is real, and whether the CH340 dominates it.
2. **Cell voltage at which WiFi becomes unreliable, and at which the ESP32 browns out**,
   to replace the inferred 3.5–3.6 V / 3.2–3.3 V figures.
3. **Touch wake from deep sleep** — `ext0` on GPIO36: does it wake, and does a resting
   finger hold it awake?
4. **Backlight current** at 100 %, the largest single consumer.
5. **Speaker terminals on a scope** with `dac_cosine` at 0 dB, on USB and on a
   mid-charge cell, for the clipping point.
6. **Playback current** at a typical level, for the power table.
7. **Pops**: amplifier turn-on with the DAC parked at 128; reset and bootloader entry.
8. **Is the SC8002B shutdown a full mute?** — anything audible with the amplifier off and
   the DAC active.
9. **Read the "103" capacitor** in the amplifier input network; a "102" would move the
   4 kHz corner to 41 kHz.
