/*
WEAPON_MODS.C

Weapon mods: a weapon from a mod (halomaps.org's weapon tags, built as an
addon, docs/addons.md) in place of one of the game's in every campaign
level: the one the player picks up, starts with and the AI carries. Each
is a file of the data root's weapons folder, d:\weapons\<id>.txt, which
opence-mods writes for the weapon the player chose it to replace:

	name = Halo 3 assault rifle
	addon = cad-assault-rifle
	root = weapons\cad assault rifle\assault rifle.weapon
	replace = weapons\assault rifle\assault rifle.weapon -> weapons\cad assault rifle\assault rifle.weapon

addon: the addon (d:\addons\<addon>.map) that holds the mod's tags; root:
the tag of it grafted onto the level with all it needs (addon_tags.c);
replace: one of the level's tags whose data the mod's stands in for (up to
four: a weapon, or a first-person model and its animations; a vehicle; a
HUD's tags, in several files of one addon, whose root "*" grafts every tag
of it). The level's
tag keeps its index, so everything that names it (the scenario's weapons,
the AI's, the starting profiles, saved games) gets the mod's instead.

A mod whose game tag is not in the level (a level without that weapon) is
left out of it. A campaign level on this machine alone; in network co-op the
host's weapon mods, which its players get from it (network_addons.c).
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"

#include "weapon_mods.h"
#include "mod_switches.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

#define WEAPON_MODS_FOLDER "d:\\weapons\\"

static struct
{
	char const *extension;
	unsigned long group_tag;
} const weapon_mods_groups[] =
{
	{ ".weapon", 'weap' },
	{ ".model", 'mode' },
	{ ".gbxmodel", 'mod2' },
	{ ".model_animations", 'antr' },
	{ ".projectile", 'proj' },
	{ ".equipment", 'eqip' },
	{ ".damage_effect", 'jpt!' },
	{ ".effect", 'effe' },
	{ ".sound", 'snd!' },
	{ ".bitmap", 'bitm' },
	{ ".weapon_hud_interface", 'wphi' },
	/* (a vehicle's, and a HUD's: the units', grenades' and numbers' HUD
	tags, and its sounds) */
	{ ".vehicle", 'vehi' },
	{ ".unit_hud_interface", 'unhi' },
	{ ".grenade_hud_interface", 'grhi' },
	{ ".hud_number", 'hud#' },
	{ ".sound_looping", 'lsnd' },
};

static char *weapon_mods_trim(
	char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
		text++;
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = 0;
	return text;
}

static void weapon_mods_copy(
	char *destination,
	char const *source,
	size_t size)
{
	strncpy(destination, source, size - 1);
	destination[size - 1] = 0;
}

unsigned long weapon_mods_group_from_extension(
	char const *extension)
{
	short index;

	for (index = 0; index < (short)NUMBEROF(weapon_mods_groups); index++)
	{
		if (!_stricmp(extension, weapon_mods_groups[index].extension))
			return weapon_mods_groups[index].group_tag;
	}
	return 0;
}

/* a tag's name and group from "path\name.extension" (the name without the
extension), FALSE if the extension is not one of a group known here */
static boolean weapon_mods_tag(
	char const *text,
	char *name,
	size_t name_size,
	unsigned long *group_tag)
{
	char copy[WEAPON_MOD_NAME_LENGTH];
	char *extension;

	weapon_mods_copy(copy, text, sizeof(copy));
	extension = strrchr(copy, '.');
	if (!extension || strchr(extension, '\\'))
		return FALSE;
	*group_tag = weapon_mods_group_from_extension(extension);
	if (!*group_tag)
		return FALSE;
	*extension = 0;
	weapon_mods_copy(name, weapon_mods_trim(copy), name_size);
	return name[0] != 0;
}

static boolean weapon_mods_read(
	char const *id,
	struct weapon_mod *mod)
{
	char path[MAX_PATH];
	char line[1024];
	FILE *file;

