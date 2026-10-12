# The over-the-shoulder camera

Video Setup > FOV and Viewmodels > **Camera** (`display.camera`) chooses
the view on foot in a campaign:

| Value | View |
| --- | --- |
| `first_person` | Through the eye: the game's own view, and the default. |
| `shoulder_right` | From just behind the right shoulder, as in Resident Evil 6. |
| `shoulder_left` | The same, from behind the left shoulder. |

It is implemented in `port/linux/game/shoulder_camera.c`.

## What changes over the shoulder

- **The body.** The player's body is drawn as others see it, holding its
  weapon. The first-person weapon and arms are not drawn. The director
  reports the view as third person (`director_get_perspective`), as it
  does for a vehicle's chase camera, so the renderer, the HUD, the motion
  sensor and aim assistance treat it as they treat that one.
- **Aiming.** The crosshair stays in the middle of the screen, and shots
  go where it points. `director_camera_deterministic` gives the shoulder
  camera, and `player_aim_projectile` (`aim_assist.c`) aims each projectile
  from the weapon at what the camera's ray through the middle of the
  screen meets, as it already does for a vehicle's third-person gunner.
  The weapon sits a hand's breadth off that ray, so the turn allowed from
  the weapon's own line is widened to 25°.
- **Sounds.** The first-person weapon is not drawn, but its animations
  still run, and their sounds play (a reload, a pump) as in first person,
  from the weapon in the body's hand (`first_person_weapons.c`).
- **The flashlight.** The flashlight shines from just in front of the eye
  along the aim, as from the first-person weapon. The body's own marker
  points at the ground at its feet (`first_person_weapon_center_flashlight`).
- **The view.** The view turns about the eye as in first person. With
  `display.direct_camera`, the camera swings round the eye the frame the
  view turns.
- **Walls.** A wall between the eye and the camera pulls the camera in
  front of it at once. When the wall is gone, the camera eases back out
  over a few ticks.

The camera sits about 1.1 m behind the eye, 52 cm to the side and 12 cm
above. These are `SHOULDER_CAMERA_DISTANCE`, `_SIDE` and `_HEIGHT` in
world units, chosen so that the body is on one side of the screen and the
crosshair stays clear of it.

## When it stays in first person

- **A zoomed weapon.** A scope looks through the eye. The view goes to
  first person while zoomed and comes back over the shoulder in a third of
  a second.
- **Vehicles.** A seat keeps its own camera, first or third person.
- **Multiplayer and network co-op.** These keep first person. Another
  player could not see round corners, and a host does not know a remote
  player's camera, which the projectiles' aim depends on.
- **Cinematics and death.** These keep their cameras.
