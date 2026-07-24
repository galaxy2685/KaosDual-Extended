# Save Editor

The inspector is read-only until it recognises and validates a supported normal progression-bearing character. It detects encrypted/plaintext representation, checks save areas, chooses the active area, and reports Gold and Level.

## Editing workflow

1. Unload the file from all four portal slots.
2. Choose **Inspect** and confirm both the requested value and save-area validity.
3. Enter Gold or Level and choose Save.
4. The editor changes only the requested field in the active area, recalculates required checksums, encrypts when applicable, writes a temporary file, reopens and verifies it, then replaces the original.
5. It retains one `.bak` backup. Use **Restore Backup** when it is shown.

The game alternates between two save areas, retaining the older one as fallback. Gold storage is unsigned 16-bit (maximum 65,535), but real-game testing indicates the gameplay cap is 65,000.

Traps, Vehicles, Creation Crystals, Items, Adventure Packs, unsupported formats, and unsupported Swap Force bottom halves are not editable. The editor does not claim universal compatibility.
