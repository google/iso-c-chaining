/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CHAIN_GL_H_
#define CHAIN_GL_H_

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "core/hash.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generic OpenGL function pointer and proc-address loader signature (matches
 * `glfwGetProcAddress`, `SDL_GL_GetProcAddress`, `eglGetProcAddress`, etc.).
 */
typedef void (*ChGlProc)(void);
typedef ChGlProc (*ChGlLoader)(const char *name);

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChGlProc GlProc;
typedef ChGlLoader GlLoader;
#endif

ch_use(Gl, GL, (ChGlLoader))

/*
 * Loaded OpenGL 3.3 Core Profile entry points stored per `Gl` instance so
 * multiple loaders or contexts remain independent without global state.
 */
typedef struct {
  PFNGLCREATESHADERPROC create_shader;
  PFNGLSHADERSOURCEPROC shader_source;
  PFNGLCOMPILESHADERPROC compile_shader;
  PFNGLGETSHADERIVPROC get_shader_iv;
  PFNGLGETSHADERINFOLOGPROC get_shader_info_log;
  PFNGLDELETESHADERPROC delete_shader;
  PFNGLCREATEPROGRAMPROC create_program;
  PFNGLATTACHSHADERPROC attach_shader;
  PFNGLLINKPROGRAMPROC link_program;
  PFNGLGETPROGRAMIVPROC get_program_iv;
  PFNGLGETPROGRAMINFOLOGPROC get_program_info_log;
  PFNGLDELETEPROGRAMPROC delete_program;
  PFNGLUSEPROGRAMPROC use_program;
  PFNGLGETUNIFORMLOCATIONPROC get_uniform_location;
  PFNGLUNIFORM1FPROC uniform_1f;
  PFNGLUNIFORM2FPROC uniform_2f;
  PFNGLUNIFORM3FPROC uniform_3f;
  PFNGLUNIFORM4FPROC uniform_4f;
  PFNGLUNIFORM1IPROC uniform_1i;
  PFNGLUNIFORMMATRIX4FVPROC uniform_matrix_4fv;
  PFNGLGENVERTEXARRAYSPROC gen_vertex_arrays;
  PFNGLBINDVERTEXARRAYPROC bind_vertex_array;
  PFNGLDELETEVERTEXARRAYSPROC delete_vertex_arrays;
  PFNGLGENBUFFERSPROC gen_buffers;
  PFNGLBINDBUFFERPROC bind_buffer;
  PFNGLBUFFERDATAPROC buffer_data;
  PFNGLDELETEBUFFERSPROC delete_buffers;
  PFNGLVERTEXATTRIBPOINTERPROC vertex_attrib_pointer;
  PFNGLENABLEVERTEXATTRIBARRAYPROC enable_vertex_attrib_array;
  PFNGLVERTEXATTRIBDIVISORPROC vertex_attrib_divisor;
  PFNGLDRAWARRAYSINSTANCEDPROC draw_arrays_instanced;
} ChGlProcs;

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChGlProcs GlProcs;
#endif

/*
 * Fluent OpenGL 3.3 Core Profile shader, VAO/VBO mesh, uniform, and draw-call
 * wrapper backed by `ChGenericEl`. Requires an active OpenGL context and a
 * `ChGlLoader` callback (such as `(ChGlLoader)glfwGetProcAddress`).
 */
struct GL {
  ChGenericEl generic;
  ChGlProcs procs;
  int loaded;
  GLuint program;
  int owns_program;
  ChGl *program_owner;
  GLuint vao;
  GLuint vbo;
  GLuint instance_vbo;
  int stride_floats;
  int vertex_count;
  int instance_stride_floats;
  int instance_count;
  ChString *last_err;
  char repr[128];

  /* Binds this `Gl` as the active instance on the `Gl` `self` stack. */
  ChGl* (*bind)(ChGenericEl*);

  /*
   * Compiles GLSL vertex and fragment shader sources and links them into
   * `gl->program` (replacing any prior program). Compile/link diagnostics are
   * stored in `last_err` and surfaced via `status()` / `chars()`.
   */
  ChGl* (*shader)(const char*, const char*);

  /*
   * Borrows the compiled shader `program` from `src` (retaining a reference
   * to `src`) so multiple mesh `Gl` instances can share a single shader without
   * recompiling GLSL.
   */
  ChGl* (*share_program)(ChGl*);

