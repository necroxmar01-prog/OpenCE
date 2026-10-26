/*
MOD_SWITCHES.H

opence-mods: the armors (d:\armors\<id>.txt) and the weapon, vehicle and HUD
mods (d:\weapons\<id>.txt) turned on or off in the game, from the main
menu's MODS list, below the mods folder's mods (mod_switches.c). The ones
turned off are kept in the saves (u:\mods_off.txt), so a new zip of mods or
opence-mods writing the folders again leaves them off.
*/

#ifndef __MOD_SWITCHES_H
#define __MOD_SWITCHES_H

#include "cseries.h"

enum
{
	_mod_switch_armor,
	_mod_switch_weapon,
	NUMBER_OF_MOD_SWITCH_KINDS
};

/* whether the armor or weapon mod of the id (its file's name, without
.txt) is turned off, as u:\mods_off.txt keeps it (player_armor.c and
weapon_mods.c leave it out) */
boolean mod_switches_off(
	short kind,
	char const *id);

/* the MODS list's (menu_functions.c). What they change is kept only by
mod_switches_save; mod_switches_revert finds the armors and weapon mods
again and takes what the file keeps */
void mod_switches_revert(
	void);
long mod_switches_count(
	void);
/* the mod's name as the list shows it ("Armor: ODST"), UTF-8 */
char const *mod_switches_name(
	long index);
boolean mod_switches_on(
	long index);
void mod_switches_turn(
	long index,
	boolean on);
/* what it is, its file and what it replaces, a line each, into text */
void mod_switches_details(
	long index,
	char *text,
	unsigned long size);
/* keeps the ones turned off; FALSE if the file cannot be written. The
armors are found again (the ARMOR list without the ones turned off); the
weapon mods apply as the next level loads */
boolean mod_switches_save(
	void);

#endif // __MOD_SWITCHES_H
