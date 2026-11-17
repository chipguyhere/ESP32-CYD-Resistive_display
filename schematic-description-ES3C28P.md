# ES3C28P / ES3N28P (2.8" ESP32-S3) — the schematic, as understood

**Source.** `2.8inch_ESP32-S3_Display_Schematic.pdf` (and its Chinese-titled twin
`2.8inch_ESP32-S3显示模块硬件原理图.pdf`), from the vendor's download package, folder
`5-原理图_Schematic`. It is one A3 sheet, exported from **Altium Designer** (PDF
keywords "Altium", producer llPDFLib 3.x). The title block reads "2.8-inch ESP32-S3
display module Schematic", dated **2025/6/11**, with no revision. The sheet file name
is still `2.8inch_ESP32-32_Display_Schematic.SchDoc`: this sheet was derived from the
ESP32-32E 2.8" board's. The same folder has an IO allocation spreadsheet
(`ESP32-S3芯片IO资源分配表.xlsx`). It agrees with the sheet except for one line,
noted in §2. The vendor's terms for redistribution aren't stated, so the PDF is not
shipped here.

**How it was read.** Unlike the other Altium sheets in this family, this export
carries **no netlist in its outline**: the bookmarks hold only the sheet name. The
PDF's text layer has every visible label, part reference, value and pin name, but not
the connectivity. Connections were therefore **read by eye** from the sheet rendered
at 300 dpi, using the text layer to confirm spellings. Most connections are by net
label, so a misread wire is unlikely, but a value could be paired with the wrong part
where labels crowd. The I²S data lines were checked against the vendor's Arduino
music demo.

**Conventions.** **TODO (schematic)** marks what the sheet doesn't settle,
**TODO (bench)** what only a test settles, and **inferred** a conclusion drawn from
part numbers and values. Pins are `Ref.pin`. Net names are in code font as printed.
Capacitor codes are given as printed (`104` = 100 nF, `105` = 1 µF, `106` = 10 µF).

