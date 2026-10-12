/*
ADDON_GRAFT.C

An addon's tags grafted onto a level's: the format work of addon_tags.c,
which reads the files and keeps the memory (docs/addons.md).

An addon is a map built for this build (Invader's -g xbox-ntsc) whose
scenario names a tag to add to the levels, a biped: the armor of
player_armor.c. The level's tags are its own, as it loads them; the addon's
are read first, where the level's go (the tag cache), and checked there by
the validator as any map's are (tag_validate_tags), so that everything here
walks tags that can be trusted. Then:

1. addon_graft_relocate makes them work where they will be kept, away from
   the tag cache: every pointer into them that the schemas name (tag_schema.h:
   blocks, data, references' names), the tag table's, the vertex and index
   buffers' and the models' parts' pointers to them, moved by the distance;
   their pixels' and samples' offsets in the addon's file moved past the
   level's own (ADDON_GRAFT_FILE_BASE), where cache_file_read finds the
   addon's (addon_tags.c).
2. addon_graft_tags adds to the level's table the tag named and every tag it
   needs, walking its references and tag indices. A tag the level has (of
   the same group and name, as the stock game's animations, effects and
   sounds most often are) is the level's: the level's own is used and not
   added. The others are added after the level's, with handles past its,
   and every reference and tag index of the tags added made the new table's.
3. addon_graft_references checks that every reference and tag index of the
   tags added names a tag of the new table of a group it takes.

Only the tags of groups the schemas know can be walked: a tag needed of
another group refuses the graft (the level loads without it).
*/

/* ---------- headers */

#include "cseries.h"
#include "tag_schema.h"
#include "bitmaps/bitmap_group.h"
#include "models/model_definitions.h"
#include "rasterizer/rasterizer_geometry.h"

#include "addon_graft.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	/* a vertex or index buffer (a D3DResource: Common, Data, Lock): its data
	is at +4 */
	ADDON_GRAFT_BUFFER_SIZE = 12,
	ADDON_GRAFT_BUFFER_DATA_OFFSET = 4,
	ADDON_GRAFT_MAXIMUM_DEPTH = 32,
	ADDON_GRAFT_MAXIMUM_TAGS = 0x10000,

	ADDON_GRAFT_BITMAP_GROUP_TAG = 'bitm',
	ADDON_GRAFT_MODEL_GROUP_TAG = 'mode',
};

/* how the walk treats what it finds */
enum
{
	/* pointers moved, file offsets moved */
	_walk_relocate = 0,
	/* the tags referenced, needed */
	_walk_collect,
	/* the references and tag indices made the new table's */
	_walk_remap,
	/* the references and tag indices checked against the new table */
	_walk_check,
};

/* ---------- structures */

