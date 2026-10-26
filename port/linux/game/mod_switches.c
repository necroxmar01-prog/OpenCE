/*
MOD_SWITCHES.C

opence-mods: the armors and the weapon, vehicle and HUD mods turned on or
off in the game (mod_switches.h). The main menu's MODS list shows them below
the mods folder's mods (menu_functions.c), a row each, ON or OFF; OK keeps
the ones turned off in the saves, a line each:

	armor odst
	weapon cad-assault-rifle

An armor turned off is left out of Change Color's ARMOR list (player_armor.c:
whoever wore it spawns as the level's Spartan); a weapon mod turned off is
left out of every level from the next one that loads, and out of what a
network co-op host offers (weapon_mods.c). Their files stay where they are,
so turning one on again needs nothing more. A line of a mod no longer in its
folder is kept, so it is still off if it comes back (a zip of mods exported
again, opence-mods writing the folders again).
*/

#include "cseries.h"
#include "cseries/errors.h"

#include "mod_switches.h"
#include "player_armor.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD_SWITCHES_FILE "u:\\mods_off.txt"
#define MOD_SWITCHES_ID_LENGTH 64
#define MOD_SWITCHES_NAME_LENGTH 160
#define MOD_SWITCHES_TAG_LENGTH 256
#define MAXIMUM_MOD_SWITCHES 128
#define MAXIMUM_MODS_OFF 256

static char const *const mod_switch_folders[NUMBER_OF_MOD_SWITCH_KINDS] = { "d:\\armors\\", "d:\\weapons\\" };
static char const *const mod_switch_words[NUMBER_OF_MOD_SWITCH_KINDS] = { "armor", "weapon" };

struct mod_off
{
	short kind;
	char id[MOD_SWITCHES_ID_LENGTH];
};

struct mod_switch
{
	short kind;
	char id[MOD_SWITCHES_ID_LENGTH];
	/* as the list shows it: what it is, then its name */
	char name[MOD_SWITCHES_NAME_LENGTH];
	/* "Armor", "Weapon", "Vehicle" or "HUD" */
	char const *what;
	/* the game's tag a weapon mod replaces first (else empty) */
	char replaces[MOD_SWITCHES_TAG_LENGTH];
	boolean on;
};

static struct
{
	/* what the file keeps (read the first time it is asked) */
	boolean off_read;
	short off_count;
	struct mod_off off[MAXIMUM_MODS_OFF];
	/* the MODS list's, as mod_switches_revert found them */
	long count;
	struct mod_switch switches[MAXIMUM_MOD_SWITCHES];
} mod_switches_globals;

/* ---------- private code */

static char *mod_switches_trim(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
		text++;
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = 0;
	return text;
}

static void mod_switches_copy(char *destination, char const *source, size_t size)
{
	strncpy(destination, source, size - 1);
	destination[size - 1] = 0;
}

static void mod_switches_off_read(void)
{
	char line[256];
	FILE *file;

	mod_switches_globals.off_read = TRUE;
	mod_switches_globals.off_count = 0;
	file = fopen(MOD_SWITCHES_FILE, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file) && mod_switches_globals.off_count < MAXIMUM_MODS_OFF)
	{
		char *text = mod_switches_trim(line);
		char *space = strchr(text, ' ');
		short kind;

		if (!*text || *text == '#' || !space)
			continue;
		*space = 0;
		space = mod_switches_trim(space + 1);
		for (kind = 0; kind < NUMBER_OF_MOD_SWITCH_KINDS; kind++)
		{
			if (!_stricmp(text, mod_switch_words[kind]) && *space && strlen(space) < MOD_SWITCHES_ID_LENGTH)
			{
				struct mod_off *off = &mod_switches_globals.off[mod_switches_globals.off_count++];

				off->kind = kind;
				mod_switches_copy(off->id, space, sizeof(off->id));
				break;
			}
		}
	}
	fclose(file);
}

