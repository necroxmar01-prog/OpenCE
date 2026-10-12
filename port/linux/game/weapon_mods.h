/*
WEAPON_MODS.H

Weapon mods (weapon_mods.c): a weapon from a mod in place of one of the
game's, in every campaign level, by the files of the data root's weapons
folder (d:\weapons\<id>.txt). addon_tags.c grafts each one's addon and
points the game's tag at the mod's.
*/

#ifndef __WEAPON_MODS_H
#define __WEAPON_MODS_H

#include "cseries.h"

enum
{
	WEAPON_MODS_MAXIMUM = 8,
	WEAPON_MOD_MAXIMUM_REPLACEMENTS = 4,
	WEAPON_MOD_NAME_LENGTH = 256,
};

/* one of the game's tags (stock) whose data the mod's (mod) stands in for */
struct weapon_mod_replacement
{
	unsigned long group_tag;
	char stock[WEAPON_MOD_NAME_LENGTH];
	char mod[WEAPON_MOD_NAME_LENGTH];
};

struct weapon_mod
{
	char id[48];
	/* the addon that brings the mod's tags (d:\addons\<addon>.map), and the
	tag of it grafted with all it needs */
	char addon[48];
	unsigned long root_group;
	char root[WEAPON_MOD_NAME_LENGTH];
	short replacement_count;
	struct weapon_mod_replacement replacements[WEAPON_MOD_MAXIMUM_REPLACEMENTS];
};

/* the weapon mods of the weapons folder that apply to the level loading (a
campaign level on this machine alone, not network co-op: a host does not
know another machine's weapons), in mods (room for maximum); how many */
short weapon_mods_for_level(
	struct weapon_mod *mods,
	short maximum);

/* a tag group for a file extension (".weapon": 'weap'), or 0 */
unsigned long weapon_mods_group_from_extension(
	char const *extension);

#endif // __WEAPON_MODS_H
