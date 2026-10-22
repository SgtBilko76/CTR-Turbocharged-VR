/*
 * Quest VR stereo renderer; see include/platform/native_vr.h for the design.
 * Part of the unity build (main.c), so it shares the renderer's statics.
 */
#include "platform/native_vr.h"

#if CTR_QUEST_VR

#include <math.h>

enum
{
	CTR_VR_PASS_NONE = -1,
	CTR_VR_PASS_LEFT = 0,
	CTR_VR_PASS_RIGHT = 1,
	CTR_VR_PASS_HUD_WHITE = 2,
	CTR_VR_PASS_HUD_BLACK = 3, /* the game's own main target */
	CTR_VR_PASS_COUNT = 4,
};

/* std140 NativeVrBlock in the GTE vertex shader */
typedef struct CtrVrUniforms
{
	float eyeFromCamera[16]; /* column-major mat4: game camera space -> eye space */
	float proj[4];           /* x = ax, y = bx, z = ay, w = by */
	float mono[4];           /* unused (classic path) */
	float params[4];         /* x = 1 while an eye pass draws */
} CtrVrUniforms;

global_variable CtrVrConfig s_vrConfig = {1, 64.0f, 120.0f, 1};
global_variable CtrVrEyePose s_vrEyes[2];
global_variable int s_vrEyesValid;
global_variable int s_vrEyeWidth = 1024;
global_variable int s_vrEyeHeight = 1024;

global_variable struct NativeRenderTarget s_vrTargets[3]; /* left, right, HUD over white */
global_variable int s_vrTargetsReady;
global_variable GLuint s_vrUbo;

global_variable int s_vrPass = CTR_VR_PASS_NONE;
global_variable struct NativeRenderTarget s_vrSavedMain;

global_variable int s_vrFrameStereo;    /* this frame draws in stereo */
global_variable int s_vrFrameCleared;   /* targets cleared for this frame */
global_variable int s_vrWorldSplits;    /* world splits seen this frame */
global_variable int s_vrWantStereo;     /* last frame had a 1P world: stereo next */
global_variable int s_vrForced;
global_variable u8 s_vrClearRGB[3];
global_variable int s_vrLogCount;
global_variable int s_vrMonoReference = -1;

global_variable CtrVrUniforms s_vrEyeUniforms[2];
global_variable int s_vrEyeUniformsReady;

void CtrVr_Configure(const CtrVrConfig *config)
{
	if (config != NULL)
	{
		s_vrConfig = *config;
		if (s_vrConfig.unitsPerMeter <= 1.0f)
			s_vrConfig.unitsPerMeter = 64.0f;
		if (s_vrConfig.cullFovDegrees < 60.0f || s_vrConfig.cullFovDegrees > 160.0f)
			s_vrConfig.cullFovDegrees = 120.0f;
	}
}

void CtrVr_SetEyePoses(const CtrVrEyePose eyes[2], int eyeWidth, int eyeHeight, int valid)
{
	if (valid && eyes != NULL)
	{
		s_vrEyes[0] = eyes[0];
		s_vrEyes[1] = eyes[1];
		if (eyeWidth > 16 && eyeHeight > 16)
		{
			s_vrEyeWidth = eyeWidth;
			s_vrEyeHeight = eyeHeight;
		}
	}
	s_vrEyesValid = valid && eyes != NULL;
}

/* Stereo needs the native 3D renderer (camera-space vertices) and depth. */
internal void CtrVr_ForceRendererOptions(void)
{
	if (s_vrForced || !s_vrConfig.enabled)
		return;
	s_vrForced = 1;
	gNativeRendererMode = NATIVE_RENDERER_NATIVE;
	gNativeDepthBufferEnabled = 1;
	gNativeProjectionMode = NATIVE_PROJECTION_PERSPECTIVE;
	printf("[CTR VR] stereo on: native 3D renderer, %.0f units/m, cull fov %.0f deg, level horizon %d\n", s_vrConfig.unitsPerMeter,
	       s_vrConfig.cullFovDegrees, s_vrConfig.levelHorizon);
}

