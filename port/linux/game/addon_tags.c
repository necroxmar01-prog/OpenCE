/*
ADDON_TAGS.C

Addons grafted onto a campaign level as it loads, so that mods' tags are in
every level without the levels being built again: the armor's
(player_armor.c), a character mod's biped, and the weapon mods'
(weapon_mods.c), each a weapon in place of one of the game's. addon_graft.c
does the format work; docs/addons.md has the whole of it.

An addon is a map built for this build (Invader's -g xbox-ntsc), in the
data root's addons folder (d:\addons\<name>.map), whose scenario names the
armor's biped (an armor file's "addon = <name>" and "biped = <tag>"). When
a campaign level loads (scenario_tags_load):

1. addon_tags_prepare, before the level's tags are read: the addon is read
   (inflated if it is compressed), its tags read where the level's go and
   checked by the validator there, as a map's are; then moved to memory of
   their own (addon_graft_relocate), and the file kept for its pixels and
   samples;
2. addon_tags_loaded, once the level's are loaded and checked: the biped and
   the tags it needs that the level has not got added to the level's table
   (addon_graft_tags), every reference of theirs checked against it, and
   the addon's vertex and index buffers registered as the level's are;
3. cache_file_read serves the reads of the addon's pixels and samples
   (offsets past ADDON_GRAFT_FILE_BASE) from the file kept, at once
   (addon_tags_read);
4. addon_tags_unloaded, as the level's tags go, lets it all go.

Several addons are grafted one after another, each onto the table the one
before left, with its pixels and samples at a base of its own
(ADDON_GRAFT_FILE_SPAN apart). A weapon mod's is grafted only if the level
has the weapon it replaces; then the game's tag is pointed at the mod's
data (its index kept: everything that names it gets the mod's).

Anything that fails is logged (debug.txt) and the level loads without that
addon: an armor's biped is then not in it, and the level's Spartan spawns;
a weapon is the game's.
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "tag_schema.h"

#include "addon_graft.h"
#include "addon_tags.h"
#include "player_armor.h"
#include "weapon_mods.h"
#include "../../third_party/zlib/zlib_prefixed.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cache_files.c's (port): the loaded tags' table, which the menus replace too */
void *cache_files_tag_instances(long *count);
void cache_files_set_tag_instances(void *instances, long count);

/* ---------- constants */

#define ADDON_TAGS_FOLDER "d:\\addons\\"
#define ADDON_TAGS_EXTENSION ".map"

enum
{
	/* a cache file's header (cache_files.c's struct cache_file_header) */
	ADDON_TAGS_HEADER_SIZE = 0x800,
	ADDON_TAGS_HEADER_SIGNATURE = 'head',
	ADDON_TAGS_FOOTER_SIGNATURE = 'foot',
	ADDON_TAGS_XBOX_VERSION = 5,
	/* the tag cache's size (physical_memory_map.h) */
	ADDON_TAGS_TAG_CACHE_SIZE = 0x1600000,
	/* the largest addon read: a character's tags and pixels (each addon's
	offsets have ADDON_GRAFT_FILE_SPAN) */
	ADDON_TAGS_MAXIMUM_FILE_SIZE = (long)ADDON_GRAFT_FILE_SPAN,
	/* the armor's and the weapon mods' */
	ADDON_TAGS_MAXIMUM_ADDONS = 1 + WEAPON_MODS_MAXIMUM,
	ADDON_TAGS_GROUP_BIPED = 'bipd',
	ADDON_TAGS_MAP_TYPE_CAMPAIGN = 0,
};

/* ---------- structures */

struct addon_tags_file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	unsigned long reserved0C;
	long tag_data_offset;
	long tag_data_size;
	byte reserved18[8];
	char name[0x20];
	char build[0x20];
	byte reserved60[0x79C];
	unsigned long footer_signature;
};

typedef char verify_addon_tags_file_header_size[
	sizeof(struct addon_tags_file_header) == ADDON_TAGS_HEADER_SIZE ? 1 : -1];

/* ---------- globals */

struct addon_tags_addon
{
	/* the addon's file, inflated: its pixels and samples are read from it */
	byte *file;
	long file_length;
	/* its tags, where they are kept */
	byte *memory;
	long tag_data_size;
	/* where its pixels and samples are in the game's reads */
	unsigned long file_base;
	boolean grafted;
	boolean buffers_registered;
	char name[64];
	/* the tag grafted with all it needs */
	unsigned long root_group;
	char root[256];
	/* a weapon mod's (else NONE): the game's tags it replaces */
	short weapon_mod;
};

