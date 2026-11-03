# Which board am I on? — what `display.begin()` detects

One sketch can run on every board this library supports. `display.begin()` works out
three things on its own, before it drives any pin that differs between boards, and
the sketch can ask for each one afterwards. Together they name the board:

| Question | Call | Answers |
|---|---|---|
| How is the board wired? | `display.board()` | `CYDR_BOARD_OWN_TOUCH` or `CYDR_BOARD_SHARED_TOUCH` |
| Which screen chip, what size? | `display.panel()`, `display.panelName()`, `display.nativeWidth()` / `nativeHeight()` | ILI9341 / ST7789 (240×320) or ST7796 (320×480) |
| Which pin is the red LED on? | `display.redLedPin()` (or `cydr_redLedPin()`) | `4` or `22` |
| Which touch chip? | `display.touchChip()`, `lv_setup.touch().chipName()` / `capacitive()` | XPT2046 (resistive), GT911 (the 2432S032C and 3248S035C) or CST820 (the 2432S024C and JC2432W328) |
| How big is the screen? | `display.diagonalTenths()` | 24, 28, 32, 35 or 40; 0 for the 32E 3.5" / 4.0" unless the [size override](README-overrides.md) names it |
| Can the screen be read back? | `display.readback()`, `display.readbackName()` | on GPIO12 (most boards), on shared GPIO13 (the 2432S032 / C) or unavailable (the 3248S035C) |
| …and green and blue? | `display.greenLedPin()`, `display.blueLedPin()` (or `cydr_greenLedPin()`, `cydr_blueLedPin()`) | `16` / `17`, or `17` / `16` on the ESP32-024 and ESP32-035 |
| So which board is it? | `display.model()`, `display.modelName()` (or `cydr_activeModel()`) | one of the rows below |

| Board | `model()` | Wiring | Panel | Red LED | GPIO4 is |
|---|---|---|---|---|---|
| ESP32-2432S028 | `CYDR_MODEL_2432S028` | own touch | ILI9341 or ST7789, 240×320 | **4** | the red LED |
| ESP32-32E 2.8" | `CYDR_MODEL_32E_28` | own touch | ILI9341 or ST7789, 240×320 | **22** | speaker amplifier enable |
| ESP32-32E 3.2" | `CYDR_MODEL_32E_32` | shared touch | ST7789, 240×320 | **22** | speaker amplifier enable |
| ESP32-32E 3.5" / 4.0" | `CYDR_MODEL_32E_35_40` | shared touch | ST7796, 320×480 | **22** | speaker amplifier enable |
| ESP32-024 2.4" | `CYDR_MODEL_024` | shared touch | ST7789 or ILI9341, 240×320 | **4** | the red LED |
| ESP32-035 3.5" | `CYDR_MODEL_035` | shared touch | ST7796, 320×480 | **4** | the red LED |
| ESP32-2432S032 | `CYDR_MODEL_2432S032` | shared touch | ST7789, 240×320 | **4** | the red LED |
| ESP32-2432S032C | `CYDR_MODEL_2432S032C` | GT911 touch on I²C, backlight 27 | ST7789, 240×320 | **4** | the red LED |
| ESP32-2432S024C | `CYDR_MODEL_2432S024C` | CST820 touch on I²C, backlight 27 | ILI9341, 240×320 | **4** | the red LED |
| Guition JC2432W328 | `CYDR_MODEL_JC2432W328` | CST820 touch on I²C, backlight 27 | ST7789, 240×320 | **4** | the red LED |
| ESP32-3248S035C | `CYDR_MODEL_3248S035C` | GT911 touch on I²C, backlight 27 | ST7796, 320×480 | **4** | the red LED |

The size override in `TouchCalibrate` can say which of the 3.5" and 4.0" a board is.
The model override can name any board outright
([README-overrides.md](README-overrides.md)).

