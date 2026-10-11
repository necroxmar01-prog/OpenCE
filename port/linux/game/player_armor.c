/*
PLAYER_ARMOR.C

The campaign Spartan's armor, chosen in the menus: Settings > Change Color >
ARMOR, before a mission starts.

An armor is a unit (a biped tag) to spawn as in place of the map's own
(the globals' player unit), a permutation of each region of its model it
names (a helmet, a chest piece), and whether it wears the profile's colour
(the multiplayer colour, Change Color's) rather than its tag's, which
fixes the campaign Spartan's green. Two are the game's own:

- MARK V: the map's Spartan, as the game has it;
- MARK V (YOUR COLOR): the map's Spartan in the profile's colour.

The others are files in the data root's armors folder (d:\armors\<id>.txt),
which a mod puts there (with the maps whose tags they name). Each is lines
of "key = value", '#' or ';' starting a comment:

    name = ODST
    description = Halo 3's helljumper armor.
    biped = halo3\characters\odst\odst
    color = profile
    permutation = helmet odst

- name: as the list shows it (UTF-8; the file's name without .txt if none);
- description: shown beside the list ('\n' a line break);
- biped: the biped tag to spawn as; none for the map's Spartan. An armor
  whose biped the loaded map has not is listed as not in this map, and the
  map's Spartan spawns instead;
- addon: the addon that brings the biped to every campaign level
  (d:\addons\<addon>.map, a map built with Invader's -g xbox-ntsc whose
  scenario names the biped: addon_tags.c grafts it as the level loads);
- color: "profile" (the default) or "tag";
- permutation: a region of the unit's model and the permutation it shows,
  any number of them (up to PLAYER_ARMOR_MAXIMUM_PERMUTATIONS).

The armor is chosen before a mission, in the menus: it applies when a
local player spawns in a campaign game on this machine alone (players.c's
player_spawn: not in multiplayer, whose units are the game engine's, nor in
network co-op, whose other machines would not know of it). Chosen from the
pause menu, it is kept for the next mission (or the next spawn): the unit
already in play is left as it is.

The choice is the machine's, kept in the saves (u:\armor.txt: the armor's
id), for every profile.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/player_ui.h"
#include "items/weapons.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "saved games/player_profile.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "text/unicode.h"
#include "units/biped_definitions.h"
#include "units/units.h"

#include "player_armor.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define PLAYER_ARMOR_MAXIMUM_ARMORS 32
#define PLAYER_ARMOR_MAXIMUM_PERMUTATIONS 8
#define PLAYER_ARMOR_ID_LENGTH 48
#define PLAYER_ARMOR_NAME_LENGTH 48
#define PLAYER_ARMOR_DESCRIPTION_LENGTH 160
#define PLAYER_ARMOR_TAG_LENGTH 256
#define PLAYER_ARMOR_FOLDER "d:\\armors\\"
#define PLAYER_ARMOR_CHOICE_FILE "u:\\armor.txt"
/* the game's own: the map's Spartan as it is, and in the profile's colour */
#define PLAYER_ARMOR_GAME_ARMORS 2

enum
{
	_player_armor_color_tag = 0,
	_player_armor_color_profile,
};

/* ---------- structures */

struct player_armor_permutation
{
	char region[TAG_STRING_LENGTH + 1];
	char permutation[TAG_STRING_LENGTH + 1];
};

struct player_armor
{
	char id[PLAYER_ARMOR_ID_LENGTH];
	wchar_t name[PLAYER_ARMOR_NAME_LENGTH];
	wchar_t description[PLAYER_ARMOR_DESCRIPTION_LENGTH];
	/* the biped's tag; empty: the map's Spartan */
	char biped[PLAYER_ARMOR_TAG_LENGTH];
	/* the addon that brings the biped to the campaign's levels
	(d:\addons\<addon>.map: addon_tags.c); empty: the levels have it, or not */
	char addon[PLAYER_ARMOR_ID_LENGTH];
	short color;
	short permutation_count;
	struct player_armor_permutation permutations[PLAYER_ARMOR_MAXIMUM_PERMUTATIONS];
};