	csmemset(mod, 0, sizeof(*mod));
	weapon_mods_copy(mod->id, id, sizeof(mod->id));
	csprintf(path, "%s%s.txt", WEAPON_MODS_FOLDER, id);
	file = fopen(path, "r");
	if (!file)
		return FALSE;
	while (fgets(line, sizeof(line), file))
	{
		char *key = weapon_mods_trim(line);
		char *value = strchr(key, '=');

		if (!*key || *key == '#' || *key == ';' || !value)
			continue;
		*value++ = 0;
		key = weapon_mods_trim(key);
		value = weapon_mods_trim(value);
		if (!_stricmp(key, "addon"))
		{
			weapon_mods_copy(mod->addon, value, sizeof(mod->addon));
		}
		else if (!_stricmp(key, "root"))
		{
			/* ("*": every tag of the addon, a HUD's, addon_graft.c) */
			if (!strcmp(value, "*"))
			{
				weapon_mods_copy(mod->root, "*", sizeof(mod->root));
				mod->root_group = 0;
			}
			else if (!weapon_mods_tag(value, mod->root, sizeof(mod->root), &mod->root_group))
				error(_error_silent, "weapon %s: root '%s' is not a tag with an extension", id, value);
		}
		else if (!_stricmp(key, "replace"))
		{
			char *arrow = strstr(value, "->");
			struct weapon_mod_replacement *replacement;
			unsigned long mod_group;

			if (!arrow || mod->replacement_count >= WEAPON_MOD_MAXIMUM_REPLACEMENTS)
			{
				error(_error_silent, "weapon %s: ignoring replace '%s' (want: replace = <game tag> -> <mod tag>)", id,
					value);
				continue;
			}
			*arrow = 0;
			replacement = &mod->replacements[mod->replacement_count];
			if (!weapon_mods_tag(weapon_mods_trim(value), replacement->stock, sizeof(replacement->stock),
					&replacement->group_tag) ||
				!weapon_mods_tag(weapon_mods_trim(arrow + 2), replacement->mod, sizeof(replacement->mod), &mod_group) ||
				mod_group != replacement->group_tag)
			{
				error(_error_silent, "weapon %s: ignoring replace of '%s': two tags of one group, with extensions", id,
					value);
				continue;
			}
			mod->replacement_count++;
		}
		else if (_stricmp(key, "name") && _stricmp(key, "description"))
		{
			error(_error_silent, "weapon %s: unknown key '%s'", id, key);
		}
	}
	fclose(file);
	if (!mod->addon[0] || !mod->root[0] || !mod->replacement_count)
	{
		error(_error_silent, "weapon %s: wants addon, root and replace; left out", id);
		return FALSE;
	}
	return TRUE;
}

short weapon_mods_for_level(
	struct weapon_mod *mods,
	short maximum)
{
	if (game_engine_running() || game_connection() != _game_connection_local)
		return 0;
	return weapon_mods_read_all(mods, maximum);
}

short weapon_mods_read_all(
	struct weapon_mod *mods,
	short maximum)
{
	WIN32_FIND_DATAA data;
	HANDLE find;
	short count = 0;

	/* (a handle of its own, not find_files_start: custom_edition_maps.c has
	why) */
	find = FindFirstFileA(WEAPON_MODS_FOLDER "*.*", &data);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		char id[sizeof(data.cFileName) + 1];
		char *extension;

		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		csmemcpy(id, data.cFileName, sizeof(data.cFileName));
		id[sizeof(data.cFileName)] = 0;
		extension = strrchr(id, '.');
		if (!extension || _stricmp(extension, ".txt"))
			continue;
		*extension = 0;
		if (strlen(id) >= sizeof(mods[0].id))
		{
			error(_error_silent, "weapon: %s.txt: the name is too long", id);
			continue;
		}
		/* (turned off in the main menu's MODS: mod_switches.c) */
		if (mod_switches_off(_mod_switch_weapon, id))
			continue;
		if (count >= maximum)
		{
			error(_error_silent, "weapon: more than %d weapon mods; %s.txt left out", maximum, id);
			continue;
		}
		if (weapon_mods_read(id, &mods[count]))
			count++;
	} while (FindNextFileA(find, &data));
	CloseHandle(find);
	return count;
}
