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

#include "demos/gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/null.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

/**
 * Object handling functions.
 */

static Gl* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Gl*)update_self_stack(&top, s, GL);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Gl* bind_gl(GenericEl *gl) {
  return self(gl);
}

static void set_error(Gl *gl, const char *msg) {
  if (gl->last_err != NULL) {
    release(gl->last_err);
    gl->last_err = NULL;
  }
  if (msg != NULL && msg[0] != '\0') {
    gl->last_err = create(String, (char*)msg)->$$;
    gl->last_err->bind((GenericEl*)-1);
  }
}

static void release_program_ref(Gl *gl) {
  if (gl->owns_program && gl->program != 0) {
    gl->procs.delete_program(gl->program);
  }
  gl->program = 0;
  gl->owns_program = 0;
  if (gl->program_owner != NULL) {
    release(gl->program_owner);
    gl->program_owner = NULL;
  }
}

static void destroy_gl_objects(Gl *gl) {
  if (!gl->loaded) {
    return;
  }
  if (gl->instance_vbo != 0) {
    gl->procs.delete_buffers(1, &gl->instance_vbo);
    gl->instance_vbo = 0;
  }
  if (gl->vbo != 0) {
    gl->procs.delete_buffers(1, &gl->vbo);
    gl->vbo = 0;
  }
  if (gl->vao != 0) {
    gl->procs.delete_vertex_arrays(1, &gl->vao);
    gl->vao = 0;
  }
  release_program_ref(gl);
  gl->vertex_count = 0;
  gl->stride_floats = 0;
  gl->instance_count = 0;
  gl->instance_stride_floats = 0;
}

static void del(void *obj) {
  Gl *gl = (Gl*)obj;
  if (gl != NULL && self((GenericEl*)gl) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      destroy_gl_objects(slf);
      if (slf->last_err != NULL) {
        release(slf->last_err);
        slf->last_err = NULL;
      }
      Gl *to_free = slf;
      self((GenericEl*)-1);
      free(to_free);
    } else {
      self((GenericEl*)-1);
    }
  }
}

/**
 * OpenGL 3.3 entry-point loader and shader compilation helpers.
 */

static ChGlProc load_proc(Gl *gl, ChGlLoader loader, const char *name) {
  ChGlProc fn = loader(name);
  if (fn == NULL && gl->last_err == NULL) {
    char buf[160];
    snprintf(buf, sizeof(buf), "Missing OpenGL 3.3 symbol: %s", name);
    set_error(gl, buf);
  }
  return fn;
}

