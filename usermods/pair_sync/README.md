# Pair Sync

Pair Sync turns two WLED devices into an isolated leader/follower pair.

## Provisioning

1. Flash the same firmware and filesystem layout to both devices.
2. Configure the leader's WLED access point SSID and password, then set `PairSync > role` to `Leader`.
3. Configure the follower's first Wi-Fi network with the leader AP credentials and a DHCP address, then set `PairSync > role` to `Follower`.
4. Leave `leaderHost` at `4.3.2.1` unless the leader AP address is customized.

The leader remains AP-only at `4.3.2.1`. The follower connects as a station. While connected to the leader's access point, open the follower's assigned IP address (for example `http://4.3.2.3`) to access its WLED interface, including before setting its role to Follower. DHCP may assign a different address. Pair Sync does not open a separate follower access point.

## Synchronization

- WLED UDP notifier packets provide immediate brightness, color, effect, palette, segment, transition, and timebase updates.
- A revisioned HTTP scene snapshot reconciles missed or incomplete UDP updates without copying device-specific LED, GPIO, Wi-Fi, or MPU configuration.
- Numbered POV files `/1.bmp` through `/255.bmp` are pulled from the leader. Every file is size-checked, SHA-256 verified, BMP-validated, and transactionally replaced before stale follower files are removed.
- Local scene changes on the follower are reconciled back to the leader's scene. Device-specific settings remain local.

## POV playback synchronization

Flash this version on **both** devices; the JPS2 clock protocol replaces the earlier single-image clock messages and HTTP-only control polling.

- The follower exchanges four timestamps with the leader over UDP port **11989**. Requests are 16 bytes and replies are 97 bytes, with no per-column or per-image-transition network traffic. Scene/resource revisions travel in the reply, so HTTP is only needed to reconcile a change.
- Acquisition uses requests at 100 ms intervals until eight acceptable samples arrive; normal operation uses one request per second. Three samples are required before playback is ready. Samples with a round trip above 20 ms, impossible timestamps, or a mismatched request sequence are rejected.
- A fixed eight-sample history favors low-delay measurements. Small clock corrections are smoothed and capped at 250 microseconds per accepted sample; errors above 20 ms are realigned directly.
- The leader supplies one playback description: session, revision, sequence anchor image, speed, cycle enable/repeat count, playlist signature, failed-image exclusions, and a 64-bit monotonic start timestamp. Both devices derive the scheduled image and column from the same ordered image widths. Manual image/speed/cycle-setting changes restart the timeline; automatic image transitions do not.
- The follower checks playlist/resource readiness before following the timeline. Per-file SHA-256 hashes are taken from the verified resource manifest and compared again when each image is incrementally decoded. Loading does not restart the shared timeline. Double buffering preloads the next image; if it is late or cannot fit, the previous image is held and synchronization is not claimed for that slot. The leader does not wait for the follower.
- After five seconds without an acceptable clock sample, timing is marked stale. An already-ready follower continues on its last clock estimate; reconnection or a new leader session reacquires timing and rechecks resources.
- The Numbered POV entry on the Info page reports `leader timeline`, `acquiring synchronization`, `synchronized`, `stale clock (holdover)`, or `image not ready`. These describe estimated timeline alignment, not measured simultaneous LED output.

To avoid repeated filesystem stalls during POV playback, the leader scans resources when entering POV or when its playlist geometry changes, and otherwise scans every ten seconds while POV is inactive. Automatic image transitions do not trigger resource scans. Leave and re-enter POV after replacing files in use. A changed/mismatched resource generation triggers follower reconciliation before the playlist is used. File transfers and initial scans can still interrupt rendering.

Clock synchronization does not use MPU measurements, physical angle, NTP, or WLED's UDP timebase. Independent LED refresh schedules and asymmetric Wi-Fi delays still limit physical alignment, especially at 4 ms per column. Test two devices before relying on column-level accuracy.

## Timing self-check

With the ESP32-S3 toolchain on PATH, run:

```sh
xtensa-esp32s3-elf-g++ -std=c++14 -Wall -Wextra -Werror -fsyntax-only test/pov_sync_clock.cpp
```

Compile-time assertions exercise offset estimation, delay filtering, drift correction, packet loss, duplicate/out-of-order timing samples, reacquisition, reboot, late joins, and 32-bit timer rollover. They do not replace a two-device Wi-Fi/LED timing test.
