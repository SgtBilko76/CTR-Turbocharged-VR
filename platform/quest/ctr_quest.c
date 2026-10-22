/*
 * CTR: Turbocharged on Meta Quest (stage 1: the game on a virtual screen).
 *
 * The game keeps its own renderer, SDL window and GL context. Three calls are
 * rerouted here by include/platform/ctr_quest_hooks.h:
 *
 *   glBindFramebuffer(.., 0)    -> an offscreen "screen" framebuffer
 *   SDL_GetWindowSizeInPixels   -> the size of that screen
 *   SDL_GL_SwapWindow           -> one OpenXR frame: the screen is copied into
 *                                  a swapchain and shown as a quad layer
 *
 * Everything runs on the game's render thread with the game's EGL context,
 * so there is no second context and no locking. The game paces itself on its
 * own wall-clock VBlank schedule; xrWaitFrame only aligns the present.
 *
 * Touch controllers become one virtual SDL gamepad (vendor 0x1209), which the
 * game opens like any other pad:
 *
 *   right trigger / left trigger -> R2 / L2      A B X Y  -> Cross Circle Square Triangle
 *   right grip / left grip       -> R1 / L1      left stick       -> analog stick
 *   right stick                  -> D-pad        left stick click -> Select
 *   left menu button             -> Start        right stick click -> R3
 *   hold both stick clicks 1 s   -> recentre the screen
 *
 * The game's rumble goes to both controllers.
 *
 * Data folder: /sdcard/CTR (assets/ctr-u.bin, config.ini, memcards, vr.cfg).
 */
#if defined(__ANDROID__)

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <SDL3/SDL.h>

#define LOG_TAG "CTRQuest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CTRQ_DATA_DIR "/sdcard/CTR"
#define CTRQ_PAD_VENDOR  0x1209
#define CTRQ_PAD_PRODUCT 0xC7C1

/* ------------------------------------------------------------------------ */
/* Settings (/sdcard/CTR/vr.cfg, key=value, all optional)                    */
/* ------------------------------------------------------------------------ */

static struct
{
	int screenPixelsW;      /* screen_pixels_w: game resolution */
	int screenPixelsH;      /* screen_pixels_h */
	float screenWidth;      /* screen_width: metres */
	float screenDistance;   /* screen_distance: metres from the head */
	float screenHeight;     /* screen_height: metres above eye level */
	float refreshRate;      /* refresh_rate: Hz (72, 80, 90, 120) */
	int flipY;              /* flip_y: 1 = flip the picture vertically */
} s_cfg = {1920, 1080, 3.0f, 2.4f, 0.0f, 72.0f, 0};

static void CtrQuest_LoadConfig(void)
{
	FILE *file = fopen(CTRQ_DATA_DIR "/vr.cfg", "r");
	char line[256];

	if (file == NULL)
	{
		return;
	}
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char key[64];
		float value;

		if (line[0] == '#' || sscanf(line, " %63[^= ] = %f", key, &value) != 2)
		{
			continue;
		}
		if (strcmp(key, "screen_pixels_w") == 0) s_cfg.screenPixelsW = (int)value;
		else if (strcmp(key, "screen_pixels_h") == 0) s_cfg.screenPixelsH = (int)value;
		else if (strcmp(key, "screen_width") == 0) s_cfg.screenWidth = value;
		else if (strcmp(key, "screen_distance") == 0) s_cfg.screenDistance = value;
		else if (strcmp(key, "screen_height") == 0) s_cfg.screenHeight = value;
		else if (strcmp(key, "refresh_rate") == 0) s_cfg.refreshRate = value;
		else if (strcmp(key, "flip_y") == 0) s_cfg.flipY = (int)value;
	}
	fclose(file);
	if (s_cfg.screenPixelsW < 320 || s_cfg.screenPixelsW > 4096) s_cfg.screenPixelsW = 1920;
	if (s_cfg.screenPixelsH < 240 || s_cfg.screenPixelsH > 4096) s_cfg.screenPixelsH = 1080;
	LOGI("vr.cfg: %dx%d px, %.2f m wide at %.2f m, %.0f Hz", s_cfg.screenPixelsW, s_cfg.screenPixelsH, s_cfg.screenWidth,
	     s_cfg.screenDistance, s_cfg.refreshRate);
}

/* ------------------------------------------------------------------------ */
/* Start-up: logcat, crash handler, data folder                              */
/* ------------------------------------------------------------------------ */

static int s_logPipe[2];

static void *CtrQuest_LogThread(void *unused)
{
	char buffer[1024];
	size_t used = 0;
	(void)unused;

	for (;;)
	{
		ssize_t got = read(s_logPipe[0], buffer + used, sizeof(buffer) - 1 - used);
		if (got <= 0)
		{
			break;
		}
		used += (size_t)got;
		buffer[used] = 0;
		char *start = buffer;
		char *newline;
		while ((newline = strchr(start, '\n')) != NULL)
		{
			*newline = 0;
			if (newline > start && newline[-1] == '\r')
			{
				newline[-1] = 0;
			}
			__android_log_write(ANDROID_LOG_INFO, "CTR", start);
			start = newline + 1;
		}
		used = strlen(start);
		memmove(buffer, start, used);
		if (used >= sizeof(buffer) - 1)
		{
			buffer[used] = 0;
			__android_log_write(ANDROID_LOG_INFO, "CTR", buffer);
			used = 0;
		}
	}
	return NULL;
}