/* ---------- globals */

static struct
{
	boolean loaded;
	short count;
	struct player_armor armors[PLAYER_ARMOR_MAXIMUM_ARMORS];
	/* the armor chosen (NONE: not read from the saves yet) */
	short selected;
} player_armor_globals = { FALSE, 0, { { { 0 } } }, NONE };

/* ---------- private code */

/* text of the file (UTF-8) as the game's 16-bit text; characters past the
basic plane are '?' */
static void armor_text_from_utf8(char const *source, wchar_t *text, short length)
{
	unsigned char const *byte = (unsigned char const *)source;
	short count = 0;

	while (*byte && count < length - 1)
	{
		unsigned long character = '?';

		if (byte[0] == '\\' && byte[1] == 'n')
		{
			character = '\n';
			byte += 2;
		}
		else if (byte[0] < 0x80)
		{
			character = byte[0];
			byte++;
		}
		else if ((byte[0] & 0xE0) == 0xC0 && (byte[1] & 0xC0) == 0x80)
		{
			character = (unsigned long)(byte[0] & 0x1F) << 6 | (byte[1] & 0x3F);
			byte += 2;
		}
		else if ((byte[0] & 0xF0) == 0xE0 && (byte[1] & 0xC0) == 0x80 && (byte[2] & 0xC0) == 0x80)
		{
			character = (unsigned long)(byte[0] & 0x0F) << 12 | (unsigned long)(byte[1] & 0x3F) << 6 | (byte[2] & 0x3F);
			byte += 3;
		}
		else
		{
			/* (a longer or broken sequence: its bytes skipped) */
			byte++;
			while ((*byte & 0xC0) == 0x80)
				byte++;
		}
		text[count++] = (wchar_t)character;
	}
	text[count] = 0;
}

static void armor_text_from_ascii(char const *source, wchar_t *text, short length)
{
	short count = 0;

	while (source[count] && count < length - 1)
	{
		text[count] = (wchar_t)(unsigned char)source[count];
		count++;
	}
	text[count] = 0;
}

static char *armor_trim(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
		text++;
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = 0;
	return text;
}

static void armor_copy(char *destination, char const *source, size_t size)
{
	strncpy(destination, source, size - 1);
	destination[size - 1] = 0;
}

/* the armor of the file armors/<id>.txt, else FALSE */
static boolean armor_read(char const *id, struct player_armor *armor)
{
	char path[MAX_PATH];
	char line[512];
	boolean named = FALSE;
	FILE *file;

	csmemset(armor, 0, sizeof(*armor));
	armor_copy(armor->id, id, sizeof(armor->id));
	armor->color = _player_armor_color_profile;
	csprintf(path, "%s%s.txt", PLAYER_ARMOR_FOLDER, id);
	file = fopen(path, "r");
	if (!file)
		return FALSE;
	while (fgets(line, sizeof(line), file))
	{
		char *key = armor_trim(line);
		char *value = strchr(key, '=');

		if (!*key || *key == '#' || *key == ';' || !value)
			continue;
		*value++ = 0;
		key = armor_trim(key);
		value = armor_trim(value);
		if (!_stricmp(key, "name"))
		{
			armor_text_from_utf8(value, armor->name, PLAYER_ARMOR_NAME_LENGTH);
			named = TRUE;
		}
		else if (!_stricmp(key, "description"))
		{
			armor_text_from_utf8(value, armor->description, PLAYER_ARMOR_DESCRIPTION_LENGTH);
		}
		else if (!_stricmp(key, "biped"))
		{
			armor_copy(armor->biped, value, sizeof(armor->biped));
		}
		else if (!_stricmp(key, "addon"))
		{
			armor_copy(armor->addon, value, sizeof(armor->addon));
		}
		else if (!_stricmp(key, "color"))
		{
			armor->color = (short)(_stricmp(value, "tag") ? _player_armor_color_profile : _player_armor_color_tag);
		}
		else if (!_stricmp(key, "permutation"))
		{
			char *space = strpbrk(value, " \t");

			if (space && armor->permutation_count < PLAYER_ARMOR_MAXIMUM_PERMUTATIONS)
			{
				struct player_armor_permutation *permutation = &armor->permutations[armor->permutation_count++];

				*space++ = 0;
				armor_copy(permutation->region, value, sizeof(permutation->region));
				armor_copy(permutation->permutation, armor_trim(space), sizeof(permutation->permutation));
			}
			else
			{
				error(_error_silent, "armor %s: ignoring permutation '%s' (want: permutation = <region> <permutation>)",
					id, value);
			}
		}
		else
		{
			error(_error_silent, "armor %s: unknown key '%s'", id, key);
		}
	}
	fclose(file);
	if (!named)
		armor_text_from_ascii(id, armor->name, PLAYER_ARMOR_NAME_LENGTH);
	return TRUE;
}

