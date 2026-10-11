/*
ADDON_CHECK.C

An addon grafted onto a level as the game grafts it (port/linux/game/
addon_tags.c, addon_graft.c), on map files alone (docs/addons.md):

1. the addon's tag data is read where the game reads a map's (the tag
   cache) and checked by the validator (port/linux/game/tag_validate.c);
2. it is relocated to memory of its own (addon_graft_relocate) and checked
   by the validator again there, its pixels' and samples' offsets past the
   level's: a relocation that missed a pointer, or moved what is not one,
   is refused or corrected;
3. the level's tag data is read into the tag cache and checked;
4. the addon's tag is grafted onto the level's table (addon_graft_tags),
   and every tag added is checked: it lies in the addon's memory, and each
   of its references and tag indices names a tag of the new table of a
   group it takes (addon_graft_references).

Prints the tags added; exits 1 if any step fails.

Built by ninja linux as build/linux/addon_check, as map_validate is.

usage: addon_check level.map addon.map tag [group]
	tag: the addon's tag to graft, as the game names it
	(halo3\characters\odst\odst); group: its group, bipd by default
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "zlib_prefixed.h"

/* ---------- constants */

enum
{
	TAG_CACHE_BASE_ADDRESS = 0x803A6000,
	TAG_CACHE_SIZE = 0x1600000,
	CACHE_FILE_HEADER_SIZE = 0x800,
	CACHE_FILE_HEADER_SIGNATURE = 'head',
	CACHE_FILE_FOOTER_SIGNATURE = 'foot',
	ADDON_GRAFT_FILE_BASE = 0x40000000,
	MAXIMUM_TAGS = 0x10000,
};

/* ---------- structures (addon_graft.h's, in this program's ABI) */

struct cache_file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	unsigned long reserved0C;
	long tag_data_offset;
	long tag_data_size;
	unsigned char reserved18[8];
	char name[0x20];
	char build[0x20];
	unsigned char reserved60[0x79C];
	unsigned long footer_signature;
};

