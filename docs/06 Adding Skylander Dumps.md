# Adding Skylander Dumps

This repository intentionally contains no dump files. Only add 1,024-byte `.sky` files you are authorised to use.

## Built-in library (packaged into SPIFFS)

Add files beneath `esp32\skylandersDumps`, then run a full ESP32 `flash`. The first folder is the game. The scanner recognises the following top-level values:

```text
Spyros Adventure
Giants
Swapforce
Trap Team
Superchargers
Imaginators
Items
Adventure Packs
Sidekicks
```

Examples:

```text
Spyros Adventure\Magic\Spyro.sky
Giants\Giants\Tree Rex.sky
Trap Team\Dark\Blackout.sky
Trap Team\Traps\Fire\Fire Trap.sky
Superchargers\Water\Vehicles\Reef Ripper.sky
Imaginators\Creation Crystals\Fire Crystal.sky
Items\Trap Team\Item.sky
Adventure Packs\Spyros Adventure\Pack.sky
Sidekicks\Giants\Sidekick.sky
```

Standard elements are Magic, Water, Fire, Life, Undead, Earth, Air, Tech, Light, Dark, and Kaos. `Alternative Types` is detected as a variant grouping. The index reads paths and metadata only; it does not need to load every dump during browsing.

Only `.sky` files are staged into SPIFFS. Images and `.gitkeep` files are ignored. Do not commit dumps: the root `.gitignore` excludes them.

## User Added

Use the **User Added** section in the WebUI to upload a single dump at runtime. `app-flash` preserves it; full `flash` can replace it. Download important runtime files before a full flash.
