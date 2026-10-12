/*
SHOULDER_CAMERA.C

The over-the-shoulder camera (Video Setup > FOV and Viewmodels > Camera,
display.camera = "shoulder_right" or "shoulder_left"): on foot in a
campaign, the view from just behind the player's shoulder, as in Resident
Evil 6. The player turns and aims as in first person (the view turns about
the eye), the body is drawn as others see it, and the first-person weapon
and arms are not: the body holds the weapon.

The crosshair stays in the middle of the screen. Where it points is where
shots go: aim_assist.c's player_aim_projectile already aims each projectile
from the weapon at what the camera's ray through the middle of the screen
meets (as it does for a vehicle's third-person gunner), with the camera
director_camera_deterministic gives, which is this one's.

A scope (a zoomed weapon) looks through the eye, in first person, and a
vehicle's seat keeps its own camera. Multiplayer and network co-op keep
first person: another player could not see round corners, and a host does
not know a remote player's camera.

Walls: the camera is pulled in front of a wall between it and the eye, and
eases back out when the wall is gone.
*/

#include "cseries.h"
#include "real_math.h"
#include "shoulder_camera.h"
#include "camera/observer.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "physics/collision_usage.h"
#include "units/units.h"

#include "network_coop.h"

#include <string.h>

/* port/linux/src/port_config.c */
const char *config_string(const char *name);
unsigned long config_changes(void);

/* (world units: one is about three metres) how far behind the eye the
camera is (about 1.1 m), how far to the side (about 52 cm: the body on the
left of the screen, clear of the crosshair) and how far above the eye
(about 12 cm) */
#define SHOULDER_CAMERA_DISTANCE 0.36f
#define SHOULDER_CAMERA_SIDE 0.17f
#define SHOULDER_CAMERA_HEIGHT 0.04f
/* how far it is kept from a wall it is pulled in front of (about 6 cm), and
how near the eye it may come (inside the head, the body is not drawn
through: the eye's own view) */
#define SHOULDER_CAMERA_WALL_MARGIN 0.02f
/* how much of the way back out it eases each tick, once the wall is gone */
#define SHOULDER_CAMERA_EASE_OUT 0.25f

enum
{
	_shoulder_camera_off = 0,
	_shoulder_camera_right,
	_shoulder_camera_left,
};

/* the last camera this machine posed for each local player: for the view
turned between ticks (shoulder_camera_direct) */
static struct
{
	boolean valid;
	real side;
	real height;
	real distance;
} shoulder_camera_last[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];

static short shoulder_camera_setting(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static short setting = _shoulder_camera_off;

	if (changes != config_changes())
	{
		const char *value = config_string("display.camera");

		changes = config_changes();
		if (value && !strcmp(value, "shoulder_right"))
			setting = _shoulder_camera_right;
		else if (value && !strcmp(value, "shoulder_left"))
			setting = _shoulder_camera_left;
		else
			setting = _shoulder_camera_off;
	}
	return setting;
}

static boolean shoulder_camera_allowed_here(
	void)
{
	return shoulder_camera_setting() != _shoulder_camera_off &&
		!game_engine_running() &&
		!network_coop_active();
}

/* on foot, alive, a biped */
static boolean shoulder_camera_unit_on_foot(
	long unit_index)
{
	struct unit_datum *unit;

	if (unit_index == NONE)
		return FALSE;
	unit = unit_try_and_get(unit_index);
	return unit &&
		unit->object.type == _object_type_biped &&
		unit->object.parent_object_index == NONE &&
		!TEST_FLAG(unit->object.damage_flags, _object_dead_bit);
}

boolean shoulder_camera_wanted(
	short local_player_index)
{
	return shoulder_camera_allowed_here() &&
		shoulder_camera_unit_on_foot(player_control_get_unit_index(local_player_index)) &&
		player_control_get_zoom_level(local_player_index) == NONE;
}

boolean shoulder_camera_wanted_for_unit(
	long unit_index)
{
	struct unit_datum *unit;

	if (!shoulder_camera_allowed_here() || !shoulder_camera_unit_on_foot(unit_index))
		return FALSE;
	unit = unit_get(unit_index);
	return unit->unit.player_index != NONE && unit_get_zoom_level(unit_index) == NONE;
}