struct addon_graft_instance
{
	unsigned long group_tag;
	unsigned long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct addon_graft_header
{
	struct addon_graft_instance *instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	unsigned char *vertex_buffers;
	long index_buffer_count;
	unsigned char *index_buffers;
	unsigned long signature;
};

typedef char verify_cache_file_header_size[sizeof(struct cache_file_header) == CACHE_FILE_HEADER_SIZE ? 1 : -1];
typedef char verify_addon_graft_instance_size[sizeof(struct addon_graft_instance) == 0x20 ? 1 : -1];

/* ---------- the validator's and the graft's (port/linux/game) */

unsigned char tag_validate_tags(void *tag_header, long tag_data_size, long file_length, char const *map_name);
long tag_validate_corrections(void);
unsigned char addon_graft_relocate(struct addon_graft_header *header, long tag_data_size, long delta, char *error,
	long error_size);
long addon_graft_tags(struct addon_graft_instance const *level_instances, long level_count,
	struct addon_graft_header *addon, char const *root_name, unsigned long root_group,
	struct addon_graft_instance *instances, char *error, long error_size);
long addon_graft_references(struct addon_graft_instance const *instances, long count, long first,
	char *error, long error_size);

void tag_validate_report(char const *message)
{
	printf("  %s\n", message);
}

/* (the game's printf family and string functions, which the validator and
the graft call by these names: port/linux/include/stdio.h, cseries.h) */
int halo_linux_vsnprintf(char *buffer, size_t count, char const *format, va_list arguments)
{
	return vsnprintf(buffer, count, format, arguments);
}

int halo_linux_snprintf(char *buffer, size_t count, char const *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

void *csmemset(void *buffer, long c, unsigned long size)
{
	return memset(buffer, (int)c, size);
}

void *csmemcpy(void *destination, void const *source, unsigned long size)
{
	return memcpy(destination, source, size);
}

unsigned long csstrlen(char const *s)
{
	return strlen(s);
}

char *csstrncpy(char *s1, char const *s2, unsigned long size)
{
	return strncpy(s1, s2, size);
}

char *csstrcat(char *s1, char const *s2)
{
	return strcat(s1, s2);
}

long csstrcmp(char const *s1, char const *s2)
{
	return strcmp(s1, s2);
}

/* ---------- code */

/* the map's bytes as the game reads them: inflated after its header if it
is compressed (map_validate.c's map_read) */
static unsigned char *map_read(char const *path, struct cache_file_header *header)
{
	FILE *file = fopen(path, "rb");
	unsigned char *bytes = NULL;
	unsigned char *compressed = NULL;
	long size;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < CACHE_FILE_HEADER_SIZE || fseek(file, 0, SEEK_SET) ||
		fread(header, CACHE_FILE_HEADER_SIZE, 1, file) != 1 ||
		header->header_signature != CACHE_FILE_HEADER_SIGNATURE ||
		header->footer_signature != CACHE_FILE_FOOTER_SIGNATURE ||
		header->file_length < CACHE_FILE_HEADER_SIZE || size > header->file_length)
	{
		fclose(file);
		return NULL;
	}
	bytes = malloc(header->file_length);
	compressed = malloc(size - CACHE_FILE_HEADER_SIZE);
	if (bytes && compressed && fread(compressed, size - CACHE_FILE_HEADER_SIZE, 1, file) == 1)
	{
		memcpy(bytes, header, CACHE_FILE_HEADER_SIZE);
		if (size == header->file_length)
		{
			memcpy(bytes + CACHE_FILE_HEADER_SIZE, compressed, size - CACHE_FILE_HEADER_SIZE);
		}
		else
		{
			uLongf inflated = header->file_length - CACHE_FILE_HEADER_SIZE;

			if (uncompress(bytes + CACHE_FILE_HEADER_SIZE, &inflated, compressed, size - CACHE_FILE_HEADER_SIZE) != Z_OK ||
				inflated != (uLongf)(header->file_length - CACHE_FILE_HEADER_SIZE))
			{
				free(bytes);
				bytes = NULL;
			}
		}
	}
	else
	{
		free(bytes);
		bytes = NULL;
	}
	free(compressed);
	fclose(file);
	return bytes;
}

static int tag_data_fits(struct cache_file_header const *header)
{
	return header->tag_data_offset >= 0 && header->tag_data_size > 0 && header->tag_data_size <= TAG_CACHE_SIZE &&
		header->tag_data_offset <= header->file_length - header->tag_data_size;
}

static void group_text(unsigned long group, char *text)
{
	int index;

	for (index = 0; index < 4; index++)
		text[index] = (char)(group >> (24 - 8 * index));
	text[4] = 0;
}

int main(int argc, char **argv)
{
	unsigned char *tag_cache;
	struct cache_file_header level_header, addon_header;
	unsigned char *level_bytes, *addon_bytes, *addon_memory;
	struct addon_graft_header *addon;
	struct addon_graft_header *level;
	static struct addon_graft_instance instances[MAXIMUM_TAGS];
	char error[400];
	unsigned long group = 'bipd';
	long delta, count, index, added = 0;

	if (argc < 4 || argc > 5)
	{
		fprintf(stderr, "usage: addon_check level.map addon.map tag [group]\n");
		return 2;
	}
	if (argc == 5)
	{
		if (strlen(argv[4]) != 4)
		{
			fprintf(stderr, "addon_check: a group is four characters (bipd)\n");
			return 2;
		}
		group = (unsigned long)(unsigned char)argv[4][0] << 24 | (unsigned long)(unsigned char)argv[4][1] << 16 |
			(unsigned long)(unsigned char)argv[4][2] << 8 | (unsigned char)argv[4][3];
	}
	tag_cache = mmap((void *)TAG_CACHE_BASE_ADDRESS, TAG_CACHE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (tag_cache != (unsigned char *)TAG_CACHE_BASE_ADDRESS)
	{
		fprintf(stderr, "addon_check: the tag cache's address is taken\n");
		return 2;
	}
	level_bytes = map_read(argv[1], &level_header);
	addon_bytes = map_read(argv[2], &addon_header);
	if (!level_bytes || !tag_data_fits(&level_header) || !addon_bytes || !tag_data_fits(&addon_header))
	{
		fprintf(stderr, "addon_check: %s or %s cannot be read\n", argv[1], argv[2]);
		return 1;
	}

	/* 1. the addon where a map's tags are read, checked */
	printf("addon %s: checked in the tag cache\n", addon_header.name);
	memset(tag_cache, 0xCD, TAG_CACHE_SIZE);
	memcpy(tag_cache, addon_bytes + addon_header.tag_data_offset, addon_header.tag_data_size);
	if (!tag_validate_tags(tag_cache, addon_header.tag_data_size, addon_header.file_length, addon_header.name))
	{
		printf("addon: refused by the validator\n");
		return 1;
	}
	printf("  %ld corrections\n", tag_validate_corrections());

	/* 2. relocated to its own memory, and checked again there */
	addon_memory = mmap(NULL, TAG_CACHE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addon_memory == MAP_FAILED)
	{
		fprintf(stderr, "addon_check: no memory for the addon\n");
		return 2;
	}
	delta = (long)(addon_memory - tag_cache);
	if (!addon_graft_relocate((struct addon_graft_header *)tag_cache, addon_header.tag_data_size, delta, error,
		sizeof(error)))
	{
		printf("addon: cannot be relocated: %s\n", error);
		return 1;
	}
	memcpy(addon_memory, tag_cache, addon_header.tag_data_size);
	printf("addon: relocated by %+ld bytes to %p; checked there\n", delta, (void *)addon_memory);
	if (!tag_validate_tags(addon_memory, addon_header.tag_data_size,
		(long)(ADDON_GRAFT_FILE_BASE + (unsigned long)addon_header.file_length), addon_header.name))
	{
		printf("addon: refused once relocated\n");
		return 1;
	}
	printf("  %ld corrections\n", tag_validate_corrections());
	if (tag_validate_corrections())
	{
		printf("addon: relocated, it needed corrections it did not need before\n");
		return 1;
	}
	addon = (struct addon_graft_header *)addon_memory;

	/* 3. the level */
	printf("level %s: checked in the tag cache\n", level_header.name);
	memset(tag_cache, 0xCD, TAG_CACHE_SIZE);
	memcpy(tag_cache, level_bytes + level_header.tag_data_offset, level_header.tag_data_size);
	if (!tag_validate_tags(tag_cache, level_header.tag_data_size, level_header.file_length, level_header.name))
	{
		printf("level: refused by the validator\n");
		return 1;
	}
	printf("  %ld corrections\n", tag_validate_corrections());
	level = (struct addon_graft_header *)tag_cache;

	/* 4. grafted, and what was added checked */
	count = addon_graft_tags(level->instances, level->tag_count, addon, argv[3], group, instances, error,
		sizeof(error));
	if (count == -1)
	{
		printf("graft: refused: %s\n", error);
		return 1;
	}
	for (index = level->tag_count; index < count; index++)
	{
		char text[5];

		group_text(instances[index].group_tag, text);
		if ((unsigned char *)instances[index].base_address < addon_memory ||
			(unsigned char *)instances[index].base_address >= addon_memory + addon_header.tag_data_size)
		{
			printf("graft: %s is not in the addon's memory\n", instances[index].name);
			return 1;
		}
		printf("  + %s.%s (%08lx)\n", instances[index].name, text, (unsigned long)instances[index].tag_index);
		if (instances[index].group_tag == 'bitm')
		{
			/* (bitmap_group.h: pixel_data at 0x30, bitmaps block at 0x60) */
			unsigned char *group = instances[index].base_address;
			long pixel_offset = *(long *)(group + 0x30 + 8);
			long bitmap_count = *(long *)(group + 0x60);
			unsigned char *bitmaps = *(unsigned char **)(group + 0x60 + 4);

			printf("      pixel data at %08lx; %ld bitmaps, the first's at %08lx, %ld bytes\n",
				(unsigned long)pixel_offset, bitmap_count,
				bitmap_count ? (unsigned long)*(long *)(bitmaps + 0x18) : 0UL,
				bitmap_count ? *(long *)(bitmaps + 0x1C) : 0L);
		}
		added++;
	}
	if (addon_graft_references(instances, count, level->tag_count, error, sizeof(error)) != 0)
	{
		printf("graft: %s\n", error);
		return 1;
	}
	printf("graft: %ld tags added to the level's %ld; every reference of theirs names a tag of the new table\n",
		added, level->tag_count);
	return 0;
}
