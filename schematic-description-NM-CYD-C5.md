# NM-CYD-C5 — the schematic, as understood

**Source.** `SCH_NM-CYD-C5-v1.0.pdf`, one A4 sheet ("P1") drawn in EasyEDA
(嘉立创EDA) and exported through jsPDF. Title block: NM-CYD-C5-v1.0, version V1.0,
created 2024-07-30, updated 2026-03-18. It comes from the vendor's GitHub package
(RockBase-iot/NM-CYD-C5, folder `Documention/2-sch`), which also holds a 3D PCB
view and a STEP model. Two units were checked against it (MACs `3c:dc:75:99:8d:70`,
multicolour silkscreen, and `38:44:be:27:02:88`, plain silkscreen); both match it,
except that neither has the AHT20 (U28) fitted ([§1.11](#111-temperature--humidity-sensor)).

**How it was read.** The PDF carries a bookmark outline that lists every net by name,
with one internal node ID (`$1N…`) per connection point, but no part or pin names.
So the **net names and which nets exist are exact**, and how many points each net
joins is known (the counts include net labels, so they aren't pin counts). Which pin
of which part sits on each net was **read by eye** from the sheet rendered at 300 and
400 dpi, as were all part values. Unnamed nets appear in the outline only as node IDs
and were traced on the drawing. Places where judgement was needed: Q1's orientation
([§4](#4-what-the-sheet-does-not-say)); which parts drawn in grey are fitted (read here
as not fitted: JP1, JP3 and the test points S1–S5).

**Conventions.** **TODO (schematic)** marks what the sheet doesn't settle, **TODO
(bench)** what only a test settles, **inferred** a conclusion from part numbers and
values, and *(bench)* a fact checked on the unit (2026-11-09). Pins are written
`Ref.pin`; net names in code font as printed; capacitor values as printed.

This file is the reference beneath [README-engineering.md](README-engineering.md#nm-cyd-c5-esp32-c5),
which interprets it, and beneath the NM-CYD-C5 topic files
([board](README-board-ESP32-C5.md), [power](README-power-ESP32-C5.md),
[sound](README-sound-ESP32-C5.md), [card](README-sdcard-ESP32-C5.md)). It is the one
place that tries to be complete rather than useful.

## 1. Bill of materials, by function

### 1.1 Processor

| Ref | Part | Role |
|---|---|---|
| U25 | ESP32-C5-WROOM-1-N16R8 | ESP32-C5, 16 MB flash, 8 MB PSRAM, PCB antenna (an IPEX "-Ant" edition exists) |
| C59 | 10 µF 16 V | `3.3V_ESP` decoupling at U25.2 |

U25 pins used: 3V3 (2) on `3.3V_ESP`; EN (3) on `RST`; RX0 (24) `U0RXD`; TX0 (25)
`U0TXD`; GND (1, 28–37). NC (19 "NC/IO15", 20, 22) unconnected. IO pins: see
[§2](#2-the-gpio-assignment).

### 1.2 USB-serial port (USB1)

| Ref | Part | Role |
|---|---|---|
| USB1 | USB-C 16-pin receptacle | Programming / serial port; VBUS on net `VBUS` |
| R22, R23 | 5.1 kΩ | CC1, CC2 to GND (sink) |
| R36, R37 | 22 Ω | D+, D− series to U6 (`USB_D+_AD` / `USB_D-_AD` → `USB_P` / `USB_N`) |
| U6 | CH340C | USB-UART bridge; VCC (16) and V3 on 3.3 V; TXD (2) → `U0RXD`, RXD (3) ← `U0TXD` |
| C14 | 100 nF 50 V | V3 decoupling |
| C13 | 100 nF 50 V | VCC decoupling |
| T1, T2 | SS8050 | Auto-reset: DTR / RTS cross-coupled through R12 / R14 (10 kΩ) to `BOOT0` and `RST` |
| R12, R14 | 10 kΩ | T1 / T2 base resistors |

U6's modem pins (CTS, DSR, RI, DCD, R232) are on single-point nets: unconnected.

### 1.3 Native USB port (USB3)

| Ref | Part | Role |
|---|---|---|
| USB3 | USB-C 16-pin receptacle | The C5's own USB (Serial/JTAG); VBUS on net `VBUS` |
| R28, R29 | 5.1 kΩ | CC to GND |
| R34, R35 | 22 Ω | D+ / D− series: `USB_D+` = IO14, `USB_D-` = IO13 (also FPC2.11 / .10) |

### 1.4 Power path

| Ref | Part | Role |
|---|---|---|
| D1 | 1N5819WS | Schottky, `VBUS` (both ports) → `5V` |
| U7 | AMS1117-3.3 | `5V` → `3.3V_ESP` (U25 only) |
| C15 / C16 / C17 | 10 µF 16 V / 10 µF 16 V / 100 nF 50 V | U7 input / output / output |
| U1 | AMS1117-3.3 | `5V` → `3.3V` (everything else) |
| C1 / C2 / C3 | 10 µF 16 V / 10 µF 16 V / 100 nF 50 V | U1 input / output / output |
| JP3, JP1 | 0 Ω, drawn grey | Links from `5V` around U7 / U1 to their outputs; read as not fitted |
| S1–S5 | test points, drawn grey | GND and rail test points |

### 1.5 Buttons

| Ref | Part | Role |
|---|---|---|
| BOOT | ZX-QC36-4.3TPWD tact switch | `BOOT0` (IO28) to GND |
| R2 | 10 kΩ | `BOOT0` pull-up to `3.3V_ESP` |
| C60 | 100 nF 50 V | across BOOT |
| RST | ZX-QC36-4.3TPWD tact switch | `RST` (EN) to GND |
| R1 | 10 kΩ | `RST` pull-up to `3.3V_ESP` |
| C5 | 100 nF 50 V | `RST` to GND (power-on reset delay) |

### 1.6 Display connector (FPC1)

| Ref | Part | Role |
|---|---|---|
| FPC1 | 18-pin TFT FPC, soldered | The panel's flex |
| Q2 | AO3400A N-MOSFET | Backlight low-side switch; gate on `TFT_BL` (IO25) |
| R10 | 10 kΩ | Q2 gate pull-down |
| R11 | 3.9 Ω | In series with the backlight LED |

FPC1 pins: 1 `TFT_RST` (= `RST`), 2 `TFT_SCK` (IO6), 3 `TFT_RS` (IO24, DC),
4 `TFT_CS` (IO23), 5 `TFT_SDI` (IO7), 6 `TFT_SDO` (IO2), 7–9 3.3 V and GND,
10–14 backlight anode / cathode (through Q2 and R11) and GND, 15–18 X+, Y−, X−, Y+
(to the touch controller). **TODO (schematic):** the exact assignment of pins 7–14;
they are drawn close together.

### 1.7 Touch controller

| Ref | Part | Role |
|---|---|---|
| U3 | XPT2046 | Resistive touch; DCLK `TP_CLK` (IO6), CS `TP_CS` (IO1), DIN `TP_DIN` (IO7), DOUT `TP_DOUT` (IO2), PENIRQ `TP_IRQ` (IO3); X+/Y+/X−/Y− to FPC1.15–18 |
| R3 | 10 kΩ | `TP_IRQ` pull-up to 3.3 V |
| C6 | 100 nF 50 V | VREF / supply decoupling |

U3's VBAT and AUX inputs: unconnected (inferred from the drawing).

### 1.8 TF / microSD

| Ref | Part | Role |
|---|---|---|
| TF | push-push microSD socket | CS / DAT3 `TF_CS` (IO10), CMD `TF_CMD` (IO7), CLK `TF_CLK` (IO6), DAT0 `TF_DATA0` (IO2) |
| RN2 | 4 × 10 kΩ array | Pull-ups to 3.3 V on the card's data and command lines (which four exactly: **TODO (schematic)**) |
| R13 | 10 kΩ | `TF_CS` pull-up |

The socket's card-detect contact is not connected to the C5.

### 1.9 RGB LED

| Ref | Part | Role |
|---|---|---|
| LED1 | WS2812C | VDD on `5V`; DIN from Q3 |
| Q3 | AO3400A | Bidirectional level shifter `IO27_2812DIN` (3.3 V side) → LED1.DIN (5 V side) |
| R25, R26, R27 | 10 kΩ | Q3's bias: pull-ups on its 3.3 V side and its 5 V side (R26 to `5V`) |
| C18 | 100 nF 50 V | LED1 decoupling |

### 1.10 Audio

| Ref | Part | Role |
|---|---|---|
| U5 | SC8002B | Mono bridge amplifier; VDD (6) 3.3 V; SHUTDOWN (1) to GND; BYPASS (2) and +IN (3) together, C8 to GND; −IN (4); VO1 (5) and VO2 (8) to SPEAK.1 / .2 |
| R4 | 4.7 kΩ | `SPEAK_IN` (IO26) series |
| C11 | 1 nF 50 V | after R4, to GND |
| R7 | 47 kΩ | second series stage |
| C12 | 100 pF | after R7, to GND |
| C10 | 330 nF | coupling |
| R8 | 22 kΩ | into −IN |
| R9 | 68 kΩ | VO1 → −IN feedback |
| C8 | 1 µF | BYPASS / +IN to GND |
| C7 | 1 µF | U5 VDD decoupling |
| SPEAK | HCZZ0015-2, 2-pin | speaker |

### 1.11 Temperature / humidity sensor

| Ref | Part | Role |
|---|---|---|
| U28 | AHT20 | I²C: SCL `TEMP_SCL` (IO8), SDA `TEMP_SDA` (IO9); VDD 3.3 V |
| R32, R33 | 4.7 kΩ | SCL / SDA pull-ups |
| C58 | 100 nF 50 V | decoupling |

**Not fitted** on either unit seen: the U28 footprint is empty, and nothing answers on
IO8 / IO9 in either pin order, though both lines read HIGH against the C5's internal
pull-down (R32 / R33 are fitted) *(bench)*.

### 1.12 Connectors

| Ref | Part | Pins |
|---|---|---|
| CN1 | HCZZ0001-4 | 1 3.3 V, 2 IO9 (with R18 10 kΩ pull-up to 3.3 V), 3 IO8, 4 GND |
| P1 | HCZZ0001-4 | 1 IO4, 2 IO8, 3 IO26, 4 GND |
| P5 | HCZZ0001-4 | 1 `5V` through Q1, 2 `LP_UART_RXD` (IO4) through R6 100 Ω, 3 `LP_UART_TXD` (IO5) through R5 100 Ω, 4 GND |
| Q1 | AO3401A P-MOSFET | between `5V` and P5.1, gate on GND |
| FPC2 | AFC01-S12FCA-00, 12-pin | 1 IO2, 2 IO6, 3 IO7, 4 IO10, 5 GND, 6 IO4, 7 IO8, 8 IO5, 9 IO9, 10 USB D−, 11 USB D+, 12 GND (13 / 14: shell, GND) |

## 2. The GPIO assignment

| GPIO | Net | Use | Notes |
|---|---|---|---|
| **0** | `IO0` | — | **free**; module pad only (one-point net) |
| 1 | `IO1` = `TP_CS` | touch CS | |
| 2 | `IO2` = `TP_DOUT` / `TFT_SDO` / `TF_DATA0` | SPI MISO | RN2 pull-up; FPC2.1; MTMS |
| 3 | `IO3` = `TP_IRQ` | touch PENIRQ | R3 pull-up; MTDI strapping (SDIO edge) |
| 4 | `IO4` = `LP_UART_RXD` | P5.2 (via R6), P1.1, FPC2.6 | **free**; LP UART RX |
| 5 | `IO5` = `LP_UART_TXD` | P5.3 (via R5), FPC2.8 | **free**; LP UART TX |
| 6 | `IO6` = `TP_CLK` / `TFT_SCK` / `TF_CLK` | SPI SCK | IO MUX FSPICLK; FPC2.2 |
| 7 | `IO7` = `TP_DIN` / `TFT_SDI` / `TF_CMD` | SPI MOSI | IO MUX FSPID; RN2 pull-up; JTAG-source strapping; FPC2.3 |
| 8 | `IO8` = `TEMP_SCL` | I²C SCL | R32; CN1.3, P1.2, FPC2.7 |
| 9 | `IO9` = `TEMP_SDA` | I²C SDA | R33, R18; CN1.2, FPC2.9 |
| 10 | `IO10` = `TF_CS` | card CS | R13 pull-up; IO MUX FSPICS0; FPC2.4 |
| 11 | `U0TXD` | UART0 TX → CH340 | |
| 12 | `U0RXD` | UART0 RX ← CH340 | |
| 13 | `IO13` = `USB_D-` | native USB D− | FPC2.10 |
| 14 | `IO14` = `USB_D+` | native USB D+ | FPC2.11 |
| 15–22 | — | module flash / PSRAM | not on the module's pins |
| 23 | `IO23` = `TFT_CS` | panel CS | |
| 24 | `IO24` = `TFT_RS` | panel DC | |
| 25 | `IO25` = `TFT_BL` | backlight (Q2) | R10 pull-down; SDIO-edge strapping |
| 26 | `IO26` = `SPEAK_IN` | speaker | P1.3; boot-mode strapping |
| 27 | `IO27` = `IO27_2812DIN` | WS2812 data (via Q3) | boot-mode and ROM-print strapping; held HIGH by Q3's pull-up |
| 28 | `BOOT0` | BOOT button | R2 pull-up; T1 auto-reset; boot-mode strapping |

**Strapping pins** (datasheet): GPIO26 / 27 / 28 boot mode (SPI boot needs GPIO28 HIGH;
download needs it LOW), GPIO27 ROM printing, GPIO25 and MTDI (GPIO3) the SDIO edges,
GPIO7 the JTAG source (no internal pull; held HIGH here by the card's pull-up). **LP GPIOs** — the
only deep-sleep wake pins — are GPIO0–6: on this board the touch IRQ (3) and the LP
UART (4 / 5) are among them; BOOT (28) is not. **ADC:** GPIO1–6 are ADC1 channels; none
is an analog input on this board.

## 3. Power rails

| Rail | Source | Switchable? | Feeds |
|---|---|---|---|
| `VBUS` | USB1 and USB3 VBUS, joined | no | D1 |
| `5V` | D1 | no | U7, U1, LED1, Q3's 5 V side, P5.1 (through Q1) |
| `3.3V_ESP` | U7 | no | U25, R1, R2 |
| `3.3V` | U1 | no | the panel, U3, the card, U6, U5, U28, CN1.1, the pull-ups |
| backlight | `3.3V` through R11, switched by Q2 | yes, IO25 | the panel's LED |

## 4. What the sheet does not say

- **Q1's orientation** (AO3401A between `5V` and P5.1, gate to GND): whether P5 can
  feed the board or only take power from it. **TODO (schematic / bench).**
- **FPC1 pins 7–14** exact assignment. **TODO (schematic).**
- **The panel module's** controller and options: the sheet shows only the connector.
  The fitted panel is an ST7789 *(bench)*; the vendor offers an ILI9341 too.
- **Speaker impedance** expected: not stated; an 8002-class amplifier implies 8 Ω.
- **U28**: drawn, but an empty footprint on both units seen *(bench)*.
- **Currents:** none; see [README-power-ESP32-C5.md](README-power-ESP32-C5.md#power-consumption).

## Appendix: named nets, from the PDF outline

Net, then the number of connection points the outline lists for it (net labels
included, so not a pin count):

`3.3V` 16 · `3.3V_ESP` 4 · `5V` 6 · `BOOT0` 5 · `GND` 55 · `IO0` 1 · `IO1` 4 · `IO2` 11 ·
`IO3` 4 · `IO4` 6 · `IO5` 5 · `IO6` 11 · `IO7` 11 · `IO8` 7 · `IO9` 6 · `IO10` 5 ·
`IO13` 5 · `IO14` 5 · `IO23` 4 · `IO24` 4 · `IO25` 4 · `IO26` 5 · `IO27` 4 · `RST` 6 ·
`U0RXD` 2 · `U0TXD` 2 · `USB_D-_AD` 2 · `USB_D+_AD` 2 · `VBUS` 4 · `X+` 2 · `X-` 2 ·
`Y+` 2 · `Y-` 2 · `CTS` 1 · `DCD` 1 · `DSR` 1 · `DTR` 1 · `RI` 1 · `RS232` 1 · `RTS` 1.

The one-point nets (`IO0`, the CH340's modem pins) are unconnected pins with a label.
`DTR` and `RTS` reach the auto-reset transistors under other labels. The 27 unnamed
nets (`$1N…`) are the local nodes inside the circuits above — the amplifier's input
network, the level shifter, the regulators' pins, the CC resistors — and appear in the
outline only as IDs.