/* the layout of a model's geometries and parts (tag_schema_models.c's) */
struct addon_graft_model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct addon_graft_model_geometry_part
{
	byte reserved[0x44];
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

typedef char verify_addon_graft_instance_size[sizeof(struct addon_graft_instance) == 0x20 ? 1 : -1];
typedef char verify_addon_graft_header_size[sizeof(struct addon_graft_header) == 0x24 ? 1 : -1];
typedef char verify_addon_graft_model_geometry_size[sizeof(struct addon_graft_model_geometry) == 0x30 ? 1 : -1];
typedef char verify_addon_graft_model_geometry_part_size[
	sizeof(struct addon_graft_model_geometry_part) == 0x68 ? 1 : -1];

struct addon_graft_walk
{
	short mode;
	boolean failed;
	/* _walk_relocate: the bytes moved, and by how much */
	byte *low;
	byte *high;
	long delta;
	/* where its pixels and samples go in the offsets the game reads */
	unsigned long file_base;
	/* _walk_collect and _walk_remap: the addon's table, and each of its tags'
	handle in the new table (NONE: not needed yet) */
	struct addon_graft_instance const *addon_instances;
	long addon_count;
	long *map;
	/* _walk_collect: the tags to walk next */
	long *queue;
	long queue_count;
	/* the level's table, whose tags stand for the addon's of the same name */
	struct addon_graft_instance const *level_instances;
	long level_count;
	long next_index;
	long first_salt;
	/* _walk_check: the new table, and the problems found */
	struct addon_graft_instance const *table;
	long table_count;
	long problems;
	char *error;
	long error_size;
};

/* ---------- private code */

static struct tag_schema_group const *graft_group_get(
	unsigned long group_tag)
{
	struct tag_schema_group const *const *list;

	for (list = tag_schema_group_lists; *list; list++)
	{
		struct tag_schema_group const *group;

		for (group = *list; group->group_tag; group++)
		{
			if (group->group_tag == group_tag)
				return group;
		}
	}
	return NULL;
}

static void graft_fail(
	struct addon_graft_walk *walk,
	char const *message)
{
	if (!walk->failed && walk->error && walk->error_size > 0)
	{
		strncpy(walk->error, message, (size_t)walk->error_size - 1);
		walk->error[walk->error_size - 1] = 0;
	}
	walk->failed = TRUE;
}

static boolean graft_name_equal(
	char const *a,
	char const *b)
{
	for (;; a++, b++)
	{
		char x = (char)(*a >= 'A' && *a <= 'Z' ? *a - 'A' + 'a' : *a);
		char y = (char)(*b >= 'A' && *b <= 'Z' ? *b - 'A' + 'a' : *b);

		if (x != y)
			return FALSE;
		if (!x)
			return TRUE;
	}
}

/* a pointer field moved by the walk's delta, if it points into the bytes
moved (NULL and anything else are left: the validator passed them) */
static void graft_move(
	struct addon_graft_walk *walk,
	void **field)
{
	byte *pointer = *field;

	if (pointer >= walk->low && pointer < walk->high)
		*field = pointer + walk->delta;
}

static void graft_move_offset(
	struct addon_graft_walk *walk,
	long *offset)
{
	*offset = (long)((unsigned long)*offset + walk->file_base);
}

/* the addon's tag handle as an index of its table, else NONE */
static long graft_addon_index(
	struct addon_graft_walk *walk,
	long tag_index)
{
	long absolute = tag_index & 0xFFFF;

	if (tag_index == NONE || absolute >= walk->addon_count ||
		walk->addon_instances[absolute].tag_index != tag_index)
	{
		return NONE;
	}
	return absolute;
}

/* the addon's tag absolute is needed: the level's of its name, else a place
after the level's, to walk */
static void graft_need(
	struct addon_graft_walk *walk,
	long absolute)
{
	struct addon_graft_instance const *instance = &walk->addon_instances[absolute];
	long level_tag;

	if (walk->map[absolute] != NONE)
		return;
	level_tag = addon_graft_find(walk->level_instances, walk->level_count, instance->group_tag, instance->name);
	if (level_tag != NONE)
	{
		walk->map[absolute] = level_tag;
		return;
	}
	if (!graft_group_get(instance->group_tag))
	{
		char message[320];

		snprintf(message, sizeof(message), "needs %.200s, of a group (%c%c%c%c) the validator does not know",
			instance->name ? instance->name : "?", (char)(instance->group_tag >> 24), (char)(instance->group_tag >> 16),
			(char)(instance->group_tag >> 8), (char)instance->group_tag);
		graft_fail(walk, message);
		return;
	}
	walk->map[absolute] = ((walk->first_salt + walk->next_index) << 16) | (walk->level_count + walk->next_index);
	walk->next_index++;
	walk->queue[walk->queue_count++] = absolute;
}

/* a reference's or tag index's handle: needed (_walk_collect), or made the
new table's (_walk_remap) */
static void graft_tag(
	struct addon_graft_walk *walk,
	long *tag_index)
{
	long absolute;

	if (*tag_index == NONE)
		return;
	absolute = graft_addon_index(walk, *tag_index);
	if (absolute == NONE)
	{
		/* (the validator made every one a tag of the addon's, or NONE) */
		*tag_index = NONE;
		return;
	}
	if (walk->mode == _walk_collect)
		graft_need(walk, absolute);
	else if (walk->map[absolute] != NONE)
		*tag_index = walk->map[absolute];
	else
		*tag_index = NONE;
}

/* (_walk_check) whether the handle names a tag of the new table of one of
the groups (a list ending with 0; NULL for any) */
static void graft_check(
	struct addon_graft_walk *walk,
	long tag_index,
	unsigned long const *groups,
	char const *field_name)
{
	struct addon_graft_instance const *instance;
	long absolute = tag_index & 0xFFFF;
	char message[320];

	if (tag_index == NONE)
		return;
	instance = absolute < walk->table_count ? &walk->table[absolute] : NULL;
	if (!instance || instance->tag_index != tag_index)
	{
		snprintf(message, sizeof(message), "its %.64s names %08lx, which is not a tag of the table",
			field_name ? field_name : "reference", (unsigned long)tag_index);
		if (!walk->problems++)
			graft_fail(walk, message);
		return;
	}
	if (groups)
	{
		unsigned long const *group;

		for (group = groups; *group; group++)
		{
			if (*group == instance->group_tag || *group == instance->parent_group_tags[0] ||
				*group == instance->parent_group_tags[1])
			{
				return;
			}
		}
		snprintf(message, sizeof(message), "its %.64s names %.200s, not of a group it takes",
			field_name ? field_name : "reference", instance->name ? instance->name : "?");
		if (!walk->problems++)
			graft_fail(walk, message);
	}
}

static void graft_element(
	struct addon_graft_walk *walk,
	byte *base,
	struct tag_schema_definition const *definition,
	short depth);

static void graft_fields(
	struct addon_graft_walk *walk,
	byte *base,
	struct tag_schema_definition const *definition,
	short depth)
{
	struct tag_schema_field const *field;

	for (field = definition->fields; field->type != _tag_schema_terminator && !walk->failed; field++)
	{
		short element;

		for (element = 0; element < field->count; element++)
		{
			byte *address = base + field->offset + element * field->size;

			switch (field->type)
			{
			case _tag_schema_block:
			{
				struct tag_block *block = (struct tag_block *)address;
				struct tag_schema_definition const *element_definition = field->definition;
				long index;

				for (index = 0; index < block->count && block->address; index++)
				{
					graft_element(walk, (byte *)block->address + index * element_definition->size, element_definition,
						(short)(depth + 1));
				}
				if (walk->mode == _walk_relocate)
					graft_move(walk, &block->address);
				break;
			}

			case _tag_schema_data:
				if (walk->mode == _walk_relocate)
					graft_move(walk, &((struct tag_data *)address)->address);
				break;

			case _tag_schema_file_data:
				if (walk->mode == _walk_relocate && ((struct tag_data *)address)->size > 0)
					graft_move_offset(walk, &((struct tag_data *)address)->file_offset);
				break;

			case _tag_schema_reference:
			{
				struct tag_reference *reference = (struct tag_reference *)address;

				if (walk->mode == _walk_relocate)
					graft_move(walk, (void **)&reference->name);
				else if (walk->mode == _walk_check)
					graft_check(walk, reference->index, field->definition, field->name);
				else
					graft_tag(walk, &reference->index);
				break;
			}

			case _tag_schema_tag_index:
				if (walk->mode == _walk_check)
					graft_check(walk, *(long *)address, field->definition, field->name);
				else if (walk->mode != _walk_relocate)
					graft_tag(walk, (long *)address);
				break;

			case _tag_schema_struct:
				graft_element(walk, address, field->definition, (short)(depth + 1));
				break;

			default:
				break;
			}
		}
	}
}

static void graft_element(
	struct addon_graft_walk *walk,
	byte *base,
	struct tag_schema_definition const *definition,
	short depth)
{
	if (depth >= ADDON_GRAFT_MAXIMUM_DEPTH)
	{
		graft_fail(walk, "has blocks too deep");
		return;
	}
	graft_fields(walk, base, definition, depth);
}

/* what the schemas do not name, moved with a tag: a model's parts' pointers
to the buffers (the header's), a bitmap group's pixels' place in the file.
(Before the tag's blocks are walked: they are found through the blocks'
pointers, which the walk moves.) */
static void graft_relocate_extras(
	struct addon_graft_walk *walk,
	struct addon_graft_instance *instance)
{
	if (instance->group_tag == ADDON_GRAFT_MODEL_GROUP_TAG)
	{
		struct model *model = instance->base_address;
		long geometry_index;

		for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
		{
			struct addon_graft_model_geometry *geometry =
				(struct addon_graft_model_geometry *)model->geometries.address + geometry_index;
			long part_index;

			for (part_index = 0; part_index < geometry->parts.count; part_index++)
			{
				struct addon_graft_model_geometry_part *part =
					(struct addon_graft_model_geometry_part *)geometry->parts.address + part_index;

				graft_move(walk, &part->vertex_buffer.base_address);
				graft_move(walk, &part->vertex_buffer.hardware_format);
				graft_move(walk, &part->triangle_buffer.base_address);
				graft_move(walk, &part->triangle_buffer.hardware_format);
			}
		}
	}
	else if (instance->group_tag == ADDON_GRAFT_BITMAP_GROUP_TAG)
	{
		/* (a cache file's bitmaps are read at their own pixels' offsets,
		xbox_texture_cache.c's texture_cache_start_loading_bitmap: the
		group's pixel data offset is only added for a bitmap made at run
		time, texture_cache_bitmap_new, which the tools leave 0 and the
		validator checks either way) */
		struct bitmap_group *group = instance->base_address;
		long bitmap_index;

		for (bitmap_index = 0; bitmap_index < group->bitmaps.count; bitmap_index++)
		{
			struct bitmap_data *bitmap = (struct bitmap_data *)group->bitmaps.address + bitmap_index;

			if (bitmap->pixels_size > 0)
				graft_move_offset(walk, &bitmap->pixels_offset);
		}
	}
}

/* ---------- public code */

long addon_graft_find(
	struct addon_graft_instance const *instances,
	long count,
	unsigned long group_tag,
	char const *name)
{
	long index;

	if (!name)
		return NONE;
	for (index = 0; index < count; index++)
	{
		if (instances[index].group_tag == group_tag && instances[index].name &&
			graft_name_equal(instances[index].name, name))
		{
			return instances[index].tag_index;
		}
	}
	return NONE;
}

boolean addon_graft_relocate(
	struct addon_graft_header *header,
	long tag_data_size,
	long delta,
	unsigned long file_base,
	char *error,
	long error_size)
{
	struct addon_graft_walk walk;
	long index;

	memset(&walk, 0, sizeof(walk));
	walk.mode = _walk_relocate;
	walk.low = (byte *)header;
	walk.high = (byte *)header + tag_data_size;
	walk.delta = delta;
	walk.file_base = file_base;
	walk.error = error;
	walk.error_size = error_size;
	if (error && error_size > 0)
		error[0] = 0;

	/* every tag of a group the schemas know */
	for (index = 0; index < header->tag_count && !walk.failed; index++)
	{
		struct addon_graft_instance *instance = &header->instances[index];
		struct tag_schema_group const *group = graft_group_get(instance->group_tag);

		if (!instance->base_address || !group || !group->definition)
			continue;
		graft_relocate_extras(&walk, instance);
		graft_element(&walk, instance->base_address, group->definition, 0);
	}
	/* the buffers' data, then the table's names and roots, then the header's
	own pointers (the walks above read through them) */
	for (index = 0; index < header->vertex_buffer_count; index++)
		graft_move(&walk, (void **)(header->vertex_buffers + index * ADDON_GRAFT_BUFFER_SIZE +
			ADDON_GRAFT_BUFFER_DATA_OFFSET));
	for (index = 0; index < header->index_buffer_count; index++)
		graft_move(&walk, (void **)(header->index_buffers + index * ADDON_GRAFT_BUFFER_SIZE +
			ADDON_GRAFT_BUFFER_DATA_OFFSET));
	for (index = 0; index < header->tag_count; index++)
	{
		graft_move(&walk, (void **)&header->instances[index].name);
		graft_move(&walk, &header->instances[index].base_address);
	}
	graft_move(&walk, (void **)&header->vertex_buffers);
	graft_move(&walk, (void **)&header->index_buffers);
	graft_move(&walk, (void **)&header->instances);

	return !walk.failed;
}

long addon_graft_tags(
	struct addon_graft_instance const *level_instances,
	long level_count,
	struct addon_graft_header *addon,
	char const *root_name,
	unsigned long root_group,
	struct addon_graft_instance *instances,
	char *error,
	long error_size)
{
	/* (the addon's table is at most a map's: 65535 tags) */
	static long map[ADDON_GRAFT_MAXIMUM_TAGS];
	static long queue[ADDON_GRAFT_MAXIMUM_TAGS];
	struct addon_graft_walk walk;
	long root;
	long index;
	long salt = 0;

	memset(&walk, 0, sizeof(walk));
	walk.mode = _walk_collect;
	walk.addon_instances = addon->instances;
	walk.addon_count = addon->tag_count;
	walk.map = map;
	walk.queue = queue;
	walk.level_instances = level_instances;
	walk.level_count = level_count;
	walk.error = error;
	walk.error_size = error_size;
	if (error && error_size > 0)
		error[0] = 0;
	if (addon->tag_count < 0 || addon->tag_count >= ADDON_GRAFT_MAXIMUM_TAGS || level_count < 0 ||
		level_count + addon->tag_count >= ADDON_GRAFT_MAXIMUM_TAGS)
	{
		graft_fail(&walk, "has too many tags for one table with the level's");
		return NONE;
	}
	for (index = 0; index < level_count; index++)
	{
		long instance_salt = (long)((unsigned long)level_instances[index].tag_index >> 16);

		if (instance_salt > salt)
			salt = instance_salt;
	}
	walk.first_salt = salt + 1;
	for (index = 0; index < addon->tag_count; index++)
		map[index] = NONE;

	root = addon_graft_find(addon->instances, addon->tag_count, root_group, root_name);
	if (root == NONE)
	{
		char message[320];

		snprintf(message, sizeof(message), "has no tag %.256s", root_name);
		graft_fail(&walk, message);
		return NONE;
	}
	/* (the root is always the addon's own, even if the level has one of its
	name: it is what the addon is for) */
	{
		long absolute = root & 0xFFFF;

		if (!graft_group_get(addon->instances[absolute].group_tag))
		{
			graft_fail(&walk, "has its tag of a group the validator does not know");
			return NONE;
		}
		map[absolute] = ((walk.first_salt + walk.next_index) << 16) | (level_count + walk.next_index);
		walk.next_index++;
		queue[walk.queue_count++] = absolute;
	}

	/* the tags it needs, breadth first */
	for (index = 0; index < walk.queue_count && !walk.failed; index++)
	{
		struct addon_graft_instance const *instance = &addon->instances[queue[index]];
		struct tag_schema_group const *group = graft_group_get(instance->group_tag);

		if (instance->base_address && group && group->definition)
			graft_element(&walk, instance->base_address, group->definition, 0);
	}
	if (walk.failed)
		return NONE;

	/* their references and tag indices, the new table's */
	walk.mode = _walk_remap;
	for (index = 0; index < walk.queue_count; index++)
	{
		struct addon_graft_instance const *instance = &addon->instances[queue[index]];
		struct tag_schema_group const *group = graft_group_get(instance->group_tag);

		if (instance->base_address && group && group->definition)
			graft_element(&walk, instance->base_address, group->definition, 0);
	}

	/* the table: the level's, then the tags added, in the order they were
	found (so each one's handle is its place) */
	memcpy(instances, level_instances, (size_t)level_count * sizeof(*instances));
	for (index = 0; index < walk.queue_count; index++)
	{
		struct addon_graft_instance *added = &instances[level_count + index];

		*added = addon->instances[queue[index]];
		added->tag_index = map[queue[index]];
	}

	return level_count + walk.queue_count;
}

long addon_graft_references(
	struct addon_graft_instance const *instances,
	long count,
	long first,
	char *error,
	long error_size)
{
	struct addon_graft_walk walk;
	long index;

	memset(&walk, 0, sizeof(walk));
	walk.mode = _walk_check;
	walk.table = instances;
	walk.table_count = count;
	walk.error = error;
	walk.error_size = error_size;
	if (error && error_size > 0)
		error[0] = 0;
	for (index = first; index < count; index++)
	{
		struct tag_schema_group const *group = graft_group_get(instances[index].group_tag);

		/* (a failure stops a walk: each tag is walked on its own) */
		walk.failed = FALSE;
		if (instances[index].base_address && group && group->definition)
			graft_element(&walk, instances[index].base_address, group->definition, 0);
	}
	return walk.problems;
}
