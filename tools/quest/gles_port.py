#!/usr/bin/env python3
"""GLES 3 (Android/Quest + desktop GLES test) edits for the CTR renderer.

Idempotent: each edit is skipped when its replacement is already present.
Run from anywhere: python3 tools/quest/gles_port.py
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
failed = []


def edit(path, old, new, count=1):
    p = os.path.join(ROOT, path)
    s = open(p, encoding="utf-8").read()
    if new in s and old not in s:
        return
    n = s.count(old)
    if n != count:
        failed.append(f"{path}: expected {count} match(es), found {n}: {old[:70]!r}")
        return
    s = s.replace(old, new)
    open(p, "w", encoding="utf-8").write(s)
    print(f"edited {path}")


# --- one switch for every GLES target --------------------------------------
edit("include/macros.h",
     "#if defined(CTR_NATIVE) && defined(__vita__)\n#define CTR_NATIVE_HAS_ADHOC       1",
     "// GLES 3 renderer: WebGL 2, Android/Quest, and the desktop GLES test build\n"
     "// (CMake CTR_NATIVE_GLES, defines CTR_NATIVE_FORCE_GLES).\n"
     "#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CTR_NATIVE_FORCE_GLES)\n"
     "#define CTR_NATIVE_GLES 1\n#else\n#define CTR_NATIVE_GLES 0\n#endif\n\n"
     "#if defined(CTR_NATIVE) && defined(__vita__)\n#define CTR_NATIVE_HAS_ADHOC       1")

GLES_INCLUDE = ("#if defined(__EMSCRIPTEN__)\n#include <GLES3/gl3.h>\n"
                "#elif CTR_NATIVE_GLES\n#include <GLES3/gl32.h>\n#include <GLES2/gl2ext.h>\n#else\n")
edit("include/platform/native_renderer_internal.h",
     "#if defined(__EMSCRIPTEN__)\n#include <GLES3/gl3.h>\n#else\n", GLES_INCLUDE)
edit("platform/native_platform.c",
     "#if defined(__EMSCRIPTEN__)\n#include <GLES3/gl3.h>\n#include <emscripten.h>\n#else\n",
     "#if defined(__EMSCRIPTEN__)\n#include <GLES3/gl3.h>\n#include <emscripten.h>\n"
     "#elif CTR_NATIVE_GLES\n#include <GLES3/gl32.h>\n#else\n")
edit("main.c", '#if !defined(__EMSCRIPTEN__)\n#include "platform/native_glad.c"',
     '#if !CTR_NATIVE_GLES\n#include "platform/native_glad.c"')

# --- context + loader -------------------------------------------------------
edit("platform/renderer/native_renderer_device.c",
     "#ifdef __EMSCRIPTEN__\n\tint minor_version = 0;\n\tint profile = SDL_GL_CONTEXT_PROFILE_ES;",
     "#ifdef __EMSCRIPTEN__\n\tint minor_version = 0;\n\tint profile = SDL_GL_CONTEXT_PROFILE_ES;\n"
     "#elif CTR_NATIVE_GLES\n\tint minor_version = 2;\n\tint profile = SDL_GL_CONTEXT_PROFILE_ES;")
edit("platform/renderer/native_renderer_device.c",
     "#if !defined(__vita__) && !defined(__EMSCRIPTEN__)\n\tGLenum err = gladLoadGL();",
     "#if !defined(__vita__) && !CTR_NATIVE_GLES\n\tGLenum err = gladLoadGL();")
edit("platform/renderer/native_renderer_submit.c",
     "#if !defined(__vita__) && !defined(__EMSCRIPTEN__)\n\tif (!GLAD_GL_KHR_debug)",
     "#if !defined(__vita__) && !CTR_NATIVE_GLES\n\tif (!GLAD_GL_KHR_debug)", count=2)

# --- shaders ------------------------------------------------------------------
edit("platform/renderer/native_renderer_shaders.c",
     '#ifdef __EMSCRIPTEN__\n#define GLSL_NOPERSPECTIVE_DEFINE "\t#define PSX_NOPERSPECTIVE\\n"',
     '#if CTR_NATIVE_GLES\n#define GLSL_NOPERSPECTIVE_DEFINE "\t#define PSX_NOPERSPECTIVE\\n"')
# Adreno honours lowp int (VRAM coordinates reach 1023), so GLES targets other
# than WebGL get highp ints; samplers without a default precision get one too.
OLD_HDR = ('#ifdef __EMSCRIPTEN__\n\t    "\t#version 300 es\\n"\n#else\n\t    "\t#version 140\\n"\n#endif\n'
           '\t    "\tprecision lowp  int;\\n"\n\t    "\tprecision highp float;\\n"\n')
NEW_HDR = ('#ifdef __EMSCRIPTEN__\n\t    "\t#version 300 es\\n"\n\t    "\tprecision lowp  int;\\n"\n'
           '#elif CTR_NATIVE_GLES\n\t    "\t#version 320 es\\n"\n\t    "\tprecision highp int;\\n"\n'
           '\t    "\tprecision highp usampler2D;\\n"\n\t    "\tprecision highp isampler2D;\\n"\n'
           '\t    "\tprecision highp sampler2D;\\n"\n'
           '#else\n\t    "\t#version 140\\n"\n\t    "\tprecision lowp  int;\\n"\n#endif\n'
           '\t    "\tprecision highp float;\\n"\n')
edit("platform/renderer/native_renderer_shaders.c", OLD_HDR, NEW_HDR, count=2)
edit("platform/renderer/native_renderer_shaders.c",
     '#ifdef __EMSCRIPTEN__\n\t    "\\t#define gl_FragColor fragColor\\n"',
     '#if CTR_NATIVE_GLES\n\t    "\\t#define gl_FragColor fragColor\\n"')

# --- desktop-only GL calls -----------------------------------------------------
edit("platform/renderer/native_renderer_gpu_state.c",
     "#ifdef __EMSCRIPTEN__\n\t(void)enable;\n#else\n\tglPolygonMode(",
     "#if CTR_NATIVE_GLES\n\t(void)enable;\n#else\n\tglPolygonMode(")
edit("platform/renderer/native_renderer_submit.c",
     "\tglMultiDrawArrays(GL_TRIANGLES, (const GLint *)firstVertex, (const GLsizei *)vertexCount, draws);\n",
     "#if CTR_NATIVE_GLES\n\tfor (int i = 0; i < draws; i++)\n\t{\n"
     "\t\tif (vertexCount[i] > 0)\n\t\t{\n\t\t\tglDrawArrays(GL_TRIANGLES, firstVertex[i], vertexCount[i]);\n\t\t}\n\t}\n"
     "#else\n\tglMultiDrawArrays(GL_TRIANGLES, (const GLint *)firstVertex, (const GLsizei *)vertexCount, draws);\n#endif\n")
edit("platform/renderer/native_renderer_passes.c",
     "\t\tglClearDepth(1.0);\n",
     "#if CTR_NATIVE_GLES\n\t\tglClearDepthf(1.0f);\n#else\n\t\tglClearDepth(1.0);\n#endif\n")
# --- PC-only features: first-run disc dialog and update check ---------------
edit("platform/native_disc_setup.c",
     "#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__) && !defined(__vita__)",
     "#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__) && !defined(__vita__) && !defined(__ANDROID__)")
edit("include/platform/native_options.h",
     "#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__) && !defined(__vita__)\n#define NATIVE_UPDATE_CHECK_SUPPORTED 1",
     "#if (defined(_WIN32) || defined(__linux__)) && !defined(__EMSCRIPTEN__) && !defined(__vita__) && !defined(__ANDROID__)\n#define NATIVE_UPDATE_CHECK_SUPPORTED 1")

if failed:
    print("\n".join(failed))
    sys.exit(1)
print("ok")
