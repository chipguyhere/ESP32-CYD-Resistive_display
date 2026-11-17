/*
 * config.h - How much RAM the bounce buffers take.
 *
 * The balls are drawn into RASTER_BUFS buffers of CHUNK_ROWS rows each, and
 * sent from them by DMA, so they are internal, DMA-capable RAM: the scarcest
 * kind.  Each takes width x CHUNK_ROWS x 2 bytes (240 wide: 480 bytes a row).
 *
 * Smaller chunks save RAM but cost time: every chunk is its own SPI
 * transaction and its own hand-off between the render and raster tasks, and
 * that overhead is paid per chunk, not per row.  A slower send may no longer
 * fit in one refresh, and then the demo runs at half the frame rate (or
 * suggests a slower refresh).  Fewer buffers leave the render task less room
 * to run ahead, so a moment's delay (an interrupt, a long LVGL redraw on the
 * same core) is more likely to make a chunk late; a late chunk drops the
 * rest of that frame, and those rows show the previous frame.
 *
 * Pick one.  Measured on an ESP32-2432S028 (ILI9341, 240 wide, 40 MHz, the
 * refresh slowed to 39 Hz, 100 see-through balls), the ball area's 240 rows:
 *
 *   preset           RAM        send time  result
 *   32 rows x 3      46,080 B   21.7 ms    39 fps; would fit a 42 Hz refresh
 *   16 rows x 3      23,040 B   23.4 ms    39 fps
 *    8 rows x 2       7,680 B   23.8 ms    19.5 fps (half rate: no longer fits)
 *    2 rows x 2       1,920 B   25.7 ms    19.5 fps
 *
 * No late chunks in any of them.  See README-engineering.md, "Beam chasing".
 *
 * LVGL's own two render buffers take 38,400 bytes at 240 wide (40 rows
 * each).  lv_setup.begin(rotation, maxBufferBytes) caps them (e.g. 9600 for
 * 20 rows each); that only makes LVGL's strip redraw in more pieces.
 */

#pragma once

// More memory: 46,080 bytes.  The fastest send; a little more headroom.
//#define CHUNK_ROWS 32
//#define RASTER_BUFS 3

// Just enough for this sketch at full rate: 23,040 bytes.  The default.
#define CHUNK_ROWS 16
#define RASTER_BUFS 3

// Conservative: 1,920 bytes.  The send slows by a tenth, which on the board
// above halves the frame rate, and late chunks -- rows showing the previous
// frame -- become more likely.
//#define CHUNK_ROWS 2
//#define RASTER_BUFS 2