void shoulder_camera_new(
	struct shoulder_camera *camera)
{
	camera->initialized = FALSE;
	camera->distance = SHOULDER_CAMERA_DISTANCE;
	camera->side = SHOULDER_CAMERA_SIDE;
	return;
}

/* the view's right, level with the ground, for a facing */
static void shoulder_camera_right(
	real_vector3d const *forward,
	real_vector3d *right)
{
	real length = square_root(forward->i * forward->i + forward->j * forward->j);

	if (length < 0.0001f)
	{
		/* looking straight up or down: any right will do */
		right->i = 0.f;
		right->j = -1.f;
	}
	else
	{
		/* (x forward, y left, z up: right is forward turned a quarter
		clockwise) */
		right->i = forward->j / length;
		right->j = -forward->i / length;
	}
	right->k = 0.f;
	return;
}

/* how far along from a point toward another the camera can go before a wall
(the whole way: 1), the wall margin kept */
static real shoulder_camera_clearance(
	real_point3d const *from,
	real_point3d const *to,
	long ignore_object_index)
{
	unsigned long flags =
		FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) |
		FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_vehicles_bit) |
		FLAG(_collision_test_objects_machines_bit);
	real_vector3d vector;
	real length;
	real t = 1.f;
	struct collision_result collision;

	vector_from_points3d(from, to, &vector);
	length = square_root(vector.i * vector.i + vector.j * vector.j + vector.k * vector.k);
	if (length < 0.0001f)
		return 1.f;
	/* (reach the margin further, so that the camera stops short of the wall
	by it) */
	vector.i *= (length + SHOULDER_CAMERA_WALL_MARGIN) / length;
	vector.j *= (length + SHOULDER_CAMERA_WALL_MARGIN) / length;
	vector.k *= (length + SHOULDER_CAMERA_WALL_MARGIN) / length;

	if (global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH)
	{
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_observer;
		if (collision_test_vector(flags, from, &vector, ignore_object_index, &collision))
		{
			real reach = collision.t * (length + SHOULDER_CAMERA_WALL_MARGIN) - SHOULDER_CAMERA_WALL_MARGIN;

			t = PIN(reach / length, 0.f, 1.f);
		}
		global_current_collision_user_depth--;
	}
	return t;
}

/* the camera for an eye and a facing: its place, with how far to the side
and behind it ended up (walls pulling it in) */
static void shoulder_camera_pose(
	long unit_index,
	real_point3d const *eye,
	real_vector3d const *forward,
	real side_wanted,
	real distance_wanted,
	real_point3d *position,
	real *side,
	real *height,
	real *distance)
{
	real_vector3d right;
	real_point3d shoulder;
	real_point3d behind;
	real t;

	shoulder_camera_right(forward, &right);

	/* to the shoulder, short of a wall beside the head */
	shoulder.x = eye->x + right.i * side_wanted;
	shoulder.y = eye->y + right.j * side_wanted;
	shoulder.z = eye->z + SHOULDER_CAMERA_HEIGHT;
	t = shoulder_camera_clearance(eye, &shoulder, unit_index);
	*side = side_wanted * t;
	shoulder.x = eye->x + right.i * *side;
	shoulder.y = eye->y + right.j * *side;
	*height = SHOULDER_CAMERA_HEIGHT * t;
	shoulder.z = eye->z + *height;

	/* then back, short of a wall behind */
	behind.x = shoulder.x - forward->i * distance_wanted;
	behind.y = shoulder.y - forward->j * distance_wanted;
	behind.z = shoulder.z - forward->k * distance_wanted;
	t = shoulder_camera_clearance(&shoulder, &behind, unit_index);
	*distance = distance_wanted * t;
	position->x = shoulder.x - forward->i * *distance;
	position->y = shoulder.y - forward->j * *distance;
	position->z = shoulder.z - forward->k * *distance;
	return;
}

static real shoulder_camera_side_sign(
	void)
{
	return shoulder_camera_setting() == _shoulder_camera_left ? -1.f : 1.f;
}

