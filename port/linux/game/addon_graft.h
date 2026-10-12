/*
ADDON_GRAFT.H

An addon's tags grafted onto a level's (addon_graft.c): the format work,
with no file or memory of its own, so that tools/addon_check.c runs it on
map files as the game does (addon_tags.c).
*/

#ifndef __ADDON_GRAFT_H
#define __ADDON_GRAFT_H

/* ---------- constants */

/* where the addons' pixels and samples are, in the offsets the game reads a
map's file with: past any map's own (the largest Xbox map is under 300 MB),
each addon's at its own base, ADDON_GRAFT_FILE_SPAN apart (addon_tags.c
has several at once: an armor's and weapons') */
#define ADDON_GRAFT_FILE_BASE 0x40000000UL
#define ADDON_GRAFT_FILE_SPAN 0x04000000UL

/* ---------- structures */

/* a tag of a map's table, as cache_files.c's struct cache_file_tag_instance */
struct addon_graft_instance
{
	unsigned long group_tag;
	unsigned long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

/* a map's tag header, as cache_files.c's struct cache_file_tag_header */
struct addon_graft_header
{
	struct addon_graft_instance *instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	byte *vertex_buffers;
	long index_buffer_count;
	byte *index_buffers;
	unsigned long signature;
};

/* ---------- prototypes/ADDON_GRAFT.C */

/* The addon's tags at header (tag_data_size bytes there), as the validator
passed them (tag_validate_tags, where they were read), made to work delta
bytes away, where the caller then copies them: every pointer into them moved
by delta (the tag table and names, blocks, data, references' names, vertex
and index buffers and the models' parts' pointers to them), and every offset
of their pixels and samples in the addon's file moved past the level's, by
file_base (ADDON_GRAFT_FILE_BASE, the first addon's). FALSE, with why in
error, if a tag cannot be. */
boolean addon_graft_relocate(
	struct addon_graft_header *header,
	long tag_data_size,
	long delta,
	unsigned long file_base,
	char *error,
	long error_size);

/* The level's table (level_count tags of level_instances) with the addon's
tag root_name of group root_group added, and every tag it needs that the
level has not got (one of the level's of the same group and name is used
instead), in instances (room for level_count + the addon's count), the
addon's references and tag indices made the new table's. The addon's tags
(relocated) must be where the game will read them. Returns the new table's
count, or NONE with why in error (no such tag, or one it needs of a group
the schemas do not know). */
long addon_graft_tags(
	struct addon_graft_instance const *level_instances,
	long level_count,
	struct addon_graft_header *addon,
	char const *root_name,
	unsigned long root_group,
	struct addon_graft_instance *instances,
	char *error,
	long error_size);

/* how many references and tag indices of the table's tags from first on do
not name a tag of the table of a group they take (0: all do); the first
one's why in error */
long addon_graft_references(
	struct addon_graft_instance const *instances,
	long count,
	long first,
	char *error,
	long error_size);

/* the tag handle of the table's tag of group (that group itself) and name
(as the game compares names: in any case), or NONE */
long addon_graft_find(
	struct addon_graft_instance const *instances,
	long count,
	unsigned long group_tag,
	char const *name);

#endif
