/*
 * Android/Quest build only: force-included into the unity build (main.c) by
 * CMake. It reroutes the three places where the game talks to "the window" so
 * that platform/quest/ctr_quest.c can show the frame inside an OpenXR session
 * without touching game code:
 *
 *   - framebuffer 0 (the window) becomes an offscreen "screen" framebuffer,
 *   - the window size is the size of that screen,
 *   - SDL_GL_SwapWindow presents the screen on a virtual screen in the headset.
 *
 * The macros also rename the declarations in the SDL and GLES headers, which
 * is fine: the replacements have identical signatures.
 */
#ifndef CTR_QUEST_HOOKS_H
#define CTR_QUEST_HOOKS_H

#if defined(__ANDROID__) || defined(CTR_QUEST_DESKTOP_TEST)

#define glBindFramebuffer         CtrQuest_BindFramebuffer
#define SDL_GetWindowSizeInPixels CtrQuest_GetWindowSizeInPixels
#define SDL_GetWindowSize         CtrQuest_GetWindowSizeInPixels
#define SDL_GL_SwapWindow         CtrQuest_SwapWindow

#endif

#endif
