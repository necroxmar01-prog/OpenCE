/*
NETWORK_ADDONS.C

Addons and armors in network co-op (network_addons.h; docs/addons.md,
"Network co-op").

The distributed netcode names tags by index, so every machine of a game must
have the same tags: the level's, and the same addons grafted onto it in the
same order (addon_tags.c). In a co-op game they are the host's:

- The host's addons: the armor it chose (if an addon brings it), every armor
  of pieces (an armor kit: any player may wear one), the armors of its own
  that the clients' players wear (of the same biped: one a client chose
  from mods the host has too), and its weapon mods,
  hashed as map sharing hashes maps (map_share_files.c). While the game is in
  its lobby the set follows what the host has; once its level loads, it is
  the set that level got.
- The host tells each client (_distributed_message_addons, an addon a
  message, reliably, again when the set changes or a machine joins): each
  addon's name (the host's, then _ and its hash's first 8 digits), size,
  hash, the tag grafted and a weapon mod's replacements.
- A client keeps them in d:\shared_addons\, named for their hashes. One it
  has not is downloaded from the host and the others as a Custom Edition map
  is (map_share.c: its level name is addons\<name>). The level is not
  loaded until it has them all (network_addons_pending: the lobby waits, as
  for a map), and then gets them in the host's order.
- Each client sends the armor it chose, a dress (player_armor.c: the biped
  and each region's permutation), when it changes and every few seconds
  (_distributed_message_armor_dress). The host spawns that machine's
  player in it (player_spawn, which only the host runs in co-op), and the
  unit goes to every machine as the host made it, permutations and all
  (network_objects.c). An armor whose biped the game has not (one the host
  has not, or chosen after the level loaded) is the level's Spartan.
- The over-the-shoulder camera (shoulder_camera.c): the shots of a player
  over the shoulder go where that player's camera points, which every
  machine works out from the unit (shoulder_camera_deterministic) if it
  knows which camera the player has. A client tells the host its own with
  its dress; the host tells every client every player's
  (_distributed_message_player_cameras, reliably, when it changes or a
  machine joins).
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "memory/data.h"
#include "game/players.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_client_manager.h"
#include "custom_edition_maps.h"
#include "map_share.h"
#include "network_distributed.h"
#include "network_addons.h"
#include "shoulder_camera.h"
#include "addon_tags.h"
#include "text/unicode.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

/* port_config.c's, network_game_globals.c's, network_server_message_handler.c's,
network_server_manager.c's, cseries_windows.c's, main.c's */
int config_boolean(const char *name);
boolean network_distributed_client_send_reliably(void *message, word size);
boolean network_distributed_server_send_to_machine_reliably(long machine_index, void *message, word size);
unsigned long network_game_server_machine_address(long machine_index);
unsigned long system_milliseconds(void);
short main_get_solo_level_from_name(char const *name);
boolean map_share_addon_download_begin(char const *level_name);
boolean map_share_download_idle(void);

/* ---------- constants */

enum
{
	NETWORK_ADDONS_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	/* the host looks at its own addons this often in the lobby */
	HOST_REBUILD_INTERVAL = 3000,
	/* a client sends its armor on a change, and again this often */
	DRESS_INTERVAL = 30000,
	/* the host waits for its addons' hashes this long as its level loads */
	HASH_WAIT = 10000,
	/* a client whose host has not told it of its addons yet loads no level:
	every host of this network version tells of them (an empty set too), so
	only one that never answers is given up on, after this long */
	MANIFEST_TIMEOUT = 120000,
	/* the players' cameras the host tells of (by their absolute index) */
	NETWORK_ADDONS_CAMERA_PLAYERS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	ADDON_TAGS_GROUP_BIPED = 'bipd',
};

enum
{
	_network_addon_armor,
	_network_addon_weapon,
};

/* ---------- structures */

struct distributed_addon_replacement
{
	unsigned long group_tag;
	char stock[WEAPON_MOD_NAME_LENGTH];
	char mod[WEAPON_MOD_NAME_LENGTH];
};

/* an addon of the host's set (the set's signature, the addon's place and
the set's count: an empty set is one message of count 0) */
struct distributed_addon
{
	unsigned long signature;
	byte index;
	byte count;
	byte role;
	byte replacement_count;
	unsigned long size;
	byte hash[MAP_SHARE_HASH_BYTES];
	char name[NETWORK_ADDON_NAME_LENGTH];
	unsigned long root_group;
	char root[WEAPON_MOD_NAME_LENGTH];
	struct distributed_addon_replacement replacements[WEAPON_MOD_MAXIMUM_REPLACEMENTS];
};

struct distributed_armor_dress
{
	byte local_player_index;
	/* the machine's camera (shoulder_camera_local_mode) */
	byte camera;
	byte pad[2];
	/* the signature of the host's set the machine has whole, every addon
	here (0: not yet): the host starts no game before (a level loaded
	without one of them would not have the host's tags) */
	unsigned long addons_ready;
	struct player_armor_dress dress;
};

