# Dynamic Pitlane Garage Order

Dynamically reorders the pitlane garages each season in **F1 25 Career Mode** according to **Constructors' Championship standings**.

---

## Overview

In real life, the garage order is based on the championship order of the year before. But in vanilla F1 25, this does not happen: even if McLaren finishes P10 or wins the championship, garage assignments remain static.

**Dynamic Pitlane Garage Order** fixes this: pitlane garages are reassigned based on merit:
- The reigning Constructors' Champion takes **Garage 1**.
- P2 takes **Garage 2**, P3 takes **Garage 3**, and so on.
- Supports Career Mode (Driver Career & My Team) across multiple seasons.
- Supports **Reverse order** to invert the pitlane order.

---

## Features

- **Merit-based garage assignment**: Garage 1 belongs to the constructor champion.
- **Career Mode multi-season support**: Seamlessly tracks points, wins, and countback across seasons.
- **Season 1 realistic baseline**: Puts McLaren in Garage 1, Ferrari in Garage 2, Red Bull in Garage 3, and Mercedes in Garage 4 from day one.
- **Reverse pitlane order**: Option to invert the order (the reigning champion gets the last garage).
- **Automatic grid capacity scaling**: Gracefully clamps to available track garages (10, 11, or 12 garages) without crashing or reverting to stock order.

---

## Settings in Swofty Launcher

Configure these settings directly in the Swofty Launcher addon settings:

| Setting | Type | Default | Description |
|---|---|---|---|
| **Enable mod** (`enabled`) | Toggle | `On` | Master switch to enable or disable automatic pitlane garage reordering. |
| **Apply in Season 1** (`apply_in_season_1`) | Toggle | `On` | Applies realistic 2024/2025 constructor standings in Season 1. |
| **Reverse garage order** (`reverse_order`) | Toggle | `Off` | Inverts the pitlane garage order (the reigning champion takes the last garage). |
| **Verbose logging** (`verbose`) | Toggle | `On` | Logs team names and assigned garage numbers to `swofty/loader.log`. |

---

## License

MIT License