static void CtrQuest_RedirectStdio(void)
{
	pthread_t thread;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (pipe(s_logPipe) != 0)
	{
		return;
	}
	dup2(s_logPipe[1], STDOUT_FILENO);
	dup2(s_logPipe[1], STDERR_FILENO);
	pthread_create(&thread, NULL, CtrQuest_LogThread, NULL);
	pthread_detach(thread);
}

/* A native crash in a big process makes Android's crash_dump hang for
 * minutes (and blocks installs), so log our own backtrace and exit at once. */
struct CtrQuestBacktrace
{
	int count;
};

static _Unwind_Reason_Code CtrQuest_UnwindFrame(struct _Unwind_Context *context, void *arg)
{
	struct CtrQuestBacktrace *state = (struct CtrQuestBacktrace *)arg;
	uintptr_t pc = _Unwind_GetIP(context);
	Dl_info info;

	if (pc == 0 || state->count >= 32)
	{
		return _URC_END_OF_STACK;
	}
	if (dladdr((void *)pc, &info) != 0 && info.dli_fname != NULL)
	{
		const char *lib = strrchr(info.dli_fname, '/');
		LOGE("  #%02d %s+0x%lx (%s)", state->count, lib ? lib + 1 : info.dli_fname, (unsigned long)(pc - (uintptr_t)info.dli_fbase),
		     info.dli_sname ? info.dli_sname : "?");
	}
	else
	{
		LOGE("  #%02d 0x%lx", state->count, (unsigned long)pc);
	}
	state->count++;
	return _URC_NO_REASON;
}

static atomic_int s_crashing;

static void CtrQuest_CrashHandler(int sig, siginfo_t *info, void *ucontext)
{
	struct CtrQuestBacktrace state = {0};
	(void)ucontext;

	if (atomic_exchange(&s_crashing, 1) != 0)
	{
		for (;;)
		{
			pause();
		}
	}
	LOGE("FATAL signal %d (code %d) at address %p", sig, info ? info->si_code : 0, info ? info->si_addr : NULL);
	_Unwind_Backtrace(CtrQuest_UnwindFrame, &state);
	_exit(128 + sig);
}

static void CtrQuest_InstallCrashHandler(void)
{
	static char altStack[64 * 1024];
	stack_t stack = {0};
	struct sigaction action;
	static const int signals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};

	stack.ss_sp = altStack;
	stack.ss_size = sizeof(altStack);
	sigaltstack(&stack, NULL);
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = CtrQuest_CrashHandler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
	{
		sigaction(signals[i], &action, NULL);
	}
}

/* Runs when SDLActivity loads libmain.so, before SDL_main. */
__attribute__((constructor)) static void CtrQuest_OnLoad(void)
{
	CtrQuest_RedirectStdio();
	CtrQuest_InstallCrashHandler();
	mkdir(CTRQ_DATA_DIR, 0775);
	mkdir(CTRQ_DATA_DIR "/assets", 0775);
	/* NativeAssets_Init takes the data folder from this variable on Linux. */
	setenv("CTR_TURBOCHARGED_DATA_DIR", CTRQ_DATA_DIR, 1);
	/* The Touch controllers also show up as Android gamepads that never send
	 * input in VR; only the virtual pad below may become a player. */
	setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x1209/0xc7c1", 1);
	CtrQuest_LoadConfig();
	if (access(CTRQ_DATA_DIR "/assets/ctr-u.bin", R_OK) != 0)
	{
		LOGE("missing " CTRQ_DATA_DIR "/assets/ctr-u.bin (errno %d): copy the NTSC-U disc image there", errno);
	}
	LOGI("CTR Quest layer loaded");
}

/* ------------------------------------------------------------------------ */
/* Screen framebuffer (the game's "window")                                  */
/* ------------------------------------------------------------------------ */

static GLuint s_screenFbo;
static GLuint s_screenTexture;
static GLuint s_screenDepth;

static void CtrQuest_EnsureScreen(void)
{
	GLint oldTexture = 0;
	GLint oldRenderbuffer = 0;
	GLenum status;

	if (s_screenFbo != 0)
	{
		return;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &oldRenderbuffer);

	glGenTextures(1, &s_screenTexture);
	glBindTexture(GL_TEXTURE_2D, s_screenTexture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, s_cfg.screenPixelsW, s_cfg.screenPixelsH);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenRenderbuffers(1, &s_screenDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, s_screenDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_cfg.screenPixelsW, s_cfg.screenPixelsH);

	glGenFramebuffers(1, &s_screenFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, s_screenFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_screenTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_screenDepth);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);
	glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)oldRenderbuffer);
	LOGI("screen framebuffer %dx%d: status 0x%x", s_cfg.screenPixelsW, s_cfg.screenPixelsH, status);
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
	if (w != NULL) *w = s_cfg.screenPixelsW;
	if (h != NULL) *h = s_cfg.screenPixelsH;
	return true;
}