/* every player's camera, as the host knows them (by the players' absolute
indices; 0: first person) */
struct distributed_player_cameras
{
	/* the host's level as its addons left it (addon_tags.c): a client whose
	own is not has not the host's tags, and is told (network_addons_mismatch) */
	unsigned long tags_signature;
	long tag_count;
	word count;
	word pad;
	struct
	{
		word player;
		byte camera;
		byte pad;
	} players[NETWORK_ADDONS_CAMERA_PLAYERS];
};

typedef char distributed_addon_size_assert[sizeof(struct distributed_addon) < 4000 ? 1 : -1];
typedef char distributed_armor_dress_size_assert[sizeof(struct distributed_armor_dress) < 4000 ? 1 : -1];
typedef char distributed_player_cameras_size_assert[sizeof(struct distributed_player_cameras) < 4000 ? 1 : -1];

/* an addon of a set: the message's, and this machine's file and catalog
file (map_share_files.c) */
struct network_addon
{
	struct distributed_addon entry;
	/* the host's addon name (no hash), for the logs */
	char base_name[NETWORK_ADDON_NAME_LENGTH];
	char path[NETWORK_ADDON_PATH_LENGTH];
	short catalog;
	boolean present;
	struct weapon_mod weapon_mod;
};

struct network_addon_set
{
	unsigned long signature;
	/* (the host's) every addon hashed or failed: the set may be told of */
	boolean settled;
	short count;
	struct network_addon addons[NETWORK_ADDONS_MAXIMUM];
};

/* ---------- globals */

static struct
{
	/* the host: the set now (rebuilt in the lobby) and the set the level
	got; what each machine was sent */
	struct network_addon_set host_set;
	struct network_addon_set loaded_set;
	boolean loaded;
	unsigned long built_at;
	unsigned long sent_signature[NETWORK_ADDONS_MACHINES];
	unsigned long sent_address[NETWORK_ADDONS_MACHINES];
	/* the host: the armors the clients chose, by machine */
	boolean dress_valid[NETWORK_ADDONS_MACHINES];
	struct player_armor_dress dresses[NETWORK_ADDONS_MACHINES];
	byte machine_cameras[NETWORK_ADDONS_MACHINES];
	unsigned long machine_ready[NETWORK_ADDONS_MACHINES];
	/* every player's camera: the host's as last told of to each machine,
	and the table (a client's from the host) */
	unsigned long cameras_sent_signature[NETWORK_ADDONS_MACHINES];
	unsigned long cameras_sent_address[NETWORK_ADDONS_MACHINES];
	byte player_cameras[NETWORK_ADDONS_CAMERA_PLAYERS];
	/* a client: the host's level as its addons left it, and whether this
	machine's was told of as different (logged once) */
	unsigned long host_tags_signature;
	long host_tag_count;
	boolean mismatch_logged;

	/* a client: the set the host told of (whole once every addon of it came),
	the one coming, and since when this game has waited for it */
	struct network_addon_set client_set;
	boolean client_complete;
	struct network_addon_set coming;
	unsigned long coming_received;
	unsigned long waiting_since;
	boolean timed_out;
	char game_level[64];
	/* a client: its armor, as last sent */
	struct player_armor_dress dress_sent;
	byte camera_sent;
	unsigned long ready_sent;
	unsigned long dress_sent_at;
	boolean dress_ever_sent;
} network_addons;

/* ---------- private code */

static struct network_game *addons_network_game(
	void)
{
	struct network_game_client *client = global_network_game_client_get();

	return client ? network_game_client_get_game(client) : NULL;
}

static boolean addons_host(
	void)
{
	return global_network_game_server_get() != NULL;
}

/* whether a level name is a campaign level's (a co-op game's) */
static boolean addons_campaign_level(
	char const *level_name)
{
	return level_name && level_name[0] &&
		(main_get_solo_level_from_name(level_name) != NONE || custom_edition_maps_level_campaign(level_name));
}

static void addons_copy(
	char *destination,
	char const *source,
	size_t size)
{
	strncpy(destination, source, size - 1);
	destination[size - 1] = 0;
}

static boolean addons_string_valid(
	char const *text,
	size_t size)
{
	return memchr(text, 0, size) != NULL;
}

static void addons_fill_header(
	struct distributed_message_header *header,
	byte type,
	word size)
{
	header->header = 0;
	build_message_header(&header->header, size, 2, 0);
	header->type = type;
	header->count = 1;
	header->game_time = game_in_progress() ? game_time_get() : 0;
}

static unsigned long addons_signature(
	struct network_addon_set const *set)
{
	unsigned long signature = 0x0ADD0115;
	short index;

	for (index = 0; index < set->count; index++)
	{
		struct distributed_addon const *entry = &set->addons[index].entry;
		unsigned char const *byte_pointer = (unsigned char const *)entry->name;
		short hash_index;

		for (; *byte_pointer; byte_pointer++)
			signature = (signature ^ *byte_pointer) * 16777619UL;
		for (hash_index = 0; hash_index < MAP_SHARE_HASH_BYTES; hash_index++)
			signature = (signature ^ entry->hash[hash_index]) * 16777619UL;
		signature = (signature ^ entry->role) * 16777619UL;
	}
	return signature ? signature : 1;
}

