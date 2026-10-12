/*
SHOULDER_CAMERA.H

The over-the-shoulder camera (Video Setup > FOV and Viewmodels > Camera,
display.camera): on foot in a campaign, the view from behind the player's
shoulder, as in Resident Evil 6 and later third-person shooters.
*/

#ifndef __SHOULDER_CAMERA_H
#define __SHOULDER_CAMERA_H

#include "cseries.h"
#include "real_math.h"

struct observer_command;
struct observer_result;

/* the director's camera data for it (struct director's camera_data) */
struct shoulder_camera
{
	boolean initialized;
	byte pad1[3];
	/* how far behind the shoulder the camera is, as eased out after a wall
	pulled it in (world units) */
	real distance;
	/* how far to the side, likewise */
	real side;
};

/* whether the local player's view is over the shoulder: display.camera
asks for it, a campaign (no multiplayer, no network co-op), the player on
foot, alive and not zoomed (a scope looks through the eye) */
boolean shoulder_camera_wanted(
	short local_player_index);

/* the same, for a unit a player controls, from the simulation's side (the
projectiles' aim, aim_assist.c) */
boolean shoulder_camera_wanted_for_unit(
	long unit_index);

void shoulder_camera_new(
	struct shoulder_camera *camera);

/* the director's camera proc (director_camera_update_proc): action begins
with the local player's index (struct camera_control) */
void shoulder_camera_update(
	struct shoulder_camera *camera,
	void const *action,
	struct observer_command *result);

/* where the camera is and where it looks for a unit, for the projectiles'
aim through the crosshair (director_camera_deterministic) */
void shoulder_camera_deterministic(
	long unit_index,
	real_point3d *position,
	real_vector3d *forward);

/* the view turned to where the player aims now (display.direct_camera,
render_interpolation.c): the camera swings round the eye with the facing,
as the next tick would put it */
void shoulder_camera_direct(
	short local_player_index,
	struct observer_result *camera,
	real_vector3d const *forward);

#endif // __SHOULDER_CAMERA_H