/* Focal scale override (NativeAspect_GetFocalScale): in stereo the game
 * culls with a wide field of view so turning the head finds geometry. */
internal int CtrVr_WideCullActive(void)
{
	return s_vrConfig.enabled && s_vrWantStereo && s_vrEyesValid;
}

internal double CtrVr_CullFocalScale(void)
{
	return 0.75 / tan((double)s_vrConfig.cullFovDegrees * 3.14159265358979 / 360.0);
}

internal void CtrVr_RecordClear(u8 r, u8 g, u8 b)
{
	s_vrClearRGB[0] = r;
	s_vrClearRGB[1] = g;
	s_vrClearRGB[2] = b;
}

/* The GTE shader always reads NativeVrBlock, so the buffer must exist and be
 * bound (zeros = off) before the first draw. */
internal void CtrVr_EnsureUbo(void)
{
	if (s_vrUbo != 0)
		return;
	CtrVrUniforms off;
	SDL_memset(&off, 0, sizeof(off));
	glGenBuffers(1, &s_vrUbo);
	glBindBuffer(GL_UNIFORM_BUFFER, s_vrUbo);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(CtrVrUniforms), &off, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferBase(GL_UNIFORM_BUFFER, CTR_VR_UBO_BINDING, s_vrUbo);
}

internal int CtrVr_InPass(void)
{
	return s_vrPass != CTR_VR_PASS_NONE;
}

internal void CtrVr_EnsureTargets(void)
{
	CtrVr_EnsureUbo();
	if (!s_vrTargetsReady)
	{
		for (int i = 0; i < 3; i++)
		{
			SDL_memset(&s_vrTargets[i], 0, sizeof(s_vrTargets[i]));
			NativeRenderer_InitRenderTarget(&s_vrTargets[i]);
		}
		s_vrTargetsReady = 1;
	}
	for (int i = 0; i < 3; i++)
	{
		struct NativeRenderTarget *target = &s_vrTargets[i];
		if (i < 2)
			NativeRenderer_EnsureRenderTarget(target, s_vrEyeWidth, s_vrEyeHeight);
		else
			NativeRenderer_EnsureRenderTarget(target, s_mainRenderTarget.width, s_mainRenderTarget.height);
		NativeRenderer_EnsureMultisampleStorage(target, s_mainRenderTarget.samples);
		target->logicalWidth = s_mainRenderTarget.logicalWidth;
		target->logicalHeight = s_mainRenderTarget.logicalHeight;
	}
}

internal void CtrVr_UploadUniforms(const CtrVrUniforms *uniforms)
{
	glBindBuffer(GL_UNIFORM_BUFFER, s_vrUbo);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(*uniforms), uniforms);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferBase(GL_UNIFORM_BUFFER, CTR_VR_UBO_BINDING, s_vrUbo);
}

internal void CtrVr_UploadDisabled(void)
{
	if (!s_vrTargetsReady)
		return;
	CtrVrUniforms off;
	SDL_memset(&off, 0, sizeof(off));
	CtrVr_UploadUniforms(&off);
}

/* ---------------------------------------------------------------------- */
/* Eye matrices                                                           */
/* ---------------------------------------------------------------------- */

internal float CtrVr_RowLength(const MATRIX *m, int row)
{
	const float x = (float)m->m[row][0], y = (float)m->m[row][1], z = (float)m->m[row][2];
	return sqrtf(x * x + y * y + z * z);
}

/* Build both eyes' camera->eye matrices from player 1's world camera.
 * Game camera space: x right, y down, z forward, the X/Y axes scaled by the
 * projection's aspect and FOV factors (sx, sy) that PushBuffer_SetMatrixVP
 * folds into matrix_ViewProj. */