/* ---------- the host */

/* the host's addons now: its armors', its weapon mods'; each hashed (the
set has only those hashed yet) */
static void host_build(
	void)
{
	struct network_addon_set set;
	char addons[NETWORK_ADDONS_MAXIMUM][PLAYER_ARMOR_DRESS_NAME + 16];
	char bipeds[NETWORK_ADDONS_MAXIMUM][PLAYER_ARMOR_DRESS_BIPED];
	struct weapon_mod mods[WEAPON_MODS_MAXIMUM];
	short armor_count = player_armor_network_addons(addons, bipeds, 4);
	short mod_count = weapon_mods_read_all(mods, WEAPON_MODS_MAXIMUM);
	short index;
	long machine_index;
	boolean unsettled = FALSE;

	/* (and the host's armors the clients' players wear) */
	for (machine_index = 0; machine_index < NETWORK_ADDONS_MACHINES && armor_count < 8; machine_index++)
	{
		struct player_armor_dress const *dress = &network_addons.dresses[machine_index];
		boolean listed = FALSE;

		if (!network_addons.dress_valid[machine_index] || !dress->biped[0])
			continue;
		for (index = 0; index < armor_count; index++)
		{
			if (!_stricmp(bipeds[index], dress->biped))
				listed = TRUE;
		}
		if (!listed && player_armor_addon_of_biped(dress->biped, addons[armor_count], sizeof(addons[0])))
		{
			addons_copy(bipeds[armor_count], dress->biped, sizeof(bipeds[0]));
			armor_count++;
		}
	}

	csmemset(&set, 0, sizeof(set));
	for (index = 0; index < armor_count + mod_count && set.count < NETWORK_ADDONS_MAXIMUM; index++)
	{
		struct network_addon *addon = &set.addons[set.count];
		struct map_share_file *file;
		char digits[MAP_SHARE_NAME_DIGITS + 1];
		short other;
		boolean shared = FALSE;

		csmemset(addon, 0, sizeof(*addon));
		if (index < armor_count)
		{
			addon->entry.role = _network_addon_armor;
			addon->entry.root_group = ADDON_TAGS_GROUP_BIPED;
			addons_copy(addon->entry.root, bipeds[index], sizeof(addon->entry.root));
			addons_copy(addon->base_name, addons[index], sizeof(addon->base_name));
		}
		else
		{
			struct weapon_mod const *mod = &mods[index - armor_count];
			short replacement;

			/* (several of one addon and root, a HUD's: each told of, with its
			replacements; addon_tags.c grafts the addon once) */
			(void)other;
			(void)shared;
			addon->entry.role = _network_addon_weapon;
			addon->entry.root_group = mod->root_group;
			addons_copy(addon->entry.root, mod->root, sizeof(addon->entry.root));
			addons_copy(addon->base_name, mod->addon, sizeof(addon->base_name));
			addon->entry.replacement_count = (byte)mod->replacement_count;
			for (replacement = 0; replacement < mod->replacement_count; replacement++)
			{
				addon->entry.replacements[replacement].group_tag = mod->replacements[replacement].group_tag;
				addons_copy(addon->entry.replacements[replacement].stock, mod->replacements[replacement].stock,
					WEAPON_MOD_NAME_LENGTH);
				addons_copy(addon->entry.replacements[replacement].mod, mod->replacements[replacement].mod,
					WEAPON_MOD_NAME_LENGTH);
			}
			addon->weapon_mod = *mod;
		}
		snprintf(addon->path, sizeof(addon->path), "d:\\addons\\%s.map", addon->base_name);
		addon->catalog = map_share_file_hash(addon->path, _map_share_file_map);
		file = map_share_file_get(addon->catalog);
		if (file && !file->ready && !file->failed)
			unsettled = TRUE;
		if (!file || !file->ready)
			continue;
		map_share_hash_digits(file->hash, digits);
		snprintf(addon->entry.name, sizeof(addon->entry.name), "%.38s_%s", addon->base_name, digits);
		addon->entry.size = file->size;
		csmemcpy(addon->entry.hash, file->hash, MAP_SHARE_HASH_BYTES);
		addon->present = TRUE;
		set.count++;
	}
	set.signature = addons_signature(&set);
	set.settled = !unsettled;
	for (index = 0; index < set.count; index++)
	{
		set.addons[index].entry.signature = set.signature;
		set.addons[index].entry.index = (byte)index;
		set.addons[index].entry.count = (byte)set.count;
	}
	if (set.settled && set.signature != network_addons.host_set.signature)
	{
		error(_error_silent, "network addons: the host offers %d addon%s", (int)set.count, set.count == 1 ? "" : "s");
		for (index = 0; index < set.count; index++)
			error(_error_silent, "network addons:   %s (%lu bytes) for %s", set.addons[index].entry.name,
				set.addons[index].entry.size, set.addons[index].entry.root);
	}
	network_addons.host_set = set;
}