static struct
{
	struct addon_tags_addon addons[ADDON_TAGS_MAXIMUM_ADDONS];
	short addon_count;
	struct weapon_mod weapon_mods[WEAPON_MODS_MAXIMUM];
	/* the table with their tags added (each graft's own; the last one is the
	game's) */
	struct addon_graft_instance *tables[ADDON_TAGS_MAXIMUM_ADDONS];
	short table_count;
} addon_tags;

/* ---------- private code */

/* (the game's free stops on NULL: debug_memory.c) */
static void addon_tags_free(
	void *pointer)
{
	if (pointer)
		free(pointer);
}

static void addon_tags_release_addon(
	struct addon_tags_addon *addon)
{
	if (addon->buffers_registered && addon->memory)
		tags_header_deregister_vertex_and_index_buffers((struct cache_file_tag_header *)addon->memory);
	if (addon->memory)
		XPhysicalFree(addon->memory);
	addon_tags_free(addon->file);
	csmemset(addon, 0, sizeof(*addon));
	addon->weapon_mod = NONE;
}

static void addon_tags_release(
	void)
{
	short index;

	for (index = 0; index < addon_tags.addon_count; index++)
		addon_tags_release_addon(&addon_tags.addons[index]);
	for (index = 0; index < addon_tags.table_count; index++)
		addon_tags_free(addon_tags.tables[index]);
	csmemset(&addon_tags, 0, sizeof(addon_tags));
}

/* the addon's file, inflated after its header if it is compressed, else
NULL (logged) */
static byte *addon_tags_file_read(
	char const *path,
	long *file_length)
{
	FILE *file = fopen(path, "rb");
	struct addon_tags_file_header header;
	byte *bytes = NULL;
	byte *compressed = NULL;
	long size;

	*file_length = 0;
	if (!file)
	{
		error(_error_silent, "addon: %s is not there", path);
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < ADDON_TAGS_HEADER_SIZE || fseek(file, 0, SEEK_SET) ||
		fread(&header, ADDON_TAGS_HEADER_SIZE, 1, file) != 1 ||
		header.header_signature != ADDON_TAGS_HEADER_SIGNATURE ||
		header.footer_signature != ADDON_TAGS_FOOTER_SIGNATURE ||
		header.version != ADDON_TAGS_XBOX_VERSION ||
		header.file_length < ADDON_TAGS_HEADER_SIZE || header.file_length > ADDON_TAGS_MAXIMUM_FILE_SIZE ||
		size > header.file_length)
	{
		error(_error_silent, "addon: %s is not a map of this build's (Invader's -g xbox-ntsc)", path);
		fclose(file);
		return NULL;
	}
	bytes = malloc(header.file_length);
	compressed = malloc(size - ADDON_TAGS_HEADER_SIZE);
	if (bytes && compressed && fread(compressed, size - ADDON_TAGS_HEADER_SIZE, 1, file) == 1)
	{
		csmemcpy(bytes, &header, ADDON_TAGS_HEADER_SIZE);
		if (size == header.file_length)
		{
			csmemcpy(bytes + ADDON_TAGS_HEADER_SIZE, compressed, size - ADDON_TAGS_HEADER_SIZE);
		}
		else
		{
			uLongf inflated = (uLongf)(header.file_length - ADDON_TAGS_HEADER_SIZE);

			if (uncompress(bytes + ADDON_TAGS_HEADER_SIZE, &inflated, compressed,
					(uLong)(size - ADDON_TAGS_HEADER_SIZE)) != Z_OK ||
				inflated != (uLongf)(header.file_length - ADDON_TAGS_HEADER_SIZE))
			{
				error(_error_silent, "addon: %s cannot be inflated", path);
				addon_tags_free(bytes);
				bytes = NULL;
			}
		}
	}
	else
	{
		error(_error_silent, "addon: %s cannot be read", path);
		addon_tags_free(bytes);
		bytes = NULL;
	}
	addon_tags_free(compressed);
	fclose(file);
	if (bytes)
		*file_length = header.file_length;
	return bytes;
}

/* ---------- public code */