internal int CtrVr_BuildEyeUniforms(void)
{
	struct GameTracker *gGT = P32_GET(struct GameTracker *, sdata->gGT);
	if (gGT == NULL)
		return 0;
	struct PushBuffer *pb = &gGT->pushBuffer[0];
	const float camLen = CtrVr_RowLength(&pb->matrix_CameraTranspose, 0);
	if (camLen < 1.0f)
		return 0;
	const float sx = CtrVr_RowLength(&pb->matrix_ViewProj, 0) / camLen;
	const float sy = CtrVr_RowLength(&pb->matrix_ViewProj, 1) / camLen;
	if (sx <= 0.0f || sy <= 0.0f)
		return 0;

	/* L: camera axes -> levelled axes (yaw only, world up kept up). */
	float L[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	if (s_vrConfig.levelHorizon)
	{
		float r[3][3];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r[i][j] = (float)pb->matrix_CameraTranspose.m[i][j] / camLen;
		/* world "down" = the world axis the camera's down axis follows */
		float down[3] = {0.0f, r[1][1] >= 0.0f ? 1.0f : -1.0f, 0.0f};
		float fwd[3] = {r[2][0], 0.0f, r[2][2]};
		float fl = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
		if (fl > 0.05f)
		{
			fwd[0] /= fl;
			fwd[2] /= fl;
			/* right = down x forward (x right, y down, z forward is right-handed) */
			float right[3] = {down[1] * fwd[2] - down[2] * fwd[1], down[2] * fwd[0] - down[0] * fwd[2], down[0] * fwd[1] - down[1] * fwd[0]};
			const float *lv[3] = {right, down, fwd};
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					L[i * 3 + j] = lv[i][0] * r[j][0] + lv[i][1] * r[j][1] + lv[i][2] * r[j][2];
		}
	}

	for (int eye = 0; eye < 2; eye++)
	{
		const CtrVrEyePose *pose = &s_vrEyes[eye];
		/* OpenXR -> game axes: C = diag(1, -1, -1); Rg = C R C, pg = C p */
		float Rg[9];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
			{
				const float ci = (i == 0) ? 1.0f : -1.0f;
				const float cj = (j == 0) ? 1.0f : -1.0f;
				Rg[i * 3 + j] = ci * pose->rot[i * 3 + j] * cj;
			}
		const float pg[3] = {pose->pos[0] * s_vrConfig.unitsPerMeter, -pose->pos[1] * s_vrConfig.unitsPerMeter,
		                     -pose->pos[2] * s_vrConfig.unitsPerMeter};
		/* M = Rg^T * L * diag(1/sx, 1/sy, 1);  t = -Rg^T pg */
		float M[9];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 3; k++)
					sum += Rg[k * 3 + i] * L[k * 3 + j];
				M[i * 3 + j] = sum * ((j == 0) ? 1.0f / sx : (j == 1) ? 1.0f / sy : 1.0f);
			}
		float t[3];
		for (int i = 0; i < 3; i++)
			t[i] = -(Rg[0 * 3 + i] * pg[0] + Rg[1 * 3 + i] * pg[1] + Rg[2 * 3 + i] * pg[2]);

		CtrVrUniforms *u = &s_vrEyeUniforms[eye];
		SDL_memset(u, 0, sizeof(*u));
		for (int col = 0; col < 3; col++)
			for (int row = 0; row < 3; row++)
				u->eyeFromCamera[col * 4 + row] = M[row * 3 + col];
		u->eyeFromCamera[12] = t[0];
		u->eyeFromCamera[13] = t[1];
		u->eyeFromCamera[14] = t[2];
		u->eyeFromCamera[15] = 1.0f;
		/* eye space x right, y down, z forward: clip = (ax x + bx z, ay y + by z, z + bias, z) */
		const float tl = pose->tanLeft, tr = pose->tanRight, tu = pose->tanUp, td = pose->tanDown;
		u->proj[0] = 2.0f / (tr - tl);
		u->proj[1] = -(tr + tl) / (tr - tl);
		u->proj[2] = -2.0f / (tu - td);
		u->proj[3] = -(tu + td) / (tu - td);
		u->params[0] = 1.0f;
	}

	if (s_vrLogCount++ % 600 == 0)
	{
		const struct Driver *d = P32_GET(struct Driver *, gGT->drivers[0]);
		float dist = 0.0f;
		if (d != NULL)
		{
			const float dx = (float)(d->posCurr.x >> 8) - pb->pos.x;
			const float dy = (float)(d->posCurr.y >> 8) - pb->pos.y;
			const float dz = (float)(d->posCurr.z >> 8) - pb->pos.z;
			dist = sqrtf(dx * dx + dy * dy + dz * dz);
		}
		printf("[CTR VR] stereo frame: eye %dx%d sx %.3f sy %.3f P %d camera-to-kart %.0f units (%.2f m)\n", s_vrEyeWidth, s_vrEyeHeight, sx, sy,
		       pb->distanceToScreen_PREV, dist, dist / s_vrConfig.unitsPerMeter);
	}
	return 1;
}