static void host_send(
	long machine_index,
	struct network_addon_set const *set)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_addon addon;
	} message;
	short index;

	for (index = 0; index < MAX(set->count, 1); index++)
	{
		csmemset(&message, 0, sizeof(message));
		addons_fill_header(&message.header, _distributed_message_addons, (word)sizeof(message));
		if (set->count)
			message.addon = set->addons[index].entry;
		message.addon.signature = set->signature;
		message.addon.count = (byte)set->count;
		message.addon.index = (byte)index;
		network_distributed_server_send_to_machine_reliably(machine_index, &message, (word)sizeof(message));
	}
}

/* every player's camera as the host knows it: its own players' its own,
the others' their machines' */
static void host_cameras(
	struct distributed_player_cameras *cameras)
{
	struct data_iterator iterator;
	short local_machine = network_game_client_get_local_machine_index();
	byte local = (byte)shoulder_camera_local_mode();
	struct player_datum *player;

	csmemset(cameras, 0, sizeof(*cameras));
	csmemset(network_addons.player_cameras, 0, sizeof(network_addons.player_cameras));
	/* (only once the host's own level is loaded: in the lobby, none) */
	if (game_map_loaded() && global_scenario_index != NONE && global_scenario_get()->type == _scenario_type_solo)
	{
		cameras->tags_signature = addon_tags_signature(NULL);
		cameras->tag_count = addon_tags_tag_count();
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		long machine_index = player->network_player_data.machine_index;
		byte camera = 0;

		if (absolute < 0 || absolute >= NETWORK_ADDONS_CAMERA_PLAYERS)
			continue;
		if (player->local_player_index != NONE || machine_index == local_machine)
			camera = local;
		else if (machine_index >= 0 && machine_index < NETWORK_ADDONS_MACHINES &&
			network_addons.dress_valid[machine_index])
		{
			camera = network_addons.machine_cameras[machine_index];
		}
		network_addons.player_cameras[absolute] = camera;
		cameras->players[cameras->count].player = (word)absolute;
		cameras->players[cameras->count].camera = camera;
		cameras->count++;
	}
}

static unsigned long cameras_signature(
	struct distributed_player_cameras const *cameras)
{
	unsigned long signature = 0x0CA3E7A5 ^ cameras->tags_signature ^ (unsigned long)cameras->tag_count;
	word index;

	for (index = 0; index < cameras->count; index++)
	{
		signature = (signature ^ cameras->players[index].player) * 16777619UL;
		signature = (signature ^ cameras->players[index].camera) * 16777619UL;
	}
	return signature ? signature : 1;
}

/* every player's camera told of to each client, when it changes or the
machine joins */
static void host_send_cameras(
	void)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_player_cameras cameras;
	} message;
	short local_machine = network_game_client_get_local_machine_index();
	unsigned long signature;
	long machine_index;

	host_cameras(&message.cameras);
	signature = cameras_signature(&message.cameras);
	for (machine_index = 0; machine_index < NETWORK_ADDONS_MACHINES; machine_index++)
	{
		unsigned long address = network_game_server_machine_address(machine_index);

		if (!address || machine_index == local_machine)
		{
			network_addons.cameras_sent_address[machine_index] = 0;
			continue;
		}
		if (network_addons.cameras_sent_address[machine_index] == address &&
			network_addons.cameras_sent_signature[machine_index] == signature)
		{
			continue;
		}
		addons_fill_header(&message.header, _distributed_message_player_cameras, (word)sizeof(message));
		network_distributed_server_send_to_machine_reliably(machine_index, &message, (word)sizeof(message));
		network_addons.cameras_sent_address[machine_index] = address;
		network_addons.cameras_sent_signature[machine_index] = signature;
	}
}

static void host_update(
	void)
{
	struct network_game *game = addons_network_game();
	struct network_addon_set const *set;
	unsigned long now = system_milliseconds();
	short local_machine = network_game_client_get_local_machine_index();
	long machine_index;

	if (!game || !addons_campaign_level(game->map.name))
	{
		network_addons.loaded = FALSE;
		return;
	}
	if (game_in_progress())
		host_send_cameras();
	/* (the set the level got, while a level is loaded: else the lobby's) */
	if (!(game_map_loaded() && global_scenario_index != NONE && global_scenario_get()->type == _scenario_type_solo))
		network_addons.loaded = FALSE;
	if (!network_addons.loaded && (now - network_addons.built_at >= HOST_REBUILD_INTERVAL || !network_addons.built_at))
	{
		host_build();
		network_addons.built_at = now ? now : 1;
	}
	set = network_addons.loaded ? &network_addons.loaded_set : &network_addons.host_set;
	if (!set->settled)
		return;
	for (machine_index = 0; machine_index < NETWORK_ADDONS_MACHINES; machine_index++)
	{
		unsigned long address = network_game_server_machine_address(machine_index);

		if (!address || machine_index == local_machine)
		{
			network_addons.sent_address[machine_index] = 0;
			network_addons.dress_valid[machine_index] = FALSE;
			continue;
		}
		if (network_addons.sent_address[machine_index] == address &&
			network_addons.sent_signature[machine_index] == set->signature)
		{
			continue;
		}
		if (network_addons.sent_address[machine_index] != address)
			network_addons.dress_valid[machine_index] = FALSE;
		host_send(machine_index, set);
		network_addons.sent_address[machine_index] = address;
		network_addons.sent_signature[machine_index] = set->signature;
	}
}

