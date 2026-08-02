/*
 * demo_scene.h -- the shared test scene.
 *
 * Used by both the host PPM harness and the on-device smoke test, so
 * that what appears on the Zaurus panel can be compared frame-for-frame
 * against a known-good render produced on the build machine. A scene
 * that differed between the two would make any on-device difference
 * impossible to attribute.
 */

#ifndef SOFT_DEMO_SCENE_H
#define SOFT_DEMO_SCENE_H

#include "soft.h"

void            demo_build(void);
const soft_vtx *demo_mesh(void);
int             demo_mesh_count(void);

/*
 * Camera. `t` is seconds; at t == 0 it sits at the fixed viewpoint the
 * reference PPM was rendered from, so a still comparison is exact, and
 * it orbits from there so a moving device test also exercises culling
 * and clipping from every angle.
 */
void demo_camera(float t, soft_mat4 *out);

/*
 * First-person camera: standing on the terrain at eye height, turning on
 * the spot. This is the view the game actually has, and the only one a
 * short draw distance can be measured against -- demo_camera() orbits
 * roughly 15 blocks clear of the terrain, so any cull tighter than that
 * removes the whole world and measures an empty screen.
 */
void demo_camera_player(float t, soft_mat4 *out);

/* Nearest palette entry to white, for HUD text and the crosshair. */
int demo_ink(const uint8_t *palette);

#endif /* SOFT_DEMO_SCENE_H */