/* whether the file keeps it off (as read last) */
static boolean mod_switches_kept_off(short kind, char const *id)
{
	short index;

	for (index = 0; index < mod_switches_globals.off_count; index++)
	{
		if (mod_switches_globals.off[index].kind == kind && !_stricmp(mod_switches_globals.off[index].id, id))
			return TRUE;
	}
	return FALSE;
}

/* the name and what a mod's file says it replaces */
static void mod_switches_describe(struct mod_switch *mod)
{
	char path[MAX_PATH];
	char line[1024];
	char name[MOD_SWITCHES_NAME_LENGTH] = "";
	boolean hud = FALSE;
	boolean vehicle = FALSE;
	FILE *file;

	mod->replaces[0] = 0;
	csprintf(path, "%s%s.txt", mod_switch_folders[mod->kind], mod->id);
	file = fopen(path, "r");
	if (file)
	{
		while (fgets(line, sizeof(line), file))
		{
			char *key = mod_switches_trim(line);
			char *value = strchr(key, '=');

			if (!*key || *key == '#' || *key == ';' || !value)
				continue;
			*value++ = 0;
			key = mod_switches_trim(key);
			value = mod_switches_trim(value);
			if (!_stricmp(key, "name") && !name[0])
			{
				mod_switches_copy(name, value, sizeof(name));
			}
			else if (!_stricmp(key, "root") && !strcmp(value, "*"))
			{
				hud = TRUE;
			}
			else if (!_stricmp(key, "replace") && !mod->replaces[0])
			{
				char *arrow = strstr(value, "->");
				char const *extension;

				if (arrow)
					*arrow = 0;
				mod_switches_copy(mod->replaces, mod_switches_trim(value), sizeof(mod->replaces));
				extension = strrchr(mod->replaces, '.');
				vehicle = extension && !_stricmp(extension, ".vehicle");
			}
		}
		fclose(file);
	}
	if (!name[0])
		mod_switches_copy(name, mod->id, sizeof(name));
	mod->what = mod->kind == _mod_switch_armor ? "Armor" : hud ? "HUD" : vehicle ? "Vehicle" : "Weapon";
	csprintf(mod->name, "%s: %.140s", mod->what, name);
}

static int mod_switches_compare(void const *a, void const *b)
{
	struct mod_switch const *first = a;
	struct mod_switch const *second = b;

	if (first->kind != second->kind)
		return first->kind - second->kind;
	return _stricmp(first->name, second->name);
}

static void mod_switches_scan(short kind)
{
	char pattern[MAX_PATH];
	WIN32_FIND_DATAA data;
	HANDLE find;

	/* (a handle of its own, not find_files_start: custom_edition_maps.c has
	why) */
	csprintf(pattern, "%s*.*", mod_switch_folders[kind]);
	find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char id[sizeof(data.cFileName) + 1];
		char *extension;
		struct mod_switch *mod;

		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		csmemcpy(id, data.cFileName, sizeof(data.cFileName));
		id[sizeof(data.cFileName)] = 0;
		extension = strrchr(id, '.');
		if (!extension || _stricmp(extension, ".txt"))
			continue;
		*extension = 0;
		if (!id[0] || strlen(id) >= MOD_SWITCHES_ID_LENGTH || mod_switches_globals.count >= MAXIMUM_MOD_SWITCHES)
			continue;
		mod = &mod_switches_globals.switches[mod_switches_globals.count++];
		csmemset(mod, 0, sizeof(*mod));
		mod->kind = kind;
		mod_switches_copy(mod->id, id, sizeof(mod->id));
		mod_switches_describe(mod);
		mod->on = !mod_switches_kept_off(kind, id);
	} while (FindNextFileA(find, &data));
	CloseHandle(find);
}

static struct mod_switch *mod_switches_get(long index)
{
	return index >= 0 && index < mod_switches_globals.count ? &mod_switches_globals.switches[index] : NULL;
}