void shoulder_camera_update(
	struct shoulder_camera *camera,
	void const *action,
	struct observer_command *result)
{
	short local_player_index = *(short const *)action;
	long unit_index = player_control_get_unit_index(local_player_index);
	real_vector3d forward;
	real_point3d eye;
	real_point3d position;
	real side;
	real height;
	real distance;
	real side_wanted;
	real distance_wanted;

	csmemset(result, 0, sizeof(*result));
	if (unit_index == NONE)
		return;

	player_control_get_facing_direction(local_player_index, &forward);
	unit_get_camera_position(unit_index, &eye);

	/* pulled in at once by a wall, eased back out when it is gone */
	side_wanted = SHOULDER_CAMERA_SIDE * shoulder_camera_side_sign();
	distance_wanted = SHOULDER_CAMERA_DISTANCE;
	if (camera->initialized)
	{
		if (side_wanted * camera->side < 0.f)
			camera->side = 0.f; /* (the other shoulder: from the middle) */
		side_wanted = camera->side + (side_wanted - camera->side) * SHOULDER_CAMERA_EASE_OUT;
		distance_wanted = camera->distance + (distance_wanted - camera->distance) * SHOULDER_CAMERA_EASE_OUT;
	}
	shoulder_camera_pose(unit_index, &eye, &forward, side_wanted, distance_wanted, &position, &side, &height,
		&distance);
	camera->side = side;
	camera->distance = distance;
	camera->initialized = TRUE;

	if (local_player_index >= 0 && local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS)
	{
		shoulder_camera_last[local_player_index].valid = TRUE;
		shoulder_camera_last[local_player_index].side = side;
		shoulder_camera_last[local_player_index].height = height;
		shoulder_camera_last[local_player_index].distance = distance;
	}

	/* the observer is told the camera's place itself (no distance for it to
	pull in: done above) */
	result->flags = FLAG(_observer_command_valid_bit);
	result->focus_position = position;
	result->focus_offset.i = 0.f;
	result->focus_offset.j = 0.f;
	result->focus_offset.k = 0.f;
	result->focus_distance = 0.f;
	result->forward = forward;
	observer_up_from_forward(&result->forward, &result->up);
	result->field_of_view = player_control_get_field_of_view(local_player_index);
	object_get_velocities(unit_index, &result->focus_velocity, NULL);
	result->timer = 0.f;
	return;
}

void shoulder_camera_deterministic(
	long unit_index,
	real_point3d *position,
	real_vector3d *forward)
{
	struct unit_datum *unit = unit_get(unit_index);
	real_point3d eye;
	real side;
	real height;
	real distance;

	unit_get_camera_position(unit_index, &eye);
	*forward = unit->unit.aiming_vector;
	shoulder_camera_pose(unit_index, &eye, forward, SHOULDER_CAMERA_SIDE * shoulder_camera_side_sign(),
		SHOULDER_CAMERA_DISTANCE, position, &side, &height, &distance);
	return;
}

void shoulder_camera_direct(
	short local_player_index,
	struct observer_result *camera,
	real_vector3d const *forward)
{
	real_vector3d old_right;
	real_vector3d new_right;
	real_point3d eye;
	real side;
	real height;
	real distance;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_NUMBER_OF_LOCAL_PLAYERS ||
		!shoulder_camera_last[local_player_index].valid)
	{
		return;
	}
	side = shoulder_camera_last[local_player_index].side;
	height = shoulder_camera_last[local_player_index].height;
	distance = shoulder_camera_last[local_player_index].distance;

	/* the eye the drawn camera was posed from, then the camera posed from it
	again with the facing of now */
	shoulder_camera_right(&camera->forward, &old_right);
	eye.x = camera->position.x + camera->forward.i * distance - old_right.i * side;
	eye.y = camera->position.y + camera->forward.j * distance - old_right.j * side;
	eye.z = camera->position.z + camera->forward.k * distance - height;
	shoulder_camera_right(forward, &new_right);
	camera->position.x = eye.x + new_right.i * side - forward->i * distance;
	camera->position.y = eye.y + new_right.j * side - forward->j * distance;
	camera->position.z = eye.z + height - forward->k * distance;
	return;
}
