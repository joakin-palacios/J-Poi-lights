# Numbered POV

This usermod adds a `Numbered POV` effect for an ESP32-S3 persistence-of-vision display.

## Images

- Upload 24-bit, uncompressed BMP files to the filesystem root.
- Use canonical names from `/1.bmp` through `/255.bmp`.
- BMP height maps to LEDs and BMP width maps to angular columns.
- Pixel 0 at the top of a BMP column maps to LED 0 in each segment.
- Extra image pixels are cropped; extra LEDs are set to black.

The image control is populated when the effect is entered. Leave and re-enter the effect after uploading or deleting images. Sparse IDs are supported, so `/1.bmp`, `/3.bmp`, and `/255.bmp` produce three slider positions.

## Timing

The `Column wait (ms)` speed control advances from 4 ms at the left edge to 260 ms at the right edge. The default is 4 ms. WLED stores 256 speed positions for this 257-value interval, so one intermediate millisecond value is skipped while both endpoints remain exact.

The effect does not consume MPU data. The separate `mpu6050_imu` usermod remains available to other effects and future features. At least one segment must use the effect; all segments using it show the same image and column with the same wait time.

No matching or loadable image produces a blinking red error pattern.