static int armor_compare(void const *a, void const *b)
{
	return _stricmp(((struct player_armor const *)a)->id, ((struct player_armor const *)b)->id);
}

static void armor_game_armors(void)
{
	struct player_armor *armor = &player_armor_globals.armors[0];

	csmemset(armor, 0, sizeof(*armor) * PLAYER_ARMOR_GAME_ARMORS);
	armor_copy(armor[0].id, "mark_v", sizeof(armor[0].id));
	armor_text_from_ascii("MARK V", armor[0].name, PLAYER_ARMOR_NAME_LENGTH);
	armor_text_from_ascii("The Spartan as the level has\nhim, in his own colors.", armor[0].description,
		PLAYER_ARMOR_DESCRIPTION_LENGTH);
	armor[0].color = _player_armor_color_tag;
	armor_copy(armor[1].id, "mark_v_color", sizeof(armor[1].id));
	armor_text_from_ascii("MARK V (YOUR COLOR)", armor[1].name, PLAYER_ARMOR_NAME_LENGTH);
	armor_text_from_ascii("The level's Spartan, in this\nprofile's color (Change Color).", armor[1].description,
		PLAYER_ARMOR_DESCRIPTION_LENGTH);
	armor[1].color = _player_armor_color_profile;
}

static void armor_choice_read(void)
{
	char id[PLAYER_ARMOR_ID_LENGTH] = "";
	FILE *file = fopen(PLAYER_ARMOR_CHOICE_FILE, "r");
	short armor;

	player_armor_globals.selected = 0;
	if (!file)
		return;
	if (fgets(id, sizeof(id), file))
	{
		char *trimmed = armor_trim(id);

		for (armor = 0; armor < player_armor_globals.count; armor++)
		{
			if (!_stricmp(player_armor_globals.armors[armor].id, trimmed))
				player_armor_globals.selected = armor;
		}
	}
	fclose(file);
}

static void armor_choice_write(void)
{
	FILE *file = fopen(PLAYER_ARMOR_CHOICE_FILE, "w");

	if (!file)
	{
		error(_error_silent, "armor: could not write %s", PLAYER_ARMOR_CHOICE_FILE);
		return;
	}
	fprintf(file, "%s\n", player_armor_globals.armors[player_armor_globals.selected].id);
	fclose(file);
}

static void armor_catalog_ensure(void)
{
	if (!player_armor_globals.loaded)
		player_armor_catalog_load();
}

static struct player_armor const *armor_get(short armor)
{
	armor_catalog_ensure();
	if (armor < 0 || armor >= player_armor_globals.count)
		armor = 0;
	return &player_armor_globals.armors[armor];
}

/* the armor's biped in the loaded map, else NONE (the map's Spartan) */
static long armor_biped(struct player_armor const *armor)
{
	return armor->biped[0] ? tag_loaded(BIPED_DEFINITION_TAG, armor->biped) : NONE;
}

/* whether a campaign level is loaded (a Custom Edition one too), not the
main menu's map nor a multiplayer one */
static boolean armor_campaign_map(void)
{
	return game_map_loaded() && global_scenario_index != NONE && global_scenario_get()->type == _scenario_type_solo;
}

/* whether this machine plays a campaign game of its own: armors apply */
static boolean armor_applies(short local_player_index)
{
	return local_player_index != NONE && armor_campaign_map() && !game_engine_running() &&
		game_connection() == _game_connection_local;
}

