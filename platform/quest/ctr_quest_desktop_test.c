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
#include <stdio.h>
#include <stdlib.h>

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

bool CtrQuest_SwapWindow(SDL_Window *window)
{
	GLint drawFbo = 0;
	GLint readFbo = 0;
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	int windowW = 0;
	int windowH = 0;

	CtrQuest_EnsureScreen();
	SDL_GetWindowSizeInPixels(window, &windowW, &windowH);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, s_screenFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBlitFramebuffer(0, 0, s_screenW, s_screenH, 0, 0, windowW, windowH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	SDL_GL_SwapWindow(window);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFbo);
	if (scissor)
	{
		glEnable(GL_SCISSOR_TEST);
	}
	return true;
}

#endif