/* whether the list found a file of it */
static boolean mod_switches_listed(short kind, char const *id)
{
	long index;

	for (index = 0; index < mod_switches_globals.count; index++)
	{
		if (mod_switches_globals.switches[index].kind == kind && !_stricmp(mod_switches_globals.switches[index].id, id))
			return TRUE;
	}
	return FALSE;
}

/* ---------- public code */

boolean mod_switches_off(
	short kind,
	char const *id)
{
	if (!mod_switches_globals.off_read)
		mod_switches_off_read();
	return mod_switches_kept_off(kind, id);
}

void mod_switches_revert(
	void)
{
	short kind;

	mod_switches_off_read();
	mod_switches_globals.count = 0;
	for (kind = 0; kind < NUMBER_OF_MOD_SWITCH_KINDS; kind++)
		mod_switches_scan(kind);
	qsort(mod_switches_globals.switches, mod_switches_globals.count, sizeof(mod_switches_globals.switches[0]),
		mod_switches_compare);
}

long mod_switches_count(
	void)
{
	return mod_switches_globals.count;
}

char const *mod_switches_name(
	long index)
{
	struct mod_switch const *mod = mod_switches_get(index);

	return mod ? mod->name : "";
}

boolean mod_switches_on(
	long index)
{
	struct mod_switch const *mod = mod_switches_get(index);

	return mod && mod->on;
}

void mod_switches_turn(
	long index,
	boolean on)
{
	struct mod_switch *mod = mod_switches_get(index);

	if (mod)
		mod->on = on;
}

void mod_switches_details(
	long index,
	char *text,
	unsigned long size)
{
	struct mod_switch const *mod = mod_switches_get(index);
	char const *file_name;

	if (!size)
		return;
	text[0] = 0;
	if (!mod)
		return;
	file_name = strrchr(mod_switch_folders[mod->kind], ':');
	if (mod->kind == _mod_switch_armor)
	{
		snprintf(text, size, "An armor of opence-mods.\n%s: Change Color > ARMOR %s.\nFile: %s%s.txt",
			mod->on ? "On" : "Off", mod->on ? "lists it" : "leaves it out (whoever wore it is the level's Spartan)",
			file_name ? file_name + 2 : "", mod->id);
	}
	else
	{
		char const *replaced = strrchr(mod->replaces, '\\');
		char replaces[MOD_SWITCHES_TAG_LENGTH + 16] = "";

		/* (the game's tag by its file's name: "assault rifle.weapon") */
		if (mod->replaces[0] && strcmp(mod->what, "HUD"))
			snprintf(replaces, sizeof(replaces), "Replaces: %s\n", replaced ? replaced + 1 : mod->replaces);
		snprintf(text, size, "A %s mod of opence-mods.\n%s%s from the next mission that loads.\nFile: %s%s.txt",
			!strcmp(mod->what, "HUD") ? "HUD" : !strcmp(mod->what, "Vehicle") ? "vehicle" : "weapon", replaces, mod->on ? "Played" : "Left out", file_name ? file_name + 2 : "", mod->id);
	}
}

boolean mod_switches_save(
	void)
{
	FILE *file = fopen(MOD_SWITCHES_FILE, "w");
	short index;
	long position;

	if (!file)
	{
		error(_error_silent, "mods: could not write %s", MOD_SWITCHES_FILE);
		return FALSE;
	}
	fprintf(file, "# the armors and weapon mods turned off in the game (MODS)\n");
	/* (the ones whose files are gone, still off if they come back) */
	for (index = 0; index < mod_switches_globals.off_count; index++)
	{
		struct mod_off const *off = &mod_switches_globals.off[index];

		if (!mod_switches_listed(off->kind, off->id))
			fprintf(file, "%s %s\n", mod_switch_words[off->kind], off->id);
	}
	for (position = 0; position < mod_switches_globals.count; position++)
	{
		struct mod_switch const *mod = &mod_switches_globals.switches[position];

		if (!mod->on)
			fprintf(file, "%s %s\n", mod_switch_words[mod->kind], mod->id);
	}
	fclose(file);
	mod_switches_off_read();
	player_armor_catalog_load();
	return TRUE;
}