/* the colour of the local player's profile (the one being edited, for
player 1, while Settings is open), else FALSE: none chosen */
static boolean armor_profile_color(short local_player_index, real_rgb_color *color)
{
	struct player_profile *edited = player_ui_get_edit_player_profile();
	struct player_profile profile;
	short color_index;

	if (edited && local_player_index == 0)
	{
		color_index = edited->primary_color_index;
	}
	else
	{
		long profile_index = player_ui_get_active_player_profile_index(local_player_index);

		if (profile_index == NONE || !player_profile_get(profile_index, &profile))
			return FALSE;
		color_index = profile.primary_color_index;
	}
	if (color_index == NONE || color_index >= (short)player_profile_number_of_available_primary_colors())
		return FALSE;
	*color = player_profile_get_rgb_color(color_index);
	return TRUE;
}

/* the unit painted in the colour, as network co-op paints its players
(players.c's players_coop_color_unit): the tag's colours a permutation
chooses, not those a function drives (the shield's glow) */
static void armor_color_unit(long unit_index, real_rgb_color const *color)
{
	struct object_datum *object = object_get(unit_index);
	struct object_definition *definition = object_definition_get(object->definition_index);
	short index;

	for (index = 0; index < definition->object.change_colors.count && index < NUMBER_OF_OBJECT_CHANGE_COLORS; index++)
	{
		struct object_change_color_definition *change_color = TAG_BLOCK_GET_ELEMENT(
			&definition->object.change_colors, index, struct object_change_color_definition);

		if (change_color->permutations.count > 0 && !change_color->scaled_by && !change_color->darken_by)
		{
			object->object.base_change_colors[index] = *color;
			object->object.outgoing_change_colors[index] = *color;
		}
	}
}

/* the region of the unit's model with the name, else NONE */
static short armor_region_index(long unit_index, char const *name)
{
	struct object_datum *object = object_get(unit_index);
	struct object_definition *definition = object_definition_get(object->definition_index);
	struct model *model;
	short region_index;

	if (definition->object.model.index == NONE)
		return NONE;
	model = model_definition_get(definition->object.model.index);
	for (region_index = 0; region_index < model->regions.count && region_index < MAXIMUM_REGIONS_PER_OBJECT; region_index++)
	{
		struct model_region *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);

		if (!_stricmp(region->name, name))
			return region_index;
	}
	return NONE;
}

static void armor_dress_unit(struct player_armor const *armor, short local_player_index, long unit_index)
{
	real_rgb_color color;
	short index;

	for (index = 0; index < armor->permutation_count; index++)
	{
		struct player_armor_permutation const *permutation = &armor->permutations[index];
		short region_index = armor_region_index(unit_index, permutation->region);

		if (region_index == NONE)
		{
			error(_error_silent, "armor %s: %s has no region '%s'", armor->id,
				tag_get_name(object_get(unit_index)->definition_index), permutation->region);
			continue;
		}
		/* (object_permute_region takes no const name) */
		{
			char name[TAG_STRING_LENGTH + 1];

			armor_copy(name, permutation->permutation, sizeof(name));
			object_permute_region(unit_index, name, region_index, TRUE);
		}
	}
	if (armor->color == _player_armor_color_profile && armor_profile_color(local_player_index, &color))
		armor_color_unit(unit_index, &color);
}

/* ---------- public code */