static int load_gl_procs(Gl *gl, ChGlLoader loader) {
  if (loader == NULL) {
    set_error(gl, "OpenGL proc loader callback is NULL");
    return 0;
  }
  gl->procs.create_shader =
      (PFNGLCREATESHADERPROC)load_proc(gl, loader, "glCreateShader");
  gl->procs.shader_source =
      (PFNGLSHADERSOURCEPROC)load_proc(gl, loader, "glShaderSource");
  gl->procs.compile_shader =
      (PFNGLCOMPILESHADERPROC)load_proc(gl, loader, "glCompileShader");
  gl->procs.get_shader_iv =
      (PFNGLGETSHADERIVPROC)load_proc(gl, loader, "glGetShaderiv");
  gl->procs.get_shader_info_log =
      (PFNGLGETSHADERINFOLOGPROC)load_proc(gl, loader, "glGetShaderInfoLog");
  gl->procs.delete_shader =
      (PFNGLDELETESHADERPROC)load_proc(gl, loader, "glDeleteShader");
  gl->procs.create_program =
      (PFNGLCREATEPROGRAMPROC)load_proc(gl, loader, "glCreateProgram");
  gl->procs.attach_shader =
      (PFNGLATTACHSHADERPROC)load_proc(gl, loader, "glAttachShader");
  gl->procs.link_program =
      (PFNGLLINKPROGRAMPROC)load_proc(gl, loader, "glLinkProgram");
  gl->procs.get_program_iv =
      (PFNGLGETPROGRAMIVPROC)load_proc(gl, loader, "glGetProgramiv");
  gl->procs.get_program_info_log =
      (PFNGLGETPROGRAMINFOLOGPROC)load_proc(gl, loader, "glGetProgramInfoLog");
  gl->procs.delete_program =
      (PFNGLDELETEPROGRAMPROC)load_proc(gl, loader, "glDeleteProgram");
  gl->procs.use_program =
      (PFNGLUSEPROGRAMPROC)load_proc(gl, loader, "glUseProgram");
  gl->procs.get_uniform_location =
      (PFNGLGETUNIFORMLOCATIONPROC)load_proc(gl, loader,
                                             "glGetUniformLocation");
  gl->procs.uniform_1f =
      (PFNGLUNIFORM1FPROC)load_proc(gl, loader, "glUniform1f");
  gl->procs.uniform_2f =
      (PFNGLUNIFORM2FPROC)load_proc(gl, loader, "glUniform2f");
  gl->procs.uniform_3f =
      (PFNGLUNIFORM3FPROC)load_proc(gl, loader, "glUniform3f");
  gl->procs.uniform_4f =
      (PFNGLUNIFORM4FPROC)load_proc(gl, loader, "glUniform4f");
  gl->procs.uniform_1i =
      (PFNGLUNIFORM1IPROC)load_proc(gl, loader, "glUniform1i");
  gl->procs.uniform_matrix_4fv =
      (PFNGLUNIFORMMATRIX4FVPROC)load_proc(gl, loader, "glUniformMatrix4fv");
  gl->procs.gen_vertex_arrays =
      (PFNGLGENVERTEXARRAYSPROC)load_proc(gl, loader, "glGenVertexArrays");
  gl->procs.bind_vertex_array =
      (PFNGLBINDVERTEXARRAYPROC)load_proc(gl, loader, "glBindVertexArray");
  gl->procs.delete_vertex_arrays =
      (PFNGLDELETEVERTEXARRAYSPROC)load_proc(gl, loader,
                                             "glDeleteVertexArrays");
  gl->procs.gen_buffers =
      (PFNGLGENBUFFERSPROC)load_proc(gl, loader, "glGenBuffers");
  gl->procs.bind_buffer =
      (PFNGLBINDBUFFERPROC)load_proc(gl, loader, "glBindBuffer");
  gl->procs.buffer_data =
      (PFNGLBUFFERDATAPROC)load_proc(gl, loader, "glBufferData");
  gl->procs.delete_buffers =
      (PFNGLDELETEBUFFERSPROC)load_proc(gl, loader, "glDeleteBuffers");
  gl->procs.vertex_attrib_pointer =
      (PFNGLVERTEXATTRIBPOINTERPROC)load_proc(gl, loader,
                                              "glVertexAttribPointer");
  gl->procs.enable_vertex_attrib_array =
      (PFNGLENABLEVERTEXATTRIBARRAYPROC)load_proc(gl, loader,
                                                  "glEnableVertexAttribArray");
  gl->procs.vertex_attrib_divisor =
      (PFNGLVERTEXATTRIBDIVISORPROC)load_proc(gl, loader,
                                              "glVertexAttribDivisor");
  gl->procs.draw_arrays_instanced =
      (PFNGLDRAWARRAYSINSTANCEDPROC)load_proc(gl, loader,
                                              "glDrawArraysInstanced");

  return gl->last_err == NULL;
}

