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
- description: shown beside the list ('\\n' a line break);
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

A saved game goes on with the player it saved: one saved in the map's
Spartan (before the armor was chosen, or before armors were) is loaded
(game_state.c lets it, its objects fitting the level with the armor's
addon), and the player is then put in the armor chosen
(player_armor_game_state_loaded), as soon as they are on foot, alive and
out of a cinematic: a unit of the armor in the Spartan's place, with its
weapons, grenades, health and shields.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "cutscene/cinematics.h"
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
#include "network_addons.h"
#include "network_coop.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define PLAYER_ARMOR_MAXIMUM_ARMORS 32
#define PLAYER_ARMOR_MAXIMUM_PERMUTATIONS 8
/* an armor's pieces chosen one by one (Halo 3's helmet, chest, shoulders):
the regions, the permutations each offers, and the regions that go with a
piece (the body with the chest) */
#define PLAYER_ARMOR_MAXIMUM_PIECES 6
#define PLAYER_ARMOR_MAXIMUM_OPTIONS 16
#define PLAYER_ARMOR_MAXIMUM_FOLLOWS 4
#define PLAYER_ARMOR_PIECE_NAME_LENGTH 24
/* the pieces chosen, of every armor (u:\armor.txt) */
#define PLAYER_ARMOR_MAXIMUM_CHOICES 64
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

struct player_armor_option
{
	char permutation[TAG_STRING_LENGTH + 1];
	wchar_t name[PLAYER_ARMOR_PIECE_NAME_LENGTH];
};

struct player_armor_piece
{
	char region[TAG_STRING_LENGTH + 1];
	wchar_t name[PLAYER_ARMOR_PIECE_NAME_LENGTH];
	short option_count;
	struct player_armor_option options[PLAYER_ARMOR_MAXIMUM_OPTIONS];
};

/* a region that takes the permutation chosen for a piece's */
struct player_armor_follow
{
	char region[TAG_STRING_LENGTH + 1];
	char piece[TAG_STRING_LENGTH + 1];
};

/* a piece chosen: of an armor (its id), the region and its permutation */
struct player_armor_choice
{
	char armor[PLAYER_ARMOR_ID_LENGTH];
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
	short piece_count;
	struct player_armor_piece pieces[PLAYER_ARMOR_MAXIMUM_PIECES];
	short follow_count;
	struct player_armor_follow follows[PLAYER_ARMOR_MAXIMUM_FOLLOWS];
};

/* ---------- globals */

static struct
{
	boolean loaded;
	short count;
	struct player_armor armors[PLAYER_ARMOR_MAXIMUM_ARMORS];
	/* the armor chosen (NONE: not read from the saves yet) */
	short selected;
	/* the local players of a saved game just loaded, to be put in the armor
	chosen (player_armor_update) */
	boolean dress_loaded[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	/* the pieces chosen (read with the armor chosen) */
	short choice_count;
	struct player_armor_choice choices[PLAYER_ARMOR_MAXIMUM_CHOICES];
} player_armor_globals = { FALSE, 0, { { { 0 } } }, NONE, { 0 }, 0, { { { 0 } } } };

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
			/* (the menus' line break, as their strings': draw_string.c) */
			character = '\r';
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
		/* (the menus' line break: draw_string.c) */
		text[count] = source[count] == '\n' ? L'\r' : (wchar_t)(unsigned char)source[count];
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

/* the first word of text (cut there), and the rest after it, else NULL */
static char *armor_word(char *text)
{
	char *space = strpbrk(text, " \t");

	if (!space)
		return NULL;
	*space++ = 0;
	return armor_trim(space);
}

static struct player_armor_piece *armor_piece_named(struct player_armor *armor, char const *region)
{
	short index;

