/*
 * Desktop test of the Quest window hooks (CMake CTR_QUEST_DESKTOP_TEST, GLES
 * builds only): the game draws into the offscreen screen framebuffer exactly
 * as on the headset, and "present" scales it into the real window instead of
 * an OpenXR swapchain. Catches redirect bugs (missing depth/stencil, viewport,
 * stray binds of the real window) without a headset.
 */
#if defined(CTR_QUEST_DESKTOP_TEST) && !defined(__ANDROID__)

#include <GLES3/gl32.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/native_vr.h"

static int s_screenW = 1920;
static int s_screenH = 1080;
static GLuint s_screenFbo;
static GLuint s_screenTexture;
static GLuint s_screenDepth;

static void CtrQuest_EnsureScreen(void)
{
	GLint oldTexture = 0;
	GLint oldRenderbuffer = 0;

	if (s_screenFbo != 0)
	{
		return;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &oldRenderbuffer);
	glGenTextures(1, &s_screenTexture);
	glBindTexture(GL_TEXTURE_2D, s_screenTexture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, s_screenW, s_screenH);
	glGenRenderbuffers(1, &s_screenDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, s_screenDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_screenW, s_screenH);
	glGenFramebuffers(1, &s_screenFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, s_screenFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_screenTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_screenDepth);
	printf("[CTR Quest test] screen %dx%d status 0x%x\n", s_screenW, s_screenH, glCheckFramebufferStatus(GL_FRAMEBUFFER));
	glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);
	glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)oldRenderbuffer);
}

void CtrQuest_BindFramebuffer(GLenum target, GLuint framebuffer)
{
	if (framebuffer == 0)
	{
		CtrQuest_EnsureScreen();
		framebuffer = s_screenFbo;
	}
	glBindFramebuffer(target, framebuffer);
}

bool CtrQuest_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{
	(void)window;
	if (w != NULL) *w = s_screenW;
	if (h != NULL) *h = s_screenH;
	return true;
}

/* Stereo test: fixed Quest 3-like eyes, the head turned by CTR_VR_YAW /
 * CTR_VR_PITCH degrees (stereo off with CTR_VR_STEREO=0). The window shows
 * the left and right eye on top and the HUD over black / over white below. */
static float CtrQuest_EnvFloat(const char *name, float fallback)
{
	const char *value = getenv(name);
	return (value != NULL && value[0] != 0) ? (float)atof(value) : fallback;
}

static void CtrQuest_TestEyes(void)
{
	static int configured;
	const float yaw = CtrQuest_EnvFloat("CTR_VR_YAW", 0.0f) * 3.14159265f / 180.0f;
	const float pitch = CtrQuest_EnvFloat("CTR_VR_PITCH", 0.0f) * 3.14159265f / 180.0f;
	CtrVrEyePose eyes[2];

	if (!configured)
	{
		CtrVrConfig config = {(int)CtrQuest_EnvFloat("CTR_VR_STEREO", 1.0f), CtrQuest_EnvFloat("CTR_VR_WORLD_SCALE", 64.0f),
		                      CtrQuest_EnvFloat("CTR_VR_CULL_FOV", 120.0f), (int)CtrQuest_EnvFloat("CTR_VR_LEVEL", 1.0f)};
		CtrVr_Configure(&config);
		configured = 1;
	}
	/* R = yaw about +Y then pitch about +X (OpenXR axes, row-major) */
	const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
	const float rot[9] = {cy, sy * sp, sy * cp, 0.0f, cp, -sp, -sy, cy * sp, cy * cp};
	for (int eye = 0; eye < 2; eye++)
	{
		const float side = (eye == 0) ? -0.032f : 0.032f;
		for (int i = 0; i < 9; i++)
			eyes[eye].rot[i] = rot[i];
		eyes[eye].pos[0] = rot[0] * side;
		eyes[eye].pos[1] = rot[3] * side;
		eyes[eye].pos[2] = rot[6] * side;
		eyes[eye].tanLeft = (eye == 0) ? -1.00f : -0.85f;
		eyes[eye].tanRight = (eye == 0) ? 0.85f : 1.00f;
		eyes[eye].tanUp = 0.95f;
		eyes[eye].tanDown = -1.05f;
	}
	CtrVr_SetEyePoses(eyes, 960, 1000, 1);
}

static void CtrQuest_BlitTexture(GLuint texture, int srcW, int srcH, int x, int y, int w, int h)
{
	static GLuint readFbo;
	if (texture == 0)
		return;
	if (readFbo == 0)
		glGenFramebuffers(1, &readFbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glBlitFramebuffer(0, 0, srcW, srcH, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

bool CtrQuest_SwapWindow(SDL_Window *window)
{
	GLint drawFbo = 0;
	GLint readFbo = 0;
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	int windowW = 0;
	int windowH = 0;
	static int lastStereo = -1;
	const int stereo = CtrVr_FinishFrame();

	CtrQuest_EnsureScreen();
	SDL_GetWindowSizeInPixels(window, &windowW, &windowH);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	if (stereo)
	{
		int hudW = 0, hudH = 0;
		const int halfW = windowW / 2, halfH = windowH / 2;
		CtrVr_GetHudSize(&hudW, &hudH);
		glClearColor(0.2f, 0.0f, 0.2f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		CtrQuest_BlitTexture(CtrVr_GetEyeTexture(0), 960, 1000, 0, halfH, halfW, halfH);
		CtrQuest_BlitTexture(CtrVr_GetEyeTexture(1), 960, 1000, halfW, halfH, halfW, halfH);
		CtrQuest_BlitTexture(CtrVr_GetHudTexture(0), hudW, hudH, 0, 0, halfW, halfH);
		CtrQuest_BlitTexture(CtrVr_GetHudTexture(1), hudW, hudH, halfW, 0, halfW, halfH);
	}
	else
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, s_screenFbo);
		glBlitFramebuffer(0, 0, s_screenW, s_screenH, 0, 0, windowW, windowH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}
	if (stereo != lastStereo)
	{
		printf("[CTR Quest test] view: %s\n", stereo ? "stereo" : "flat");
		lastStereo = stereo;
	}
	SDL_GL_SwapWindow(window);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFbo);
	if (scissor)
	{
		glEnable(GL_SCISSOR_TEST);
	}
	CtrQuest_TestEyes();
	return true;
}

#endif