static GLuint compile_shader_stage(Gl *gl, GLenum type, const char *label,
                                   const char *src) {
  GLuint shader = gl->procs.create_shader(type);
  GLint ok = 0;
  gl->procs.shader_source(shader, 1, &src, NULL);
  gl->procs.compile_shader(shader);
  gl->procs.get_shader_iv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    char msg[600];
    log[0] = '\0';
    gl->procs.get_shader_info_log(shader, sizeof(log), NULL, log);
    snprintf(msg, sizeof(msg), "%s shader compilation failed: %s", label, log);
    set_error(gl, msg);
    gl->procs.delete_shader(shader);
    return 0;
  }
  return shader;
}

/**
 * Fluent OpenGL 3.3 methods.
 */

static Gl* build_shader(const char *vert_src, const char *frag_src) {
  Gl *gl = slf;
  if (!gl->loaded) {
    return $(gl);
  }
  if (vert_src == NULL || frag_src == NULL) {
    set_error(gl, "Shader source cannot be NULL");
    return $(gl);
  }

  set_error(gl, NULL);
  GLuint vs = compile_shader_stage(gl, GL_VERTEX_SHADER, "Vertex", vert_src);
  if (vs == 0) {
    return $(gl);
  }
  GLuint fs = compile_shader_stage(gl, GL_FRAGMENT_SHADER, "Fragment",
                                   frag_src);
  if (fs == 0) {
    gl->procs.delete_shader(vs);
    return $(gl);
  }

  GLuint prog = gl->procs.create_program();
  gl->procs.attach_shader(prog, vs);
  gl->procs.attach_shader(prog, fs);
  gl->procs.link_program(prog);
  gl->procs.delete_shader(vs);
  gl->procs.delete_shader(fs);

  GLint linked = 0;
  gl->procs.get_program_iv(prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[512];
    char msg[600];
    log[0] = '\0';
    gl->procs.get_program_info_log(prog, sizeof(log), NULL, log);
    snprintf(msg, sizeof(msg), "Shader program link failed: %s", log);
    set_error(gl, msg);
    gl->procs.delete_program(prog);
    return $(gl);
  }

  release_program_ref(gl);
  gl->program = prog;
  gl->owns_program = 1;
  return $(gl);
}

static Gl* share_program(Gl *src) {
  Gl *gl = slf;
  if (src == gl) {
    return $(gl);
  }
  if (src == NULL || !src->loaded || src->program == 0) {
    if (src != NULL && src->generic.refs == 0) {
      release(src);
    }
    set_error(gl, "Source Gl has no compiled shader program to share");
    return $(gl);
  }

  Gl *owner = src->program_owner != NULL ? src->program_owner : src;
  GLuint prog = owner->program;
  claim_element(owner, gl);
  if (src != owner && src->generic.refs == 0) {
    release(src);
  }
  release_program_ref(gl);
  gl->program = prog;
  gl->owns_program = 0;
  gl->program_owner = owner;
  return $(gl);
}

static Gl* upload_mesh(const GLfloat *data, int float_count,
                       int stride_floats) {
  Gl *gl = slf;
  if (!gl->loaded) {
    return $(gl);
  }
  if (float_count < 0 || stride_floats <= 0 ||
      (float_count > 0 && data == NULL)) {
    set_error(gl, "Invalid mesh vertex data or stride");
    return $(gl);
  }

  if (gl->vao == 0) {
    gl->procs.gen_vertex_arrays(1, &gl->vao);
  }
  if (gl->vbo == 0) {
    gl->procs.gen_buffers(1, &gl->vbo);
  }
  gl->stride_floats = stride_floats;
  gl->vertex_count = float_count / stride_floats;

  gl->procs.bind_vertex_array(gl->vao);
  gl->procs.bind_buffer(GL_ARRAY_BUFFER, gl->vbo);
  if (float_count > 0) {
    gl->procs.buffer_data(GL_ARRAY_BUFFER,
                          (GLsizeiptr)((size_t)float_count * sizeof(GLfloat)),
                          data, GL_STATIC_DRAW);
  }
  gl->procs.bind_vertex_array(0);
  return $(gl);
}

