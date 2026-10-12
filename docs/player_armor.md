# The campaign Spartan's armor

Settings > Change Color has an ARMOR button: a list of armors for the
campaign's Spartan, as Change Color lists the colours. The armor is chosen
before a mission and worn when it starts (`port/linux/game/player_armor.c`).

## What an armor is

- a unit to spawn as: a biped tag, in place of the globals' player unit;
- a permutation of each region of its model it names (a helmet, a chest
  piece), set with `object_permute_region`;
- the colour it wears: the profile's (Change Color's) or its tag's, which
  fixes the campaign Spartan's green.

Two are the game's own, first in the list:

| Armor | What it is |
| --- | --- |
| MARK V | The level's Spartan, as the game has him. The default: the game is unchanged until another is chosen. |
| MARK V (YOUR COLOR) | The level's Spartan in the profile's colour. |

The others are files in the data root's `armors` folder (`d:\armors\<id>.txt`,
beside `maps`), read again each time the list opens. A mod puts them there,
with the maps that hold their tags. Each file is lines of `key = value`;
`#` or `;` starts a comment:

```
name = ODST
description = Halo 3's helljumper armor.
biped = halo3\characters\odst\odst
color = profile
permutation = helmet odst
```

| Key | Meaning |
| --- | --- |
| `name` | As the list shows it (UTF-8). Without one, the file's name. |
| `description` | Shown beside the list. `\n` is a line break. |
| `biped` | The biped tag to spawn as. Without one, the level's Spartan. |
| `color` | `profile` (the default) or `tag`. |
| `permutation` | `<region> <permutation>` of the unit's model, up to 8 lines. |
| `addon` | An addon that brings the biped to every campaign level (`d:\addons\<addon>.map`): [addons.md](addons.md). |

## Pieces

An armor can have pieces chosen one by one, as Halo 3's helmet, chest and
shoulders are. Each piece is a region of the biped's model, and each of its
options a permutation of that region. ARMOR's PIECES button opens them for
the armor on the list's row, and wears it. A row shows each piece with the
option worn ("HELMET: EVA"); choosing one lists its options.

```
piece = helmet HELMET
option = helmet mark_vi MARK VI
option = helmet eva EVA
follow = body chest
```

| Key | Meaning |
| --- | --- |
| `piece` | `<region> <name>`: a piece, up to 6. |
| `option` | `<region> <permutation> <name>`: an option of the piece, up to 16 each. |
| `follow` | `<region> <piece's region>`: a region worn as that piece is (the body with the chest). |

The option worn is the one chosen, else the one the armor's `permutation`
line names for the region, else the first. The choices are kept with the
armor chosen, in `u:\armor.txt`, a line each (`piece <armor id> <region>
<permutation>`), for every armor.

opence-mods makes such an armor of a pack of several armors of one skeleton
(halo3 spartans tags): its models cut by bone into one model with a region
per piece and a permutation per armor (`armor_kit.py`).

An armor whose biped the loaded level has not got is listed as `(NOT HERE)`,
and the level's Spartan spawns instead (`debug.txt` says so). The armor's
tags must be in the level's cache file, or come with an addon, which the
game grafts onto each campaign level as it loads ([addons.md](addons.md)).

## When it applies

- When a local player spawns in a campaign level (an Xbox level or a
  Custom Edition singleplayer map) played on this machine alone, split
  screen co-op included (`players.c`'s `player_spawn`).
- Chosen while a mission is under way (the pause menu's SETTINGS reaches
  Change Color too), it is kept for the next mission or spawn: the unit in
  play is left as it is.

Not in multiplayer, whose units are the game engine's (the globals'
multiplayer unit), nor in network co-op, whose other machines would not know
of it.

The choice is this machine's, for every profile, kept in the saves as the
armor's id (`u:\armor.txt`).

## Limits

- A level's cinematics animate their own objects, and some scripts play
  custom animations on the player's unit: a biped with another animation
  graph than the Spartan's (`characters\cyborg\cyborg`) may not have them.