/* the addon read, checked by the validator where the level's tags go
(tag_cache), and moved to memory of its own; FALSE (logged, let go) if it
cannot be */
static boolean addon_tags_read_addon(
	void *tag_cache,
	struct addon_tags_addon *addon,
	char const *name)
{
	char path[MAX_PATH];
	char reason[400];
	struct addon_tags_file_header const *header;

	csprintf(path, "%s%s%s", ADDON_TAGS_FOLDER, name, ADDON_TAGS_EXTENSION);
	strncpy(addon->name, name, sizeof(addon->name) - 1);
	addon->file = addon_tags_file_read(path, &addon->file_length);
	if (!addon->file)
		return FALSE;
	header = (struct addon_tags_file_header const *)addon->file;
	if (header->tag_data_offset < ADDON_TAGS_HEADER_SIZE || header->tag_data_size <= 0 ||
		header->tag_data_size > ADDON_TAGS_TAG_CACHE_SIZE ||
		header->tag_data_offset > header->file_length - header->tag_data_size)
	{
		error(_error_silent, "addon: %s: its tag data is outside it", name);
		return FALSE;
	}

	/* its tags where a map's are read, checked as a map's are */
	addon->tag_data_size = header->tag_data_size;
	csmemset(tag_cache, 0xCD, ADDON_TAGS_TAG_CACHE_SIZE);
	csmemcpy(tag_cache, addon->file + header->tag_data_offset, header->tag_data_size);
	if (!tag_validate_tags(tag_cache, header->tag_data_size, header->file_length, name))
	{
		error(_error_silent, "addon: %s: refused by the validator (debug.txt above)", name);
		return FALSE;
	}

	/* moved to memory of their own (the contiguous memory the game's
	buffers are in, as the tag cache is) */
	addon->memory = XPhysicalAlloc(header->tag_data_size, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!addon->memory)
	{
		error(_error_silent, "addon: %s: no memory for its %ld bytes of tags", name, header->tag_data_size);
		return FALSE;
	}
	if (!addon_graft_relocate(tag_cache, header->tag_data_size, (long)(addon->memory - (byte *)tag_cache),
		addon->file_base, reason, sizeof(reason)))
	{
		error(_error_silent, "addon: %s: cannot be moved: %s", name, reason);
		return FALSE;
	}
	csmemcpy(addon->memory, tag_cache, header->tag_data_size);
	error(_error_silent, "addon: %s read (%ld bytes of tags) for %s", name, header->tag_data_size, addon->root);
	return TRUE;
}

boolean addon_tags_prepare(
	void *tag_cache,
	short map_type)
{
	char name[64];
	short weapon_mod_count;
	short index;

	addon_tags_release();
	if (map_type != ADDON_TAGS_MAP_TYPE_CAMPAIGN)
		return FALSE;

	/* the armor's, then each weapon mod's */
	if (player_armor_addon(name, sizeof(name), addon_tags.addons[0].root, sizeof(addon_tags.addons[0].root)))
	{
		struct addon_tags_addon *addon = &addon_tags.addons[addon_tags.addon_count];

		addon->weapon_mod = NONE;
		addon->root_group = ADDON_TAGS_GROUP_BIPED;
		addon->file_base = ADDON_GRAFT_FILE_BASE + addon_tags.addon_count * ADDON_GRAFT_FILE_SPAN;
		addon_tags.addon_count++;
		if (!addon_tags_read_addon(tag_cache, addon, name))
		{
			addon_tags_release_addon(addon);
			addon_tags.addon_count--;
		}
	}
	weapon_mod_count = weapon_mods_for_level(addon_tags.weapon_mods, WEAPON_MODS_MAXIMUM);
	for (index = 0; index < weapon_mod_count; index++)
	{
		struct weapon_mod const *mod = &addon_tags.weapon_mods[index];
		struct addon_tags_addon *addon = &addon_tags.addons[addon_tags.addon_count];
		short other;
		boolean shared = FALSE;

		/* (two weapon mods from one addon: read once, grafted once) */
		for (other = 0; other < addon_tags.addon_count; other++)
		{
			if (addon_tags.addons[other].weapon_mod != NONE && !_stricmp(addon_tags.addons[other].name, mod->addon) &&
				addon_tags.addons[other].root_group == mod->root_group &&
				!_stricmp(addon_tags.addons[other].root, mod->root))
			{
				shared = TRUE;
			}
		}
		if (shared)
			continue;
		addon->weapon_mod = index;
		addon->root_group = mod->root_group;
		strncpy(addon->root, mod->root, sizeof(addon->root) - 1);
		addon->file_base = ADDON_GRAFT_FILE_BASE + addon_tags.addon_count * ADDON_GRAFT_FILE_SPAN;
		addon_tags.addon_count++;
		if (!addon_tags_read_addon(tag_cache, addon, mod->addon))
		{
			addon_tags_release_addon(addon);
			addon_tags.addon_count--;
		}
	}

	return addon_tags.addon_count > 0;
}