static Gl* upload_mesh_buf(TypedF32Array *verts, int stride_floats) {
  Gl *gl = slf;
  if (verts == NULL) {
    set_error(gl, "Vertex TypedF32Array cannot be NULL");
    return $(gl);
  }
  upload_mesh(verts->data, verts->length, stride_floats);
  if (verts->generic.refs == 0) {
    release(verts);
  }
  return $(gl);
}

static Gl* set_attr(GLuint location, GLint size, int offset_floats) {
  Gl *gl = slf;
  if (!gl->loaded || gl->vao == 0 || gl->vbo == 0 || gl->stride_floats <= 0) {
    return $(gl);
  }
  gl->procs.bind_vertex_array(gl->vao);
  gl->procs.bind_buffer(GL_ARRAY_BUFFER, gl->vbo);
  gl->procs.vertex_attrib_pointer(
      location, size, GL_FLOAT, GL_FALSE,
      (GLsizei)(gl->stride_floats * (int)sizeof(GLfloat)),
      (const void*)((size_t)offset_floats * sizeof(GLfloat)));
  gl->procs.enable_vertex_attrib_array(location);
  gl->procs.bind_vertex_array(0);
  return $(gl);
}

static Gl* upload_instance_data(const GLfloat *data, int float_count,
                                int stride_floats) {
  Gl *gl = slf;
  if (!gl->loaded) {
    return $(gl);
  }
  if (float_count < 0 || stride_floats <= 0 ||
      (float_count > 0 && data == NULL)) {
    set_error(gl, "Invalid instance data or stride");
    return $(gl);
  }

  if (gl->instance_vbo == 0) {
    gl->procs.gen_buffers(1, &gl->instance_vbo);
  }
  gl->instance_stride_floats = stride_floats;
  gl->instance_count = float_count / stride_floats;

  gl->procs.bind_buffer(GL_ARRAY_BUFFER, gl->instance_vbo);
  gl->procs.buffer_data(
      GL_ARRAY_BUFFER,
      (GLsizeiptr)((size_t)float_count * sizeof(GLfloat)),
      float_count > 0 ? data : NULL, GL_DYNAMIC_DRAW);
  gl->procs.bind_buffer(GL_ARRAY_BUFFER, 0);
  return $(gl);
}

static Gl* upload_instance_buf(TypedF32Array *inst, int stride_floats) {
  Gl *gl = slf;
  if (inst == NULL) {
    set_error(gl, "Instance TypedF32Array cannot be NULL");
    return $(gl);
  }
  upload_instance_data(inst->data, inst->length, stride_floats);
  if (inst->generic.refs == 0) {
    release(inst);
  }
  return $(gl);
}

static Gl* set_instance_attr(GLuint location, GLint size, int offset_floats,
                             GLuint divisor) {
  Gl *gl = slf;
  if (!gl->loaded || gl->vao == 0 || gl->instance_stride_floats <= 0) {
    return $(gl);
  }
  if (gl->instance_vbo == 0) {
    gl->procs.gen_buffers(1, &gl->instance_vbo);
  }
  gl->procs.bind_vertex_array(gl->vao);
  gl->procs.bind_buffer(GL_ARRAY_BUFFER, gl->instance_vbo);
  gl->procs.vertex_attrib_pointer(
      location, size, GL_FLOAT, GL_FALSE,
      (GLsizei)(gl->instance_stride_floats * (int)sizeof(GLfloat)),
      (const void*)((size_t)offset_floats * sizeof(GLfloat)));
  gl->procs.enable_vertex_attrib_array(location);
  gl->procs.vertex_attrib_divisor(location, divisor);
  gl->procs.bind_vertex_array(0);
  gl->procs.bind_buffer(GL_ARRAY_BUFFER, 0);
  return $(gl);
}

static Gl* use_program_obj(void) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0) {
    gl->procs.use_program(gl->program);
  }
  return $(gl);
}