/* ---------- a client */

static void client_reset(
	void)
{
	csmemset(&network_addons.client_set, 0, sizeof(network_addons.client_set));
	csmemset(&network_addons.coming, 0, sizeof(network_addons.coming));
	network_addons.client_complete = FALSE;
	network_addons.coming_received = 0;
	network_addons.waiting_since = 0;
	network_addons.timed_out = FALSE;
	network_addons.dress_ever_sent = FALSE;
}

/* the set the host told of, whole: which addons this machine has */
static void client_set_complete(
	void)
{
	struct network_addon_set *set = &network_addons.client_set;
	short index;

	*set = network_addons.coming;
	network_addons.client_complete = TRUE;
	CreateDirectoryA("d:\\shared_addons", NULL);
	for (index = 0; index < set->count; index++)
	{
		struct network_addon *addon = &set->addons[index];
		FILE *file;

		snprintf(addon->path, sizeof(addon->path), "%s%s.map", NETWORK_ADDONS_FOLDER, addon->entry.name);
		addons_copy(addon->base_name, addon->entry.name, sizeof(addon->base_name));
		if (strrchr(addon->base_name, '_'))
			*strrchr(addon->base_name, '_') = 0;
		file = fopen(addon->path, "rb");
		if (file)
		{
			long size = -1;

			if (fseek(file, 0, SEEK_END) == 0)
				size = ftell(file);
			fclose(file);
			addon->present = size == (long)addon->entry.size;
		}
		/* (a weapon mod as weapon_mods.c reads its file) */
		if (addon->entry.role == _network_addon_weapon)
		{
			struct weapon_mod *mod = &addon->weapon_mod;
			short replacement;

			csmemset(mod, 0, sizeof(*mod));
			addons_copy(mod->id, addon->base_name, sizeof(mod->id));
			addons_copy(mod->addon, addon->base_name, sizeof(mod->addon));
			mod->root_group = addon->entry.root_group;
			addons_copy(mod->root, addon->entry.root, sizeof(mod->root));
			mod->replacement_count = MIN(addon->entry.replacement_count, WEAPON_MOD_MAXIMUM_REPLACEMENTS);
			for (replacement = 0; replacement < mod->replacement_count; replacement++)
			{
				mod->replacements[replacement].group_tag = addon->entry.replacements[replacement].group_tag;
				addons_copy(mod->replacements[replacement].stock, addon->entry.replacements[replacement].stock,
					WEAPON_MOD_NAME_LENGTH);
				addons_copy(mod->replacements[replacement].mod, addon->entry.replacements[replacement].mod,
					WEAPON_MOD_NAME_LENGTH);
			}
		}
		error(_error_silent, "network addons: the host's %s: %s", addon->entry.name,
			addon->present ? "here" : "to download");
	}
	if (!set->count)
		error(_error_silent, "network addons: the host offers none");
}

static void client_handle_addon(
	struct distributed_addon const *entry)
{
	struct network_addon_set *coming = &network_addons.coming;

	if (entry->count > NETWORK_ADDONS_MAXIMUM || (entry->count && entry->index >= entry->count) ||
		!addons_string_valid(entry->name, sizeof(entry->name)) ||
		!addons_string_valid(entry->root, sizeof(entry->root)) ||
		entry->replacement_count > WEAPON_MOD_MAXIMUM_REPLACEMENTS)
	{
		return;
	}
	if (entry->count && (!map_share_name_safe(entry->name) || !map_share_name_hashed(entry->name)))
		return;
	if (coming->signature != entry->signature || coming->count != entry->count)
	{
		csmemset(coming, 0, sizeof(*coming));
		coming->signature = entry->signature;
		coming->count = entry->count;
		network_addons.coming_received = 0;
	}
	if (entry->count)
	{
		short replacement;

		for (replacement = 0; replacement < entry->replacement_count; replacement++)
		{
			if (!addons_string_valid(entry->replacements[replacement].stock, WEAPON_MOD_NAME_LENGTH) ||
				!addons_string_valid(entry->replacements[replacement].mod, WEAPON_MOD_NAME_LENGTH))
			{
				return;
			}
		}
		coming->addons[entry->index].entry = *entry;
		network_addons.coming_received |= 1UL << entry->index;
	}
	if (network_addons.coming_received == (entry->count ? (1UL << entry->count) - 1 : 0) &&
		(!network_addons.client_complete || network_addons.client_set.signature != coming->signature))
	{
		client_set_complete();
	}
}