/* ------------------------------------------------------------------------ */
/* Small maths                                                               */
/* ------------------------------------------------------------------------ */

static XrQuaternionf CtrQuest_QuatYaw(float yaw)
{
	XrQuaternionf q = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
	return q;
}

/* Yaw of a head orientation, 0 = looking down -Z. */
static float CtrQuest_YawOf(XrQuaternionf q)
{
	/* Forward vector = q * (0,0,-1). */
	float fx = -2.0f * (q.x * q.z + q.w * q.y);
	float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	return atan2f(-fx, -fz);
}

/* ------------------------------------------------------------------------ */
/* OpenXR state                                                              */
/* ------------------------------------------------------------------------ */

#define XR_CHECK(call)                                                                   \
	do                                                                                   \
	{                                                                                    \
		XrResult xrCheckResult = (call);                                                 \
		if (XR_FAILED(xrCheckResult))                                                    \
		{                                                                                \
			LOGE("%s failed: %d (line %d)", #call, (int)xrCheckResult, __LINE__);       \
			return 0;                                                                    \
		}                                                                                \
	} while (0)

enum
{
	HAND_LEFT,
	HAND_RIGHT,
};

static int s_xrTried;
static int s_xrOk;
static XrInstance s_instance;
static XrSystemId s_system;
static XrSession s_session;
static XrSpace s_localSpace;
static XrSpace s_viewSpace;
static XrSessionState s_sessionState = XR_SESSION_STATE_UNKNOWN;
static int s_sessionRunning;
static int s_hasRefreshRateExt;

static XrSwapchain s_swapchain;
static uint32_t s_swapchainCount;
static XrSwapchainImageOpenGLESKHR s_swapchainImages[8];
static GLuint s_swapchainFbos[8];
static int s_swapchainSrgb;
static int s_srgbWriteControl;

static GLuint s_copyProgram;
static GLint s_copyDecodeLoc;
static GLint s_copyFlipLoc;
static GLuint s_copyVao;

static XrActionSet s_actionSet;
static XrPath s_handPath[2];
static XrAction s_triggerAction, s_gripAction, s_stickAction, s_stickXAction, s_stickYAction, s_stickClickAction;
static XrAction s_aAction, s_bAction, s_xAction, s_yAction, s_menuAction, s_hapticAction;

/* Screen anchor: head position + yaw when the screen was (re)centred. */
static int s_anchorValid;
static int s_recentreRequested = 1;
static XrVector3f s_anchorPos;
static float s_anchorYaw;
static double s_bothClicksSince;

static double CtrQuest_Now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int CtrQuest_HasExtension(const XrExtensionProperties *props, uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++)
	{
		if (strcmp(props[i].extensionName, name) == 0)
		{
			return 1;
		}
	}
	return 0;
}