static Gl* set_uniform_1f(const char *name, float v0) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0 && name != NULL) {
    gl->procs.use_program(gl->program);
    GLint loc = gl->procs.get_uniform_location(gl->program, name);
    if (loc >= 0) {
      gl->procs.uniform_1f(loc, v0);
    }
  }
  return $(gl);
}

static Gl* set_uniform_2f(const char *name, float v0, float v1) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0 && name != NULL) {
    gl->procs.use_program(gl->program);
    GLint loc = gl->procs.get_uniform_location(gl->program, name);
    if (loc >= 0) {
      gl->procs.uniform_2f(loc, v0, v1);
    }
  }
  return $(gl);
}

static Gl* set_uniform_3f(const char *name, float v0, float v1, float v2) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0 && name != NULL) {
    gl->procs.use_program(gl->program);
    GLint loc = gl->procs.get_uniform_location(gl->program, name);
    if (loc >= 0) {
      gl->procs.uniform_3f(loc, v0, v1, v2);
    }
  }
  return $(gl);
}

static Gl* set_uniform_4f(const char *name, float v0, float v1, float v2,
                          float v3) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0 && name != NULL) {
    gl->procs.use_program(gl->program);
    GLint loc = gl->procs.get_uniform_location(gl->program, name);
    if (loc >= 0) {
      gl->procs.uniform_4f(loc, v0, v1, v2, v3);
    }
  }
  return $(gl);
}

static Gl* set_uniform_1i(const char *name, int v0) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0 && name != NULL) {
    gl->procs.use_program(gl->program);
    GLint loc = gl->procs.get_uniform_location(gl->program, name);
    if (loc >= 0) {
      gl->procs.uniform_1i(loc, v0);
    }
  }
  return $(gl);
}

static Gl* set_uniform_mat4(const char *name, const GLfloat *mat4) {
  Gl *gl = slf;
  if (gl->loaded && gl->program != 0 && name != NULL && mat4 != NULL) {
    gl->procs.use_program(gl->program);
    GLint loc = gl->procs.get_uniform_location(gl->program, name);
    if (loc >= 0) {
      gl->procs.uniform_matrix_4fv(loc, 1, GL_FALSE, mat4);
    }
  }
  return $(gl);
}

static Gl* set_depth_test(int enabled) {
  if (enabled) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
  } else {
    glDisable(GL_DEPTH_TEST);
  }
  return slf;
}

static Gl* set_cull_face(int enabled) {
  if (enabled) {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
  } else {
    glDisable(GL_CULL_FACE);
  }
  return slf;
}

static Gl* set_viewport(int x, int y, int width, int height) {
  glViewport(x, y, width, height);
  return slf;
}

static Gl* clear_gl_buffers(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  return slf;
}

static Gl* draw_arrays(GLenum mode, GLint first, GLsizei count) {
  Gl *gl = slf;
  if (!gl->loaded || gl->program == 0 || gl->vao == 0) {
    return $(gl);
  }
  gl->procs.use_program(gl->program);
  gl->procs.bind_vertex_array(gl->vao);
  glDrawArrays(mode, first, count);
  gl->procs.bind_vertex_array(0);
  return $(gl);
}

static Gl* draw_arrays_instanced(GLenum mode, GLint first, GLsizei count,
                                 GLsizei instance_count) {
  Gl *gl = slf;
  if (!gl->loaded || gl->program == 0 || gl->vao == 0 || instance_count <= 0) {
    return $(gl);
  }
  gl->procs.use_program(gl->program);
  gl->procs.bind_vertex_array(gl->vao);
  gl->procs.draw_arrays_instanced(mode, first, count, instance_count);
  gl->procs.bind_vertex_array(0);
  return $(gl);
}

static Gl* destroy_gl(void) {
  Gl *gl = slf;
  destroy_gl_objects(gl);
  return $(gl);
}

