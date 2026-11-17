# NM-CYD-C5 (ESP32-C5) — power

How the NM-CYD-C5 is powered and what a sketch can do about it. In short: it runs
from USB only, nothing on it can be switched off except the backlight, and it can
deep-sleep but not turn itself off. The classic ESP32 boards are in
[README-power-ESP32.md](README-power-ESP32.md); the parts and nets are in
[schematic-description-NM-CYD-C5.md](schematic-description-NM-CYD-C5.md).

## How power reaches the board

USB 5 V from either USB-C port feeds the 5 V rail through a Schottky diode. Two
AMS1117-3.3 regulators make two 3.3 V rails from it: one for the ESP32-C5 module
alone, one for everything else (the panel, touch, the card, the CH340, the
amplifier). The WS2812 LED runs from 5 V. [Details](README-engineering.md#nm-cyd-c5-power).

## Power characteristics at a glance

### Power sources

**Can it be powered by USB?** **Yes**, from either USB-C port, and the whole board
runs from either one.

**Can it run on battery?** **No.** There is no battery connector, charger or
protection. Unplug USB and the board is off.

**Another input?** **Partly.** The P5 connector's pin 1 is the 5 V rail, behind a
P-channel MOSFET (Q1). It is meant to *supply* a GPS module; whether 5 V fed *into*
it powers the board is not settled. **TODO (schematic / bench):** Q1's orientation,
and whether the board runs from 5 V on P5. Software can't see which input is in use.

**Can software tell which source is powering it?** **No.** There is only USB.

### Power management chip and protection

**Is there a PMIC?** **No.** The power path is a Schottky diode (D1, 1N5819WS) from
both ports' VBUS (joined) to the 5 V rail, and two AMS1117-3.3 linear regulators
(U7 for the module's `3.3V_ESP`, U1 for the peripherals' `3.3V`). Nothing in it is
connected to the ESP32-C5.

### Indicators

**Is there a power light?** **No.** The only light is the WS2812 on GPIO27, which is
entirely under software control and off until a sketch writes to it.

### Shutting down, sleeping, waking

**Can the board shut itself off?** **No.** Nothing switches the rails. The closest it
gets is ESP32-C5 deep sleep, with both regulators, the CH340, the amplifier and the
LED still powered.

**Can it deep-sleep and wake on a timer?** **Yes**, the ESP32-C5's own timer wake.
The rails stay up.

**Which interrupts can wake it?** Only the C5's LP GPIOs (GPIO0–6) can wake it from
deep sleep. On this board that makes **touch** a deep-sleep wake source: the touch
IRQ is GPIO3, LOW while pressed (inferred from the pin list; **TODO (bench)**). P5's
UART receive line (GPIO4) is the LP UART's, so a GPS module's data could wake it too
(inferred). The **BOOT button (GPIO28) is not an LP GPIO** and can only wake the
board from light sleep.

**Does the touchscreen practically wake it?** **Probably.** The XPT2046 holds its IRQ
line LOW while the screen is pressed, as long as its last command left the IRQ
enabled — which `touch.begin()` and every touch read do. **TODO (bench).**

**Must anything be done at boot to stay on?** **No.**

### The power button

**Is there a power button?** **No.** RST resets the chip (and the panel, whose reset
is on the same line); it doesn't cut power. There is no hard-off: unplug USB.

### Other buttons

**Is BOOT usable as a button?** **Yes**: GPIO28, LOW when pressed, 10 k pull-up on
the board. Held while the board resets, it enters the bootloader. The CH340's
auto-reset transistors also pull it LOW when a serial program toggles DTR and RTS.
[Board file](README-board-ESP32-C5.md#led-buttons-and-other-on-board-io).

**Is RST software-visible?** **No** — it is on EN.

### Peripheral power switches

**What can software switch?** **Only the backlight**: GPIO25 drives the gate of a
low-side MOSFET (Q2), active HIGH, with a pull-down that keeps it off while the pin
floats (in reset, and in deep sleep unless the pin is held). `display.setBacklight(0)`
turns it off.

**What can't be switched off?** The panel itself (no power switch; its reset is on
EN, so it can only be sent to Sleep In by command), the XPT2046, the amplifier
(shutdown tied to GND), the CH340 (on the peripheral 3.3 V rail), the card slot and
the WS2812 (5 V; it draws a little even when dark).

### Power consumption

| State | Current from USB 5 V | How measured |
|---|---|---|
| Running, backlight full, Wi-Fi off | **TODO (bench)** | USB power meter |
| Running, backlight off | **TODO (bench)** | USB power meter |
| Deep sleep, backlight off, panel in Sleep In | **TODO (bench)** | USB power meter |

Lower bound for deep sleep, inferred from datasheets: two AMS1117s at about 5 mA
quiescent each, plus the CH340, the amplifier's idle current and the WS2812's — in
the tens of milliamps. This board is not built for sleeping on a battery.

### Recommendations

- **`setup()`:** nothing power-related is required. `display.begin()` turns the
  backlight on.
- **USB-only (the only kind):** no configuration; there is no battery to protect.
- **For the lowest sleep current:** backlight off, the panel sent to Sleep In
  (command 0x10), the WS2812 written dark, then deep sleep with touch (GPIO3, wake on
  LOW) or a timer as the wake source. Expect tens of milliamps regardless, from the
  regulators and the parts that can't be switched.