Three pairs of boards look alike to the first two checks: the 2432S028 and the 32E 2.8"
(same wiring and panels), the 32E 3.2" and the ESP32-024, and the 32E 3.5" / 4.0" and
the ESP32-035 (same wiring, same panel). The red-LED check splits all three, so every
board in the table gets its own answer. The 3.5" and 4.0" are built alike and can't be
told apart; neither needs to be. The 2432S032 is recognised by its screen alone (see
[Panel](#panel-asked-by-its-id)), and the 2432S032C, 2432S024C, JC2432W328 and
3248S035C by their touch chips answering on I²C (see [Capacitive boards](#capacitive-boards)).
`model()` is `CYDR_MODEL_UNKNOWN` only when GPIO4
couldn't be measured (WiFi was already on) on a 240×320 board; a 320×480 board is then
taken for a 32E 3.5" / 4.0".

The **`BoardDetect`** example shows all of this on the screen, plus what each model has
for battery support, a light sensor and an amplifier switch — that table is in
[README-board.md](README-board.md#which-boards-these-are).

The 3.2" and the ESP32-024 need different settings — the ST7789 ESP32-024's screen shows inverse
video with the 3.2"'s, and its touch axes are swapped — so `begin()` waits for the
red-LED answer before it sets the screen up. The 32E 3.5" and the ESP32-035 share a
screen setup, but the ESP32-035's touch runs the other way left-to-right. The ESP32-024
also comes with an ILI9341, whose touch is laid out like the 3.2"'s rather than the
ST7789 ESP32-024's; the panel ID tells the two apart.

All three LED pins were confirmed on all six boards on 2026-10-18 (2432S028, 32E 2.8",
3.2", two 3.5"/4.0", ESP32-024): each lit the pins these calls return while its screen
showed the matching color, and every LED matched. The ESP32-035's were confirmed the
same way on 2026-10-23.

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
answers a different one. The panel alone sets the screen size.

The ESP32-2432S032's panel has **no wire to GPIO12** — it only listens — so nothing
answers there. When nothing at all comes back on GPIO12, the library asks all the same
questions again and listens on the panel's data-in line, GPIO13, which these panels can
also answer on. An ST7789 answering there on shared-touch wiring is a 2432S032 (or
2432S032C). Any other panel answering there — an ILI9341 or ST7796, or an ST7789 whose
wiring couldn't be told — is a board no one has seen yet: the library sets that panel up
(so the screen is the right size), leaves `model()` unknown, and says so on Serial. An
ST7789 on own-touch wiring is the known 2.8" panel and is named as before. If no ID is
recognised, the library prints the bytes on Serial and guesses from the wiring
(ST7796 on shared touch, ILI9341 on own touch); `display.setPanel()` before
`begin()` skips detection. [Details](README-board.md#screen-variants).

## Red LED: GPIO4's voltage

On the 2432S028, the ESP32-024 and the ESP32-035 GPIO4 is the red LED; on every ESP32-32E board it is
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
| ESP32-035 3.5" | **1.58 V** | 1582–1584 mV, 2026-10-23 (the 32E 3.5" beside it: 2545 mV) |

Below 2.0 V means the red LED is on GPIO4. One unit of each was measured.
[Why the two voltages differ](README-engineering.md#telling-gpio4s-two-jobs-apart).

```cpp
void setup() {
    lv_setup.begin();                       // or display.begin(): detection happens here
    int led = display.redLedPin();          // 4 or 22, active LOW on both
    pinMode(led, OUTPUT);
    digitalWrite(led, HIGH);                // off
    int green = display.greenLedPin();      // 16, or 17 on the ESP32-024 / -035
    int blue  = display.blueLedPin();       // 17, or 16 on the ESP32-024 / -035
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
  On the 32E boards the board's own pull-up keeps the amplifier off; on the 2432S028,
  the ESP32-024 and the ESP32-035 the LED stays dark.
- **On the 2432S032 GPIO4 isn't measured**: the screen has already named the board,
  and its schematic puts the red LED on GPIO4.
- **If GPIO4 can't be measured**, `model()` is unknown and a shared-touch 240×320
  board is set up as a 3.2" — so on an ESP32-024 the colors come out inverted and touch
  runs sideways — and a 320×480 one as a 32E 3.5" / 4.0", so on an ESP32-035 touch
  runs backwards left-to-right and green and blue are swapped. Starting WiFi after
  `begin()` avoids it.
- `cydr_redLedProbeMillivolts()` returns the reading behind the answer (0 when none
  was taken), for a sketch that wants to log it.

## When detection is wrong

A board variant that detection gets wrong can be corrected without changing any code:
`TouchCalibrate` saves **overrides** for this board, and `begin()` applies them. What
follows from the answers above — color order, inversion, mirroring, touch axes, the LED
and backlight pins, what GPIO34 carries — can be overridden that way, and so can the
panel type when the panel didn't identify itself (never when it did). See [README-overrides.md](README-overrides.md).

## Capacitive boards

On every board, `begin()` checks for a capacitive touch chip on I²C (SDA 33, SCL 32). It
looks for a **CST820** at address `0x15`, then a **GT911** at `0x5D` or `0x14`. If one
answers, the touch driver uses it on **`Wire1`** and leaves `Wire` for the sketch. The
chip reports screen pixels, so there is nothing to calibrate: `loadCalibration()`
returns `false`, `capacitive()` returns `true`, and `TouchCalibrate` goes straight to
its drawing test. Both chips are polled. Their reset line (GPIO25) and interrupt line
are left alone.

| Board | Touch chip | How it's named |
|---|---|---|
| ESP32-2432S032C | GT911 | Its screen names the 2432S032 PCB (see [Panel](#panel-asked-by-its-id)). The GT911 then marks it as the C. Checked on one unit, 2026-10-29. |
| ESP32-2432S024C | CST820 (chip ID `0xB7`) | CST820 with an ILI9341. GPIO21 is the touch chip's interrupt line, so the backlight-pin check comes out inconclusive. When a capacitive chip answers, the backlight is then taken to be GPIO27. Checked on one unit, 2026-10-31. |
| ESP32-3248S035C | GT911 at `0x5D` | GT911 on shared-touch wiring with an ST7796 — or with a panel that answers no ID, which is then taken for an ST7796, as this board's is. GPIO4 isn't measured: it only follows the internal pulls here. Checked on one unit, 2026-11-02. |
| Guition JC2432W328 | CST820 (chip ID `0xB7`) | CST820 with an ST7789 on shared-touch wiring. The wiring and red-LED reading are an ESP32-024's; the CST820 tells them apart. Inversion stays off, as on the ST7789 ESP32-024. Checked on one unit, 2026-10-31. |

Touch follows the finger on both CST820 boards with no mirroring or swap, as on the
GT911. The RGB LED is red / green / blue on GPIO4 / 16 / 17 on both (bench, 2026-10-31).

A capacitive chip on some other board is still driven, but `model()` is
`CYDR_MODEL_UNKNOWN` and Serial says so. If detection names the wrong board, the
[model override](README-overrides.md) in `TouchCalibrate` can name the right one.
