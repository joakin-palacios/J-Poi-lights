# Pair Sync

Pair Sync turns two WLED devices into an isolated leader/follower pair.

## Provisioning

1. Flash the same firmware and filesystem layout to both devices.
2. Configure the leader's WLED access point SSID and password, then set `PairSync > role` to `Leader`.
3. Configure the follower's first Wi-Fi network with the leader AP credentials and a DHCP address, then set `PairSync > role` to `Follower`.
4. Leave `leaderHost` at `4.3.2.1` unless the leader AP address is customized.
5. Configure a normal WLED button and set `returnButton` to its zero-based index if physical maintenance exit is required.

The leader remains AP-only at `4.3.2.1`. The follower normally remains station-only. Use the Pair Sync entry on the leader's Info page to open the follower's full WLED AP for five minutes. The follower returns early from its Info page or configured physical button.

## Synchronization

- WLED UDP notifier packets provide immediate brightness, color, effect, palette, segment, transition, and timebase updates.
- A revisioned HTTP scene snapshot reconciles missed or incomplete UDP updates without copying device-specific LED, GPIO, Wi-Fi, or MPU configuration.
- Numbered POV files `/1.bmp` through `/255.bmp` are pulled from the leader. Every file is size-checked, SHA-256 verified, BMP-validated, and transactionally replaced before stale follower files are removed.
- Returning from maintenance invalidates the follower's scene and resource revisions, so the leader overwrites maintenance changes.