This file is the reference beneath [README-engineering.md](README-engineering.md#es3c28p-esp32-s3)
and the S3 topic files ([board](README-board-ESP32-S3.md), [power](README-power-ESP32-S3.md),
[sound](README-sound-ESP32-S3.md), [card](README-sdcard-ESP32-S3.md)). It tries to be
complete rather than useful.

## 1. Bill of materials, by function

### 1.1 Processor and memory

| Ref | Part | Role |
|---|---|---|
| U7 | ESP32-S3R8 (QFN56) | The processor, with 8 MB octal PSRAM in the package |
| W1 | 25VQ128 | 16 MB QSPI flash on `FSPI_CS/CLK/D/Q/WP/HD` (S3 pins 29–35), powered from `VDD_SPI` |
| X1 | 40 MHz crystal | With C62 / C63 22 pF |
| J1, R43 (0R), C52 / C53 (NC), L1 2 nH | SMD antenna feed | `LNA_IN` |
| C54 106, C55 105, C56 104, C57 104, C61 104, C64 10 nF, C65 1 µF, C59 104, C60 105, L2 24 nH | Supply decoupling | |

### 1.2 USB (USB2, Type-C)

| Ref | Part | Role |
|---|---|---|
| USB2 | Type-C receptacle | D− / D+ to S3 GPIO19 / GPIO20 (`USB-` / `USB+`) through R6 / R5 (0R) |
| R7, R8 | 5.1 k | CC1 / CC2 to GND: a USB-C sink |
| D5, D6, D7 | LESD8LH5.0CT5G | ESD on VBUS and the data lines |
| D8 | B5819W | VBUS → `+5` |

There is **no USB-serial chip**: the port is the S3's own USB Serial/JTAG.

### 1.3 Power path and charger

| Ref | Part | Role |
|---|---|---|
| D8 | B5819W Schottky | USB `VBUS` → `+5` |
| Q3 | SL2305 P-MOSFET | `BAT+` → `+5` when USB is absent; gate on `VBUS` with R13 100 k to GND (on when VBUS is low) |
| U2 | TP4054 | Linear Li-ion charger: VCC from `VBUS`, BAT to `BAT+`, PROG R12 3.3 k to GND (≈ 300 mA, inferred), CHRG via R11 10 k to GND (no LED) |
| C3 10 µF, C4 100 nF / C5 10 µF, C6 100 nF | | Charger input and output capacitors |
| JP1 | 2-pin connector | Pin 1 `BAT+`, pin 2 GND (the board's **BAT** connector; pin order **TODO (bench)**) |
| U3 | ME6217C33M5G | `+5` → `VCC3V3` (everything but audio); C7 104, C8 106 in, C9 105, C10 104 out |
| U4 | ME6217C33M5G | `+5` → `AU_VCC3V3` (the codec); C11 104, C12 106 in, C13 106, C14 104 out |

### 1.4 Battery measurement

R14 200 k from `BAT+` to `BAT_ADC`, R15 200 k from `BAT_ADC` to GND, C16 104 across
R15. `BAT_ADC` goes to S3 GPIO9, and C15 104 is at the pin.

### 1.5 Buttons

| Ref | Net | Notes |
|---|---|---|
| KEY1 | `RESET` (CHIP_PU) to GND | R9 10 k pull-up, C2 105 for the power-on delay. Also the panel's reset. |
| KEY2 | `IO0` to GND | R10 10 k pull-up. BOOT. |

### 1.6 Display (QD1, QD2833 18-pin FPC, welded)

| QD1 pin | Name | Net |
|---|---|---|
| 1 | GND | GND |
| 2 | RESET | `RESET` (EN) |
| 3 | SCL | `LCD_SCK` (GPIO12) |
| 4 | RS | `LCD_RS` (GPIO46) |
| 5 | CS | `LCD_CS` (GPIO10) |
| 6 | SDA | `LCD_MOSI` (GPIO11) |
| 7 | SDO | `LCD_MISO` (GPIO13) |
| 8 | GND | GND |
| 9 | VDD | `VCC3V3` (C50 104) |
| 10 | LEDA | `VCC3V3` |
| 11–14 | LEDK1–4 | `LEDK` |
| 15–18 | XL, YU, XR, YD | not connected (resistive-touch pins; the capacitive panel doesn't use them) |

The panel datasheet in the package is for the **ILI9341V**, and the module
specification is `QD2833-Specifications-A.pdf`.

### 1.7 Backlight

`LCD_BL` (GPIO45) → R31 0R → gate of Q4 (BSS138), with R32 10 k gate-to-GND. Q4's
drain → R33 5.6 Ω → `LEDK`. The low-side switch is active HIGH, and the pull-down
keeps it off while the pin floats.

### 1.8 Capacitive touch (P1, 6-pin FPC)

| P1 pin | Name | Net |
|---|---|---|
| 1 | VDD | `VCC3V3` |
| 2 | SCL | `CTP_SCL` = `IIC_SCL` = `AU_SCL` (GPIO15) |
| 3 | SDA | `CTP_SDA` = `IIC_SDA` = `AU_SDA` (GPIO16) |
| 4 | INT | `CTP_INT` (GPIO17) |
| 5 | RST | `CTP_RST` (GPIO18) |
| 6 | GND | GND |

R29 / R30 4.7 k pull-ups on SDA / SCL to `VCC3V3`. The sheet is annotated "IIC Slave
device address: 0x38". The controller (FT6336G, per the package's datasheet) is on
the touch panel's flex.

### 1.9 TF / microSD (SD_CARD1)

| Card pin | Name | Net | Pull-up |
|---|---|---|---|
| 1 | DATA2 | `SD_D2` (GPIO48) | R17 10 k |
| 2 | CD/DAT3 | `SD_D3` (GPIO47) | R34 10 k |
| 3 | CMD | `SD_CMD` (GPIO40) | R35 10 k |
| 4 | VDD | `VCC3V3` | C51 104 |
| 5 | CLK | `SD_CLK` (GPIO38) | R36 10 k |
| 6 | VSS | GND | |
| 7 | DATA0 | `SD_D0` (GPIO39) | R37 10 k |
| 8 | DATA1 | `SD_D1` (GPIO41) | R18 10 k |
| 9 | NC | — | |

### 1.10 Audio codec (U5, ES8311)

| U5 pin | Name | Net |
|---|---|---|
| 1 | CCLK | `AU_SCL` (GPIO15) |
| 2 | MCLK | `I2S_MCK` (GPIO4) |
| 3 | PVDD | `AU_VCC3V3` |
| 4 | DVDD | `AU_VCC3V3` |
| 5 | DGND | `AU_GND` |
| 6 | SCLK/DMIC_SCL | `I2S_SCK` (GPIO5) |
| 7 | ASDOUT | `I2S_DO` (GPIO6) |
| 8 | LRCK | `I2S_LRC` (GPIO7) |
| 9 | DSDIN | `I2S_DI` (GPIO8) |
| 10 | AGND | `AU_GND` |
| 11 | AVDD | `AU_VCC3V3` |
| 12 | OUTP | `OUTP` (via C25 1 µF) |
| 13 | OUTN | `OUTN` (via C26 1 µF) |
| 14 | DACVREF | C27 1 µF |
| 15 | ADCVREF | C28 1 µF |
| 16 | VMID | C29 1 µF |
| 17 | MIC1N | `MIC_N` via C30 1 µF and R19 0R |
| 18 | MIC1P/DMIC_SDA | `MIC_P` via C31 1 µF and R20 0R |
| 19 | CDATA | `AU_SDA` (GPIO16) |
| 20 | CE | `AU_GND` (I²C address 0x18; the sheet's annotation is cut off at "0x1…") |
| 21 | EP | `AU_GND` |

`AU_GND` joins GND through R16 0R. Note that the net names `I2S_DO` / `I2S_DI` are
from the codec's side: `I2S_DO` is the codec's ADC output, read on S3 GPIO6, and
`I2S_DI` is the codec's DAC input, driven from S3 GPIO8.

### 1.11 Microphone (MIC1, LMA2718B381-OA7)

Pin 1 OUT → `MIC_OUT`, pin 3 VDD ← `MIC_VDD`, pins 2 / 4 `AU_GND`. `MIC_VDD` comes from
`AU_VCC3V3` through L3 (0R) with C32 100 nF / C33 10 µF. `MIC_OUT` couples into
`MIC_P` (C37 106; C34–C40 10 pF / 30 pF / 33 pF RF filtering on `MIC_P` / `MIC_N`).
**TODO (schematic):** the exact coupling of `MIC_OUT` / `MIC_N`, which is crowded on
the sheet.

### 1.12 Speaker amplifier (U6, SC8002B)

| U6 pin | Name | Net |
|---|---|---|
| 1 | SHUTDOWN | `AUDIO_EN` (GPIO1), R26 10 k pull-up to `VCC3V3`: shut down unless driven LOW |
| 2 | BYPASS | C41 1 µF to `AP_GND` |
| 3 | +IN | `OUTP` via R21 0R, C44 0.39 µF, R24 20 k |
| 4 | −IN | `OUTN` via R22 0R, C45 0.39 µF, R25 20 k; R28 20 k feedback to VO1 |
| 5 | VO1 | `SP-` |
| 6 | VDD | `+5` (C48 1 µF, C49 10 µF) |
| 7 | GND | `AP_GND` (R27 0R to GND) |
| 8 | VO2 | `SP+` |

C42 / C46 103 and C43 / C47 100 pF filter `OUTP` / `OUTN` to `AP_GND`. JP3 is the
**SPEAKER** connector, pin 2 `SP+` and pin 1 `SP-`. The gain is 2 × R28 / R25 = 2
(6 dB), bridge-tied (inferred). The package's datasheet folder has the FM8002E, a
pin-compatible part. **TODO (bench):** which one is fitted.

### 1.13 RGB LED (LED2, XL-5050RGBC-WS2812B)

VDD `+5`, GND, DIN ← `RGB_LED` (GPIO42) directly, with no level shifter. DOUT is not
connected.

### 1.14 Connectors

| Ref | Pin 1 | Pin 2 | Pin 3 | Pin 4 | Notes |
|---|---|---|---|---|---|
| P2 (UART) | Q5 drain | GND | `TXD0` (GPIO43) via R39 100 Ω and R44 499 Ω | `RXD0` (GPIO44) via R38 100 Ω | Q5 SL2305: source `+5`, gate R40 10 k to GND, so always on. Pin 1 is `+5` through it, both ways (body diode and channel). |
| P3 (IO) | `IO2` | `IO3` | `IO14` | `IO21` | No power pin |
| P4 (IIC) | `VCC3V3` | GND | `IIC_SCL` (GPIO15) | `IIC_SDA` (GPIO16) | |

R44 499 Ω sits in series on `TXD0` at the S3 pin. All three are 4-pin + 2 mounting
pads (5, 6 to GND).

## 2. The GPIO assignment

| GPIO | Net | Use | Notes |
|---|---|---|---|
| 0 | `IO0` | BOOT (KEY2) | strapping |
| 1 | `AUDIO_EN` | amplifier SHUTDOWN | 10 k pull-up |
| **2** | `IO2` | P3 | **free** |
| **3** | `IO3` | P3 | **free**; strapping (JTAG source) |
| 4 | `I2S_MCK` | codec MCLK | |
| 5 | `I2S_SCK` | codec BCLK | |
| 6 | `I2S_DO` | codec ADC data → S3 | the spreadsheet calls this "I2S output"; wrong |
| 7 | `I2S_LRC` | codec LRCK | |
| 8 | `I2S_DI` | S3 → codec DAC data | |
| 9 | `BAT_ADC` | battery / 2 | ADC1_CH8 |
| 10 | `LCD_CS` | panel CS | FSPICS0 (IO_MUX) |
| 11 | `LCD_MOSI` | panel SDA | FSPID |
| 12 | `LCD_SCK` | panel SCL | FSPICLK |
| 13 | `LCD_MISO` | panel SDO | FSPIQ |
| **14** | `IO14` | P3 | **free** |
| 15 | `IIC_SCL` / `CTP_SCL` / `AU_SCL` | I²C SCL | 4.7 k pull-up |
| 16 | `IIC_SDA` / `CTP_SDA` / `AU_SDA` | I²C SDA | 4.7 k pull-up |
| 17 | `CTP_INT` | touch INT | |
| 18 | `CTP_RST` | touch RST | |
| 19, 20 | `USB-`, `USB+` | USB | |
| **21** | `IO21` | P3 | **free** |
| 26–37 | flash / PSRAM | — | not usable |
| 38 | `SD_CLK` | card | |
| 39 | `SD_D0` | card | |
| 40 | `SD_CMD` | card | |
| 41 | `SD_D1` | card | |
| 42 | `RGB_LED` | WS2812 | |
| **43** | `TXD0` | P2 | free while `Serial` is on USB |
| **44** | `RXD0` | P2 | free while `Serial` is on USB |
| 45 | `LCD_BL` | backlight | strapping (VDD_SPI voltage); 10 k pull-down |
| 46 | `LCD_RS` | panel DC | strapping (boot messages) |
| 47 | `SD_D3` | card | |
| 48 | `SD_D2` | card | |

Strapping pins are GPIO0, 3, 45 and 46. ADC1 covers GPIO1–10 and ADC2 covers
GPIO11–20, so the battery (GPIO9) reads with Wi-Fi on. RTC GPIOs, which can wake the
chip from deep sleep, are 0–21: they include BOOT, the touch INT (17) and the four IO
connector pins.

## 3. Power rails

| Rail | Source | Switchable? | Feeds |
|---|---|---|---|
| `VBUS` | USB-C | no | D8, the charger, Q3's gate |
| `BAT+` | the cell on JP1 | no | Q3, the charger, the battery divider |
| `+5` | VBUS − D8, or BAT+ through Q3 | no | U3, U4, the amplifier, the WS2812, P2 pin 1 |
| `VCC3V3` | U3 ME6217C33 | no | the S3, flash, panel, backlight LEDs, touch, card, I²C pull-ups, P4 |
| `AU_VCC3V3` | U4 ME6217C33 | no | the codec and microphone |
| `VDD_SPI` | the S3 | (internal) | the flash |
| `LEDK` | panel backlight cathodes | GPIO45 via Q4 | |

## 4. What the sheet does not say

- The board's silkscreen revision; neither unit seen was compared with the sheet in
  detail. **TODO (bench).**
- Which end of each connector is pin 1 on the board. **TODO (bench).**
- Whether the amplifier fitted is the SC8002B drawn or the FM8002E whose datasheet
  ships in the package. **TODO (bench).**
- The ES3N28P (no touch): the specification covers both, so it is presumably this
  sheet with P1 unpopulated or the panel without a touch layer (inferred).
- Observed 2026-11-09 on two units: the panel answers ILI9341 ID `00 93 41` on
  GPIO13, the FT6336 answers at 0x38, and the ES8311 answers at 0x18 and plays.