	for (index = 0; index < armor->piece_count; index++)
	{
		if (!_stricmp(armor->pieces[index].region, region))
			return &armor->pieces[index];
	}
	return NULL;
}

/* An armor file's pieces (FALSE: the line is not one):

	piece = <region> <name>                  a piece chosen in the menus
	option = <region> <permutation> <name>   one it offers (the first: the default)
	follow = <region> <piece's region>       a region worn as that piece is
*/
static boolean armor_read_piece(char const *id, struct player_armor *armor, char const *key, char *value)
{
	char *rest = armor_word(value);

	if (!rest || strlen(value) > TAG_STRING_LENGTH)
		return FALSE;
	if (!_stricmp(key, "piece"))
	{
		struct player_armor_piece *piece = armor_piece_named(armor, value);

		if (!piece)
		{
			if (armor->piece_count >= PLAYER_ARMOR_MAXIMUM_PIECES)
				return FALSE;
			piece = &armor->pieces[armor->piece_count++];
			armor_copy(piece->region, value, sizeof(piece->region));
		}
		armor_text_from_utf8(rest, piece->name, PLAYER_ARMOR_PIECE_NAME_LENGTH);
		return TRUE;
	}
	if (!_stricmp(key, "option"))
	{
		struct player_armor_piece *piece = armor_piece_named(armor, value);
		char *name = armor_word(rest);
		struct player_armor_option *option;

		if (!piece || piece->option_count >= PLAYER_ARMOR_MAXIMUM_OPTIONS || strlen(rest) > TAG_STRING_LENGTH)
			return FALSE;
		option = &piece->options[piece->option_count++];
		armor_copy(option->permutation, rest, sizeof(option->permutation));
		if (name && *name)
			armor_text_from_utf8(name, option->name, PLAYER_ARMOR_PIECE_NAME_LENGTH);
		else
			armor_text_from_ascii(rest, option->name, PLAYER_ARMOR_PIECE_NAME_LENGTH);
		return TRUE;
	}
	if (armor->follow_count >= PLAYER_ARMOR_MAXIMUM_FOLLOWS || strlen(rest) > TAG_STRING_LENGTH)
		return FALSE;
	armor_copy(armor->follows[armor->follow_count].region, value, sizeof(armor->follows[0].region));
	armor_copy(armor->follows[armor->follow_count].piece, rest, sizeof(armor->follows[0].piece));
	armor->follow_count++;
	(void)id;
	return TRUE;
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
		else if (!_stricmp(key, "piece") || !_stricmp(key, "option") || !_stricmp(key, "follow"))
		{
			if (!armor_read_piece(id, armor, key, value))
				error(_error_silent, "armor %s: ignoring %s '%s'", id, key, value);
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

/* The choice file: the armor chosen's id, then a line for each piece
chosen, of any armor ("piece <armor id> <region> <permutation>"). */
static void armor_choice_read(void)
{
	char line[256];
	FILE *file = fopen(PLAYER_ARMOR_CHOICE_FILE, "r");
	boolean first = TRUE;
	short armor;

	player_armor_globals.selected = 0;
	player_armor_globals.choice_count = 0;
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		char *text = armor_trim(line);

		if (first)
		{
			first = FALSE;
			for (armor = 0; armor < player_armor_globals.count; armor++)
			{
				if (!_stricmp(player_armor_globals.armors[armor].id, text))
					player_armor_globals.selected = armor;
			}
		}
		else if (!strncmp(text, "piece ", 6) && player_armor_globals.choice_count < PLAYER_ARMOR_MAXIMUM_CHOICES)
		{
			char *armor_id = armor_trim(text + 6);
			char *region = armor_word(armor_id);
			char *permutation = region ? armor_word(region) : NULL;

			if (permutation && *permutation && strlen(armor_id) < PLAYER_ARMOR_ID_LENGTH &&
				strlen(region) <= TAG_STRING_LENGTH && strlen(permutation) <= TAG_STRING_LENGTH)
			{
				struct player_armor_choice *choice = &player_armor_globals.choices[player_armor_globals.choice_count++];

				armor_copy(choice->armor, armor_id, sizeof(choice->armor));
				armor_copy(choice->region, region, sizeof(choice->region));
				armor_copy(choice->permutation, permutation, sizeof(choice->permutation));
			}
		}
	}
	fclose(file);
}

static void armor_choice_write(void)
{
	FILE *file = fopen(PLAYER_ARMOR_CHOICE_FILE, "w");
	short index;

	if (!file)
	{
		error(_error_silent, "armor: could not write %s", PLAYER_ARMOR_CHOICE_FILE);
		return;
	}
	fprintf(file, "%s\n", player_armor_globals.armors[player_armor_globals.selected].id);
	for (index = 0; index < player_armor_globals.choice_count; index++)
	{
		struct player_armor_choice const *choice = &player_armor_globals.choices[index];

		fprintf(file, "piece %s %s %s\n", choice->armor, choice->region, choice->permutation);
	}
	fclose(file);
}

/* the permutation chosen for the armor's region, else NULL */
static char const *armor_choice_get(struct player_armor const *armor, char const *region)
{
	short index;

	for (index = 0; index < player_armor_globals.choice_count; index++)
	{
		struct player_armor_choice const *choice = &player_armor_globals.choices[index];

		if (!_stricmp(choice->armor, armor->id) && !_stricmp(choice->region, region))
			return choice->permutation;
	}
	return NULL;
}

static void armor_choice_set(struct player_armor const *armor, char const *region, char const *permutation)
{
	struct player_armor_choice *choice = NULL;
	short index;

	for (index = 0; index < player_armor_globals.choice_count && !choice; index++)
	{
		if (!_stricmp(player_armor_globals.choices[index].armor, armor->id) &&
			!_stricmp(player_armor_globals.choices[index].region, region))
		{
			choice = &player_armor_globals.choices[index];
		}
	}
	if (!choice)
	{
		if (player_armor_globals.choice_count >= PLAYER_ARMOR_MAXIMUM_CHOICES)
		{
			/* (the oldest let go) */
			csmemmove(&player_armor_globals.choices[0], &player_armor_globals.choices[1],
				sizeof(player_armor_globals.choices[0]) * (PLAYER_ARMOR_MAXIMUM_CHOICES - 1));
			player_armor_globals.choice_count--;
		}
		choice = &player_armor_globals.choices[player_armor_globals.choice_count++];
		armor_copy(choice->armor, armor->id, sizeof(choice->armor));
		armor_copy(choice->region, region, sizeof(choice->region));
	}
	armor_copy(choice->permutation, permutation, sizeof(choice->permutation));
}

/* the option of the piece worn: the one chosen, else the permutation the
armor names for its region, else the first */
static short armor_piece_option(struct player_armor const *armor, struct player_armor_piece const *piece)
{
	char const *chosen = armor_choice_get(armor, piece->region);
	short index;

	if (!chosen)
	{
		for (index = 0; index < armor->permutation_count; index++)
		{
			if (!_stricmp(armor->permutations[index].region, piece->region))
				chosen = armor->permutations[index].permutation;
		}
	}
	for (index = 0; chosen && index < piece->option_count; index++)
	{
		if (!_stricmp(piece->options[index].permutation, chosen))
			return index;
	}
	return 0;
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

static void armor_dress_set(struct player_armor_dress *dress, char const *region, char const *permutation);

/* what the armor puts on a unit: its biped, and the permutation of each
region (its own, then the pieces chosen and the regions that go with them) */
static void armor_dress_of(struct player_armor const *armor, struct player_armor_dress *dress)
{
	short index;

	csmemset(dress, 0, sizeof(*dress));
	armor_copy(dress->biped, armor->biped, sizeof(dress->biped));
	dress->profile_color = armor->color == _player_armor_color_profile;

	#define DRESS_SET(region_name, permutation_name) \
		armor_dress_set(dress, (region_name), (permutation_name))
	for (index = 0; index < armor->permutation_count; index++)
		DRESS_SET(armor->permutations[index].region, armor->permutations[index].permutation);
	for (index = 0; index < armor->piece_count; index++)
	{
		struct player_armor_piece const *piece = &armor->pieces[index];

		if (piece->option_count > 0)
			DRESS_SET(piece->region, piece->options[armor_piece_option(armor, piece)].permutation);
	}
	for (index = 0; index < armor->follow_count; index++)
	{
		struct player_armor_piece const *piece = NULL;
		short other;

		for (other = 0; other < armor->piece_count; other++)
		{
			if (!_stricmp(armor->pieces[other].region, armor->follows[index].piece))
				piece = &armor->pieces[other];
		}
		if (piece && piece->option_count > 0)
			DRESS_SET(armor->follows[index].region, piece->options[armor_piece_option(armor, piece)].permutation);
	}
	#undef DRESS_SET
}

/* the dress's region (added, or its permutation changed) */
static void armor_dress_set(struct player_armor_dress *dress, char const *region, char const *permutation)
{
	short index;

	for (index = 0; index < dress->region_count; index++)
	{
		if (!_stricmp(dress->regions[index].region, region))
			break;
	}
	if (index >= PLAYER_ARMOR_DRESS_REGIONS)
		return;
	if (index == dress->region_count)
		dress->region_count++;
	armor_copy(dress->regions[index].region, region, sizeof(dress->regions[index].region));
	armor_copy(dress->regions[index].permutation, permutation, sizeof(dress->regions[index].permutation));
}

/* the dress's regions put on the unit (a region or permutation its model has
not is left: logged), and, with color, its colour */
static void armor_dress_apply(struct player_armor_dress const *dress, long unit_index, real_rgb_color const *color)
{
	short index;

	for (index = 0; index < dress->region_count && index < PLAYER_ARMOR_DRESS_REGIONS; index++)
	{
		short region_index = armor_region_index(unit_index, dress->regions[index].region);
		/* (object_permute_region takes no const name) */
		char name[TAG_STRING_LENGTH + 1];

		if (region_index == NONE)
		{
			error(_error_silent, "armor: %s has no region '%s'", tag_get_name(object_get(unit_index)->definition_index),
				dress->regions[index].region);
			continue;
		}
		armor_copy(name, dress->regions[index].permutation, sizeof(name));
		object_permute_region(unit_index, name, region_index, TRUE);
	}
	if (color)
		armor_color_unit(unit_index, color);
}

static void armor_dress_unit(struct player_armor const *armor, short local_player_index, long unit_index)
{
	struct player_armor_dress dress;
	real_rgb_color color;

	armor_dress_of(armor, &dress);
	armor_dress_apply(&dress, unit_index,
		dress.profile_color && armor_profile_color(local_player_index, &color) ? &color : NULL);
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
		error(_error_silent, "armor: %d armors (%d from %s) [" PLAYER_ARMOR_PATCH_TEXT "]", player_armor_globals.count,
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

short player_armor_piece_count(
	short armor)
{
	return armor_get(armor)->piece_count;
}

static struct player_armor_piece const *armor_piece_get(short armor, short piece)
{
	struct player_armor const *definition = armor_get(armor);

	return piece >= 0 && piece < definition->piece_count ? &definition->pieces[piece] : NULL;
}

void player_armor_piece_name(
	short armor,
	short piece,
	wchar_t *text,
	short length)
{
	struct player_armor_piece const *definition = armor_piece_get(armor, piece);

	text[0] = 0;
	if (definition)
	{
		ustrncpy(text, definition->name[0] ? definition->name : L"?", length - 1);
		text[length - 1] = 0;
	}
}

short player_armor_option_count(
	short armor,
	short piece)
{
	struct player_armor_piece const *definition = armor_piece_get(armor, piece);

	return definition ? definition->option_count : 0;
}

void player_armor_option_name(
	short armor,
	short piece,
	short option,
	wchar_t *text,
	short length)
{
	struct player_armor_piece const *definition = armor_piece_get(armor, piece);

	text[0] = 0;
	if (definition && option >= 0 && option < definition->option_count)
	{
		ustrncpy(text, definition->options[option].name, length - 1);
		text[length - 1] = 0;
	}
}

short player_armor_option_worn(
	short armor,
	short piece)
{
	struct player_armor_piece const *definition = armor_piece_get(armor, piece);

	return definition ? armor_piece_option(armor_get(armor), definition) : NONE;
}

void player_armor_option_choose(
	short armor,
	short piece,
	short option)
{
	struct player_armor_piece const *definition = armor_piece_get(armor, piece);

	if (!definition || option < 0 || option >= definition->option_count)
		return;
	armor_choice_set(armor_get(armor), definition->region, definition->options[option].permutation);
	armor_choice_write();
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

/* network co-op's host (it spawns every player, and its objects go to the
others as made, permutations and all: network_objects.c): the dress of a
player, this machine's (the armor chosen here) or another's (what its
machine sent: network_addons.c), else FALSE */
static boolean armor_coop_dress(long player_index, short local_player_index, struct player_armor_dress *dress)
{
	if (!armor_campaign_map() || game_engine_running() || !network_coop_active() || network_game_distributed_client())
		return FALSE;
	/* (this machine's player: the armor chosen here, even before the
	player is tied to its controller) */
	if (local_player_index != NONE || (player_index != NONE &&
		player_get(player_index)->network_player_data.machine_index == network_game_client_get_local_machine_index()))
	{
		armor_dress_of(armor_get(player_armor_selected()), dress);
		return TRUE;
	}
	return network_addons_player_dress(player_index, dress);
}

long player_armor_player_unit_definition(
	long player_index,
	short local_player_index,
	long default_definition_index)
{
	struct player_armor_dress dress;
	long biped_index;

	if (!armor_coop_dress(player_index, local_player_index, &dress))
		return player_armor_unit_definition(local_player_index, default_definition_index);
	if (!dress.biped[0])
	{
		error(_error_silent, "armor: co-op player %ld wears the level's Spartan", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index));
		return default_definition_index;
	}
	biped_index = tag_loaded(BIPED_DEFINITION_TAG, dress.biped);
	if (biped_index == NONE)
	{
		error(_error_silent, "armor: co-op player %ld's %s is not in this game; the level's Spartan spawns",
			DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), dress.biped);
		return default_definition_index;
	}
	error(_error_silent, "armor: co-op player %ld spawns as %s", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), dress.biped);
	return biped_index;
}

void player_armor_player_unit_created(
	long player_index,
	short local_player_index,
	long unit_index)
{
	struct player_armor_dress dress;

	if (unit_index == NONE)
		return;
	if (!armor_coop_dress(player_index, local_player_index, &dress))
	{
		player_armor_unit_created(local_player_index, unit_index);
		return;
	}
	/* (the colours are co-op's: each player's profile's, players.c) */
	if (dress.biped[0] && object_get(unit_index)->definition_index == tag_loaded(BIPED_DEFINITION_TAG, dress.biped))
		armor_dress_apply(&dress, unit_index, NULL);
}

void player_armor_local_dress(
	struct player_armor_dress *dress)
{
	armor_dress_of(armor_get(player_armor_selected()), dress);
}

short player_armor_network_addons(
	char (*addons)[PLAYER_ARMOR_DRESS_NAME + 16],
	char (*bipeds)[PLAYER_ARMOR_DRESS_BIPED],
	short maximum)
{
	short count = 0;
	short armor;

	armor_catalog_ensure();
	/* (the armor chosen, then those of pieces: one a player may wear) */
	for (armor = NONE; armor < player_armor_globals.count && count < maximum; armor++)
	{
		struct player_armor const *definition =
			armor == NONE ? armor_get(player_armor_selected()) : &player_armor_globals.armors[armor];
		short other;
		boolean listed = FALSE;

		if (!definition->addon[0] || !definition->biped[0] || (armor != NONE && definition->piece_count <= 0))
			continue;
		for (other = 0; other < count; other++)
		{
			if (!_stricmp(addons[other], definition->addon))
				listed = TRUE;
		}
		if (listed)
			continue;
		armor_copy(addons[count], definition->addon, sizeof(addons[0]));
		armor_copy(bipeds[count], definition->biped, sizeof(bipeds[0]));
		count++;
	}
	return count;
}

boolean player_armor_addon_of_biped(
	char const *biped,
	char *addon,
	long size)
{
	short armor;

	armor_catalog_ensure();
	if (!biped || !biped[0] || size <= 0)
		return FALSE;
	for (armor = 0; armor < player_armor_globals.count; armor++)
	{
		struct player_armor const *definition = &player_armor_globals.armors[armor];

		if (definition->addon[0] && !_stricmp(definition->biped, biped))
		{
			armor_copy(addon, definition->addon, (size_t)size);
			return TRUE;
		}
	}
	return FALSE;
}

void player_armor_unit_created(
	short local_player_index,
	long unit_index)
{
	if (local_player_index >= 0 && local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS)
		player_armor_globals.dress_loaded[local_player_index] = FALSE;
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

void player_armor_game_state_loaded(
	void)
{
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
		player_armor_globals.dress_loaded[local_player_index] = TRUE;
}

/* the player's unit (saved in another) given the armor's place: a unit of
the armor where it is, facing as it faces, with its weapons, grenades,
health and shields, then the old one deleted; FALSE if not now (in a
vehicle, dead, in a cinematic: tried again) */
static boolean armor_dress_loaded_player(
	struct player_armor const *armor,
	short local_player_index,
	long biped_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	struct player_datum *player;
	struct unit_datum *old_unit;
	struct unit_datum *new_unit;
	struct object_placement_data placement;
	long old_unit_index;
	long new_unit_index;

	if (player_index == NONE)
		return TRUE;
	player = player_get(player_index);
	old_unit_index = player->unit_index;
	if (old_unit_index == NONE)
		return FALSE;
	old_unit = unit_get(old_unit_index);
	if (old_unit->definition_index == biped_index)
		return TRUE;
	if (old_unit->object.parent_object_index != NONE || TEST_FLAG(old_unit->object.damage_flags, _object_dead_bit) ||
		cinematic_in_progress() || old_unit->unit.grenade_throw_state != 0)
	{
		return FALSE;
	}

	object_placement_data_new(&placement, biped_index, NONE);
	placement.position = old_unit->object.position;
	placement.forward = old_unit->object.forward;
	placement.up = old_unit->object.up;
	placement.translational_velocity = old_unit->object.translational_velocity;
	csmemcpy(placement.change_colors, old_unit->object.base_change_colors, sizeof(placement.change_colors));
	new_unit_index = object_new(&placement);
	if (new_unit_index == NONE)
	{
		error(_error_silent, "armor %s: no room for player %d's unit; the saved one stays", armor->id,
			local_player_index);
		return TRUE;
	}
	/* (object_new may move the old one in the objects' memory) */
	old_unit = unit_get(old_unit_index);
	new_unit = unit_get(new_unit_index);
	new_unit->object.owner_player_index = old_unit->object.owner_player_index;
	new_unit->object.owner_team_index = old_unit->object.owner_team_index;
	new_unit->object.body_vitality = old_unit->object.body_vitality;
	new_unit->object.shield_vitality = old_unit->object.shield_vitality;
	new_unit->unit.desired_facing_vector = old_unit->unit.desired_facing_vector;
	new_unit->unit.desired_aiming_vector = old_unit->unit.desired_aiming_vector;
	new_unit->unit.aiming_vector = old_unit->unit.aiming_vector;
	new_unit->unit.desired_looking_vector = old_unit->unit.desired_looking_vector;
	new_unit->unit.looking_vector = old_unit->unit.looking_vector;
	new_unit->unit.integrated_light_battery = old_unit->unit.integrated_light_battery;
	unit_port_move_inventory(old_unit_index, new_unit_index);
	armor_dress_unit(armor, local_player_index, new_unit_index);
	error(_error_silent, "armor %s: player %d (as saved, %s) is now %s", armor->id, local_player_index,
		tag_get_name(old_unit->definition_index), armor->biped);
	players_set_local_player_unit(local_player_index, new_unit_index);
	object_delete(old_unit_index);
	return TRUE;
}

/* the level's player unit (the Spartan of its globals), else NONE */
static long armor_level_player_unit(
	void)
{
	struct game_globals *globals = scenario_get_game_globals();

	if (!globals || globals->player_information.count <= 0)
		return NONE;
	return TAG_BLOCK_GET_ELEMENT(&globals->player_information, 0, struct game_globals_player_information)->player_unit.index;
}

/* network co-op's host: the player's unit given its armor's place, as a
loaded game's is (armor_dress_loaded_player), when it is not of the armor's
biped: made some way other than a spawn (a cutscene's revert, a unit the
scripts gave), it would be the level's Spartan on every machine. FALSE if not
now (in a vehicle, dead, in a cinematic: tried again). */
static boolean armor_coop_redress_player(
	long player_index)
{
	struct player_datum *player = player_get(player_index);
	struct player_armor_dress dress;
	struct unit_datum *old_unit;
	struct unit_datum *new_unit;
	struct object_placement_data placement;
	long old_unit_index = player->unit_index;
	long new_unit_index;
	long biped_index;

	if (old_unit_index == NONE || !armor_coop_dress(player_index, player->local_player_index, &dress) ||
		!dress.biped[0])
	{
		return TRUE;
	}
	biped_index = tag_loaded(BIPED_DEFINITION_TAG, dress.biped);
	old_unit = unit_get(old_unit_index);
	if (biped_index == NONE || old_unit->definition_index == biped_index ||
		old_unit->definition_index != armor_level_player_unit())
	{
		return TRUE;
	}
	if (old_unit->object.parent_object_index != NONE || TEST_FLAG(old_unit->object.damage_flags, _object_dead_bit) ||
		cinematic_in_progress() || old_unit->unit.grenade_throw_state != 0)
	{
		return FALSE;
	}

	object_placement_data_new(&placement, biped_index, NONE);
	placement.position = old_unit->object.position;
	placement.forward = old_unit->object.forward;
	placement.up = old_unit->object.up;
	placement.translational_velocity = old_unit->object.translational_velocity;
	csmemcpy(placement.change_colors, old_unit->object.base_change_colors, sizeof(placement.change_colors));
	new_unit_index = object_new(&placement);
	if (new_unit_index == NONE)
	{
		error(_error_silent, "armor: no room for co-op player %ld's unit; the level's Spartan stays",
			DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index));
		return TRUE;
	}
	/* (object_new may move the old one in the objects' memory) */
	old_unit = unit_get(old_unit_index);
	new_unit = unit_get(new_unit_index);
	new_unit->object.owner_player_index = old_unit->object.owner_player_index;
	new_unit->object.owner_team_index = old_unit->object.owner_team_index;
	new_unit->object.body_vitality = old_unit->object.body_vitality;
	new_unit->object.shield_vitality = old_unit->object.shield_vitality;
	new_unit->unit.desired_facing_vector = old_unit->unit.desired_facing_vector;
	new_unit->unit.desired_aiming_vector = old_unit->unit.desired_aiming_vector;
	new_unit->unit.aiming_vector = old_unit->unit.aiming_vector;
	new_unit->unit.desired_looking_vector = old_unit->unit.desired_looking_vector;
	new_unit->unit.looking_vector = old_unit->unit.looking_vector;
	new_unit->unit.integrated_light_battery = old_unit->unit.integrated_light_battery;
	unit_port_move_inventory(old_unit_index, new_unit_index);
	armor_dress_apply(&dress, new_unit_index, NULL);
	error(_error_silent, "armor: co-op player %ld (%s) is now %s", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
		tag_get_name(old_unit->definition_index), dress.biped);

	/* (as player_spawn gives a unit: players_set_local_player_unit is a
	game on this machine alone's) */
	old_unit->unit.player_index = NONE;
	unit_set_actively_controlled(old_unit_index, FALSE);
	new_unit->unit.player_index = player_index;
	unit_set_actively_controlled(new_unit_index, TRUE);
	player->unit_index = new_unit_index;
	if (player->local_player_index != NONE)
		player_control_new_unit(player->local_player_index, new_unit_index);
	object_delete(old_unit_index);
	return TRUE;
}

/* network co-op's host, twice a second: every player in its armor. Each
player's unit is logged when it changes, however it was made. */
static void armor_coop_redress(
	void)
{
	static long logged_units[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	static long logged_scenario = NONE;
	struct data_iterator iterator;
	struct player_datum *player;

	if (!network_coop_active() || network_game_distributed_client() || game_time_get() % 15 != 0)
		return;
	if (logged_scenario != global_scenario_index)
	{
		struct player_armor const *armor = armor_get(player_armor_selected());
		long index;

		logged_scenario = global_scenario_index;
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
			logged_units[index] = NONE;
		error(_error_silent, "armor: this machine (#%d) wears %s (%s)", network_game_client_get_local_machine_index(),
			armor->id, armor->biped[0] ? armor->biped : "the level's Spartan");
	}
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

		armor_coop_redress_player(iterator.datum_index);
		if (absolute >= 0 && absolute < HALO_PORT_MAXIMUM_NETWORK_PLAYERS && player->unit_index != logged_units[absolute])
		{
			logged_units[absolute] = player->unit_index;
			if (player->unit_index != NONE)
			{
				error(_error_silent, "armor: co-op player %ld (machine #%d, local %d) is in %s", absolute,
					player->network_player_data.machine_index, player->local_player_index,
					tag_get_name(object_get(player->unit_index)->definition_index));
			}
		}
	}
}

void player_armor_update(
	void)
{
	short local_player_index;

	armor_coop_redress();

	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		struct player_armor const *armor;
		long biped_index;

		if (!player_armor_globals.dress_loaded[local_player_index])
			continue;
		armor = armor_get(player_armor_selected());
		biped_index = armor_biped(armor);
		if (!armor_applies(local_player_index) || !armor->biped[0] || biped_index == NONE ||
			armor_dress_loaded_player(armor, local_player_index, biped_index))
		{
			player_armor_globals.dress_loaded[local_player_index] = FALSE;
		}
	}
}