  /*
   * Creates or replaces `gl->vao` and `gl->vbo`, uploading `float_count`
   * interleaved `GLfloat`s with vertex stride `stride_floats`.
   */
  ChGl* (*mesh)(const GLfloat*, int, int);

  /*
   * Uploads vertex data from a `TypedF32Array*` (`verts->data`,
   * `verts->length`) with vertex stride `stride_floats`, releasing unowned
   * temporary arrays (`refs == 0`) automatically.
   */
  ChGl* (*mesh_buf)(ChTypedF32Array*, int);

  /*
   * Defines a float vertex attribute `(location, components, offset_floats)`
   * on the active `vao`/`vbo` using `gl->stride_floats`.
   */
  ChGl* (*attr)(GLuint, GLint, int);

  /*
   * Creates or streams `gl->instance_vbo` with `float_count` per-instance
   * `GLfloat`s (`GL_DYNAMIC_DRAW`) at stride `stride_floats`. Passing
   * `(NULL, 0, stride)` pre-creates the VBO for `instance_attr()`.
   */
  ChGl* (*instance_data)(const GLfloat*, int, int);

  /*
   * Streams per-instance data from a `TypedF32Array*` (`inst->data`,
   * `inst->length`), releasing unowned temporary arrays (`refs == 0`).
   */
  ChGl* (*instance_buf)(ChTypedF32Array*, int);

  /*
   * Defines a per-instance float vertex attribute `(location, components,
   * offset_floats, divisor)` backed by `gl->instance_vbo`.
   */
  ChGl* (*instance_attr)(GLuint, GLint, int, GLuint);

  /* Binds `gl->program` via `glUseProgram`. */
  ChGl* (*use)(void);

  /* Sets a scalar float uniform on `gl->program` (binds program first). */
  ChGl* (*uniform_1f)(const char*, float);

  /* Sets a `vec2` float uniform on `gl->program`. */
  ChGl* (*uniform_2f)(const char*, float, float);

  /* Sets a `vec3` float uniform on `gl->program`. */
  ChGl* (*uniform_3f)(const char*, float, float, float);

  /* Sets a `vec4` float uniform on `gl->program`. */
  ChGl* (*uniform_4f)(const char*, float, float, float, float);

  /* Sets an integer uniform on `gl->program`. */
  ChGl* (*uniform_1i)(const char*, int);

  /* Sets a column-major 4x4 float matrix uniform (`mat4`) on `gl->program`. */
  ChGl* (*uniform_mat4)(const char*, const GLfloat*);

  /* Enables or disables depth testing (`GL_DEPTH_TEST`). */
  ChGl* (*depth_test)(int);

  /* Enables or disables back-face culling (`GL_CULL_FACE`, CCW front). */
  ChGl* (*cull_face)(int);

  /* Sets `glViewport(x, y, width, height)` on the current GL context. */
  ChGl* (*viewport)(int, int, int, int);

  /* Sets `glClearColor(r, g, b, a)` and clears color + depth buffers. */
  ChGl* (*clear)(float, float, float, float);

  /* Binds `program` and `vao`, calls `glDrawArrays(mode, first, count)`. */
  ChGl* (*draw)(GLenum, GLint, GLsizei);

  /*
   * Binds `program` and `vao`, calls `glDrawArraysInstanced(mode, first,
   * count, instance_count)` (no-op when `instance_count <= 0`).
   */
  ChGl* (*draw_instanced)(GLenum, GLint, GLsizei, GLsizei);

  /*
   * Deletes owned `vbo`, `vao`, and `program` handles while the OpenGL context
   * is still current (`del()` also calls this automatically).
   */
  ChGl* (*destroy)(void);

  /*
   * Returns a fluent `Hash*` snapshot of `loaded`, `program`, `vao`, `vbo`,
   * `vertex_count`, `stride_floats`, `gl_version`, `glsl_version`, and `error`.
   */
  ChHash* (*info)(void);

  /*
   * Returns a `StatusOr*` wrapping `info()` when healthy, or `last_err` on
   * loader, shader compilation, or program link failure.
   */
  ChStatusOr* (*status)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_GL_H_ */
