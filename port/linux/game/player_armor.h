/*
PLAYER_ARMOR.H

The campaign Spartan's armor, chosen in the menus (Settings > Change Color >
ARMOR, before a mission): player_armor.c.
*/

#ifndef __PLAYER_ARMOR_H
#define __PLAYER_ARMOR_H

/* how much of the mods' patches this build has, in its own words (the log's
armor line, and the executable: opence-mods reads it to tell an older
patched build, which lacks what newer mods need, from this one). One more
with each patch to the mods' code. */
#define PLAYER_ARMOR_PATCH_TEXT "opence-mods patch 29"

/* what an armor puts on a player's unit: the biped (empty: the map's player
unit), whether it wears the profile's colour, and the permutation of each
region of its model (its own, and the pieces chosen). A network co-op
player's travels to the host this way (network_coop_armor.c). */
enum
{
	PLAYER_ARMOR_DRESS_REGIONS = 16,
	PLAYER_ARMOR_DRESS_BIPED = 256,
	PLAYER_ARMOR_DRESS_NAME = 32,
};

struct player_armor_dress
{
	char biped[PLAYER_ARMOR_DRESS_BIPED];
	byte profile_color;
	byte region_count;
	byte pad[2];
	struct
	{
		char region[PLAYER_ARMOR_DRESS_NAME];
		char permutation[PLAYER_ARMOR_DRESS_NAME];
	} regions[PLAYER_ARMOR_DRESS_REGIONS];
};

/* the armors: the game's own two, then those of the data root's armors
folder (d:\armors\*.txt), read again by player_armor_catalog_load */
void player_armor_catalog_load(void);
short player_armor_count(void);
void player_armor_name(short armor, wchar_t *text, short length);
void player_armor_description(short armor, wchar_t *text, short length);
/* whether the loaded campaign level has the armor's biped (TRUE outside
one, as in the main menu: it is checked when a level loads) */
boolean player_armor_available(short armor);

/* an armor's pieces (Halo 3's helmet, chest, shoulders: 0 if it has none),
each piece's name, its options and their names, the one worn, and one
chosen (kept in u:\armor.txt, for that armor) */
short player_armor_piece_count(short armor);
void player_armor_piece_name(short armor, short piece, wchar_t *text, short length);
short player_armor_option_count(short armor, short piece);
void player_armor_option_name(short armor, short piece, short option, wchar_t *text, short length);
short player_armor_option_worn(short armor, short piece);
void player_armor_option_choose(short armor, short piece, short option);

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

/* (player_spawn, players.c) the same for any player: in network co-op,
the host spawns each in the armor its machine chose (network_addons.c) */
long player_armor_player_unit_definition(long player_index, short local_player_index, long default_definition_index);
void player_armor_player_unit_created(long player_index, short local_player_index, long unit_index);

/* the armor this machine chose, as a dress (network co-op sends it to the
host: network_addons.c) */
void player_armor_local_dress(struct player_armor_dress *dress);
/* (network co-op's host) the addons a player may wear: the armor chosen's,
then those of armors of pieces; their bipeds */
short player_armor_network_addons(char (*addons)[PLAYER_ARMOR_DRESS_NAME + 16],
	char (*bipeds)[PLAYER_ARMOR_DRESS_BIPED], short maximum);
/* (network co-op's host) the addon of this machine's armor of the biped (one
a client's player wears), else FALSE */
boolean player_armor_addon_of_biped(char const *biped, char *addon, long size);

/* (game_state.c, a saved game loaded) its local players to be put in the
armor chosen, by player_armor_update (players.c, each tick) once they can be */
void player_armor_game_state_loaded(void);
void player_armor_update(void);

#endif
