# Numbered POV

This usermod adds a `Numbered POV` effect for an ESP32-S3 persistence-of-vision display.

## Images

- Upload 24-bit, uncompressed BMP files to the filesystem root.
- Use canonical names from `/1.bmp` through `/255.bmp`.
- BMP height maps to LEDs and BMP width maps to playback columns.
- Pixel 0 at the top of a BMP column maps to LED 0 in each segment.
- Extra image pixels are cropped; extra LEDs are set to black.

The image control is populated when the effect is entered. Leave and re-enter the effect after uploading or deleting images. Sparse IDs are supported, so `/1.bmp`, `/3.bmp`, and `/255.bmp` produce three slider positions. Files with unsupported or invalid BMP headers are skipped.

## Timing

The `Column wait (ms)` speed control advances from 4 ms at the left edge to 260 ms at the right edge. The default is 4 ms. WLED stores 256 speed positions for this 257-value interval, so one intermediate millisecond value is skipped while both endpoints remain exact.

The effect does not consume MPU data. The separate `mpu6050_imu` usermod remains available to other effects and future features. At least one segment must use the effect; all segments using it show the same image and column with the same wait time.

With [Pair Sync](../pair_sync/README.md) configured, the leader owns the image, speed, cycle settings, and playback start time. The follower calculates the scheduled image and column from the estimated leader clock and joins after loading a verified image. When an image is not ready, the last decoded image keeps playing; before the first image is ready there is a loading/error indication. Brief packet loss uses clock holdover; status is shown on the Info page. With Pair Sync disabled or absent, playback uses a local 64-bit monotonic timer.

Images are fingerprinted as they are loaded, not on every frame. Replace images between playback sessions, then leave and re-enter the effect to rescan. No rotation-angle mode is used.

No matching or loadable image produces a blinking red error pattern.

## Cycle BMPs

**Cycle BMPs** is unchecked by default (`o1=false`). Checking it shows **Repeats per BMP**, an integer slider from **1 to 20** (`c3`, default 1). Unchecking it hides that slider and keeps the currently displayed BMP playing normally. The stored repeat count has no playback effect while cycling is off. These controls use normal WLED state and presets.

One repeat is one complete traversal of the image's columns. Starting from the selected image, each BMP is scheduled for that many traversals before advancing in numerical order, skipping missing numbers and wrapping to the beginning. For IDs 1, 3, 8 with two repeats: `1, 1, 3, 3, 8, 8, 1, ...`. Different image widths have different durations: `width × column wait × repeats`.

Manually choosing another image, changing speed/repeats while cycling, or enabling cycling restarts the sequence from the selection. Automatic image changes do not restart it. The leader and follower use the same sequence epoch, ordered image widths, and repeat count, not independent completion counters. Late joins and delayed renders catch up to the scheduled image/column. The Info page reports the actual displayed image, which may lag the scheduled selection during loading.

### Dynamic double buffering

The current BMP and the next BMP use two separately allocated pixel buffers. Each allocation is sized from the validated BMP dimensions and row padding; existing capacity is reused and enlarged only as needed. There is **no 300 × 11 limit** or other fixed pixel-buffer size. A 300 × 11 image uses 9,900 bytes; two such images use 19,800 bytes plus metadata/workspace. WLED's allocator prefers PSRAM when appropriate. Both buffers are released when leaving the effect.

The next image is read, SHA-256 checked, and copied in **1 KiB steps between render calls**. A ready transition switches buffer slots without file I/O or another allocation. One buffer is enough for a single-image cycle. The white-joker gradient is still applied during rendering.

If a buffer cannot be allocated or a file cannot be read, the current image stays intact and the Info page identifies the error and BMP number. Failed loads are not retried every frame. While cycling, the leader excludes failed images for that session and restarts the remaining sequence, sharing its exclusion list with the follower. A follower-only load failure holds the previous image during that slot and rejoins later slots without shifting the shared timeline. Manually reselecting a failed image or leaving/re-entering the effect permits a retry. Files are not deleted.

If preloading misses a transition deadline, the previous image keeps playing until a scheduled image is ready; timing does not accumulate drift. Very short images or a busy filesystem can still cause skipped columns/images. Two buffers reduce loading stalls but do not guarantee every scheduled frame reaches the LEDs. Pair Sync's existing **768 KiB per-file transfer limit** is separate from dynamic pixel allocation and remains unchanged.

Cycle scheduling and BMP boundary-validation checks can be compiled with:

```sh
xtensa-esp32s3-elf-g++ -std=c++14 -Wall -Wextra -Werror -fsyntax-only test/pov_cycle.cpp
```

The checkbox, conditional visibility, and 1–20 slider checks run with `node --test tools/pov-cycle-test.js`.

## White joker and swatch gradient

Exact RGB white (`#FFFFFF`) is the single reserved joker color. All white pixels in a column use the same replacement color, calculated from that column's position across the full image width. Every other BMP color, including near-white and black, stays unchanged.

The effect exposes WLED's three color swatches and a **Joker colors** control (`c2`, values 1–3, default 3):

| Joker colors | Replacement for white pixels |
| --- | --- |
| 1 | Swatch 1 throughout the image |
| 2 | Smooth gradient from swatch 1 at the first column to swatch 2 at the last column |
| 3 | Smooth gradient from swatch 1 through swatch 2 at the center to swatch 3 at the last column |

A one-column image uses swatch 1. With an even image width, the center lies between two columns. The gradient restarts at swatch 1 when the image loops; it is not a cyclic gradient. Black swatches are valid colors, not a signal to skip a swatch. Legacy `c2=0` uses one color, and values above 3 use three.

Replacement colors use WLED's normal RGBW color blending and segment controls, not the selected palette. The blend is calculated once per segment render; no image copy or extra synchronization traffic is needed. Swatches and `c2` are saved in normal WLED state/presets and synchronized through the existing scene mechanism. Joker changes do not restart the playback timeline.

Export joker areas as exact white without antialiasing or color adjustments. Near-white edge pixels will retain their original colors. To display white in joker areas, set the applicable replacement swatches to white.

Gradient and exact-white self-checks can be compiled with:

```sh
xtensa-esp32s3-elf-g++ -std=c++14 -Wall -Wextra -Werror -fsyntax-only test/pov_joker.cpp
```

The UI range/restore checks run with `node --test tools/pov-joker-test.js`.
