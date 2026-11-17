# NM-CYD-C5 (ESP32-C5) — sound

The NM-CYD-C5 has a small amplifier and a 2-pin speaker connector, fed from **GPIO26**.
The ESP32-C5 has **no DAC**. Instead, its I²S peripheral turns 16-bit samples into a
1-bit **PDM** stream at about 6 MHz on GPIO26, and the board's filter smooths that back
into sound. Through a small speaker this sounds clearly better than the classic CYDs'
8-bit DAC, especially for treble and at low volume, and there is no dither hiss
*(bench, 2026-11-09)*. The **MP3Player** example plays this way on the C5 — copy its
`AudioOutputC5Pdm.h`. The classic boards are in [README-sound-ESP32.md](README-sound-ESP32.md).

## Sound characteristics at a glance

### Signal chain

**What is the output device?** A 2-pin speaker connector ("SPEAK", 1.25 mm). No
speaker on the board itself. A small 8 Ω speaker is the usual fit (inferred: the
amplifier is an 8002-class part).

**What generates the analog signal?** **The I²S peripheral's PCM-to-PDM converter on
GPIO26**, in its "PDM DAC" line mode: DMA feeds it 16-bit PCM, it upsamples to
128 × 48 kHz = 6.144 MHz and sigma-delta modulates to one bit. There is no DAC on the
ESP32-C5 and no codec on the board. ([How it compares with PWM](README-engineering.md#nm-cyd-c5-sound).)

**Is there an amplifier?** **Yes**, an SC8002B (U5), a mono bridge-tied amplifier on
the 3.3 V rail, about **1.85× from GPIO26 to the speaker** (inferred from its
resistors). Its SHUTDOWN pin is tied to GND: it is **always on** and can't be switched
off for idle noise or power.

**What sits between GPIO26 and the amplifier?** A two-pole RC low-pass with both
corners near **34 kHz**, then a 330 nF coupling capacitor that removes the DC. The PDM
stream's 6 MHz switching is far above the filter and doesn't reach the speaker.

**Mono or stereo?** **Mono.**

**Is there an input path?** **No** microphone or line-in.

### Getting samples out

**Which driver is the intended route?** **ESP-IDF's `i2s_pdm` TX driver** (core 3.3.x),
mono, 16-bit:

```cpp
#include <driver/i2s_pdm.h>

i2s_chan_handle_t tx;
i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
cc.auto_clear = true;                  // an underrun sends zeros: mid-scale, silent
i2s_new_channel(&cc, &tx, nullptr);
i2s_pdm_tx_config_t pc = {
    .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(44100),
    .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = { .clk = I2S_GPIO_UNUSED, .dout = GPIO_NUM_26, .dout2 = I2S_GPIO_UNUSED,
                  .invert_flags = { .clk_inv = false } },
};
pc.slot_cfg.hp_en = false;             // lets the start-up ramp below work
i2s_channel_init_pdm_tx_mode(tx, &pc);
i2s_channel_enable(tx);
// then i2s_channel_write(tx, samples, bytes, &written, timeout) with int16_t samples
```

For MP3 or WAV through ESP8266Audio, use the MP3Player example's
`AudioOutputC5Pdm.h`, which wraps exactly this.

**Which sample rates and bit depths work?** **8 to 48 kHz, 16-bit.** The converter's
upsampling divider is rate ÷ 100 and may not exceed 480, so rates above 48 kHz aren't
possible. 44.1 kHz MP3s played without an underrun while the LVGL screen scrolled
*(bench)*.

**What sample format?** Signed 16-bit mono. Zero is mid-scale (the line's average
sits at half the supply).

**Does the audio path conflict with anything?** **Partly.** It takes the C5's only
I²S peripheral. GPIO26 is also pin 3 of the **P1** connector. GPIO26 is a boot-mode
strapping pin, but it only matters while BOOT is held at reset. Nothing in this
library uses GPIO26 or I²S.

**Is there a volume control?** **No.** Volume is sample scaling in software, and
with 16-bit samples that costs nothing audible.

### Limits that keep it clean

**What amplitude limit must the sketch honor?** **None needed.** A 440 Hz tone from
full scale down in 3 dB steps was clean at low levels and slightly gritty in the top
few steps, but not enough to matter, even at full scale *(bench)*. To trade loudness
for cleanness, cap the level below full scale.

**Pops and clicks?** Avoided by the order of things. GPIO26 rests at 0 V before the
first sound, and mid-scale is half the supply, so jumping straight there would thump
through the coupling capacitor. Start the channel with the high-pass filter off and
ramp from −32767 to 0 over about 300 ms, then **leave it running**: between tracks and
while paused, send zeros (or let `auto_clear` do it). Stopping the channel drops the
line back to 0 V, which thumps.

**Idle behaviour?** With the channel running and fed zeros, the line sits at
mid-scale and the speaker is silent. **TODO (bench):** whether Wi-Fi activity is
audible.

**Bandwidth?** The 34 kHz filter passes the whole audio band; the speaker is the
limit.

### Power interaction

**What does the audio path draw?** **Unknown** — the amplifier is powered all the
time. **TODO (bench).** See [README-power-ESP32-C5.md](README-power-ESP32-C5.md).

### Recommendations

- **A beep, without I²S:** `ledcAttach(26, 1000, 8); ledcWrite(26, 128); delay(200);
  ledcWrite(26, 0); ledcDetach(26);` — heard clearly *(bench)*. Leave GPIO26 driven
  LOW afterwards.
- **Anything sampled:** the PDM set-up above, started once and left running.
- **Compute samples with integers.** The ESP32-C5 has no floating-point unit: a test
  synthesizing a three-note chord with `sinf()` at 44.1 kHz couldn't keep up and the
  DMA ran dry. A sine table and integer phase accumulators cost almost nothing. (The
  MP3 decoder is fixed-point already.)
- `audioPower()` and `ampEnable()` — the two calls every board with sound should
  offer — would both be no-ops here (no audio rail switch, no amplifier enable). This
  library doesn't define them for any board yet.
