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

#include <stddef.h>

#include "core/boolean.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/test.h"
#include "core/types.h"
#include "demos/glfw.h"

static const char *TEST_VERT =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec2 u_offset;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * vec4(a_pos + u_offset, 0.0, 1.0);\n"
    "  v_color = a_color;\n"
    "}\n";

static const char *TEST_FRAG =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "uniform float u_alpha;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  frag_color = vec4(v_color, u_alpha);\n"
    "}\n";

static void test_gl_pipeline_and_errors(Test *t) {
  /* 1. NULL loader returns a safe error StatusOr without crashing. */
  Gl *unloaded = create(Gl, NULL)->$$;
  $(t)->section("Gl loader validation & shader compilation errors")
      ->expect_err($(unloaded)->status(), "loader callback is NULL")
      ->expect_err($(unloaded)->shader(TEST_VERT, TEST_FRAG)->status(),
                   "loader callback is NULL");
  release(unloaded);

  /* 2. Open headless OpenGL 3.3 Core context via Glfw. */
  Glfw *win = create(Glfw, 64, 64, "Gl Unit Test Context")
      ->gl_version(3, 3, GLFW_OPENGL_CORE_PROFILE)
      ->visible(0)
      ->vsync(0)
      ->open()
      ->$$;

  $(t)->expect_ok($(win)->status());

  if (win->handle != NULL) {
    /* 6 vertices forming a fullscreen quad `(x, y, r, g, b)`. */
    static const GLfloat quad[] = {
        -1.0f, -1.0f,  0.0f, 1.0f, 0.5f,
         1.0f, -1.0f,  0.0f, 1.0f, 0.5f,
         1.0f,  1.0f,  0.0f, 1.0f, 0.5f,
        -1.0f, -1.0f,  0.0f, 1.0f, 0.5f,
         1.0f,  1.0f,  0.0f, 1.0f, 0.5f,
        -1.0f,  1.0f,  0.0f, 1.0f, 0.5f,
    };

    Gl *gl = create(Gl, (GlLoader)glfwGetProcAddress)
        ->shader(TEST_VERT, TEST_FRAG)
        ->mesh(quad, 30, 5)
        ->attr(0, 2, 0)
        ->attr(1, 3, 2)
        ->$$;

    $(t)->section("Gl shader, mesh, uniforms, clear & draw")
        ->expect_ok($(gl)->status())
        ->expect_num($(gl)->info()->get("vertex_count"), 6)
        ->expect_bool($(gl)->info()->get("loaded"), 1);

    static const GLfloat identity_mat4[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    $(gl)->viewport(0, 0, 64, 64)
         ->depth_test(1)
         ->cull_face(1)
         ->clear(0.0f, 0.0f, 0.0f, 1.0f)
         ->uniform_mat4("u_mvp", identity_mat4)
         ->uniform_2f("u_offset", 0.0f, 0.0f)
         ->uniform_1f("u_alpha", 1.0f)
         ->draw(GL_TRIANGLES, 0, 6);
    $(win)->swap();

    /* Quad color is `(0.0, 1.0, 0.5)` -> `[0, 255, 128, 255]` (+/- rounding). */
    TypedIntArray *px = $(win)->read_pixel(32, 32)->$$;
    $(t)->expect_num(create(Number, (double)$(px)->at(0)), 0)
        ->expect_num(create(Number, (double)$(px)->at(1)), 255)
        ->expect_bool(
            create(Boolean, $(px)->at(2) >= 127 && $(px)->at(2) <= 128), 1);
    release(px);

    /* Test share_program + mesh_buf(TypedF32Array*) and program lifetime after source release. */
    Gl *shared_mesh = create(Gl, (GlLoader)glfwGetProcAddress)
        ->share_program(gl)
        ->mesh_buf(
            create(TypedF32Array, 30)->from_buf(quad, 30),
            5)
        ->attr(0, 2, 0)
        ->attr(1, 3, 2)
        ->$$;

    $(t)->expect_ok($(shared_mesh)->status())
        ->expect_num($(shared_mesh)->info()->get("program"), (double)gl->program)
        ->expect_num($(shared_mesh)->info()->get("vertex_count"), 6);

    release(gl);

    $(shared_mesh)->clear(0.0f, 0.0f, 0.0f, 1.0f)
                  ->uniform_mat4("u_mvp", identity_mat4)
                  ->uniform_2f("u_offset", 0.0f, 0.0f)
                  ->uniform_1f("u_alpha", 1.0f)
                  ->draw(GL_TRIANGLES, 0, 6);
    $(win)->swap();

    TypedIntArray *shared_px = $(win)->read_pixel(32, 32)->$$;
    $(t)->expect_num(create(Number, (double)$(shared_px)->at(1)), 255);
    release(shared_px);

    /* Invalid shader surfaces compiler error in StatusOr while keeping VBO/VAO cleanup intact. */
    $(shared_mesh)->shader("not valid glsl", TEST_FRAG);
    $(t)->expect_err($(shared_mesh)->status(), "shader compilation failed");

    release(shared_mesh);

    /* 3. Instanced rendering via instance_data / instance_buf / instance_attr / draw_instanced. */
    static const char *INST_VERT =
        "#version 330 core\n"
        "layout(location = 0) in vec2 a_pos;\n"
        "layout(location = 1) in vec2 a_inst_offset;\n"
        "layout(location = 2) in vec3 a_inst_color;\n"
        "out vec3 v_color;\n"
        "void main() {\n"
        "  gl_Position = vec4(a_pos + a_inst_offset, 0.0, 1.0);\n"
        "  v_color = a_inst_color;\n"
        "}\n";
    static const char *INST_FRAG =
        "#version 330 core\n"
        "in vec3 v_color;\n"
        "out vec4 frag_color;\n"
        "void main() { frag_color = vec4(v_color, 1.0); }\n";
    static const GLfloat unit_quad[12] = {
        -0.4f, -0.4f,  0.4f, -0.4f,  0.4f,  0.4f,
        -0.4f, -0.4f,  0.4f,  0.4f, -0.4f,  0.4f,
    };
    /* Two instances: left red `(-0.5, 0)`, right blue `(+0.5, 0)`. */
    TypedF32Array *inst = create(TypedF32Array, 10)
        ->push_n(10,
                 -0.5f, 0.0f, 1.0f, 0.0f, 0.0f,
                  0.5f, 0.0f, 0.0f, 0.0f, 1.0f)
        ->$$;

    Gl *inst_gl = create(Gl, (GlLoader)glfwGetProcAddress)
        ->shader(INST_VERT, INST_FRAG)
        ->mesh(unit_quad, 12, 2)
        ->attr(0, 2, 0)
        ->instance_data(NULL, 0, 5)
        ->instance_attr(1, 2, 0, 1)
        ->instance_attr(2, 3, 2, 1)
        ->instance_buf(inst, 5)
        ->clear(0.0f, 0.0f, 0.0f, 1.0f)
        ->draw_instanced(GL_TRIANGLES, 0, 6, inst->length / 5)
        ->$$;
    $(win)->swap();

    TypedIntArray *left_px = $(win)->read_pixel(16, 32)->$$;
    TypedIntArray *right_px = $(win)->read_pixel(48, 32)->$$;
    $(t)->section("Gl OpenGL 3.3 instanced rendering")
        ->expect_ok($(inst_gl)->status())
        ->expect_num(create(Number, (double)inst_gl->instance_count), 2)
        ->expect_num(create(Number, (double)$(left_px)->at(0)), 255)
        ->expect_num(create(Number, (double)$(left_px)->at(2)), 0)
        ->expect_num(create(Number, (double)$(right_px)->at(0)), 0)
        ->expect_num(create(Number, (double)$(right_px)->at(2)), 255);

    release(right_px);
    release(left_px);
    release(inst_gl);
    release(inst);
  }

  release(win);
}

int main(void) {
  Test *t = create(Test, "OpenGL 3.3 Gl Wrapper Unit Tests")->$$;

  test_gl_pipeline_and_errors(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