static int CtrQuest_InitInstance(JavaVM *vm, jobject activity)
{
	PFN_xrInitializeLoaderKHR initializeLoader = NULL;
	XrLoaderInitInfoAndroidKHR loaderInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	XrInstanceCreateInfoAndroidKHR androidInfo = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	XrExtensionProperties *props;
	uint32_t extCount = 0;
	const char *extensions[4];
	uint32_t enabled = 0;

	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initializeLoader);
	if (initializeLoader != NULL)
	{
		loaderInfo.applicationVM = vm;
		loaderInfo.applicationContext = activity;
		initializeLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo);
	}

	XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL));
	props = (XrExtensionProperties *)calloc(extCount ? extCount : 1, sizeof(*props));
	for (uint32_t i = 0; i < extCount; i++)
	{
		props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, props);
	if (!CtrQuest_HasExtension(props, extCount, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME))
	{
		LOGE("runtime lacks XR_KHR_opengl_es_enable");
		free(props);
		return 0;
	}
	extensions[enabled++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	if (CtrQuest_HasExtension(props, extCount, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
	{
		extensions[enabled++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
	}
	if (CtrQuest_HasExtension(props, extCount, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		extensions[enabled++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
		s_hasRefreshRateExt = 1;
	}
	free(props);

	androidInfo.applicationVM = vm;
	androidInfo.applicationActivity = activity;
	createInfo.next = &androidInfo;
	strcpy(createInfo.applicationInfo.applicationName, "CTR Turbocharged VR");
	createInfo.applicationInfo.applicationVersion = 1;
	strcpy(createInfo.applicationInfo.engineName, "ctr-native");
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
	createInfo.enabledExtensionCount = enabled;
	createInfo.enabledExtensionNames = extensions;
	XR_CHECK(xrCreateInstance(&createInfo, &s_instance));

	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(s_instance, &systemInfo, &s_system));
	return 1;
}

static XrPath CtrQuest_Path(const char *text)
{
	XrPath path = XR_NULL_PATH;
	xrStringToPath(s_instance, text, &path);
	return path;
}

static int CtrQuest_CreateAction(XrAction *action, const char *name, XrActionType type, int perHand)
{
	XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};

	info.actionType = type;
	strcpy(info.actionName, name);
	strcpy(info.localizedActionName, name);
	if (perHand)
	{
		info.countSubactionPaths = 2;
		info.subactionPaths = s_handPath;
	}
	XR_CHECK(xrCreateAction(s_actionSet, &info, action));
	return 1;
}

static int CtrQuest_InitActions(void)
{
	XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
	XrActionSuggestedBinding bindings[32];
	XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	uint32_t n = 0;

	strcpy(setInfo.actionSetName, "ctr");
	strcpy(setInfo.localizedActionSetName, "Crash Team Racing");
	XR_CHECK(xrCreateActionSet(s_instance, &setInfo, &s_actionSet));

	s_handPath[HAND_LEFT] = CtrQuest_Path("/user/hand/left");
	s_handPath[HAND_RIGHT] = CtrQuest_Path("/user/hand/right");

	if (!CtrQuest_CreateAction(&s_triggerAction, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
	    !CtrQuest_CreateAction(&s_gripAction, "grip", XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
	    !CtrQuest_CreateAction(&s_stickAction, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 1) ||
	    !CtrQuest_CreateAction(&s_stickXAction, "stick_x", XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
	    !CtrQuest_CreateAction(&s_stickYAction, "stick_y", XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
	    !CtrQuest_CreateAction(&s_stickClickAction, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, 1) ||
	    !CtrQuest_CreateAction(&s_aAction, "a", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
	    !CtrQuest_CreateAction(&s_bAction, "b", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
	    !CtrQuest_CreateAction(&s_xAction, "x", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
	    !CtrQuest_CreateAction(&s_yAction, "y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
	    !CtrQuest_CreateAction(&s_menuAction, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
	    !CtrQuest_CreateAction(&s_hapticAction, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1))
	{
		return 0;
	}

#define BIND(xrAction, xrPath)                              \
	do                                                      \
	{                                                       \
		bindings[n].action = (xrAction);                    \
		bindings[n].binding = CtrQuest_Path(xrPath);        \
		n++;                                                \
	} while (0)
	BIND(s_triggerAction, "/user/hand/left/input/trigger/value");
	BIND(s_triggerAction, "/user/hand/right/input/trigger/value");
	BIND(s_gripAction, "/user/hand/left/input/squeeze/value");
	BIND(s_gripAction, "/user/hand/right/input/squeeze/value");
	BIND(s_stickAction, "/user/hand/left/input/thumbstick");
	BIND(s_stickAction, "/user/hand/right/input/thumbstick");
	BIND(s_stickXAction, "/user/hand/left/input/thumbstick/x");
	BIND(s_stickXAction, "/user/hand/right/input/thumbstick/x");
	BIND(s_stickYAction, "/user/hand/left/input/thumbstick/y");
	BIND(s_stickYAction, "/user/hand/right/input/thumbstick/y");
	BIND(s_stickClickAction, "/user/hand/left/input/thumbstick/click");
	BIND(s_stickClickAction, "/user/hand/right/input/thumbstick/click");
	BIND(s_aAction, "/user/hand/right/input/a/click");
	BIND(s_bAction, "/user/hand/right/input/b/click");
	BIND(s_xAction, "/user/hand/left/input/x/click");
	BIND(s_yAction, "/user/hand/left/input/y/click");
	BIND(s_menuAction, "/user/hand/left/input/menu/click");
	BIND(s_hapticAction, "/user/hand/left/output/haptic");
	BIND(s_hapticAction, "/user/hand/right/output/haptic");
#undef BIND

	suggested.interactionProfile = CtrQuest_Path("/interaction_profiles/oculus/touch_controller");
	suggested.countSuggestedBindings = n;
	suggested.suggestedBindings = bindings;
	XR_CHECK(xrSuggestInteractionProfileBindings(s_instance, &suggested));

	attach.countActionSets = 1;
	attach.actionSets = &s_actionSet;
	XR_CHECK(xrAttachSessionActionSets(s_session, &attach));
	return 1;
}

static int CtrQuest_HasGlExtension(const char *name)
{
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (GLint i = 0; i < count; i++)
	{
		const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (ext != NULL && strcmp(ext, name) == 0)
		{
			return 1;
		}
	}
	return 0;
}

static int CtrQuest_CreateSwapchain(void)
{
	XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	int64_t formats[64];
	uint32_t formatCount = 0;
	int64_t chosen = GL_RGBA8;
	GLint oldFramebuffer = 0;

	XR_CHECK(xrEnumerateSwapchainFormats(s_session, 64, &formatCount, formats));
	for (uint32_t i = 0; i < formatCount; i++)
	{
		if (formats[i] == GL_SRGB8_ALPHA8)
		{
			chosen = GL_SRGB8_ALPHA8;
			s_swapchainSrgb = 1;
		}
	}
	/* The game's pixels are already gamma-encoded. With an sRGB swapchain the
	 * compositor decodes them once, so they must be written unchanged: turn
	 * off sRGB encoding on write, or decode in the copy shader if we can't. */
	s_srgbWriteControl = CtrQuest_HasGlExtension("GL_EXT_sRGB_write_control");
	LOGI("swapchain format 0x%llx srgb=%d write_control=%d", (long long)chosen, s_swapchainSrgb, s_srgbWriteControl);

	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = chosen;
	info.sampleCount = 1;
	info.width = (uint32_t)s_cfg.screenPixelsW;
	info.height = (uint32_t)s_cfg.screenPixelsH;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	XR_CHECK(xrCreateSwapchain(s_session, &info, &s_swapchain));

	XR_CHECK(xrEnumerateSwapchainImages(s_swapchain, 0, &s_swapchainCount, NULL));
	if (s_swapchainCount > 8)
	{
		s_swapchainCount = 8;
	}
	for (uint32_t i = 0; i < s_swapchainCount; i++)
	{
		s_swapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
	}
	XR_CHECK(xrEnumerateSwapchainImages(s_swapchain, s_swapchainCount, &s_swapchainCount, (XrSwapchainImageBaseHeader *)s_swapchainImages));

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
	glGenFramebuffers((GLsizei)s_swapchainCount, s_swapchainFbos);
	for (uint32_t i = 0; i < s_swapchainCount; i++)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, s_swapchainFbos[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_swapchainImages[i].image, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldFramebuffer);
	return 1;
}

static GLuint CtrQuest_CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		LOGE("copy shader: %s", log);
	}
	return shader;
}

static int CtrQuest_InitCopyPass(void)
{
	static const char *vs = "#version 300 es\n"
	                        "uniform float u_flip;\n"
	                        "out vec2 v_uv;\n"
	                        "void main() {\n"
	                        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	                        "  v_uv = vec2(p.x, mix(p.y, 1.0 - p.y, u_flip));\n"
	                        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	                        "}\n";
	static const char *fs = "#version 300 es\n"
	                        "precision highp float;\n"
	                        "uniform sampler2D u_screen;\n"
	                        "uniform float u_decode;\n"
	                        "in vec2 v_uv;\n"
	                        "out vec4 o_color;\n"
	                        "void main() {\n"
	                        "  vec3 c = texture(u_screen, v_uv).rgb;\n"
	                        "  vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
	                        "  o_color = vec4(mix(c, lin, u_decode), 1.0);\n"
	                        "}\n";
	GLuint vertex = CtrQuest_CompileShader(GL_VERTEX_SHADER, vs);
	GLuint fragment = CtrQuest_CompileShader(GL_FRAGMENT_SHADER, fs);
	GLint ok = 0;
	GLint oldProgram = 0;

	s_copyProgram = glCreateProgram();
	glAttachShader(s_copyProgram, vertex);
	glAttachShader(s_copyProgram, fragment);
	glLinkProgram(s_copyProgram);
	glGetProgramiv(s_copyProgram, GL_LINK_STATUS, &ok);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!ok)
	{
		LOGE("copy program failed to link");
		return 0;
	}
	glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
	glUseProgram(s_copyProgram);
	glUniform1i(glGetUniformLocation(s_copyProgram, "u_screen"), 0);
	s_copyDecodeLoc = glGetUniformLocation(s_copyProgram, "u_decode");
	s_copyFlipLoc = glGetUniformLocation(s_copyProgram, "u_flip");
	glUseProgram((GLuint)oldProgram);
	glGenVertexArrays(1, &s_copyVao);
	return 1;
}

static int CtrQuest_InitXr(void)
{
	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	jobject activity = (jobject)SDL_GetAndroidActivity();
	JavaVM *vm = NULL;
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = NULL;
	XrGraphicsRequirementsOpenGLESKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	XrGraphicsBindingOpenGLESAndroidKHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
	XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};

	if (env == NULL || activity == NULL)
	{
		LOGE("no JNI env / activity");
		return 0;
	}
	(*env)->GetJavaVM(env, &vm);
	activity = (*env)->NewGlobalRef(env, activity);

	if (!CtrQuest_InitInstance(vm, activity))
	{
		return 0;
	}

	xrGetInstanceProcAddr(s_instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements);
	if (getRequirements == NULL)
	{
		LOGE("xrGetOpenGLESGraphicsRequirementsKHR missing");
		return 0;
	}
	XR_CHECK(getRequirements(s_instance, s_system, &requirements));

	binding.display = eglGetCurrentDisplay();
	binding.config = (EGLConfig)SDL_EGL_GetCurrentConfig();
	binding.context = eglGetCurrentContext();
	sessionInfo.next = &binding;
	sessionInfo.systemId = s_system;
	XR_CHECK(xrCreateSession(s_instance, &sessionInfo, &s_session));

	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XR_CHECK(xrCreateReferenceSpace(s_session, &spaceInfo, &s_localSpace));
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(s_session, &spaceInfo, &s_viewSpace));

	if (!CtrQuest_InitActions() || !CtrQuest_CreateSwapchain() || !CtrQuest_InitCopyPass())
	{
		return 0;
	}
	if (s_srgbWriteControl)
	{
		/* Only our swapchain is sRGB; the game's targets are all UNORM. */
		glDisable(GL_FRAMEBUFFER_SRGB_EXT);
	}
	LOGI("OpenXR ready (GL %s)", (const char *)glGetString(GL_VERSION));
	return 1;
}

static void CtrQuest_RequestRefreshRate(void)
{
	PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = NULL;
	PFN_xrRequestDisplayRefreshRateFB requestRate = NULL;
	float rates[16];
	uint32_t count = 0;
	float best = 0.0f;

	if (!s_hasRefreshRateExt)
	{
		return;
	}
	xrGetInstanceProcAddr(s_instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&enumerateRates);
	xrGetInstanceProcAddr(s_instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&requestRate);
	if (enumerateRates == NULL || requestRate == NULL || XR_FAILED(enumerateRates(s_session, 16, &count, rates)))
	{
		return;
	}
	for (uint32_t i = 0; i < count; i++)
	{
		if (rates[i] <= s_cfg.refreshRate + 0.5f && rates[i] > best)
		{
			best = rates[i];
		}
	}
	if (best > 0.0f)
	{
		requestRate(s_session, best);
		LOGI("display refresh rate %.1f Hz", best);
	}
}

static void CtrQuest_HandleEvents(void)
{
	XrEventDataBuffer event;

	for (;;)
	{
		memset(&event, 0, sizeof(event));
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		if (xrPollEvent(s_instance, &event) != XR_SUCCESS)
		{
			break;
		}
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged *)&event;
			s_sessionState = changed->state;
			LOGI("session state %d", (int)s_sessionState);
			if (s_sessionState == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XR_SUCCEEDED(xrBeginSession(s_session, &begin)))
				{
					s_sessionRunning = 1;
					s_recentreRequested = 1;
					CtrQuest_RequestRefreshRate();
				}
			}
			else if (s_sessionState == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(s_session);
				s_sessionRunning = 0;
			}
			else if (s_sessionState == XR_SESSION_STATE_EXITING || s_sessionState == XR_SESSION_STATE_LOSS_PENDING)
			{
				SDL_Event quit;
				s_sessionRunning = 0;
				memset(&quit, 0, sizeof(quit));
				quit.type = SDL_EVENT_QUIT;
				SDL_PushEvent(&quit);
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			s_recentreRequested = 1;
			break;
		default:
			break;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Input: Touch controllers -> virtual SDL gamepad                           */
/* ------------------------------------------------------------------------ */

static SDL_Joystick *s_pad;
static float s_rumble;
static float s_rumbleSent;

static bool SDLCALL CtrQuest_PadRumble(void *userdata, Uint16 low, Uint16 high)
{
	(void)userdata;
	s_rumble = (float)(low > high ? low : high) / 65535.0f;
	return true;
}

static void CtrQuest_AttachPad(void)
{
	SDL_VirtualJoystickDesc desc;
	SDL_JoystickID id;

	if (s_pad != NULL || !SDL_WasInit(SDL_INIT_JOYSTICK))
	{
		return;
	}
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.vendor_id = CTRQ_PAD_VENDOR;
	desc.product_id = CTRQ_PAD_PRODUCT;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.name = "Quest Touch";
	desc.Rumble = CtrQuest_PadRumble;
	id = SDL_AttachVirtualJoystick(&desc);
	if (id == 0)
	{
		LOGE("SDL_AttachVirtualJoystick: %s", SDL_GetError());
		return;
	}
	s_pad = SDL_OpenJoystick(id);
	LOGI("virtual gamepad %u attached (gamepad=%d)", (unsigned)id, SDL_IsGamepad(id) ? 1 : 0);
}

static float CtrQuest_GetFloat(XrAction action, int hand)
{
	XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
	XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};

	info.action = action;
	info.subactionPath = hand >= 0 ? s_handPath[hand] : XR_NULL_PATH;
	if (XR_FAILED(xrGetActionStateFloat(s_session, &info, &state)) || !state.isActive)
	{
		return 0.0f;
	}
	return state.currentState;
}

static int CtrQuest_GetBool(XrAction action, int hand)
{
	XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
	XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};

	info.action = action;
	info.subactionPath = hand >= 0 ? s_handPath[hand] : XR_NULL_PATH;
	if (XR_FAILED(xrGetActionStateBoolean(s_session, &info, &state)) || !state.isActive)
	{
		return 0;
	}
	return state.currentState ? 1 : 0;
}

/* Thumbstick from the vector action, with the x/y components as fallback
 * (the vector binding alone can read 0,0 on Quest). */
static XrVector2f CtrQuest_GetStick(int hand)
{
	XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
	XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
	XrVector2f result = {0.0f, 0.0f};

	info.action = s_stickAction;
	info.subactionPath = s_handPath[hand];
	if (XR_SUCCEEDED(xrGetActionStateVector2f(s_session, &info, &state)) && state.isActive)
	{
		result = state.currentState;
	}
	if (result.x == 0.0f && result.y == 0.0f)
	{
		result.x = CtrQuest_GetFloat(s_stickXAction, hand);
		result.y = CtrQuest_GetFloat(s_stickYAction, hand);
	}
	return result;
}

static Sint16 CtrQuest_AxisValue(float v)
{
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return (Sint16)(v * 32767.0f);
}

static void CtrQuest_SetButton(int button, int down)
{
	SDL_SetJoystickVirtualButton(s_pad, button, down != 0);
}

static void CtrQuest_ApplyHaptics(void)
{
	float amplitude = s_rumble;

	if (amplitude == s_rumbleSent)
	{
		return;
	}
	s_rumbleSent = amplitude;
	for (int hand = 0; hand < 2; hand++)
	{
		XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO};
		info.action = s_hapticAction;
		info.subactionPath = s_handPath[hand];
		if (amplitude > 0.0f)
		{
			XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
			vibration.amplitude = amplitude;
			vibration.duration = 300000000; /* refreshed while the game keeps rumbling */
			vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
			xrApplyHapticFeedback(s_session, &info, (const XrHapticBaseHeader *)&vibration);
		}
		else
		{
			xrStopHapticFeedback(s_session, &info);
		}
	}
}

static void CtrQuest_UpdateInput(void)
{
	static int dpad[4];
	int focused = (s_sessionState == XR_SESSION_STATE_FOCUSED);
	XrActiveActionSet active = {s_actionSet, XR_NULL_PATH};
	XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
	XrVector2f left = {0.0f, 0.0f};
	XrVector2f right = {0.0f, 0.0f};
	int leftClick = 0;
	int rightClick = 0;

	CtrQuest_AttachPad();
	if (focused)
	{
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		focused = XR_SUCCEEDED(xrSyncActions(s_session, &sync));
	}
	if (focused)
	{
		left = CtrQuest_GetStick(HAND_LEFT);
		right = CtrQuest_GetStick(HAND_RIGHT);
		leftClick = CtrQuest_GetBool(s_stickClickAction, HAND_LEFT);
		rightClick = CtrQuest_GetBool(s_stickClickAction, HAND_RIGHT);

		/* Hold both stick clicks for a second: put the screen in front again. */
		if (leftClick && rightClick)
		{
			double now = CtrQuest_Now();
			if (s_bothClicksSince == 0.0)
			{
				s_bothClicksSince = now;
			}
			else if (now - s_bothClicksSince > 1.0 && s_bothClicksSince > 0.0)
			{
				s_recentreRequested = 1;
				s_bothClicksSince = -1.0; /* once per hold */
			}
		}
		else
		{
			s_bothClicksSince = 0.0;
		}
	}

	if (s_pad == NULL)
	{
		return;
	}

	/* Right stick -> D-pad, with hysteresis. */
	{
		const float on = 0.55f;
		const float off = 0.35f;
		float values[4] = {right.y, -right.y, -right.x, right.x}; /* up, down, left, right */
		for (int i = 0; i < 4; i++)
		{
			dpad[i] = dpad[i] ? (values[i] > off) : (values[i] > on);
		}
	}

	SDL_LockJoysticks();
	SDL_SetJoystickVirtualAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX, CtrQuest_AxisValue(left.x));
	SDL_SetJoystickVirtualAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY, CtrQuest_AxisValue(-left.y));
	SDL_SetJoystickVirtualAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);
	SDL_SetJoystickVirtualAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY, 0);
	SDL_SetJoystickVirtualAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, CtrQuest_AxisValue(focused ? CtrQuest_GetFloat(s_triggerAction, HAND_LEFT) : 0.0f));
	SDL_SetJoystickVirtualAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, CtrQuest_AxisValue(focused ? CtrQuest_GetFloat(s_triggerAction, HAND_RIGHT) : 0.0f));
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_SOUTH, focused && CtrQuest_GetBool(s_aAction, -1));
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_EAST, focused && CtrQuest_GetBool(s_bAction, -1));
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_WEST, focused && CtrQuest_GetBool(s_xAction, -1));
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_NORTH, focused && CtrQuest_GetBool(s_yAction, -1));
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, focused && CtrQuest_GetFloat(s_gripAction, HAND_LEFT) > 0.5f);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, focused && CtrQuest_GetFloat(s_gripAction, HAND_RIGHT) > 0.5f);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_START, focused && CtrQuest_GetBool(s_menuAction, -1));
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_BACK, leftClick && !rightClick);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_RIGHT_STICK, rightClick && !leftClick);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_DPAD_UP, focused && dpad[0]);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN, focused && dpad[1]);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT, focused && dpad[2]);
	CtrQuest_SetButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, focused && dpad[3]);
	SDL_UnlockJoysticks();

	if (focused)
	{
		CtrQuest_ApplyHaptics();
	}
}

