# Which board am I on? — what `display.begin()` detects

One sketch can run on every board this library supports. `display.begin()` works out
three things on its own, before it drives any pin that differs between boards, and
the sketch can ask for each one afterwards:

| Question | Call | Answers |
|---|---|---|
| How is the board wired? | `display.board()` | `CYDR_BOARD_OWN_TOUCH` or `CYDR_BOARD_SHARED_TOUCH` |
| Which screen chip, what size? | `display.panel()`, `display.panelName()`, `display.nativeWidth()` / `nativeHeight()` | ILI9341 / ST7789 (240×320) or ST7796 (320×480) |
| Which pin is the red LED on? | `display.redLedPin()` (or `cydr_redLedPin()`) | `4` or `22` |

| Board | Wiring | Panel | Red LED | GPIO4 is |
|---|---|---|---|---|
| ESP32-2432S028 | own touch | ILI9341 or ST7789, 240×320 | **4** | the red LED |
| ESP32-32E 2.8" | own touch | ILI9341 or ST7789, 240×320 | **22** | speaker amplifier enable |
| ESP32-32E 3.2" | shared touch | ST7789, 240×320 | **22** | speaker amplifier enable |
| ESP32-32E 3.5" / 4.0" | shared touch | ST7796, 320×480 | **22** | speaker amplifier enable |

The 2432S028 and the 32E 2.8" are wired alike for the display and touch, so wiring and
panel alone don't tell them apart. The red-LED check does.

Red on GPIO22 is confirmed on the 32E 2.8". On the 3.2", 3.5" and 4.0" it is what the
GPIO4 reading implies; **TODO (bench):** watch the LED on those three.

## Wiring: own touch or shared touch

Each board holds its backlight pin LOW with a resistor. The library turns on the
ESP32's internal pull-up on both candidate pins and sees which one stays LOW:

- **GPIO21 stays LOW → own touch.** Backlight on GPIO21; the touch chip has its own
  pins (CLK 25, MOSI 32, MISO 39, CS 33). 2432S028 and 32E 2.8".
- **GPIO27 stays LOW → shared touch.** Backlight on GPIO27; the touch chip shares the
  display's SPI bus (13 / 14 / 12, CS 33). 32E 3.2", 3.5" and 4.0".

Nothing is driven, so this is safe on every board. Pin details in
[README-board.md](README-board.md#which-pins-are-already-taken).

## Panel: asked by its ID

The display's data-out pin is GPIO12 on every board, and the library asks the panel
for its ID there. The ILI9341 and ST7796 answer their own ID questions, the ST7789
answers a different one. The panel alone sets the screen size. If no ID is
recognised, the library prints the bytes on Serial and guesses from the wiring
(ST7796 on shared touch, ILI9341 on own touch); `display.setPanel()` before
`begin()` skips detection. [Details](README-board.md#screen-variants).

## Red LED: GPIO4's voltage

On the 2432S028 GPIO4 is the red LED; on every ESP32-32E board it is the speaker
amplifier's enable and the red LED is on GPIO22. The library turns on GPIO4's
internal pull-down and reads the pin's **voltage**:

| Board | GPIO4 with the internal pull-down | Measured |
|---|---|---|
| ESP32-2432S028 | **1.64 V** | 1643 mV, 2026-10-13 |
| ESP32-32E 2.8" | **2.50 V** | earlier probe, 2026-10-13 |
| ESP32-32E 3.2" | **2.52 V** | 2520 mV, 2026-10-13 |
| ESP32-32E 3.5" | **2.49 V** | 2493–2495 mV, 2026-10-13 |
| ESP32-32E 4.0" | **2.51 V** | 2506–2521 mV, 2026-10-13 |

Below 2.0 V means the red LED is on GPIO4. One unit of each was measured.
On a shared-touch board the library doesn't measure at all: none of those is a
2432S028, so the answer is 22. [Why the two voltages differ](README-engineering.md#telling-gpio4s-two-jobs-apart).

```cpp
void setup() {
    lv_setup.begin();                       // or display.begin(): detection happens here
    int led = display.redLedPin();          // 4 or 22, active LOW on both
    pinMode(led, OUTPUT);
    digitalWrite(led, HIGH);                // off
    bool hasAmpSwitch = led != 4;           // GPIO4 is the amplifier enable on the 32E boards
    // ...only now start WiFi
}
```

Rules:

- **Start WiFi after `display.begin()`.** GPIO4 is on ADC2, which WiFi takes over on
  the classic ESP32. `begin()` measures once and remembers the answer, so later calls
  — with WiFi running or not — return the same value. If `cydr_redLedPin()` is called
  *before* `begin()` with WiFi already on, it can't measure and returns **-1**.
- **Don't drive GPIO4 before `begin()`.** The reading is of what the board does to the
  pin; a sketch that has already made it an output gets a meaningless answer.
- **It leaves GPIO4 as it found it** — an input with no pull — and gives the ADC back.
  On the 32E boards the board's own pull-up keeps the amplifier off; on the 2432S028
  the LED stays dark.
- `cydr_redLedProbeMillivolts()` returns the reading behind the answer (0 when none
  was taken), for a sketch that wants to log it.

## Capacitive boards fool these checks

This library is for **resistive** touch (the XPT2046) only. A capacitive CYD such as
the **ESP32-2432S032C** (GT911 touch on I²C) has the same GPIO27 backlight resistor as
the shared-touch boards, so `begin()` calls it shared touch and says the red LED is on
22. On the unit probed (2026-10-13) the red LED was really on GPIO4 (1620 mV), its
panel didn't answer an ID read on GPIO12 — so the library guessed ST7796 (320×480) —
and touch doesn't work. Use a capacitive-touch library for those.
