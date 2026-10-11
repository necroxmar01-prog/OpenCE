/*
ADDON_TAGS.C

The armor's addon (player_armor.c) grafted onto a campaign level as it
loads, so that a character mod's biped is in every level without the
levels being built again. addon_graft.c does the format work;
docs/addons.md has the whole of it.

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

Anything that fails is logged (debug.txt) and the level loads as it is: the
armor's biped is then not in it, and the level's Spartan spawns.
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "tag_schema.h"

#include "addon_graft.h"
#include "addon_tags.h"
#include "player_armor.h"
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
	/* the largest addon read: a character's tags and pixels */
	ADDON_TAGS_MAXIMUM_FILE_SIZE = 256 * 1024 * 1024,
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

static struct
{
	/* the addon's file, inflated: its pixels and samples are read from it */
	byte *file;
	long file_length;
	/* its tags, where they are kept */
	byte *memory;
	long tag_data_size;
	/* the table with its tags added */
	struct addon_graft_instance *instances;
	boolean buffers_registered;
	char name[64];
	char tag[256];
} addon_tags;

/* ---------- private code */

/* (the game's free stops on NULL: debug_memory.c) */
static void addon_tags_free(
	void *pointer)
{
	if (pointer)
		free(pointer);
}

static void addon_tags_release(
	void)
{
	if (addon_tags.buffers_registered && addon_tags.memory)
		tags_header_deregister_vertex_and_index_buffers((struct cache_file_tag_header *)addon_tags.memory);
	if (addon_tags.memory)
		XPhysicalFree(addon_tags.memory);
	addon_tags_free(addon_tags.file);
	addon_tags_free(addon_tags.instances);
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

boolean addon_tags_prepare(
	void *tag_cache,
	short map_type)
{
	char addon[64];
	char path[MAX_PATH];
	char reason[400];
	struct addon_tags_file_header const *header;

	addon_tags_release();
	if (map_type != ADDON_TAGS_MAP_TYPE_CAMPAIGN ||
		!player_armor_addon(addon, sizeof(addon), addon_tags.tag, sizeof(addon_tags.tag)))
	{
		return FALSE;
	}
	csprintf(path, "%s%s%s", ADDON_TAGS_FOLDER, addon, ADDON_TAGS_EXTENSION);
	addon_tags.file = addon_tags_file_read(path, &addon_tags.file_length);
	if (!addon_tags.file)
	{
		addon_tags_release();
		return FALSE;
	}
	header = (struct addon_tags_file_header const *)addon_tags.file;
	strncpy(addon_tags.name, addon, sizeof(addon_tags.name) - 1);
	if (header->tag_data_offset < ADDON_TAGS_HEADER_SIZE || header->tag_data_size <= 0 ||
		header->tag_data_size > ADDON_TAGS_TAG_CACHE_SIZE ||
		header->tag_data_offset > header->file_length - header->tag_data_size)
	{
		error(_error_silent, "addon: %s: its tag data is outside it", addon);
		addon_tags_release();
		return FALSE;
	}

	/* its tags where a map's are read, checked as a map's are */
	addon_tags.tag_data_size = header->tag_data_size;
	csmemset(tag_cache, 0xCD, ADDON_TAGS_TAG_CACHE_SIZE);
	csmemcpy(tag_cache, addon_tags.file + header->tag_data_offset, header->tag_data_size);
	if (!tag_validate_tags(tag_cache, header->tag_data_size, header->file_length, addon))
	{
		error(_error_silent, "addon: %s: refused by the validator (debug.txt above)", addon);
		addon_tags_release();
		return FALSE;
	}

	/* moved to memory of their own (the contiguous memory the game's
	buffers are in, as the tag cache is) */
	addon_tags.memory = XPhysicalAlloc(header->tag_data_size, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!addon_tags.memory)
	{
		error(_error_silent, "addon: %s: no memory for its %ld bytes of tags", addon, header->tag_data_size);
		addon_tags_release();
		return FALSE;
	}
	if (!addon_graft_relocate(tag_cache, header->tag_data_size, (long)(addon_tags.memory - (byte *)tag_cache),
		reason, sizeof(reason)))
	{
		error(_error_silent, "addon: %s: cannot be moved: %s", addon, reason);
		addon_tags_release();
		return FALSE;
	}
	csmemcpy(addon_tags.memory, tag_cache, header->tag_data_size);
	error(_error_silent, "addon: %s read (%ld bytes of tags) for %s", addon, header->tag_data_size, addon_tags.tag);

	return TRUE;
}

void addon_tags_loaded(
	void)
{
	struct addon_graft_header *addon = (struct addon_graft_header *)addon_tags.memory;
	struct addon_graft_instance *level_instances;
	long level_count;
	long count;
	char reason[400];

	if (!addon)
		return;
	level_instances = cache_files_tag_instances(&level_count);
	if (!level_instances)
	{
		addon_tags_release();
		return;
	}
	addon_tags.instances = malloc((size_t)(level_count + addon->tag_count) * sizeof(*addon_tags.instances));
	if (!addon_tags.instances)
	{
		error(_error_silent, "addon: %s: no memory for the tag table", addon_tags.name);
		addon_tags_release();
		return;
	}
	count = addon_graft_tags(level_instances, level_count, addon, addon_tags.tag, ADDON_TAGS_GROUP_BIPED,
		addon_tags.instances, reason, sizeof(reason));
	if (count == NONE)
	{
		error(_error_silent, "addon: %s cannot be grafted: %s", addon_tags.name, reason);
		addon_tags_release();
		return;
	}
	if (addon_graft_references(addon_tags.instances, count, level_count, reason, sizeof(reason)))
	{
		error(_error_silent, "addon: %s: a tag added names one it should not: %s", addon_tags.name, reason);
		addon_tags_release();
		return;
	}
	tags_header_register_vertex_and_index_buffers((struct cache_file_tag_header *)addon_tags.memory);
	addon_tags.buffers_registered = TRUE;
	cache_files_set_tag_instances(addon_tags.instances, count);
	error(_error_silent, "addon: %s: %ld tags added to the level's %ld", addon_tags.name, count - level_count,
		level_count);
}

boolean addon_tags_read(
	long offset,
	long size,
	void *buffer)
{
	unsigned long file_offset;

	if (!addon_tags.instances || (unsigned long)offset < ADDON_GRAFT_FILE_BASE)
		return FALSE;
	file_offset = (unsigned long)offset - ADDON_GRAFT_FILE_BASE;
	if (size < 0 || file_offset > (unsigned long)addon_tags.file_length ||
		(unsigned long)size > (unsigned long)addon_tags.file_length - file_offset)
	{
		/* (the validator kept them in the file; a read past it is zeros) */
		csmemset(buffer, 0, size > 0 ? size : 0);
		return TRUE;
	}
	csmemcpy(buffer, addon_tags.file + file_offset, size);
	return TRUE;
}

void addon_tags_unloaded(
	void)
{
	addon_tags_release();
}