/* ------------------------------------------------------------------------ */
/* Frame                                                                     */
/* ------------------------------------------------------------------------ */

static void CtrQuest_UpdateAnchor(XrTime time)
{
	XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
	const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

	if (!s_recentreRequested && s_anchorValid)
	{
		return;
	}
	if (XR_FAILED(xrLocateSpace(s_viewSpace, s_localSpace, time, &location)) || (location.locationFlags & needed) != needed)
	{
		return; /* stays pending until tracking is valid */
	}
	s_anchorPos = location.pose.position;
	s_anchorYaw = CtrQuest_YawOf(location.pose.orientation);
	s_anchorValid = 1;
	s_recentreRequested = 0;
	LOGI("screen centred: head (%.2f %.2f %.2f) yaw %.1f deg", s_anchorPos.x, s_anchorPos.y, s_anchorPos.z, s_anchorYaw * 57.2958f);
}

/* Copy the game's screen into the swapchain image, keeping the GL state the
 * game's renderer caches. */
static void CtrQuest_CopyScreen(uint32_t index)
{
	GLint program, vao, activeTexture, texture, sampler, drawFbo, readFbo, viewport[4];
	GLboolean scissor, blend, depth, stencil, cull, colorMask[4];

	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
	scissor = glIsEnabled(GL_SCISSOR_TEST);
	blend = glIsEnabled(GL_BLEND);
	depth = glIsEnabled(GL_DEPTH_TEST);
	stencil = glIsEnabled(GL_STENCIL_TEST);
	cull = glIsEnabled(GL_CULL_FACE);

	glBindFramebuffer(GL_FRAMEBUFFER, s_swapchainFbos[index]);
	glViewport(0, 0, s_cfg.screenPixelsW, s_cfg.screenPixelsH);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(s_copyProgram);
	glUniform1f(s_copyDecodeLoc, (s_swapchainSrgb && !s_srgbWriteControl) ? 1.0f : 0.0f);
	glUniform1f(s_copyFlipLoc, s_cfg.flipY ? 1.0f : 0.0f);
	glBindTexture(GL_TEXTURE_2D, s_screenTexture);
	glBindSampler(0, 0);
	glBindVertexArray(s_copyVao);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glBindVertexArray((GLuint)vao);
	glBindSampler(0, (GLuint)sampler);
	glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
	glActiveTexture((GLenum)activeTexture);
	glUseProgram((GLuint)program);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFbo);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	if (blend) glEnable(GL_BLEND);
	if (depth) glEnable(GL_DEPTH_TEST);
	if (stencil) glEnable(GL_STENCIL_TEST);
	if (cull) glEnable(GL_CULL_FACE);
}

