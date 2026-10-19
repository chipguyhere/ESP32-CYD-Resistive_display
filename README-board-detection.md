# Which board am I on? — what `display.begin()` detects

One sketch can run on every board this library supports. `display.begin()` works out
three things on its own, before it drives any pin that differs between boards, and
the sketch can ask for each one afterwards. Together they name the board:

| Question | Call | Answers |
|---|---|---|
| How is the board wired? | `display.board()` | `CYDR_BOARD_OWN_TOUCH` or `CYDR_BOARD_SHARED_TOUCH` |
| Which screen chip, what size? | `display.panel()`, `display.panelName()`, `display.nativeWidth()` / `nativeHeight()` | ILI9341 / ST7789 (240×320) or ST7796 (320×480) |
| Which pin is the red LED on? | `display.redLedPin()` (or `cydr_redLedPin()`) | `4` or `22` |
| …and green and blue? | `display.greenLedPin()`, `display.blueLedPin()` (or `cydr_greenLedPin()`, `cydr_blueLedPin()`) | `16` / `17`, or `17` / `16` on the ESP32-024 |
| So which board is it? | `display.model()`, `display.modelName()` (or `cydr_activeModel()`) | one of the rows below |

| Board | `model()` | Wiring | Panel | Red LED | GPIO4 is |
|---|---|---|---|---|---|
| ESP32-2432S028 | `CYDR_MODEL_2432S028` | own touch | ILI9341 or ST7789, 240×320 | **4** | the red LED |
| ESP32-32E 2.8" | `CYDR_MODEL_32E_28` | own touch | ILI9341 or ST7789, 240×320 | **22** | speaker amplifier enable |
| ESP32-32E 3.2" | `CYDR_MODEL_32E_32` | shared touch | ST7789, 240×320 | **22** | speaker amplifier enable |
| ESP32-32E 3.5" / 4.0" | `CYDR_MODEL_32E_35_40` | shared touch | ST7796, 320×480 | **22** | speaker amplifier enable |
| ESP32-024 2.4" | `CYDR_MODEL_024` | shared touch | ST7789, 240×320 | **4** | the red LED |

Two pairs of boards look alike to the first two checks: the 2432S028 and the 32E 2.8"
(same wiring and panels), and the 32E 3.2" and the ESP32-024 (same wiring, same
panel). The red-LED check splits both pairs, so every board in the table gets its own
answer. The 3.5" and 4.0" are built alike and can't be told apart; neither needs to be.
`model()` is `CYDR_MODEL_UNKNOWN` only when GPIO4 couldn't be measured (WiFi was
already on) on a 240×320 board.

The **`BoardDetect`** example shows all of this on the screen, plus what each model has
for battery support, a light sensor and an amplifier switch — that table is in
[README-board.md](README-board.md#which-boards-these-are).

The 3.2" and the ESP32-024 need different settings — the ESP32-024's screen shows inverse
video with the 3.2"'s, and its touch axes are swapped — so `begin()` waits for the
red-LED answer before it sets the screen up.

All three LED pins were confirmed on all six boards on 2026-10-18 (2432S028, 32E 2.8",
3.2", two 3.5"/4.0", ESP32-024): each lit the pins these calls return while its screen
showed the matching color, and every LED matched.

## Wiring: own touch or shared touch

Each board holds its backlight pin LOW with a resistor. The library turns on the
ESP32's internal pull-up on both candidate pins and sees which one stays LOW:

- **GPIO21 stays LOW → own touch.** Backlight on GPIO21; the touch chip has its own
  pins (CLK 25, MOSI 32, MISO 39, CS 33). 2432S028 and 32E 2.8".
- **GPIO27 stays LOW → shared touch.** Backlight on GPIO27; the touch chip shares the
  display's SPI bus (13 / 14 / 12, CS 33). 32E 3.2", 3.5", 4.0" and ESP32-024.

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

On the 2432S028 and the ESP32-024 GPIO4 is the red LED; on every ESP32-32E board it is
the speaker amplifier's enable and the red LED is on GPIO22. The library turns on
GPIO4's internal pull-down and reads the pin's **voltage**, on every board:

| Board | GPIO4 with the internal pull-down | Measured |
|---|---|---|
| ESP32-2432S028 | **1.64 V** | 1643 mV, 2026-10-13 |
| ESP32-32E 2.8" | **2.50 V** | earlier probe, 2026-10-13 |
| ESP32-32E 3.2" | **2.52 V** | 2520 mV, 2026-10-13 |
| ESP32-32E 3.5" | **2.49 V** | 2493–2495 mV, 2026-10-13 |
| ESP32-32E 4.0" | **2.51 V** | 2506–2521 mV, 2026-10-13 |
| ESP32-024 2.4" | **1.60 V** | 1601–1608 mV, 2026-10-18 |

Below 2.0 V means the red LED is on GPIO4. One unit of each was measured.
[Why the two voltages differ](README-engineering.md#telling-gpio4s-two-jobs-apart).

```cpp
void setup() {
    lv_setup.begin();                       // or display.begin(): detection happens here
    int led = display.redLedPin();          // 4 or 22, active LOW on both
    pinMode(led, OUTPUT);
    digitalWrite(led, HIGH);                // off
    int green = display.greenLedPin();      // 16, or 17 on the ESP32-024
    int blue  = display.blueLedPin();       // 17, or 16 on the ESP32-024
    bool hasAmpSwitch = led != 4;           // GPIO4 is the amplifier enable on the 32E boards
    Serial.println(display.modelName());    // e.g. "ESP32-024 2.4\""
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
  and the ESP32-024 the LED stays dark.
- **If GPIO4 can't be measured**, `model()` is unknown and a shared-touch 240×320
  board is set up as a 3.2" — so on an ESP32-024 the colors come out inverted and touch
  runs sideways. Starting WiFi after `begin()` avoids it.
- `cydr_redLedProbeMillivolts()` returns the reading behind the answer (0 when none
  was taken), for a sketch that wants to log it.

## Capacitive boards fool these checks

This library is for **resistive** touch (the XPT2046) only. A capacitive CYD such as
the **ESP32-2432S032C** (GT911 touch on I²C) has the same GPIO27 backlight resistor as
the shared-touch boards, so `begin()` calls it shared touch. On the unit probed
(2026-10-13) the red LED is on GPIO4 (1620 mV), which the library now reports
correctly, but its panel didn't answer an ID read on GPIO12 — so the library guesses
ST7796 (320×480) and names it a 3.5" / 4.0" — and touch doesn't work. Use a
capacitive-touch library for those.
