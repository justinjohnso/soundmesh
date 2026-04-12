# SoundMesh Pilot Operations Runbook

This document defines the procedures for operating the SoundMesh pilot rollout (April 2026).

## Preflight Checklist
- [ ] **Hardware Power:** Ensure UDA1334 DACs are powered by **5V VIN**. 3.3V will result in low/no audio.
- [ ] **Firmware Match:** All nodes must run the same firmware version (check `schemaVersion` in `/api/status`).
- [ ] **Portal Token:** Ensure you have the admin token (`soundmesh2026`) for control operations.
- [ ] **Channel Check:** Verify no other heavy 2.4GHz traffic is on Channel 11.

## Rollout Flow (Canary)
1. **Root Node (SRC):** Power on the SRC node first. Wait for the Portal AP to appear.
2. **First Canary (OUT):** Power on one OUT node. Verify it joins the mesh (Layer 1).
3. **Audio Check:** Confirm "Crystal Clear" audio from the canary node.
4. **Scale Up:** Power on remaining nodes one by one, verifying each joins correctly.

## Abort Criteria
Immediately power down or disconnect nodes if:
- **Robotic Stutter:** Audio develops a periodic buzz or dropout pattern > 1s duration.
- **Root Disconnect:** The SRC node portal becomes unreachable via USB.
- **High Loss:** `/api/status` reports `loss_pct` > 15% consistently on more than half the nodes.

## Rollback / Reset Procedure
1. **Soft Reset:** Press the onboard RESET button on the affected node.
2. **Factory Reset:** Hold the BOOT button (GPIO 43) for 5 seconds during power-up to clear NVS (if implemented) or re-flash.
3. **Re-flash:** Use `pio run -e src -t upload` or `out` to restore a known-good baseline firmware.

## Escalation
- **Firmware Issues:** Report to the engineering team with a copy of the serial log (USB CDC mirror).
- **Audio Quality:** Document the environment (distance, obstructions) and node count.