/* whether the level (table) has a game tag a weapon mod replaces */
static boolean addon_tags_weapon_mod_wanted(
	struct weapon_mod const *mod,
	struct addon_graft_instance const *table,
	long count)
{
	short replacement;

	for (replacement = 0; replacement < mod->replacement_count; replacement++)
	{
		if (addon_graft_find(table, count, mod->replacements[replacement].group_tag,
				mod->replacements[replacement].stock) != NONE)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* the weapon mod's game tags pointed at its own data (each keeps its index
and name: everything that names it gets the mod's) */
static void addon_tags_weapon_mod_replace(
	struct weapon_mod const *mod,
	struct addon_graft_instance *table,
	long count)
{
	short replacement;

	for (replacement = 0; replacement < mod->replacement_count; replacement++)
	{
		struct weapon_mod_replacement const *pair = &mod->replacements[replacement];
		long stock = addon_graft_find(table, count, pair->group_tag, pair->stock);
		long replacing = addon_graft_find(table, count, pair->group_tag, pair->mod);

		if (stock == NONE)
			continue;
		if (replacing == NONE)
		{
			error(_error_silent, "weapon %s: %s is not in its addon", mod->id, pair->mod);
			continue;
		}
		table[stock & 0xFFFF].base_address = table[replacing & 0xFFFF].base_address;
		error(_error_silent, "weapon %s: %s is now %s", mod->id, pair->stock, pair->mod);
	}
}

void addon_tags_loaded(
	void)
{
	struct addon_graft_instance *table;
	long level_count;
	long count;
	short index;

	if (!addon_tags.addon_count)
		return;
	table = cache_files_tag_instances(&level_count);
	if (!table)
	{
		addon_tags_release();
		return;
	}
	count = level_count;
	for (index = 0; index < addon_tags.addon_count; index++)
	{
		struct addon_tags_addon *addon = &addon_tags.addons[index];
		struct addon_graft_header *header = (struct addon_graft_header *)addon->memory;
		struct weapon_mod const *mod = addon->weapon_mod != NONE ? &addon_tags.weapon_mods[addon->weapon_mod] : NULL;
		struct addon_graft_instance *grafted;
		long grafted_count;
		char reason[400];

		if (!header)
			continue;
		if (mod && !addon_tags_weapon_mod_wanted(mod, table, count))
		{
			error(_error_silent, "weapon %s: this level has not the weapon it replaces", mod->id);
			continue;
		}
		grafted = malloc((size_t)(count + header->tag_count) * sizeof(*grafted));
		if (!grafted)
		{
			error(_error_silent, "addon: %s: no memory for the tag table", addon->name);
			continue;
		}
		grafted_count = addon_graft_tags(table, count, header, addon->root, addon->root_group, grafted, reason,
			sizeof(reason));
		if (grafted_count == NONE)
		{
			error(_error_silent, "addon: %s cannot be grafted: %s", addon->name, reason);
			addon_tags_free(grafted);
			continue;
		}
		if (addon_graft_references(grafted, grafted_count, count, reason, sizeof(reason)))
		{
			error(_error_silent, "addon: %s: a tag added names one it should not: %s", addon->name, reason);
			addon_tags_free(grafted);
			continue;
		}
		tags_header_register_vertex_and_index_buffers((struct cache_file_tag_header *)addon->memory);
		addon->buffers_registered = TRUE;
		addon->grafted = TRUE;
		error(_error_silent, "addon: %s: %ld tags added to the level's %ld", addon->name, grafted_count - count,
			count);
		addon_tags.tables[addon_tags.table_count++] = grafted;
		table = grafted;
		count = grafted_count;

		/* the weapon mods of this addon (one, or several sharing it) */
		{
			short other;

			for (other = 0; other < (short)WEAPON_MODS_MAXIMUM && mod; other++)
			{
				struct weapon_mod const *each = &addon_tags.weapon_mods[other];

				if (each->addon[0] && !_stricmp(each->addon, mod->addon) && each->root_group == mod->root_group &&
					!_stricmp(each->root, mod->root))
				{
					addon_tags_weapon_mod_replace(each, table, count);
				}
			}
		}
	}
	if (addon_tags.table_count)
		cache_files_set_tag_instances(table, count);
}

boolean addon_tags_read(
	long offset,
	long size,
	void *buffer)
{
	short index;

	if ((unsigned long)offset < ADDON_GRAFT_FILE_BASE)
		return FALSE;
	for (index = 0; index < addon_tags.addon_count; index++)
	{
		struct addon_tags_addon const *addon = &addon_tags.addons[index];
		unsigned long file_offset;

		if (!addon->grafted || (unsigned long)offset < addon->file_base ||
			(unsigned long)offset - addon->file_base >= ADDON_GRAFT_FILE_SPAN)
		{
			continue;
		}
		file_offset = (unsigned long)offset - addon->file_base;
		if (size < 0 || file_offset > (unsigned long)addon->file_length ||
			(unsigned long)size > (unsigned long)addon->file_length - file_offset)
		{
			/* (the validator kept them in the file; a read past it is zeros) */
			csmemset(buffer, 0, size > 0 ? size : 0);
			return TRUE;
		}
		csmemcpy(buffer, addon->file + file_offset, size);
		return TRUE;
	}
	return FALSE;
}

void addon_tags_unloaded(
	void)
{
	addon_tags_release();
}