/* ---------------------------------------------------------------------- */
/* Frame and passes                                                       */
/* ---------------------------------------------------------------------- */

/* End of NativeRenderer_BeginScene. */
internal void CtrVr_OnBeginScene(void)
{
	if (s_vrMonoReference < 0)
	{
		const char *mono = getenv("CTR_VR_MONO_REF");
		s_vrMonoReference = (mono != NULL && mono[0] == '1');
	}
	CtrVr_ForceRendererOptions();
	CtrVr_EnsureUbo();
	struct GameTracker *gGT = P32_GET(struct GameTracker *, sdata->gGT);
	const int onePlayer = (gGT != NULL) && (gGT->numPlyrCurrGame == 1) && ((gGT->gameMode1 & MAIN_MENU) == 0);
	s_vrWantStereo = s_vrWantStereo && onePlayer;
	s_vrFrameStereo = s_vrConfig.enabled && s_vrEyesValid && s_vrWantStereo && NATIVE_DRAW3D_ACTIVE();
	s_vrFrameCleared = 0;
	s_vrWorldSplits = 0;
	s_vrEyeUniformsReady = 0;
}

internal int CtrVr_IsWorldSplit(const GPUDrawSplit *split)
{
	return split->drawenv.dfe && (split->worldDepth != NATIVE_GPU_WORLD_DEPTH_NONE) && !split->nativeOverlayLayer;
}

/* NativeGpu_DrawSplitRangePass: 0 = skip this split in the current pass. */
internal int CtrVr_SplitInPass(const GPUDrawSplit *split)
{
	const int world = CtrVr_IsWorldSplit(split);
	if (world && split->numVerts + split->staticCount > 0)
		s_vrWorldSplits++;
	switch (s_vrPass)
	{
	case CTR_VR_PASS_NONE:
		return 1;
	case CTR_VR_PASS_LEFT:
		return world || !split->drawenv.dfe;
	case CTR_VR_PASS_RIGHT:
		return world;
	case CTR_VR_PASS_HUD_BLACK:
		/* desktop debug (CTR_VR_MONO_REF=1): the normal mono frame here */
		return split->drawenv.dfe && (!world || s_vrMonoReference);
	default:
		return split->drawenv.dfe && !world;
	}
}

internal void CtrVr_ClearTarget(struct NativeRenderTarget *target, float r, float g, float b)
{
	glBindFramebuffer(GL_FRAMEBUFFER, NativeRenderer_GetDrawFramebuffer(target));
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean mask[4], depthMask;
	GLint stencilMask, stencilBackMask;
	GLfloat clearColor[4];
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
	glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
	glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &stencilBackMask);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilMask(0xff);
	glClearColor(r, g, b, 1.0f);
	glClearDepthf(1.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	glDepthMask(depthMask);
	glStencilMaskSeparate(GL_FRONT, (GLuint)stencilMask);
	glStencilMaskSeparate(GL_BACK, (GLuint)stencilBackMask);
	glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
	if (scissor)
		glEnable(GL_SCISSOR_TEST);
}

