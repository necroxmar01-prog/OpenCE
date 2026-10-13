/*
NETWORK_ADDONS.H

Addons and armors in network co-op (network_addons.c): every machine of a
co-op game grafts the host's addons, downloaded from it as maps are
(map_share.c), and the host spawns each player in the armor its machine
chose.
*/

#ifndef __NETWORK_ADDONS_H
#define __NETWORK_ADDONS_H

#include "cseries.h"
#include "player_armor.h"
#include "weapon_mods.h"

enum
{
	NETWORK_ADDONS_MAXIMUM = 12,
	NETWORK_ADDON_NAME_LENGTH = 48,
	NETWORK_ADDON_PATH_LENGTH = 256,
	/* where a client keeps the host's addons, named for their hashes */
	NETWORK_ADDON_FOLDER_LENGTH = 32,
};

#define NETWORK_ADDONS_FOLDER "d:\\shared_addons\\"
/* a download's level name (map_share.c): this, then the addon's name */
#define NETWORK_ADDONS_LEVEL_PREFIX "addons\\"

/* an addon of the game: its file, its name in the logs, the tag grafted,
and a weapon mod's game tags it replaces */
struct network_addon_load
{
	char path[NETWORK_ADDON_PATH_LENGTH];
	char name[NETWORK_ADDON_NAME_LENGTH];
	unsigned long root_group;
	char root[WEAPON_MOD_NAME_LENGTH];
	boolean weapon;
	struct weapon_mod weapon_mod;
};

/* The addons a network co-op game's levels get, in the order every
machine grafts them (addon_tags.c, as a level loads): the host's own, or on
a client the host's it has downloaded. NONE: not a network co-op game (the
level gets this machine's own, as in a game alone). */
short network_addons_for_level(
	struct network_addon_load *addons,
	short maximum);

/* (a client) whether the host's addons are not all here yet: the level is
not loaded (cache_files.c gives it no time to precache), and players
joining a game in progress wait (map_share_holds_players) */
boolean network_addons_pending(
	void);

/* (the host, spawning a player of another machine) the armor that machine
chose, else FALSE */
boolean network_addons_player_dress(
	long player_index,
	struct player_armor_dress *dress);

/* (the host, network_server_manager.c) whether a machine has the host's
addons whole: no game starts before every machine has (one that loaded the
level without one would not have the host's tags) */
boolean network_addons_machine_ready(
	long machine_index);

/* (shoulder_camera.c) the camera of a player of another machine: 0 first
person, 1 over the right shoulder, 2 over the left (as its machine told the
host, and the host every machine) */
short network_addons_player_camera(
	long player_index);

/* (map_share.c) a download's level name that is an addon's; its file there
(the host's), else FALSE */
boolean network_addons_level_name(
	char const *level_name);
boolean network_addons_host_path(
	char const *level_name,
	char *path,
	long size);
/* (map_share.c) the files this machine sends of the game's addons: the
catalog's (map_share_files.c), NONE where there is none */
short network_addons_catalog_file(
	short index);
/* (map_share.c) an addon downloaded whole (its final path) */
void network_addons_downloaded(
	char const *level_name);

/* the messages (network_distributed.c, and the host's message handler), in
the lobby too, from the sender's connection */
boolean network_addons_handles_message(
	word const *message,
	word size);
void network_addons_handle_message(
	long machine_index,
	word const *message,
	word size,
	boolean from_stream);

/* every frame (map_share_update) */
void network_addons_update(
	void);

#endif