static Hash* gl_info(void) {
  Gl *gl = slf;
  const char *gl_ver = (const char*)glGetString(GL_VERSION);
  const char *glsl_ver = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
  return create(Hash, 8)
      ->put("loaded", create(Boolean, gl->loaded))
      ->put("program", create(Number, (double)gl->program))
      ->put("vao", create(Number, (double)gl->vao))
      ->put("vbo", create(Number, (double)gl->vbo))
      ->put("vertex_count", create(Number, (double)gl->vertex_count))
      ->put("stride_floats", create(Number, (double)gl->stride_floats))
      ->put("gl_version",
            gl_ver != NULL ? (GenericEl*)create(String, (char*)gl_ver)
                           : (GenericEl*)create(Null))
      ->put("glsl_version",
            glsl_ver != NULL ? (GenericEl*)create(String, (char*)glsl_ver)
                             : (GenericEl*)create(Null))
      ->put("error",
            gl->last_err != NULL
                ? (GenericEl*)create(String, gl->last_err->chars)
                : (GenericEl*)create(Null));
}

static StatusOr* gl_status(void) {
  Gl *gl = slf;
  if (gl->last_err != NULL) {
    return create(StatusOr, NULL, gl->last_err->chars);
  }
  return create(StatusOr, $(gl)->info(), NULL);
}

static char* to_chars(void) {
  Gl *gl = slf;
  if (gl->last_err != NULL) {
    return gl->last_err->chars;
  }
  snprintf(gl->repr, sizeof(gl->repr),
           "<Gl prog=%u vao=%u vbo=%u verts=%d>",
           gl->program, gl->vao, gl->vbo, gl->vertex_count);
  return gl->repr;
}

/**
 * Constructor.
 */

Gl* ch_new_Gl(ChGlLoader loader) {
  Gl *gl = malloc(sizeof(Gl));
  gl->generic.magic = CH_MAGIC;
  gl->generic.type_val = GL;
  gl->generic.prev_self = NULL;
  gl->generic.next_self = NULL;
  gl->generic.refs = 0;

  memset(&gl->procs, 0, sizeof(gl->procs));
  gl->program = 0;
  gl->owns_program = 0;
  gl->program_owner = NULL;
  gl->vao = 0;
  gl->vbo = 0;
  gl->instance_vbo = 0;
  gl->stride_floats = 0;
  gl->vertex_count = 0;
  gl->instance_stride_floats = 0;
  gl->instance_count = 0;
  gl->last_err = NULL;
  gl->repr[0] = '\0';
  gl->loaded = load_gl_procs(gl, loader);

  gl->bind = bind_gl;
  gl->generic.bind = (void* (*)(GenericEl*))bind_gl;
  gl->generic.save = save;
  gl->generic.del = del;
  gl->generic.to_chars = to_chars;

  gl->shader = build_shader;
  gl->share_program = share_program;
  gl->mesh = upload_mesh;
  gl->mesh_buf = upload_mesh_buf;
  gl->attr = set_attr;
  gl->instance_data = upload_instance_data;
  gl->instance_buf = upload_instance_buf;
  gl->instance_attr = set_instance_attr;
  gl->use = use_program_obj;
  gl->uniform_1f = set_uniform_1f;
  gl->uniform_2f = set_uniform_2f;
  gl->uniform_3f = set_uniform_3f;
  gl->uniform_4f = set_uniform_4f;
  gl->uniform_1i = set_uniform_1i;
  gl->uniform_mat4 = set_uniform_mat4;
  gl->depth_test = set_depth_test;
  gl->cull_face = set_cull_face;
  gl->viewport = set_viewport;
  gl->clear = clear_gl_buffers;
  gl->draw = draw_arrays;
  gl->draw_instanced = draw_arrays_instanced;
  gl->destroy = destroy_gl;
  gl->info = gl_info;
  gl->status = gl_status;

  self((GenericEl*)gl);

  return gl;
}
