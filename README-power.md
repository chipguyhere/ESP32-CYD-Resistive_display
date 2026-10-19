# Resistive-touch CYD boards — power (ESP32-32E 2.8")

How this board is powered and what a sketch can do about it, for a developer trying to
run on battery as long as possible. There is no `chipguy_*_power` library for this board
— its charger has no connection to the ESP32 — so this file lives with the display
library and answers the same questionnaire every power README in the family answers.
A "No" here is a fact about the board, not an omission. The electronics behind every
figure — the schematic, the part datasheets, the arithmetic — are in
[README-engineering.md](README-engineering.md#power).

> **Which board.** Everything in this file is the **ESP32-32E 2.8"**. The
> **ESP32-2432S028** has no battery connector and no power management: it runs from USB
> or 5 V on its VIN pin, nothing on it can be powered down except the backlight, and even
> deep sleep draws several mA. The **3.2"**, **3.5"** and **4.0"** boards each have a
> battery connector and read the cell on **GPIO34** exactly as the 2.8" does — one cell
> read 2.07 V on each, 2.08–2.12 V with none fitted (2026-10-18). The rest of their power
> section hasn't been examined — **TODO (bench)**
> ([Other boards](README-board.md#other-boards)).
>
> The **ESP32-024** has a battery connector, but its power section is a different design:
> an **FM5324GA** "power bank" chip — lithium charger and 5 V booster in one — whose
> output feeds two AMS1117 3.3 V regulators, one for the ESP32 and one for the screen
> side. There is no battery reading: the cell never reaches an ESP32 pin, and GPIO34 is
> the light sensor. **SW1** is the FM5324GA's key and isn't wired to the ESP32
> (schematic; pressing it changed no GPIO). Observed on battery alone, 2026-10-18:
>
> - Plugging a cell in does **not** start the board; **one press of SW1** does.
> - Another single press, or holding it (2–10 s), does nothing.
> - **Two quick presses turn the board off.**
>
> So a person can switch it on and off, but a sketch can neither do that nor see the
> button. **TODO (bench):** whether the FM5324GA also switches itself off when the
> board draws little (power-bank chips often do, which would end a deep sleep for good),
> the deep-sleep current, and charging behavior. The chip's three level-LED outputs are
> unconnected, so there is no charge light.

**The short version.** This is the ESP32-2432S028 CYD with a battery added, and added
properly. A 2-pin connector takes a single lithium cell; a **TP4054** charger fills it
whenever USB is present; a load-sharing circuit hands the board between USB and the
cell without a glitch; and the cell voltage, halved, comes to **GPIO34** so a sketch can
read it. The 3.3 V regulator is a low-dropout type, so the board runs across nearly the
whole discharge curve and its deep-sleep floor is in the **~100–200 µA** class rather
than the sibling's milliamps. The charger is autonomous — nothing about it can be read
or set — there is no power switch and no way to shut off, and there is no charge light.
The audio amplifier is off until you turn it on.

Every figure below is **inferred** from the schematic and datasheets — nothing has been
measured on this board yet. `**TODO (bench):**` marks what a test would settle.

## How power reaches the board

USB 5 V (or 5 V on the UART header's 5V pin, which behaves identically — see
[README-board.md](README-board.md#connectors)) feeds the charger and, through a diode,
the board's main rail. When USB is absent a transistor connects the cell to that rail
instead; when USB returns it hands back without a brown-out. The main rail is ~4.6 V on
USB and the cell voltage (3.0–4.2 V) on battery; a low-dropout regulator makes 3.3 V
from it for the ESP32 and everything else. The audio amplifier runs from the main rail,
the USB serial chip from 3.3 V — so both are powered on battery too.
[Details](README-engineering.md#the-nets).

---

## Power characteristics at a glance

### Power sources

**Can this board be powered by USB?** Yes, entirely. USB runs the whole board — ESP32,
display, backlight, touch, amplifier, card slot, USB serial — and charges the battery
if one is fitted. Note that some USB-C cables won't power it at all; see
[README-board.md](README-board.md#which-boards-these-are).

**Can this board run on battery power?** Yes — a **single lithium cell** (3.0–4.2 V) on
the 2-pin connector ([polarity](README-board.md#connectors)). Handover between USB and
cell is automatic in both directions. The regulator keeps 3.3 V up until the cell is
around **3.5–3.6 V**, and the ESP32 keeps running below that to roughly a **3.2–3.3 V**
cell (inferred) — usable to the cell's floor; this is a real battery board.

**TODO (bench):** find the cell voltage at which (a) WiFi becomes unreliable and (b) the
ESP32 browns out, to replace the inferred figures.

**Can the board recognize power arriving from an input that is neither the battery nor
USB?** No. The UART header's 5V pin is electrically the same as USB. There is no DC jack
and no PoE.

**Can software tell which source is currently powering the board?** Partly, and only
indirectly. No pin sees USB, and the charger's status pin doesn't reach the ESP32. What
the sketch *can* see is the battery reading on GPIO34: a cell being charged sits at or is
pulled toward 4.2 V, so **a reading above ~4.15 V means USB is almost certainly
present**. It is a heuristic, not a signal — a full cell freshly unplugged from USB
also reads ~4.2 V for a while.

**Can we ask the hardware to stay powered only while on external power?** No. The
opposite half of that wish is free: while USB is present the cell is out of the
circuit and only charges. Nothing stops it taking over when USB goes away.

### Power management chip and protection

**Does this board have a PMIC?** No. The power path is discrete — a **TP4054** lithium
charger, a Schottky diode and a P-MOSFET for load sharing, and an **ME6217C33**
low-dropout 3.3 V regulator — and none of it is configurable or readable.
[Parts and reasoning](README-engineering.md#power).

**Does the board automatically prevent a lithium cell from discharging below an
absolute minimum?** No — not on the board. The charger is a charger only. Whatever
undervoltage cutoff exists is on the cell's own protection PCB, if it has one. **Use a
protected cell.** It matters more here than on a board with a worse regulator: this one
keeps the ESP32 running down to roughly a 3.2 V cell, which is *below* where an
unprotected cell should ever go. The graceful floor is entirely the sketch's — read
GPIO34 and sleep or stop before the cell gets low. The ESP32's own brown-out, when it
finally comes, is an abrupt reset, not a shutdown.

**Can software decide whether the battery charges at all, or set a charge level or
voltage?** No. The charger runs whenever USB is present, at about **300 mA** (inferred),
terminating at **4.2 V**, restarting on its own when the cell drifts down to ~4.05 V.
Board draw plus charge current can exceed what a 500 mA USB 2.0 port supplies; charge
from a 1 A-or-better source. A weak port just slows the charge — nothing breaks.

### Battery measurement

**Can we read the battery level?** Yes, as a **voltage**: the cell is divided by two
into **GPIO34** (ADC1, input-only). Starting point:

```cpp
// Battery voltage in millivolts.  ADC1 (works alongside WiFi; ADC2 doesn't).
analogSetPinAttenuation(34, ADC_11db);          // once, in setup()
uint32_t mv = 0;
for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(34);
mv = mv / 16 * 2;                                // undo the divide-by-two
```

Use `analogReadMilliVolts()`, not a scaled `analogRead()` — the former is calibrated,
the latter can be 10 % off on a classic ESP32.

**What is the practical resolution of that reading?** Roughly **±50 mV at the cell**
(inferred). The divider is filtered on the board, so readings don't droop and don't
need a "discard the first sample" ritual; averaging 16 is still worthwhile. Good for a
battery icon and a low-battery cutoff. Not good for state-of-charge better than ~10 %
(the lithium curve is nearly flat between 3.9 and 3.6 V), and not coulomb counting.
[Why those numbers](README-engineering.md#battery-divider-on-gpio34).

**Can we tell whether charging is active?** No — the charger's status pin is grounded,
not connected to anything. **Can we tell whether charging voltage is available?** No,
not directly. Both only by the ~4.15 V heuristic under
[power sources](#power-sources).

**Is this board capable of detecting whether a battery is present?** No, and here is
the consequence: with **no cell fitted and USB present, GPIO34 reads ~4.2 V — a
convincing "full" that is a lie.** It is indistinguishable from a genuinely full cell.
A sketch that must know can look for the *behaviour* of a cell (a reading that sags
under a WiFi burst and recovers); nothing on the board answers directly.

**Does the reading mean the same thing on USB and on battery?** No. On USB the cell is
being charged and reads high — up to 4.2 V. A state-of-charge estimate is only
meaningful on battery, and only after the cell has rested a minute or so from a charge
or a heavy load.

### Indicators

**Does the board have a power light?** No. There is no visual sign the board is powered
until the backlight comes up — and on battery, a sketch that sleeps with the backlight
off looks exactly like a board that is off.

**Does the board have a battery / charge light?** No. Nothing tells you the cell is
charging or charged; unplug it after a couple of hours, or watch GPIO34 from the
sketch. The RGB LED is a general-purpose software LED, not a status indicator — pins,
polarity and the reset-glow caveat are in [README-board.md](README-board.md#led-buttons-and-other-on-board-io).

### Shutting down, sleeping, waking

**Can this board shut itself off?** No. There is no load switch, no latch and no PMIC.
The lowest state is ESP32 **deep sleep with the rail still up**, which on this board is
a genuinely low state — see [power consumption](#power-consumption).

**Can it still shut itself off when USB is present?** Not applicable — it can't shut off
at all.

**Can the board be powered down but wake on a timer?** Only as deep sleep with the rail
up; the RTC timer wakes it normally.

**Which interrupts can wake it?** The ESP32's normal RTC-capable pins, several of which
are usefully wired:

| Wake source | GPIO | How | Notes |
|---|---|---|---|
| Touch panel pen-down | 36 | `ext0`, wake on LOW | The one genuinely good wake source. The library leaves the touch controller in the mode that drives this line. |
| BOOT button | 0 | `ext0`, wake on LOW | Also the strapping pin — holding it through a reset enters the bootloader. |
| Header IO35 | 35 | `ext0` | Needs an external pull-up for a switch-to-GND. |
| SPI header IO27 | 27 | `ext0` / `ext1` / touch pad | Free when no SPI device is on the header; a normal GPIO with internal pull-ups, so a switch-to-GND needs nothing extra. Also the one pin here that can wake on a capacitive touch pad. |
| RTC timer | — | normal | |

Battery sense on GPIO34 is RTC-capable but a wake-on-low-battery would need the ULP;
`ext0` on a digital threshold is unreliable. None of the peripherals can be powered off
independently and all keep running while the CPU sleeps.

**Does the board have an IMU that can wake it on motion?** No IMU.

**Does the board have a touchscreen that can practically wake the device?** Yes, on
paper: `esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0)` before `esp_deep_sleep_start()`
should wake on a touch. **TODO (bench):** verify it wakes, and that a resting finger
doesn't hold it awake.

**When we boot, is there an action that must be taken to prevent the device from
immediately powering down?** No. Nothing needs latching.

### The power button

**Does the board have a power button?** No, and nothing in the power path for one to
control. If you make your own battery pigtail, a switch in the BAT+ lead is the one
place an "off" can go, and it is invisible to software. Without one there is no way to
turn the board off short of unplugging both USB and the cell. **Getting out of a hang**
means the RST button, which restarts the chip but does not remove power.

### Other buttons

**Does this device expose the BOOT button to the user?** Yes — GPIO0, pulled up, a
normal tactile button on the board edge, usable as a UI button and as a wake source.
The consequence of it being the strapping pin: a user who holds "wake" while pressing
RST gets a dead-looking board (the bootloader) until the next reset.

**Is there a reset button, and is it software-visible?** Yes, RST; not software-visible.

### Peripheral power switches

**What parts of the board have power switches we can control?** Two:

| Rail / part | Control | Default | Notes |
|---|---|---|---|
| **Backlight** | GPIO21 via `display.setBacklight(0..100)` | on at 100 % after `lv_setup.begin()` | The largest consumer on the board (**TODO (bench):** how much), so the knob that matters most. |
| **Audio amplifier** | **GPIO4, LOW = on** | **off** — a pull-up holds it off until a sketch drives GPIO4 LOW | Costs nothing when off, a few mA plus speaker power when on. Sequencing is in [README-sound.md](README-sound.md). |

**Which peripherals cannot be powered off?** Everything else:

| Peripheral | What you can do instead |
|---|---|
| Display panel logic | Nothing yet — the panel has a sleep command that this library doesn't expose. An opportunity for a `display.sleep()` / `wake()` pair. |
| Touch controller | Nothing needed — it idles at µA between reads, and it's the wake source you want anyway. |
| USB serial chip | Nothing. It is on 3.3 V, so it is powered on battery and its idle draw is part of the floor — possibly the largest single item. **TODO (bench):** measure. |
| Regulator, charger, load-sharing parts, battery divider | Nothing needed — each is µA-class on battery. |
| RGB LED | Drive all three pins HIGH. |
| microSD slot | Remove the card — an idle card can draw from hundreds of µA to a few mA. |

### Power consumption

Nothing measured yet; the table exists so the numbers have somewhere to land. Measure
with a USB power meter for the USB column and a meter in series with the cell for the
battery column — the two differ (on USB the charger may be charging).

| State | From USB, 5 V | From the cell | How measured |
|---|---|---|---|
| Awake, backlight 100 %, WiFi idle | **TODO (bench)** | **TODO (bench)** | |
| Awake, backlight 100 %, WiFi transmitting | **TODO (bench)** | **TODO (bench)** | |
| Awake, backlight 0 %, amp off | **TODO (bench)** | **TODO (bench)** | |
| Awake, playing audio at a typical level | **TODO (bench)** | **TODO (bench)** | |
| Light sleep, backlight 0 % | **TODO (bench)** | **TODO (bench)** | |
| Deep sleep, backlight 0 %, panel not slept, card fitted | **TODO (bench)** | **TODO (bench)** | |
| Deep sleep, backlight 0 %, card removed, RGB off, amp off | **TODO (bench)** | **TODO (bench)** | |
| "Off" (rail cut) | **no such state** | **no such state** | |

**Lower bound, inferred:** on battery the deep-sleep floor is **on the order of
100–200 µA** before whatever the display logic and an idle card add — a
months-of-deep-sleep number on a 1000 mAh cell, and fifty to a hundred times better
than the non-battery sibling. Which parts contribute what is itemized in
[README-engineering.md](README-engineering.md#deep-sleep-floor-by-part-inferred).
Runtime figures will be computed once the table is filled in.

### Recommendations

**What should `setup()` do?** For a sketch that cares about current:

1. `pinMode` / `digitalWrite` **GPIO22, 16, 17 HIGH** first, before anything else, to stop
   the RGB LED glowing.
2. Leave **GPIO4 alone** (or drive it HIGH) unless the sketch makes sound — the
   amplifier is off by default and should stay that way.
3. `lv_setup.begin()`, then `display.setBacklight(n)` to the lowest brightness the
   application tolerates.
4. `analogSetPinAttenuation(34, ADC_11db)` if the sketch reads the battery.
5. Don't fit an SD card unless the sketch uses it.

**Always-on scenario with a battery we don't need:** nothing can be configured — the
charger holds the cell at 4.2 V while USB stays connected and tops it up each time it
drifts to ~4.05 V. A cell held at 4.2 V continuously ages faster than one cycled
gently. If the board lives on USB, the honest answer is **remove the cell**; there is no
"keep it at 80 %" setting.

**Battery fitted but we choose not to run on it:** not possible — with USB absent the
board runs from the cell until the cell says otherwise. While USB *is* present this is
already the behaviour. The sketch can only make the on-battery period short: when the
GPIO34 reading drops below ~4.15 V (USB gone), turn the backlight off and deep sleep
with a touch wake.

**Battery is the primary source:**

- **Low-battery floor:** **3.3–3.4 V**, the usual figure for a protected cell — the
  cell's health, not the electronics, sets the floor here. (If a reset loop appears
  during WiFi bring-up at a higher cell voltage, raise the floor to that; **TODO
  (bench)**.) Below the floor: backlight off, amp off, deep sleep with a timer wake to
  re-check; there is no "off" to reach for.
- Sleep as much as possible: **deep sleep with `ext0` wake on GPIO36 (touch)** and/or
  GPIO0 (BOOT), plus a timer if the app needs periodic checks. `setBacklight(0)` before
  sleeping.
- If WiFi is used, reduce transmit power (`WiFi.setTxPower(...)`) to soften the bursts
  that are the first thing to brown out on a sagging supply.
- Read the battery through the averaged `analogReadMilliVolts()` snippet above, and
  only trust a state-of-charge estimate when the reading is below ~4.15 V (i.e. on
  battery) and the load has been steady for a while.

The library itself does nothing for any of this beyond `setBacklight()`. Sleep, wake,
the battery reading and the amplifier pin are all the sketch's.