void player_armor_catalog_load(
	void)
{
	WIN32_FIND_DATAA data;
	HANDLE find;
	short previous_count = player_armor_globals.count;
	char previous_id[PLAYER_ARMOR_ID_LENGTH] = "";
	short armor;

	if (player_armor_globals.loaded && player_armor_globals.selected != NONE)
		armor_copy(previous_id, player_armor_globals.armors[player_armor_globals.selected].id, sizeof(previous_id));
	armor_game_armors();
	player_armor_globals.count = PLAYER_ARMOR_GAME_ARMORS;
	/* (a handle of its own, not find_files_start: custom_edition_maps.c
	has why) */
	find = FindFirstFileA(PLAYER_ARMOR_FOLDER "*.*", &data);
	if (find != INVALID_HANDLE_VALUE)
	{
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
			if (strlen(id) >= PLAYER_ARMOR_ID_LENGTH)
			{
				error(_error_silent, "armor: %s.txt: the name is too long", id);
				continue;
			}
			if (player_armor_globals.count >= PLAYER_ARMOR_MAXIMUM_ARMORS)
			{
				error(_error_silent, "armor: more than %d armors; %s.txt left out", PLAYER_ARMOR_MAXIMUM_ARMORS, id);
				continue;
			}
			if (armor_read(id, &player_armor_globals.armors[player_armor_globals.count]))
				player_armor_globals.count++;
		} while (FindNextFileA(find, &data));
		CloseHandle(find);
	}
	qsort(&player_armor_globals.armors[PLAYER_ARMOR_GAME_ARMORS], player_armor_globals.count - PLAYER_ARMOR_GAME_ARMORS,
		sizeof(player_armor_globals.armors[0]), armor_compare);
	player_armor_globals.loaded = TRUE;
	if (previous_id[0])
	{
		/* (the choice kept by its id, wherever the list put it) */
		player_armor_globals.selected = 0;
		for (armor = 0; armor < player_armor_globals.count; armor++)
		{
			if (!strcmp(player_armor_globals.armors[armor].id, previous_id))
				player_armor_globals.selected = armor;
		}
	}
	else
	{
		armor_choice_read();
	}
	if (player_armor_globals.count != previous_count)
		error(_error_silent, "armor: %d armors (%d from %s)", player_armor_globals.count,
			player_armor_globals.count - PLAYER_ARMOR_GAME_ARMORS, PLAYER_ARMOR_FOLDER);
}

short player_armor_count(
	void)
{
	armor_catalog_ensure();
	return player_armor_globals.count;
}

void player_armor_name(
	short armor,
	wchar_t *text,
	short length)
{
	ustrncpy(text, armor_get(armor)->name, length - 1);
	text[length - 1] = 0;
}

void player_armor_description(
	short armor,
	wchar_t *text,
	short length)
{
	ustrncpy(text, armor_get(armor)->description, length - 1);
	text[length - 1] = 0;
}

boolean player_armor_available(
	short armor)
{
	struct player_armor const *definition = armor_get(armor);

	if (!definition->biped[0] || !armor_campaign_map())
		return TRUE;
	return armor_biped(definition) != NONE;
}

short player_armor_selected(
	void)
{
	armor_catalog_ensure();
	return player_armor_globals.selected;
}

void player_armor_select(
	short armor)
{
	armor_catalog_ensure();
	if (armor < 0 || armor >= player_armor_globals.count)
		return;
	player_armor_globals.selected = armor;
	armor_choice_write();
}

long player_armor_unit_definition(
	short local_player_index,
	long default_definition_index)
{
	struct player_armor const *armor;
	long biped_index;

	if (!armor_applies(local_player_index))
		return default_definition_index;
	armor = armor_get(player_armor_selected());
	if (!armor->biped[0])
		return default_definition_index;
	biped_index = armor_biped(armor);
	if (biped_index == NONE)
	{
		error(_error_silent, "armor %s: this map has not %s; the map's own player unit spawns", armor->id,
			armor->biped);
		return default_definition_index;
	}
	error(_error_silent, "armor %s: player %d spawns as %s", armor->id, local_player_index, armor->biped);
	return biped_index;
}

void player_armor_unit_created(
	short local_player_index,
	long unit_index)
{
	if (!armor_applies(local_player_index) || unit_index == NONE)
		return;
	armor_dress_unit(armor_get(player_armor_selected()), local_player_index, unit_index);
}

boolean player_armor_addon(
	char *addon,
	long addon_size,
	char *biped,
	long biped_size)
{
	struct player_armor const *armor = armor_get(player_armor_selected());

	if (!armor->addon[0] || !armor->biped[0] || addon_size <= 0 || biped_size <= 0)
		return FALSE;
	armor_copy(addon, armor->addon, (size_t)addon_size);
	armor_copy(biped, armor->biped, (size_t)biped_size);
	return TRUE;
}