internal void CtrVr_BeginPass(int pass)
{
	s_vrPass = pass;
	if (pass != CTR_VR_PASS_HUD_BLACK)
	{
		s_vrSavedMain = s_mainRenderTarget;
		s_mainRenderTarget = s_vrTargets[pass];
	}
	glBindFramebuffer(GL_FRAMEBUFFER, NativeRenderer_GetDrawFramebuffer(&s_mainRenderTarget));
	NativeRenderer_SetViewPort(0, 0, s_mainRenderTarget.width, s_mainRenderTarget.height);
	NativeRenderer_InvalidateScissorRectCache();
	if (pass <= CTR_VR_PASS_RIGHT && s_vrEyeUniformsReady)
		CtrVr_UploadUniforms(&s_vrEyeUniforms[pass]);
	else
		CtrVr_UploadDisabled();
}

internal void CtrVr_EndPass(void)
{
	if (s_vrPass != CTR_VR_PASS_HUD_BLACK)
	{
		s_vrTargets[s_vrPass] = s_mainRenderTarget;
		s_mainRenderTarget = s_vrSavedMain;
	}
	s_vrPass = CTR_VR_PASS_NONE;
	glBindFramebuffer(GL_FRAMEBUFFER, NativeRenderer_GetDrawFramebuffer(&s_mainRenderTarget));
	NativeRenderer_SetViewPort(0, 0, s_mainRenderTarget.width, s_mainRenderTarget.height);
	NativeRenderer_InvalidateScissorRectCache();
}

/* NativeGpu_DrawPreparedFrame: 1 = the batch was drawn in stereo. */
internal int CtrVr_DrawBatch(void (*drawSplits)(void))
{
	if (!s_vrFrameStereo)
	{
		s_vrPass = CTR_VR_PASS_NONE;
		drawSplits(); /* counts world splits for the next frame's decision */
		return 1;
	}
	CtrVr_EnsureTargets();
	if (!s_vrFrameCleared)
	{
		const float r = s_vrClearRGB[0] / 255.0f, g = s_vrClearRGB[1] / 255.0f, b = s_vrClearRGB[2] / 255.0f;
		CtrVr_ClearTarget(&s_vrTargets[0], r, g, b);
		CtrVr_ClearTarget(&s_vrTargets[1], r, g, b);
		CtrVr_ClearTarget(&s_vrTargets[2], 1.0f, 1.0f, 1.0f);
		CtrVr_ClearTarget(&s_mainRenderTarget, 0.0f, 0.0f, 0.0f);
		s_vrFrameCleared = 1;
	}
	if (!s_vrEyeUniformsReady)
		s_vrEyeUniformsReady = CtrVr_BuildEyeUniforms();
	for (int pass = 0; pass < CTR_VR_PASS_COUNT; pass++)
	{
		CtrVr_BeginPass(pass);
		drawSplits();
		CtrVr_EndPass();
	}
	CtrVr_UploadDisabled();
	return 1;
}

int CtrVr_FinishFrame(void)
{
	const int stereo = s_vrFrameStereo && s_vrFrameCleared && s_vrEyeUniformsReady;
	/* every pass counted the same world splits; any means a 3D race view */
	s_vrWantStereo = (s_vrWorldSplits > 0);
	if (stereo)
	{
		NativeRenderer_ResolveMultisample(&s_vrTargets[0]);
		NativeRenderer_ResolveMultisample(&s_vrTargets[1]);
		NativeRenderer_ResolveMultisample(&s_vrTargets[2]);
		NativeRenderer_ResolveMultisample(&s_mainRenderTarget);
	}
	s_vrFrameStereo = 0;
	return stereo;
}

unsigned int CtrVr_GetEyeTexture(int eye)
{
	return (eye >= 0 && eye < 2 && s_vrTargetsReady) ? (unsigned int)s_vrTargets[eye].texture : 0u;
}

unsigned int CtrVr_GetHudTexture(int white)
{
	if (white)
		return s_vrTargetsReady ? (unsigned int)s_vrTargets[2].texture : 0u;
	return (unsigned int)s_mainRenderTarget.texture;
}

void CtrVr_GetHudSize(int *width, int *height)
{
	if (width != NULL)
		*width = s_mainRenderTarget.width;
	if (height != NULL)
		*height = s_mainRenderTarget.height;
}

#endif /* CTR_QUEST_VR */