/* the signature of the host's set when this machine has it whole, else 0 */
static unsigned long client_ready_signature(
	void)
{
	short index;

	if (!network_addons.client_complete)
		return 0;
	for (index = 0; index < network_addons.client_set.count; index++)
	{
		if (!network_addons.client_set.addons[index].present)
			return 0;
	}
	return network_addons.client_set.signature;
}

/* the client's armor sent to the host, when it changes and now and then */
static void client_send_dress(
	void)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_armor_dress dress;
	} message;
	unsigned long now = system_milliseconds();

	csmemset(&message, 0, sizeof(message));
	player_armor_local_dress(&message.dress.dress);
	message.dress.camera = (byte)shoulder_camera_local_mode();
	message.dress.addons_ready = client_ready_signature();
	if (network_addons.dress_ever_sent && now - network_addons.dress_sent_at < DRESS_INTERVAL &&
		!csmemcmp(&message.dress.dress, &network_addons.dress_sent, sizeof(message.dress.dress)) &&
		message.dress.camera == network_addons.camera_sent && message.dress.addons_ready == network_addons.ready_sent)
	{
		return;
	}
	addons_fill_header(&message.header, _distributed_message_armor_dress, (word)sizeof(message));
	network_distributed_client_send_reliably(&message, (word)sizeof(message));
	network_addons.dress_sent = message.dress.dress;
	network_addons.camera_sent = message.dress.camera;
	network_addons.ready_sent = message.dress.addons_ready;
	network_addons.dress_sent_at = now;
	network_addons.dress_ever_sent = TRUE;
}

static void client_update(
	void)
{
	struct network_game *game = addons_network_game();
	short index;

	if (!game || !addons_campaign_level(game->map.name))
	{
		if (network_addons.client_complete || network_addons.coming.count || network_addons.waiting_since)
			client_reset();
		network_addons.game_level[0] = 0;
		return;
	}
	if (strcmp(network_addons.game_level, game->map.name))
	{
		addons_copy(network_addons.game_level, game->map.name, sizeof(network_addons.game_level));
		network_addons.waiting_since = system_milliseconds();
		network_addons.timed_out = FALSE;
	}
	if (!network_addons.client_complete && !network_addons.timed_out &&
		system_milliseconds() - network_addons.waiting_since >= MANIFEST_TIMEOUT)
	{
		network_addons.timed_out = TRUE;
		error(_error_silent, "network addons: the host has not told of its addons in %d seconds: the level as it is",
			(int)(MANIFEST_TIMEOUT / 1000));
	}
	client_send_dress();
	if (!network_addons.client_complete || !map_share_download_idle())
		return;
	/* (one at a time, in the host's order) */
	for (index = 0; index < network_addons.client_set.count; index++)
	{
		struct network_addon *addon = &network_addons.client_set.addons[index];
		char level_name[64];

		if (addon->present)
			continue;
		snprintf(level_name, sizeof(level_name), "%s%s", NETWORK_ADDONS_LEVEL_PREFIX, addon->entry.name);
		map_share_addon_download_begin(level_name);
		break;
	}
}

/* ---------- public code */

short network_addons_for_level(
	struct network_addon_load *addons,
	short maximum)
{
	struct network_addon_set const *set;
	short count = 0;
	short index;

	if (!global_network_game_client_get() && !addons_host())
		return NONE;
	if (addons_host())
	{
		struct network_game *game = addons_network_game();

		if (!game || !addons_campaign_level(game->map.name))
			return NONE;
		/* (the set the level gets is the one late joiners are told of: its
		addons hashed first, which takes a moment) */
		{
			unsigned long since = system_milliseconds();

			host_build();
			while (!network_addons.host_set.settled && system_milliseconds() - since < HASH_WAIT)
			{
				Sleep(20);
				map_share_files_update();
				host_build();
			}
		}
		network_addons.loaded_set = network_addons.host_set;
		network_addons.loaded = TRUE;
		set = &network_addons.loaded_set;
	}
	else
	{
		if (!network_addons.client_complete)
			return 0;
		set = &network_addons.client_set;
	}
	for (index = 0; index < set->count && count < maximum; index++)
	{
		struct network_addon const *addon = &set->addons[index];
		struct network_addon_load *load = &addons[count];

		if (!addon->present)
		{
			/* (the game cannot go on: logged; network_addons_pending held it) */
			error(_error_silent, "network addons: %s is not here", addon->entry.name);
			continue;
		}
		csmemset(load, 0, sizeof(*load));
		addons_copy(load->path, addon->path, sizeof(load->path));
		addons_copy(load->name, addon->base_name, sizeof(load->name));
		load->root_group = addon->entry.root_group;
		addons_copy(load->root, addon->entry.root, sizeof(load->root));
		load->weapon = addon->entry.role == _network_addon_weapon;
		if (load->weapon)
			load->weapon_mod = addon->weapon_mod;
		count++;
	}
	return count;
}

