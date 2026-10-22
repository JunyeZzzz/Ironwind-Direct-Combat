# Ironwind: Hands-On Combat

> Turn Kenshi combat from issuing orders into taking the blade yourself.

Ironwind is a direct-control combat mod for Kenshi. Press `V` to take control of your current character, move with WASD, fight through an over-the-shoulder or first-person camera, choose attack and guard directions with the mouse, and use `E` for nearby vanilla interactions.

Ironwind does not replace Kenshi with a custom damage system. Your input decides **when to act and which direction to commit to**; Kenshi still decides technique availability, reach, hit detection, blocks, dodges, armour, damage, reactions and XP.

## Highlights

- Direct WASD character movement with a continuous over-the-shoulder/first-person camera.
- Four-direction mouse attacks: hold LMB, choose a direction, release for one committed strike.
- Manual directional guard or player-authorised vanilla AI direction selection while RMB is held.
- Vanilla melee state, attack area, weapon reach, skills, block/dodge rolls, armour, damage and XP.
- Complete hit and miss animations, native stagger ownership, and no fixed-duration reaction overrides.
- Compact two-row, eight-part health display using Kenshi's real medical states and colours.
- Confirmed-hit red X feedback and directional availability arrows, without custom hand/weapon attack glows.
- Nearby character interaction through `E`, using the original context menu.
- Character-centred audio while direct control is active.
- In-game Chinese settings page plus advanced INI calibration.

## Core controls

| Input | Action |
| --- | --- |
| `V` | Toggle Ironwind direct control |
| `W/A/S/D` | Camera-relative movement |
| Hold LMB + move mouse | Select attack direction |
| Release LMB | Commit one strike |
| Hold RMB | Directional guard, or authorise vanilla AI guard direction |
| RMB while aiming | Cancel the uncommitted attack |
| `E` | Interact with the nearby selected character |
| `F` | Switch over-the-shoulder / first person |
| Mouse wheel | Adjust shoulder distance; enter first person at the near end |
| `Z` / `X` | Vanilla Kenshi 1× / 5× game speed |

## Vanilla-first combat

Hostile attacks enter Kenshi's native attack state instead of using a hand-made damage cone. Ironwind does not auto-approach, auto-turn, force a hit or continue an AI combo after your single authorised strike. Neutral hostility is submitted only after the authoritative victim hit path confirms a real hit, so a miss, dodge or block does not provoke a target early.

Heavy hit reactions lock Ironwind input until Kenshi's native stagger/reaction state ends. Light hits are not artificially extended, and heavy reactions are not cancelled by a fixed timer. A missed swing is allowed to finish visually just like a damaging one.

## Current scope

- The current release focuses on melee. Crossbows are not yet covered by Ironwind control and remain vanilla.
- Player-authored active dodge is not currently provided. Vanilla enemy dodges and vanilla defence choices remain intact.
- Holding LMB selects direction but does not yet add a separate character wind-up pose.
- Automatic interaction selection currently focuses on characters rather than buildings or ground objects.
- First-person body hiding uses a character shader and may conflict with mods replacing the same shader.

## Requirements

- Kenshi 1.0.65, 64-bit.
- RE_Kenshi 0.3.5.
- Enable `IronwindCombat.mod` only; do not enable the legacy `WASDCombatPlugin.mod` alongside it.

Ironwind: Hands-On Combat is maintained by **cjy233**. It is based historically on Direct Control for Kenshi v1.5.0 by smokefoolius and remains licensed under GNU GPL v3. Legacy `WASDCombatPlugin.dll/.ini` names are retained only for installation compatibility.

Repository: <https://github.com/JunyeZzzz/Ironwind-Direct-Combat>