static void CtrQuest_RunFrame(void)
{
	XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frameState = {XR_TYPE_FRAME_STATE};
	XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
	XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
	XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	const XrCompositionLayerBaseHeader *layers[1];
	uint32_t layerCount = 0;
	static int frameCount;

	CtrQuest_HandleEvents();
	if (!s_sessionRunning)
	{
		return;
	}
	if (XR_FAILED(xrWaitFrame(s_session, &waitInfo, &frameState)) || XR_FAILED(xrBeginFrame(s_session, &beginInfo)))
	{
		return;
	}
	CtrQuest_UpdateAnchor(frameState.predictedDisplayTime);
	CtrQuest_UpdateInput();

	if (frameState.shouldRender && s_anchorValid)
	{
		XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		uint32_t index = 0;

		wait.timeout = XR_INFINITE_DURATION;
		if (XR_SUCCEEDED(xrAcquireSwapchainImage(s_swapchain, &acquire, &index)) &&
		    XR_SUCCEEDED(xrWaitSwapchainImage(s_swapchain, &wait)))
		{
			const float sinYaw = sinf(s_anchorYaw);
			const float cosYaw = cosf(s_anchorYaw);
			CtrQuest_CopyScreen(index);
			xrReleaseSwapchainImage(s_swapchain, &release);

			quad.space = s_localSpace;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = s_swapchain;
			quad.subImage.imageRect.extent.width = s_cfg.screenPixelsW;
			quad.subImage.imageRect.extent.height = s_cfg.screenPixelsH;
			quad.pose.orientation = CtrQuest_QuatYaw(s_anchorYaw);
			/* forward of yaw = (-sin, 0, -cos) */
			quad.pose.position.x = s_anchorPos.x - sinYaw * s_cfg.screenDistance;
			quad.pose.position.y = s_anchorPos.y + s_cfg.screenHeight;
			quad.pose.position.z = s_anchorPos.z - cosYaw * s_cfg.screenDistance;
			quad.size.width = s_cfg.screenWidth;
			quad.size.height = s_cfg.screenWidth * (float)s_cfg.screenPixelsH / (float)s_cfg.screenPixelsW;
			layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&quad;
		}
	}

	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	xrEndFrame(s_session, &endInfo);

	if ((++frameCount % 600) == 1)
	{
		LOGI("xr frame %d state %d layers %u", frameCount, (int)s_sessionState, layerCount);
	}
}

bool CtrQuest_SwapWindow(SDL_Window *window)
{
	(void)window;
	if (!s_xrTried)
	{
		s_xrTried = 1;
		s_xrOk = CtrQuest_InitXr();
		if (!s_xrOk)
		{
			LOGE("OpenXR init failed; nothing will show in the headset");
		}
	}
	if (s_xrOk)
	{
		CtrQuest_RunFrame();
	}
	return true;
}

#endif /* __ANDROID__ */