boolean network_addons_pending(
	void)
{
	struct network_game *game = addons_network_game();
	short index;

	if (addons_host() || !game || !addons_campaign_level(game->map.name))
		return FALSE;
	if (!network_addons.client_complete)
		return !network_addons.timed_out;
	for (index = 0; index < network_addons.client_set.count; index++)
	{
		if (!network_addons.client_set.addons[index].present)
			return TRUE;
	}
	return FALSE;
}

boolean network_addons_player_dress(
	long player_index,
	struct player_armor_dress *dress)
{
	struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
	long machine_index = player ? player->network_player_data.machine_index : NONE;

	if (machine_index < 0 || machine_index >= NETWORK_ADDONS_MACHINES || !network_addons.dress_valid[machine_index])
		return FALSE;
	*dress = network_addons.dresses[machine_index];
	return TRUE;
}

boolean network_addons_machine_ready(
	long machine_index)
{
	struct network_game *game = addons_network_game();
	struct network_addon_set const *set;

	if (!addons_host() || !game || !addons_campaign_level(game->map.name) ||
		machine_index == network_game_client_get_local_machine_index())
	{
		return TRUE;
	}
	if (machine_index < 0 || machine_index >= NETWORK_ADDONS_MACHINES)
		return TRUE;
	set = network_addons.loaded ? &network_addons.loaded_set : &network_addons.host_set;
	/* (the host's set not told of yet: nobody can have it) */
	if (!set->settled)
		return FALSE;
	return network_addons.dress_valid[machine_index] && network_addons.machine_ready[machine_index] == set->signature;
}

boolean network_addons_mismatch(
	wchar_t *text,
	short size)
{
	long tag_count = addon_tags_tag_count();
	unsigned long signature = addon_tags_signature(NULL);

	if (addons_host() || !global_network_game_client_get() || !game_in_progress() ||
		!network_addons.host_tag_count || !tag_count)
	{
		return FALSE;
	}
	if (signature == network_addons.host_tags_signature && tag_count == network_addons.host_tag_count)
	{
		network_addons.mismatch_logged = FALSE;
		return FALSE;
	}
	if (!network_addons.mismatch_logged)
	{
		network_addons.mismatch_logged = TRUE;
		error(_error_silent, "network addons: this level is not the host's: %ld tags (the host's %ld), "
			"addons %08lX (the host's %08lX)", tag_count, network_addons.host_tag_count, signature,
			network_addons.host_tags_signature);
	}
	if (text && size > 0)
	{
		/* (the same addons, grafted the same, onto another table: this
		machine's map is not the host's, another edition of the game's) */
		if (signature == network_addons.host_tags_signature)
		{
			usnprintf(text, size - 1, L"This level's map file is not the host's (%ld tags, the host %ld):\r\n"
				L"the game data of one of you is another edition", tag_count, network_addons.host_tag_count);
		}
		else
		{
			usnprintf(text, size - 1, L"Your game did not load the host's mods as the host did\r\n"
				L"(%ld tags, the host %ld): leave and join again", tag_count, network_addons.host_tag_count);
		}
		text[size - 1] = 0;
	}
	return TRUE;
}

short network_addons_player_camera(
	long player_index)
{
	long absolute = player_index != NONE ? DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;

	if (absolute < 0 || absolute >= NETWORK_ADDONS_CAMERA_PLAYERS)
		return 0;
	if (addons_host())
	{
		struct player_datum *player = player_get(player_index);
		long machine_index = player->network_player_data.machine_index;

		return machine_index >= 0 && machine_index < NETWORK_ADDONS_MACHINES &&
			network_addons.dress_valid[machine_index] ? network_addons.machine_cameras[machine_index] : 0;
	}
	return network_addons.player_cameras[absolute];
}

boolean network_addons_level_name(
	char const *level_name)
{
	return level_name && !_strnicmp(level_name, NETWORK_ADDONS_LEVEL_PREFIX, strlen(NETWORK_ADDONS_LEVEL_PREFIX));
}

boolean network_addons_host_path(
	char const *level_name,
	char *path,
	long size)
{
	struct network_addon_set const *sets[2];
	short set_index;

	if (!addons_host() || !network_addons_level_name(level_name))
		return FALSE;
	sets[0] = &network_addons.loaded_set;
	sets[1] = &network_addons.host_set;
	for (set_index = 0; set_index < 2; set_index++)
	{
		short index;

		if (set_index == 0 && !network_addons.loaded)
			continue;
		for (index = 0; index < sets[set_index]->count; index++)
		{
			struct network_addon const *addon = &sets[set_index]->addons[index];

			if (!_stricmp(addon->entry.name, level_name + strlen(NETWORK_ADDONS_LEVEL_PREFIX)))
			{
				addons_copy(path, addon->path, (size_t)size);
				return TRUE;
			}
		}
	}
	return FALSE;
}

