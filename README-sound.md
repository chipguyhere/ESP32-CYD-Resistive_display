# Resistive-touch CYD boards — sound

How to get a beep, a sample or an MP3 out of the speaker without breaking anything
else. Written for the **ESP32-32E 2.8"**; how the other boards differ is just below. Answered against the same questionnaire as the family's other
`README-sound.md` files (`../README-sound.md`), so a "No" is a fact about the board.
The circuit values, driver internals and the reasoning behind each rule are in
[README-engineering.md](README-engineering.md#sound).

**Status: plays, not scoped.** The DAC side of this path is identical to the
ESP32-2432S028 sibling's, where it was worked out by ear with real music (findings
marked **(listened, 2026-09-18)** were made there). What is different here is
everything after the DAC pin: the amplifier runs from the board's main rail, has a
**shutdown pin on GPIO4**, and sits behind a filter that cuts treble. The bundled
`MP3Player` example, with the GPIO4 enable sequencing added, **plays MP3s from the card
on this board (2026-09-19)**, which confirms the enable polarity and the DAC path; the
finer points — pops, the treble roll-off, idle silence — are still **TODO (listen)**,
and anything a scope would settle is **TODO (bench)**.

**The 3.5" board** has the same enable: on one unit, a tone from the DAC on GPIO26 was
heard on its speaker only while GPIO4 was LOW **(listened, 2026-10-13)**. Its GPIO4
pull-up also reads the same as the 2.8"'s. Everything else in this file — the filter,
the gain, the rules below — was worked out on the 2.8" and is **TODO (listen)** on the
3.5" and 4.0".

**The 3.2" board** plays MP3s from the card through `MP3Player` with the same GPIO4
sequencing, touch working throughout, and nothing wrong heard **(listened,
2026-10-13)**. Its filter and gain haven't been examined.

**The ESP32-2432S028** has the same DAC pin and the same rules, but **no amplifier
switch**: its amplifier is always on, and GPIO4 is its red LED. `MP3Player` asks
`display.redLedPin()` and leaves GPIO4 alone when it returns 4, so the same sketch runs
there; the pop-free sequencing then rests on the DAC ramps alone.

