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