short network_addons_catalog_file(
	short index)
{
	struct network_addon_set const *set = addons_host() ?
		(network_addons.loaded ? &network_addons.loaded_set : &network_addons.host_set) :
		(network_addons.client_complete ? &network_addons.client_set : NULL);

	if (!set || index < 0 || index >= set->count || !set->addons[index].present)
		return NONE;
	return addons_host() ? set->addons[index].catalog : map_share_file_find(set->addons[index].entry.hash);
}

void network_addons_downloaded(
	char const *level_name)
{
	short index;

	for (index = 0; index < network_addons.client_set.count; index++)
	{
		struct network_addon *addon = &network_addons.client_set.addons[index];

		if (network_addons_level_name(level_name) &&
			!_stricmp(addon->entry.name, level_name + strlen(NETWORK_ADDONS_LEVEL_PREFIX)))
		{
			addon->present = TRUE;
			error(_error_silent, "network addons: %s downloaded", addon->entry.name);
		}
	}
}

boolean network_addons_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_addons || header.type == _distributed_message_armor_dress ||
		header.type == _distributed_message_player_cameras;
}

void network_addons_handle_message(
	long machine_index,
	word const *message,
	word size,
	boolean from_stream)
{
	struct distributed_message_header header;
	byte const *entries = (byte const *)message + sizeof(header);
	long length = (long)size - (long)sizeof(header);

	if (!network_addons_handles_message(message, size) || !from_stream)
		return;
	csmemcpy(&header, message, sizeof(header));
	if (header.type == _distributed_message_addons)
	{
		struct distributed_addon entry;

		/* (the host's, to a client) */
		if (machine_index != NONE || addons_host() || length != (long)sizeof(entry))
			return;
		csmemcpy(&entry, entries, sizeof(entry));
		client_handle_addon(&entry);
	}
	else if (header.type == _distributed_message_player_cameras)
	{
		struct distributed_player_cameras cameras;
		word index;

		/* (the host's, to a client) */
		if (machine_index != NONE || addons_host() || length != (long)sizeof(cameras))
			return;
		csmemcpy(&cameras, entries, sizeof(cameras));
		if (cameras.count > NETWORK_ADDONS_CAMERA_PLAYERS)
			return;
		network_addons.host_tags_signature = cameras.tags_signature;
		network_addons.host_tag_count = cameras.tag_count;
		csmemset(network_addons.player_cameras, 0, sizeof(network_addons.player_cameras));
		for (index = 0; index < cameras.count; index++)
		{
			if (cameras.players[index].player < NETWORK_ADDONS_CAMERA_PLAYERS &&
				cameras.players[index].camera <= 2)
			{
				network_addons.player_cameras[cameras.players[index].player] = cameras.players[index].camera;
			}
		}
	}
	else
	{
		struct distributed_armor_dress dress;
		short index;

		/* (a client's, to the host) */
		if (machine_index < 0 || machine_index >= NETWORK_ADDONS_MACHINES || !addons_host() ||
			length != (long)sizeof(dress))
		{
			return;
		}
		csmemcpy(&dress, entries, sizeof(dress));
		if (!addons_string_valid(dress.dress.biped, sizeof(dress.dress.biped)) ||
			dress.dress.region_count > PLAYER_ARMOR_DRESS_REGIONS)
		{
			return;
		}
		for (index = 0; index < dress.dress.region_count; index++)
		{
			if (!addons_string_valid(dress.dress.regions[index].region, PLAYER_ARMOR_DRESS_NAME) ||
				!addons_string_valid(dress.dress.regions[index].permutation, PLAYER_ARMOR_DRESS_NAME))
			{
				return;
			}
		}
		if (!network_addons.dress_valid[machine_index] ||
			csmemcmp(&network_addons.dresses[machine_index], &dress.dress, sizeof(dress.dress)))
		{
			error(_error_silent, "network addons: machine #%ld wears %s (%d regions)", machine_index,
				dress.dress.biped[0] ? dress.dress.biped : "the level's Spartan", (int)dress.dress.region_count);
		}
		network_addons.dresses[machine_index] = dress.dress;
		network_addons.machine_cameras[machine_index] = dress.camera <= 2 ? dress.camera : 0;
		if (network_addons.machine_ready[machine_index] != dress.addons_ready && dress.addons_ready)
			error(_error_silent, "network addons: machine #%ld has the host's addons", machine_index);
		network_addons.machine_ready[machine_index] = dress.addons_ready;
		network_addons.dress_valid[machine_index] = TRUE;
	}
}

void network_addons_update(
	void)
{
	if (addons_host())
		host_update();
	else if (global_network_game_client_get())
		client_update();
	else
	{
		if (network_addons.client_complete || network_addons.coming.count)
			client_reset();
		network_addons.loaded = FALSE;
		network_addons.game_level[0] = 0;
		csmemset(network_addons.player_cameras, 0, sizeof(network_addons.player_cameras));
		network_addons.host_tags_signature = 0;
		network_addons.host_tag_count = 0;
		network_addons.mismatch_logged = FALSE;
	}
}