**The ESP32-024** is like the 2432S028 here: an amplifier with no switch — its shutdown
pin is wired to ground — so it is always on, fed from the DAC on GPIO26, and GPIO4 is the
red LED. (The board probed carries an LTK8002D; the schematic names an SC8002B, a
compatible part.) It runs from the 3.3 V rail, so at full scale it may clip a little
before the DAC runs out — **TODO (listen):** whether `setMaxSwing()` helps at top
volume. [Details](README-engineering.md#the-esp32-024). `MP3Player` plays
cleanly on it, with the red LED left dark **(listened, 2026-10-18)**. Because the
amplifier is always on, anything that moves GPIO26 is heard: a sketch that sets pull-ups
or pull-downs on it, or drives it as a plain GPIO, clicks the speaker.

**GPIO25** is the touch clock only on the own-touch boards (2432S028, 32E 2.8"). On the
3.2", 3.5" and 4.0" touch shares the display's bus and GPIO25 is not the library's — but
something on those boards pulls it up, so the "channel 1 only" rule below still stands
until that is known. On the ESP32-024 GPIO25 has no pull-up, and a tone on it was not
heard; where it goes is **TODO (bench)**.

**The short version.** An 8-bit DAC on **GPIO26**, through a filter that rolls off above
**~4 kHz**, into a small mono amplifier with fixed gain (about ×1.6, roughly half a watt
at full scale on USB), **switched on by driving GPIO4 LOW**, driving a 4–8 Ω speaker on
a 2-pin connector. The amplifier is **off at reset** — a sketch that forgets GPIO4 gets
silence. Three rules: **clock the DAC from the default PLL, not the APLL** (the APLL
produces static); **enable the amplifier only after the DAC is parked at mid-scale** and
disable it before the DAC is touched again (the pop-free sequence); and **only ever
enable DAC channel 1** — channel 0 is the touch clock.

---

## Driving sound from this board — the recipe

Read this first if the job is "make the board play something". It is the distilled
result of getting MP3 playback working on the sibling board, plus the one rule this
board adds (the amplifier enable). The reference implementation is
**`examples/MP3Player/`** — copy `AudioOutputCydDac.h` from it rather than writing a
DAC output from scratch, and read `player.cpp` for the task structure.

### The rules, in the order they bite

1. **DAC channel 1 (GPIO26) only.** `DAC_CHANNEL_MASK_CH1`, `DAC_CHANNEL_MODE_SIMUL`.
   GPIO25 is DAC channel 0 *and* the touch clock: enabling it — with
   `DAC_CHANNEL_MASK_ALL`, `dacWrite(25, …)`, or a library that "uses both DACs" —
   silently kills touch. **ESP8266Audio's `AudioOutputInternalDAC` does exactly that.**
   Use ESP8266Audio's decoders and file sources freely; never its internal-DAC output.
2. **`DAC_DIGI_CLK_SRC_DEFAULT`, never `DAC_DIGI_CLK_SRC_APLL`.** On the APLL the output
   has random full-scale spikes — static that ignores the software volume. The default
   clock's floor is 19.6 kHz; a lower-rate source is **upsampled by an integer factor
   with linear interpolation** (8 kHz ×3, 11.025 / 16 kHz ×2), not run on the APLL.
   [What the static is](README-engineering.md#dac-driver-internals-esp-idf-5x-dac_continuous).
3. **`dac_continuous`, DMA-fed, from its own FreeRTOS task.** The output's write blocks
   when the DMA queue is full, and that is what paces the decoder. UI and player talk
   through a command queue and a status snapshot; nothing on the audio task touches
   LVGL. Which core depends on whether the sketch uses WiFi:
   - **No WiFi: core 0**, so LVGL and `loop()` keep core 1 to themselves. That is what
     the reference does.
   - **With WiFi: core 1, at a priority above `loopTask` (1)** — 3 is plenty. WiFi
     owns core 0 in bursts; the audio task doesn't belong under it.
   Either way, keep the queue deep (rule 5) — that is what actually tolerates the other
   core's business.
4. **Every `dac_continuous_write()` is exactly `buf_size` samples.** Pad with the hold
   level (128) if you have less. Odd sizes stall the DMA and then block the next write
   forever; sizes below `buf_size` shrink the queue. Pass a finite timeout (1000 ms) so
   a stall logs instead of wedging the task.
5. **Queue depth in hundreds of milliseconds.** 12 descriptors × 2048 bytes (~190 ms at
   44.1 kHz) ran with zero underruns; 4 × 256 samples (~23 ms) is thinner than an SD
   read stall. Mount the card at 20 MHz (`SD.begin(5, SPI, 20000000)`), not the 4 MHz
   `SD.begin()` default.
6. **If the decoder is ESP8266Audio, `ConsumeSample()` must return `false` sometimes.**
   `AudioGeneratorMP3::loop()` only returns when the output refuses a sample. An output
   that always accepts decodes the whole file inside one `loop()` and the task never
   services its queue again (the symptom: audio plays, nothing else responds). Return
   `false` once per chunk written; the decoder re-offers the refused sample next call.
   Then **sleep one tick after every `loop()`** — the blocking write only sleeps the task
   while decoding keeps up, and a file that decodes slower than real time would
   otherwise trip the task watchdog. Register a status callback that also sleeps a tick
   (a damaged file makes `loop()` retry inside one call), and seek past an ID3v2 tag
   before `mp3->begin()` rather than using `AudioFileSourceID3`.
   [Why](README-engineering.md#the-gpio25-conflict).
7. **Full-scale samples are fine on USB.** No audible clipping at ±127 on real music.
   Volume is software scaling before the 8-bit quantizer; mix stereo to mono. On
   battery, see [limits](#limits-that-keep-it-clean).
8. **Pops:** park GPIO26 low (`pinMode(26, OUTPUT); digitalWrite(26, LOW)`) in `setup()`
   before the DAC ever enables, ramp 0 → 128 over ~200 ms after enabling, ramp back down
   before disabling, and between tracks leave the DAC enabled at 128 rather than
   disabling it. On this board the ramps are belt-and-braces — the amplifier is off
   while they happen (next rule).
9. **Amplifier enable — GPIO4, LOW = on, and sequence it.** Drive it HIGH (off) in
   `setup()` before anything else touches audio; drive it LOW only *after* the DAC is
   enabled and parked at 128; drive it HIGH again *before* the DAC is disabled,
   reconfigured, or left floating. Mute while paused or stopped by driving it HIGH —
   it costs nothing and nothing on GPIO26 can be heard while it is off. (Implemented in
   the reference `AudioOutputCydDac.h`, which owns the pin: `setAmpEnabled()`.)
10. **`setup()` order:** `lv_setup.begin()` first — it claims GPIO25 — then the SD card,
    then start the audio task. The DAC channel itself is created lazily, when the first
    track starts, and the amplifier comes on at the end of that.

### The shape of the code

```cpp
// The output object (see AudioOutputCydDac.h for the real thing).
dac_continuous_config_t cfg = {
    .chan_mask = DAC_CHANNEL_MASK_CH1,       // rule 1
    .desc_num  = 12,                         // rule 5
    .buf_size  = 2048,
    .freq_hz   = rate * upsample,            // rule 2: >= 19600
    .offset    = 0,
    .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,   // rule 2
    .chan_mode = DAC_CHANNEL_MODE_SIMUL,
};
dac_continuous_new_channels(&cfg, &dac);
dac_continuous_enable(dac);
// ramp 0 -> 128 (rule 8), then:
digitalWrite(4, LOW);                        // rule 9: amplifier on
// ... then, from the audio task, only ever:
dac_continuous_write(dac, chunk, 2048, nullptr, 1000);   // rule 4
```

```cpp
// The task (see player.cpp).  Decoder objects live here and nowhere else.
static void audioTask(void *) {
    out = new AudioOutputCydDac();          // channel 1, default clock, chunked
    mp3 = new AudioGeneratorMP3();
    for (;;) {
        if (xQueueReceive(cmdQueue, &cmd, decoding ? 0 : pdMS_TO_TICKS(50)))
            handleCommand(cmd);             // play / pause / stop / next / prev
        if (decoding && !mp3->loop())       // returns once per chunk (rule 6)
            startNextTrack();
        publishStatus();                    // snapshot under a spinlock
    }
}
// Core 0 without WiFi, core 1 (above loopTask) with it — rule 3.
xTaskCreatePinnedToCore(audioTask, "audio", 16 * 1024, nullptr, 3, nullptr, AUDIO_CORE);
```

Diagnostics worth keeping: register the driver's `on_stop` callback
(`dac_continuous_register_event_callback`, handler must be `IRAM_ATTR`) and count the
stops you didn't cause — that is a free underrun counter.

### Known-good numbers

From the sibling board; the DAC side is identical, and `MP3Player` runs on this board
with the same configuration.

| Item | Value |
| --- | --- |
| Source | 44.1 kHz stereo MP3 (libmad via ESP8266Audio 2.4.1), SD card at 20 MHz |
| DAC | channel 1, `DAC_DIGI_CLK_SRC_DEFAULT`, 12 × 2048-byte DMA buffers |
| Task | 16 KB stack, priority 3, core 0 (no WiFi in the sketch); LVGL on core 1 untouched |
| Image | ~830 KB with LVGL 9 + libmad — fits the default 1.2 MB app slot |
| Underruns | 0 over several tracks |

---

## Sound characteristics at a glance

### Signal chain

**What is the output device?** A 2-pin speaker connector (MX1.25, like every connector
on the board — see [README-board.md](README-board.md#connectors)). **Pin 1 (the
silkscreen dot) is SP+, pin 2 is SP−.** It is a bridge-tied output, so neither pin is
ground: don't tie SP− to anything, and polarity only matters if a second speaker is
ever driven in phase. No speaker is fitted; the amplifier is rated for **4–8 Ω**. No
headphone jack, no line-out.

**What generates the analog signal?** The ESP32's internal **8-bit DAC channel 1** on
**GPIO26**. There is no external DAC and no codec.

**Is there an amplifier?** Yes — an **SC8002B** (LM4871 family), mono, **powered from
the board's main rail**: about 4.6 V on USB, the cell voltage on battery, so there is
sound on battery at reduced headroom. Its **shutdown pin is GPIO4, active LOW** (LOW =
amplifier on), with a pull-up on the board, so the amplifier is **off at reset** until a
sketch drives GPIO4 LOW. Gain is **fixed at about ×1.6** by resistors. On USB it can
deliver more than the DAC can drive it to.
[Gain and headroom arithmetic](README-engineering.md#amplifier).

**What sits between the source and the amplifier?** A resistor-capacitor network that
does three things: DC-blocks the DAC (so its idle level never reaches the speaker, only
its changes do), **rolls off treble above ~4 kHz** (a telephone-ish sound; voice, beeps
and most music are fine, cymbals and sibilance are dulled — and the DAC's stair-steps
are well hidden), and sets the gain. [Values and poles](README-engineering.md#input-network).

**Mono or stereo?** Mono. One DAC channel, one amplifier, one speaker.

**Is there an input path?** No microphone, no line-in.

### Getting samples out

**Which driver is the intended route?** On the ESP32 Arduino core 3.x, three routes in
order of usefulness:

1. **`dac_continuous`** (ESP-IDF, `driver/dac_continuous.h`) — DMA-fed, the way to play
   PCM. The configuration is in [the recipe](#the-shape-of-the-code) above; the rules
   about chunk size, queue depth and clock source are there because each one cost an
   afternoon on the sibling board.
2. **`dacWrite(26, value)`** (Arduino) from a timer or a tight loop — fine for a tone or
   a very short sample; no DMA, so it competes with LVGL for the CPU.
3. **`ledcWriteTone(26, freq)`** — a PWM square wave through the filter. The crudest
   beep, and a one-liner; the ~4 kHz low-pass takes some of the edge off. Stop it with
   `ledcWriteTone(26, 0)` and re-assert the idle state (below). Remember GPIO4 LOW
   first, or there is no beep.

A fourth option, `dac_cosine` (`driver/dac_cosine.h`), generates a sine in hardware with
a selectable 0 / −6 / −12 / −18 dB attenuation — the only "hardware gain" anywhere in
this chain, and only for that generator. It is the ideal tool for a clipping test.

**Which sample rates work, and at what bit depth?** The DAC is **8-bit**, full stop —
quiet passages hiss, and volume is best applied before quantizing. With the default
clock the continuous driver supports **19.6 kHz and up**. **8 / 11.025 / 16 kHz would
need the APLL, and the APLL brings static** — upsample by an integer factor and stay on
the default clock. 44.1 kHz on the default clock is the tested configuration (listened,
2026-09-18). There is no point in rates above ~22 kHz: the filter passes nothing useful
above ~8 kHz.

**What sample format does the driver want?** `dac_continuous_write()` takes **8-bit
unsigned** bytes (silence = 128). Signed 16-bit PCM from a WAV needs `(s16 >> 8) + 128`
after any volume scaling.

**Does the audio path conflict with anything else on the board?** **Yes — GPIO25.** The
ESP32's *other* DAC channel is **GPIO25, which this library uses as the touch clock**.
Anything that enables both DAC channels takes GPIO25 away from the touch driver and
**touch stops working, silently**. Enable channel 1 only, and check any third-party
audio library's internal-DAC mode for which channels it grabs before trusting it.
**ESP8266Audio (2.4.1) cannot be trusted here:** its `AudioOutputInternalDAC` uses
channel 0 for mono and both for stereo, and drives GPIO25 low in its constructor. Touch
dies the moment audio starts. `MP3Player` carries a replacement `AudioOutput` subclass
(`AudioOutputCydDac.h`) that drives channel 1 only; the decoders themselves are fine.

**Is there a volume or gain control?** No — only a mute. The amplifier gain is fixed
and the DAC has no gain register. Volume is **sample scaling in software**, and the
consequence is the 8-bit one: at 25 % volume you are playing 6-bit audio. `MP3Player`
has dither on the quantizer behind a checkbox; whether it helps quiet passages is
**TODO (listen)**. The amplifier shutdown on GPIO4 is a hard mute, useful between
sounds; it is not a volume step.

### Limits that keep it clean

**What amplitude limit must the sketch honor?** **None on USB** (inferred): the DAC's
full swing lands below the amplifier's clipping point, at about **0.4 W into 8 Ω /
0.9 W into 4 Ω**. **On battery the margin goes away** — at a mid-charge cell the
amplifier can just about swing what full scale asks for, and at a low cell it can't.
So use the full 8 bits on USB; on battery, if the peaks sound rough, a software ceiling
of ~±100 counts around 128 (`setMaxSwing()` in `AudioOutputCydDac.h`) is the knob, and
it could be tied to the GPIO34 battery reading. **TODO (bench):** scope the speaker
terminals with `dac_cosine` at 0 dB on USB and on a mid-charge cell.
[Arithmetic](README-engineering.md#headroom-arithmetic).

**Are there pops or clicks?** From the DAC, yes — a half-scale thump when it enables or
disables — and this board can hide them completely by **sequencing the amplifier
enable**: GPIO4 HIGH (off) while the DAC is enabled and ramped up to 128, then GPIO4
LOW; GPIO4 HIGH again before the DAC is ramped down or disabled. Keep the ramps as well;
they cost nothing. What remains is the amplifier's own turn-on, which the part
soft-starts — a faint tick at most. **TODO (bench):** confirm, and check whether reset /
bootloader entry pops (it shouldn't; the pull-up holds the amplifier off through reset).
[The physics](README-engineering.md#pop-physics).

**What is the idle behavior?** Silent, if the sketch leaves the amplifier off — and it
is off by default. **Rule: GPIO4 HIGH whenever nothing is playing.** If the amplifier is
left on between sounds instead, GPIO26 must be in a known state — `dacWrite(26, 128)` —
never a floating input, which picks up WiFi and the display bus and hisses or ticks.
**TODO (bench):** whether anything at all is audible with the amplifier off and the DAC
active, i.e. whether the shutdown really is a full mute.

**Bandwidth limits?** About **16 Hz to 4 kHz** (−3 dB), rolling off gently above. That
is the limit on this board — not the sample rate, not the speaker. Voice, beeps and most
music are fine; cymbals and sibilance are dulled.

### Power interaction

**What does the audio path draw?** Nothing worth counting when the amplifier is off —
the one place this board beats the sibling for power. When on, a few mA from the main
rail (straight from the cell on battery) plus the speaker power while playing; playback
current at a typical level is **TODO (bench)** and belongs in
[README-power.md](README-power.md#power-consumption)'s table, not here.

**Does anything about sound interfere with sleep?** No, provided the amplifier is off.
In deep sleep GPIO4 floats and the board's pull-up takes it HIGH — amplifier off — so
the DAC pin floating is inaudible. Drive GPIO4 HIGH before sleeping anyway (and
`rtc_gpio_hold_en(GPIO_NUM_4)` if being explicit), so the transition into sleep is not
an on→off pop. GPIO26 is not a wake source and shares nothing with one.

### Recommendations

**Make a beep** (untested):

```cpp
pinMode(4, OUTPUT);                 // amplifier enable, active LOW; off at reset
dacWrite(26, 128);                  // park the input at mid-scale first...
delay(20);                          // ...and let the coupling cap settle
digitalWrite(4, LOW);               // amplifier on
ledcAttach(26, 1000, 8);            // core 3.x LEDC API
ledcWriteTone(26, 1000);
delay(100);
ledcWriteTone(26, 0);
ledcDetach(26);
dacWrite(26, 128);                  // back to mid-scale
digitalWrite(4, HIGH);              // amplifier off — silent, and nothing to idle
```

**Play 8-bit PCM from flash:** the `dac_continuous` configuration in [the
recipe](#the-shape-of-the-code), feeding `dac_continuous_write()` from a task so LVGL's
`lv_timer_handler()` keeps running; convert to 8-bit unsigned; ramp in and out; amplifier
on only once the DAC is parked at 128.

**`setup()` order:** `pinMode(4, OUTPUT); digitalWrite(4, HIGH)` (amplifier off,
explicitly), `lv_setup.begin()` (it claims GPIO25 for touch — nothing audio-related
should touch that pin afterwards), then bring up the DAC on channel 1 only, park it at
128, and only then GPIO4 LOW.

**Idle state:** amplifier off (GPIO4 HIGH). If it must stay on, DAC enabled at 128 —
never a floating input.

**Play MP3 from the SD card:** see `examples/MP3Player/` — ESP8266Audio's libmad decoder
in a FreeRTOS task on core 0, `AudioOutputCydDac.h` feeding `dac_continuous` and owning
the GPIO4 amplifier enable, LVGL on core 1. It plays on this board.

**Library TODOs**, none started: a `sound.beep()` / `sound.play8bit()` pair that bakes in
the channel-1-only rule, the default-PLL rule, whole-chunk DMA writes, the ramped
enable/disable, and the amplifier-enable sequencing. Until then, everything here is the
sketch's job.
