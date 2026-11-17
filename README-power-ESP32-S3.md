# ES3C28P (ESP32-S3) — power

How the black 2.8" ESP32-S3 board is powered and what a sketch can do about it. In
short, it runs from USB or from a single lithium cell, and charges the cell while
USB is connected. Nothing on it can turn the board off. It can deep-sleep, and a
touch or the BOOT button can wake it. The power path is the same design as the
ESP32-32E 2.8"'s. The classic boards are in [README-power-ESP32.md](README-power-ESP32.md),
and the parts and nets are in [schematic-description-ES3C28P.md](schematic-description-ES3C28P.md).

## How power reaches the board

USB 5 V comes in through a Schottky diode. With USB unplugged, a P-channel MOSFET
connects the battery in its place. Either way, the result is the board's 5 V rail,
which is really "USB minus a diode, or the battery". Two ME6217 regulators make 3.3 V
from it: one for the ESP32-S3, the panel and everything else, and one for the audio
codec alone. A TP4054 charges the battery from USB.
[Details](README-engineering.md#es3c28p-power).

## Power characteristics at a glance

### Power sources

**Can it be powered by USB?** **Yes**, from the USB-C port.

**Can it run on battery?** **Yes**: one lithium-ion / LiPo cell (3.7 V nominal,
4.2 V full) on the 2-pin **BAT** connector. The board switches between USB and
battery by itself.

**Is there battery protection?** **No.** There is no undervoltage cutoff: the board
keeps drawing from the cell until the regulator drops out, below about 3.4 V, and
then still draws a little. **Use a cell with its own protection circuit**, or it will
be over-discharged and ruined.

**Another input?** **Probably.** The UART connector's pin 1 is joined to the 5 V rail
through a MOSFET that conducts both ways (inferred from the schematic). It supplies a
device plugged in there, and 5 V fed in there probably powers the board.
**TODO (bench).**

**Can software tell which source is powering it?** **Not directly.** There is no USB
detect pin. The battery reading suggests it: with USB connected, the reading sits at
the charger's level.

### Power management chip and protection

**Is there a PMIC?** **No.** The parts are a TP4054 linear charger, a
diode-and-MOSFET switch between USB and battery, and two ME6217C33 regulators. None of
them connects to the ESP32-S3 except through the battery divider.

**Charge current?** About **300 mA** (set by a 3.3 kΩ resistor; inferred from the
TP4054's formula). Charging runs whenever USB is present, and the chip stops by
itself at 4.2 V. Software can't start, stop or watch it.

### Battery measurement

**How is the battery read?** **GPIO9** (ADC1, channel 8) sees half the battery
voltage, through a 200 k / 200 k divider with a 100 nF capacitor:

```cpp
float volts = analogReadMilliVolts(9) * 2 / 1000.0f;
```

Being ADC1, it works with Wi-Fi on. The divider draws about 10 µA from the cell all
the time.

**What does the reading mean?** About 4.2 V full and 3.4 V nearly empty. Under load
the reading sags; take it while the board is quiet for a fair comparison.

**With USB connected and no battery fitted**, the pin sees the charger's output. One
unit read **4.10 V** that way *(bench, 2026-11-09)*. **TODO (bench):** confirm that
no battery was fitted for that reading, and see whether it holds steady.

**Practical resolution?** About 10–20 mV of cell voltage after averaging a few
readings (inferred: the ESP32-S3's ADC with its factory calibration).

### Indicators

**Is there a power or charge light?** **No.** The charger's status pin isn't wired to
a light or to the ESP32-S3. The only light is the WS2812 on GPIO42, which software
controls.

### Shutting down, sleeping, waking

**Can the board shut itself off?** **No.** Nothing switches the rails. The closest it
gets is ESP32-S3 deep sleep, with both regulators and every peripheral still powered.

**Can it deep-sleep and wake on a timer?** **Yes**, the ESP32-S3's own timer.

**Which interrupts can wake it?** Any RTC GPIO (0–21) can wake it from deep sleep. On
this board that includes:

- **Touch**: the FT6336's INT on **GPIO17** goes LOW while the screen is touched
  (inferred from the datasheet; **TODO (bench)**).
- **BOOT** (GPIO0, LOW when pressed).
- The IO connector's GPIO2, 3, 14 and 21.

**Must anything be done at boot to stay on?** **No.**

### The power button

**Is there a power button?** **No.** There is no SW1 power key. RESET resets the chip
and the panel, whose reset is on the same line, but doesn't cut power. On battery the
only way to turn the board off is to unplug the cell. `display.powerKey()` reports
`CYDR_PWRKEY_NONE`.

### Other buttons

**Is BOOT usable as a button?** **Yes**: GPIO0, LOW when pressed, 10 k pull-up on the
board. Held while the board resets, it enters the bootloader.
[Board file](README-board-ESP32-S3.md#led-buttons-and-other-on-board-io).

**Is RESET software-visible?** **No**: it is on EN.

### Peripheral power switches

**What can software switch?**
- **The backlight**: GPIO45 drives a low-side MOSFET, active HIGH, with a pull-down
  that keeps it off during reset and in deep sleep unless the pin is held.
  `display.setBacklight(0)` turns it off.
- **The speaker amplifier**: GPIO1 is its SHUTDOWN pin, with a pull-up. It is **off
  unless a sketch drives GPIO1 LOW**, and goes back off in deep sleep unless the pin
  is held.

**What can't be switched off?** The panel itself (only its Sleep In command, 0x10),
the touch chip (its reset on GPIO18 can hold it, though), the codec and its own
regulator, the microphone, the card slot and the WS2812 (5 V; it draws a little even
when dark).

### Power consumption

| State | Current | How measured |
|---|---|---|
| Running, backlight full, Wi-Fi off, on USB | **TODO (bench)** | USB power meter |
| Running, backlight full, on battery | **TODO (bench)** | PPK2 in ampere mode on the BAT lead |
| Deep sleep, backlight off, panel in Sleep In, on battery | **TODO (bench)** | PPK2 |

The lower bound for deep sleep, inferred from datasheets, is about 200 µA. That comes
from two ME6217s at about 50 µA each, the WS2812 (often 0.5–1 mA dark), the
codec's and touch chip's idle currents, and the divider's 10 µA. In practice it is
probably around a milliamp, mostly from the WS2812. A 1000 mAh cell would last weeks
asleep, not months.

### Recommendations

- **`setup()`:** nothing power-related is required. `display.begin()` turns the
  backlight on, and the amplifier stays off until a sketch turns it on.
- **Always-on on USB:** no configuration.
- **On battery:** use a protected cell. Read `analogReadMilliVolts(9) * 2` now and
  then, and put the board to sleep yourself below about 3.5 V, since nothing else will
  stop the discharge.
- **For the lowest sleep current:** backlight off, the panel sent to Sleep In
  (command 0x10), the WS2812 written dark, the amplifier off (GPIO1 HIGH, or let the
  pull-up do it), then deep sleep with touch (GPIO17 LOW), BOOT (GPIO0 LOW) or a timer
  as the wake source.
