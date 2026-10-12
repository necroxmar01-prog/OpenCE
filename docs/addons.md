# Addons: characters and weapons grafted onto the campaign's levels

An armor (docs/player_armor.md) is a biped the player spawns as. The
campaign's levels have only their own bipeds, so an armor from a character
mod would need every level built again with its tags. An *addon* brings
them instead: a small cache file of this build's with the biped and what it
needs, whose tags the game adds to each campaign level's as it loads
(`port/linux/game/addon_tags.c`, `addon_graft.c`).

## The files

| File | What it is |
| --- | --- |
| `d:\addons\<name>.map` | The addon: a cache file for this build (Invader's `-g xbox-ntsc`) whose scenario names the biped in its biped palette |
| `d:\armors\<id>.txt` | The armor: `biped = <tag>` and `addon = <name>` |

```
name = ODST
biped = halo3\characters\odst\odst
addon = odst
color = profile
```

## Loading

When a campaign level loads (`scenario_tags_load`, `cache_files.c`) and the
armor chosen names an addon:

1. **Before the level's tags are read** (`addon_tags_prepare`): the addon is
   read whole (inflated if it is compressed), its tag data copied where a
   map's goes (the tag cache) and checked there by the validator, as any
   map's is (`tag_validate_tags`). Then its tags are moved to memory of
   their own (`XPhysicalAlloc`, the contiguous memory the vertex and index
   buffers must be in) by `addon_graft_relocate`: every pointer into them
   the schemas name (`tag_schema.h`: blocks, data, references' names), the
   tag table's, the buffers', and the models' parts' pointers to the
   buffers, moved by the distance; and the offsets of their pixels and
   samples in the addon's file moved by `ADDON_GRAFT_FILE_BASE`
   (0x40000000), past any map's own. A cache file's bitmap is read at its
   own pixels' offset (`texture_cache_start_loading_bitmap`): each bitmap's
   is moved, not the group's pixel data offset, which the tools leave 0.
2. **Once the level's tags are loaded and checked** (`addon_tags_loaded`):
   the biped and every tag it needs, walked through the schemas' references
   and tag indices (`addon_graft_tags`), are added after the level's tags,
   with handles past the level's. A tag the level has, of the same group and
   name, stands for the addon's (the Spartan's animations, effects and
   sounds, which character mods reuse), so only what is new is added: for
   the ODST of the sample pack, 21 tags. Every reference and tag index of the
   tags added is then checked to name a tag of the new table of a group it
   takes (`addon_graft_references`), and the addon's vertex and index
   buffers are registered as a map's are. The menus' tags
   (`menu_tags_loaded`) are added after.
3. **Reads** of the addon's pixels and samples (`cache_file_read` with an
   offset past `ADDON_GRAFT_FILE_BASE`) are served from the file kept in
   memory, at once (`addon_tags_read`).
4. **When the level goes** (`scenario_tags_unload`), after the texture and
   sound caches let go of the addon's bitmaps and sounds, its buffers,
   memory and table go too.

Anything that fails (no file, not a cache file of this build, refused by
the validator, a tag needed of a group the schemas do not know) is logged
in `debug.txt` and the level loads as it is: the armor's biped is then not
in it, and the level's Spartan spawns (`player_armor.c`).

Only campaign levels (the header's map type 0) get an addon; a level loaded
for multiplayer, or a Custom Edition map (loaded by its own loader), does
not.

## Checking an addon

`build/linux/addon_check` (`tools/addon_check.c`) runs the graft on map
files as the game does, with the game's own validator and graft code:

```sh
build/linux/addon_check maps/c20.map addons/odst.map 'halo3\characters\odst\odst'
```

It checks the addon in the tag cache, relocated, and grafted onto the
level, and lists the tags added and where their pixels are read from.

## Building one

A character pack made for Halo PC (the Halo Editing Kit's tags, as
halomaps.org has them) is built for this build with Invader:

1. its tags repaired (`invader-bludgeon -T everything`: packs extracted with
   old tools have broken markers and vertices);
2. its `.gbxmodel` (Halo PC's) recovered to JMS (`invader-recover`) and
   compiled as a `.model` (`invader-model -T model`), the objects' model
   references moved to it;
3. each multipurpose map of its model shaders reordered from Halo PC's
   channels (red auxiliary mask, green self-illumination, blue specular,
   alpha color change) to this build's (red specular, green
   self-illumination, blue color change, alpha auxiliary mask), as the
   Custom Edition loader does when it draws them
   (`custom_edition_caches.md`, Channel orders): Invader does not reorder
   them for the Xbox, and without it the character is drawn with patches of
   color and glow. It is compressed as DXT5, whose alpha keeps the mask's
   greys;
4. what it needs that it does not bring (the Spartan's animations, effects,
   sounds, the globals) extracted from one of the player's own campaign
   maps (`invader-extract -r`), which is why the addon is the player's own
   build and is not shared: the level's copies stand for these when it is
   grafted;
5. a scenario with the biped in its palette built with `invader-build -g
   xbox-ntsc`; tags that only the build needs and the graft never adds
   (grenades' effects, menus) may be empty.

## Weapon mods

A weapon mod puts a mod's weapon in place of one of the game's in every
campaign level: the one the player picks up and starts with, and the one
the AI carries. The code is in `port/linux/game/weapon_mods.c`.

Each weapon mod is a file of the data root's weapons folder,
`d:\weapons\<id>.txt`:

```
name = Halo 3 assault rifle
addon = cad-assault-rifle
root = weapons\cad assault rifle\assault rifle.weapon
replace = weapons\assault rifle\assault rifle.weapon -> weapons\cad assault rifle\assault rifle.weapon
```

| Key | What it is |
| --- | --- |
| `addon` | The addon whose tags the weapon mod uses. |
| `root` | The tag grafted from that addon, with everything it needs. |
| `replace` | One of the level's tags whose data the mod's tag stands in for, of the same group, given with extensions. Up to four. |

A pack that brings only a weapon's first-person model and animations gets
two `replace` lines, one for the game weapon's `fp\fp.model` and one for
its `fp\fp.model_animations`. Its addon's root is a weapon that names them.

As a level loads, `addon_tags.c` grafts the addons one after another: the
armor's first, then each weapon mod's. Each addon is grafted onto the table
the one before left, and gets its own range of the file offsets the game
reads (`ADDON_GRAFT_FILE_SPAN` apart, up to 64 MB each). A weapon mod's
addon is grafted only if the level has a tag it replaces.

After the graft, each game tag the weapon mod replaces is pointed at the
mod tag's data. The game tag keeps its index and name, so everything that
names it gets the mod's weapon: the scenario's weapons, the AI's, the
starting profiles and saved games.

Weapon mods apply only to a campaign level on this machine alone. In
network co-op the other machines would run the game's weapon.

## Limits

- One armor at a time, the one chosen, and up to eight weapon mods.
- A weapon mod whose tags have the same names as the game's adds nothing:
  the level's own tags are the ones used.
- A saved game holds the player's unit by its tag handle: loading a saved
  game made with another armor (or another addon) is not supported.
- A biped with another animation graph than the level's Spartan's may lack
  animations the level's scripts and cinematics play on the player.
