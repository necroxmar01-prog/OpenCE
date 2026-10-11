/*
ADDON_TAGS.H

The armor's addon grafted onto the campaign level as it loads
(addon_tags.c): cache_files.c's scenario_tags_load and scenario_tags_unload,
and cache_file_read for the addon's pixels and samples.
*/

#ifndef __ADDON_TAGS_H
#define __ADDON_TAGS_H

/* Before a level's tags are read (map_type the level's header's: only a
campaign level, 0, gets one): the addon of the armor chosen
(player_armor.c) read, checked by the validator where the level's tags go
(tag_cache), and moved to memory of its own. FALSE if there is none, or it
cannot be (logged): the level loads as it is. */
boolean addon_tags_prepare(
	void *tag_cache,
	short map_type);

/* Once the level's tags are loaded and checked: the addon's tag and those it
needs added to the table (cache_files_set_tag_instances), its vertex and
index buffers registered. Nothing if addon_tags_prepare gave none. */
void addon_tags_loaded(
	void);

/* the reads of the addon's pixels and samples (offsets past
ADDON_GRAFT_FILE_BASE): served from it at once, TRUE; FALSE for any other
read */
boolean addon_tags_read(
	long offset,
	long size,
	void *buffer);

/* when the level's tags go, after the texture and sound caches let go of
the addon's bitmaps and sounds: its buffers, memory and table let go */
void addon_tags_unloaded(
	void);

#endif
