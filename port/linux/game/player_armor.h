/*
PLAYER_ARMOR.H

The campaign Spartan's armor, chosen in the menus (Settings > Change Color >
ARMOR, before a mission): player_armor.c.
*/

#ifndef __PLAYER_ARMOR_H
#define __PLAYER_ARMOR_H

/* the armors: the game's own two, then those of the data root's armors
folder (d:\armors\*.txt), read again by player_armor_catalog_load */
void player_armor_catalog_load(void);
short player_armor_count(void);
void player_armor_name(short armor, wchar_t *text, short length);
void player_armor_description(short armor, wchar_t *text, short length);
/* whether the loaded campaign level has the armor's biped (TRUE outside
one, as in the main menu: it is checked when a level loads) */
boolean player_armor_available(short armor);

/* the armor chosen, kept in the saves (u:\armor.txt) */
short player_armor_selected(void);
void player_armor_select(short armor);

/* (addon_tags.c, as a level loads) the addon of the armor chosen and the
biped it brings, if it has one */
boolean player_armor_addon(char *addon, long addon_size, char *biped, long biped_size);

/* (player_spawn, players.c) the unit a local campaign player spawns as,
given the map's own, and what the armor changes on it once made */
long player_armor_unit_definition(short local_player_index, long default_definition_index);
void player_armor_unit_created(short local_player_index, long unit_index);

/* (game_state.c, a saved game loaded) its local players to be put in the
armor chosen, by player_armor_update (players.c, each tick) once they can be */
void player_armor_game_state_loaded(void);
void player_armor_update(void);

#endif
