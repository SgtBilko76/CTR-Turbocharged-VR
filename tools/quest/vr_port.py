#!/usr/bin/env python3
"""Quest stage 3: hook the stereo renderer (platform/renderer/native_renderer_vr.c)
into the native renderer. Idempotent: re-running leaves patched files alone.

Every hook sits behind `#if defined(CTR_QUEST_VR) && CTR_QUEST_VR`, which only
the Quest builds define (include/platform/native_vr.h, pulled in by the
force-included ctr_quest_hooks.h), so desktop/web builds are unchanged.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GUARD = "#if defined(CTR_QUEST_VR) && CTR_QUEST_VR"


def edit(path, old, new, marker):
    full = os.path.join(ROOT, path)
    with open(full) as f:
        text = f.read()
    if marker in text:
        print("already  ", path, "-", marker)
        return
    if text.count(old) != 1:
        sys.exit("anchor not found once in %s: %r" % (path, old[:80]))
    with open(full, "w") as f:
        f.write(text.replace(old, new))
    print("patched  ", path, "-", marker)


# 1. unity build: the VR renderer after the other renderer files
edit("main.c",
     '#include "platform/renderer/native_renderer_device.c"\n',
     '#include "platform/renderer/native_renderer_device.c"\n'
     '#include "platform/renderer/native_renderer_vr.c" // Quest stereo (CTR_QUEST_VR)\n',
     "native_renderer_vr.c")

# 2. GTE vertex shader: eye re-projection of camera-space vertices
edit("platform/renderer/native_renderer_shaders.c",
     '#define GTE_ORDER_DEPTH_ATTRIBUTE "\\tuniform mat4 ObjectToCamera;\\n\\tuniform vec4 NativeView;\\n"\n',
     GUARD + '\n'
     '// Quest stereo: NativeVrBlock (platform/renderer/native_renderer_vr.c). In an\n'
     '// eye pass, native vertices are rebuilt in game camera space (directly, or\n'
     '// from screen*w through NativeView) and projected through the eye pose and\n'
     '// field of view instead of the game camera.\n'
     '#define GTE_VR_UNIFORMS "\\tlayout(std140) uniform NativeVrBlock { mat4 vrEyeFromCamera; vec4 vrProj; vec4 vrMono; vec4 vrParams; };\\n"\n'
     '#define GTE_VR_OVERRIDE                                                                                                   \\\n'
     '\t"\\tif (vrParams.x > 0.5) {\\n"                                                                                     \\\n'
     '\t"\\t\\tvec3 vrCam = vec3(0.0); float vrBias = 0.0; bool vrOk = false;\\n"                                         \\\n'
     '\t"\\t\\tif (a_extra.z > 1.5) {\\n"                                                                                  \\\n'
     '\t"\\t\\t\\tvrCam = (ObjectToCamera * vec4(a_position.xyz, 1.0)).xyz; vrBias = a_position.w; vrOk = true;\\n"     \\\n'
     '\t"\\t\\t} else if (a_extra.z > 0.5 && a_position.w > 0.0 && abs(NativeView.x) > 0.0 && abs(NativeView.y) > 0.0) {\\n" \\\n'
     '\t"\\t\\t\\tvrCam = vec3((a_position.x - NativeView.z * a_position.w) / NativeView.x,\\n"                            \\\n'
     '\t"\\t\\t\\t             (a_position.y - NativeView.w * a_position.w) / NativeView.y, a_position.w);\\n"            \\\n'
     '\t"\\t\\t\\tvrBias = a_position.z - a_position.w; vrOk = true;\\n"                                                  \\\n'
     '\t"\\t\\t}\\n"                                                                                                        \\\n'
     '\t"\\t\\tif (vrOk) {\\n"                                                                                              \\\n'
     '\t"\\t\\t\\tvec3 e = (vrEyeFromCamera * vec4(vrCam, 1.0)).xyz;\\n"                                                  \\\n'
     '\t"\\t\\t\\tgl_Position = vec4(vrProj.x * e.x + vrProj.y * e.z, vrProj.z * e.y + vrProj.w * e.z, e.z + vrBias, e.z);\\n" \\\n'
     '\t"\\t\\t}\\n"                                                                                                        \\\n'
     '\t"\\t}\\n"\n'
     '#else\n'
     '#define GTE_VR_UNIFORMS ""\n'
     '#define GTE_VR_OVERRIDE ""\n'
     '#endif\n'
     '#define GTE_ORDER_DEPTH_ATTRIBUTE "\\tuniform mat4 ObjectToCamera;\\n\\tuniform vec4 NativeView;\\n" GTE_VR_UNIFORMS\n',
     "GTE_VR_UNIFORMS")

edit("platform/renderer/native_renderer_shaders.c",
     '\t"\\tv_clipSpace = (a_extra.z > 1.5) ? 2.0 + a_extra.w : a_extra.z;\\n"\n',
     '\t"\\tv_clipSpace = (a_extra.z > 1.5) ? 2.0 + a_extra.w : a_extra.z;\\n" GTE_VR_OVERRIDE\n',
     'a_extra.z;\\n" GTE_VR_OVERRIDE')

edit("platform/renderer/native_renderer_shaders.c",
     '''	GLint textureSampler = 0;
	GLint presentLutSampler = 2;
	glUseProgram(program);''',
     GUARD + '''
	{
		const GLuint vrBlock = glGetUniformBlockIndex(program, "NativeVrBlock");
		if (vrBlock != GL_INVALID_INDEX)
		{
			glUniformBlockBinding(program, vrBlock, CTR_VR_UBO_BINDING);
		}
	}
#endif
	GLint textureSampler = 0;
	GLint presentLutSampler = 2;
	glUseProgram(program);''',
     'glUniformBlockBinding(program, vrBlock')

# 3. GPU: replay each batch per eye/HUD pass, filter splits per pass
edit("platform/native_gpu.c",
     'internal void NativeGpu_DrawSplitRangePass(const GPUDrawSplit *split, int semiTransPass, BlendMode blendMode, bool depthWrite, int startVertex, int numVerts)\n{\n',
     GUARD + '\n'
     'internal int CtrVr_SplitInPass(const GPUDrawSplit *split);\n'
     'internal int CtrVr_DrawBatch(void (*drawSplits)(void));\n'
     '#endif\n\n'
     'internal void NativeGpu_DrawSplitRangePass(const GPUDrawSplit *split, int semiTransPass, BlendMode blendMode, bool depthWrite, int startVertex, int numVerts)\n{\n'
     + GUARD + '\n'
     '\t// Stereo: each pass draws only its share (eyes: world, HUD: the rest).\n'
     '\tif (!CtrVr_SplitInPass(split))\n'
     '\t{\n'
     '\t\treturn;\n'
     '\t}\n'
     '#endif\n',
     "if (!CtrVr_SplitInPass(split))")

edit("platform/native_gpu.c",
     '''//
// Draws all polygons after AggregatePTAG
//
internal void NativeGpu_DrawPreparedFrame(''',
     '''#ifndef __vita__
internal void NativeGpu_DrawSplitsForFrame(void)
{
	if (NATIVE_DEPTH_BUFFER_ACTIVE())
	{
		NativeGpu_DrawWorldDepthSplits();
	}
	else
		for (int i = 1; i <= s_gpuDrawSplitCount; i++)
		{
			DrawSplit(&s_gpuDrawSplits[i]);
		}
}
#endif

//
// Draws all polygons after AggregatePTAG
//
internal void NativeGpu_DrawPreparedFrame(''',
     "internal void NativeGpu_DrawSplitsForFrame(void)")

edit("platform/native_gpu.c",
     '''#else
	if (NATIVE_DEPTH_BUFFER_ACTIVE())
	{
		NativeGpu_DrawWorldDepthSplits();
	}
	else
		for (int i = 1; i <= s_gpuDrawSplitCount; i++)
		{
			DrawSplit(&s_gpuDrawSplits[i]);
		}
#endif


	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);''',
     '''#else
''' + GUARD + '''
	CtrVr_DrawBatch(NativeGpu_DrawSplitsForFrame);
#else
	NativeGpu_DrawSplitsForFrame();
#endif
#endif


	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);''',
     "CtrVr_DrawBatch(NativeGpu_DrawSplitsForFrame)")

# 4. targets: inside a pass the swapped-in target is bound as is
edit("platform/renderer/native_renderer_targets.c",
     '''internal void NativeRenderer_BindMainRenderTarget(void)
{
''',
     GUARD + '''
internal int CtrVr_InPass(void);
#endif

internal void NativeRenderer_BindMainRenderTarget(void)
{
''' + GUARD + '''
	// Stereo pass: the eye/HUD target is swapped in; don't resize it.
	if (CtrVr_InPass())
	{
		glBindFramebuffer(GL_FRAMEBUFFER, NativeRenderer_GetDrawFramebuffer(&s_mainRenderTarget));
		return;
	}
#endif
''',
     "if (CtrVr_InPass())")

# 5. aspect: wide culling FOV while stereo is on
edit("platform/native_aspect.c",
     '''double NativeAspect_GetFocalScale(void)
{
''',
     GUARD + '''
internal int CtrVr_WideCullActive(void);
internal double CtrVr_CullFocalScale(void);
#endif

double NativeAspect_GetFocalScale(void)
{
''' + GUARD + '''
	// Stereo: the eyes reproject camera-space geometry, so the game's own
	// frustum only decides culling. Make it wide enough for head turns.
	if (NativeAspect_IsActive() && CtrVr_WideCullActive())
	{
		return CtrVr_CullFocalScale();
	}
#endif
''',
     "CtrVr_WideCullActive()")

# 6. vram: background colour for the eye targets
edit("platform/renderer/native_renderer_vram.c",
     '''void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
''',
     GUARD + '''
internal void CtrVr_RecordClear(u8 r, u8 g, u8 b);
#endif

void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
''' + GUARD + '''
	CtrVr_RecordClear(r, g, b);
#endif
''',
     "CtrVr_RecordClear(r, g, b);\n#endif")

# 7. device: per-frame stereo decision
edit("platform/renderer/native_renderer_device.c",
     '''		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE);''',
     '''		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
''' + GUARD + '''
	CtrVr_OnBeginScene();
#endif
	NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE);''',
     "CtrVr_OnBeginScene();")

edit("platform/renderer/native_renderer_device.c",
     '''void NativeRenderer_BeginScene(void)
{
''',
     GUARD + '''
internal void CtrVr_OnBeginScene(void);
#endif

void NativeRenderer_BeginScene(void)
{
''',
     "internal void CtrVr_OnBeginScene(void);")

# 8. hooks header pulls in CTR_QUEST_VR
edit("include/platform/ctr_quest_hooks.h",
     '''#if defined(__ANDROID__) || defined(CTR_QUEST_DESKTOP_TEST)

#define glBindFramebuffer''',
     '''#if defined(__ANDROID__) || defined(CTR_QUEST_DESKTOP_TEST)

#include "platform/native_vr.h"

#define glBindFramebuffer''',
     '#include "platform/native_vr.h"')
