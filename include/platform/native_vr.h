/*
 * Quest VR stereo renderer (Android and the desktop Quest test build only).
 *
 * The game builds each frame's draw lists once. In a stereo frame every
 * batch of draw splits is replayed four times:
 *   pass 0, 1  left/right eye: only 3D world splits, with the camera-space
 *              position of every vertex re-projected through the eye's head
 *              pose and field of view in the vertex shader (NativeVrBlock);
 *   pass 2     HUD over white  (all 2D/overlay splits, into a white target);
 *   pass 3     HUD over black  (same, into the game's own main target).
 * The HUD's alpha comes from the black/white difference, so every PS1 blend
 * mode keeps working on a transparent HUD layer. Offscreen (render-to-VRAM)
 * splits run once, in pass 0.
 *
 * platform/quest/ctr_quest.c (or the desktop test) supplies the eye poses
 * before the game draws a frame and presents the eye/HUD textures at swap.
 */
#ifndef NATIVE_VR_H
#define NATIVE_VR_H

#if defined(__ANDROID__) || defined(CTR_QUEST_DESKTOP_TEST)
#define CTR_QUEST_VR 1
#else
#define CTR_QUEST_VR 0
#endif

#if CTR_QUEST_VR

/* Uniform buffer binding point of the shaders' NativeVrBlock. */
#define CTR_VR_UBO_BINDING 3

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CtrVrEyePose
{
	/* Eye orientation relative to the recentre anchor, row-major 3x3, OpenXR
	 * axes (x right, y up, z backwards). */
	float rot[9];
	/* Eye position relative to the anchor, metres, same axes. */
	float pos[3];
	/* Field of view tangents (left and down negative). */
	float tanLeft, tanRight, tanUp, tanDown;
} CtrVrEyePose;

typedef struct CtrVrConfig
{
	int enabled;          /* stereo=1: 3D world in races, 0 = flat screen only */
	float unitsPerMeter;  /* world_scale: game units per metre */
	float cullFovDegrees; /* cull_fov: vertical FOV the game culls with in stereo */
	int levelHorizon;     /* level_horizon: drop the chase camera's pitch/roll */
} CtrVrConfig;

void CtrVr_Configure(const CtrVrConfig *config);
/* Poses the NEXT frame the game draws is rendered with (valid=0: flat). */
void CtrVr_SetEyePoses(const CtrVrEyePose eyes[2], int eyeWidth, int eyeHeight, int valid);
/* After the game's present, before swap: 1 if that frame was drawn in stereo.
 * Resolves the eye and HUD targets so they can be sampled. */
int CtrVr_FinishFrame(void);
unsigned int CtrVr_GetEyeTexture(int eye);
/* HUD drawn over black (0) and over white (1); size of both. */
unsigned int CtrVr_GetHudTexture(int white);
void CtrVr_GetHudSize(int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif /* CTR_QUEST_VR */

#endif
