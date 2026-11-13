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

/**
 * Interactive OpenGL 3.3 Core Profile + GLFW example using fluent ISO C
 * chaining (`Glfw` + independent `Gl` class with VAO, VBO, and GLSL 330).
 *
 * Controls (interactive window):
 *   Space        Pause / resume triangle rotation
 *   Mouse scroll Adjust rotation speed
 *   R / G / B    Switch background clear tint
 *   D            Restore default dark slate background
 *   Esc          Close window cleanly
 *
 * Build & run with CMake (requires GLFW3 + OpenGL development headers):
 *   cmake -B build && cmake --build build
 *   ./build/chain_glfw
 *   ./build/chain_glfw --test
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/test.h"
#include "core/types.h"
#include "demos/gl.h"
#include "demos/glfw.h"

static const char *VERTEX_SHADER_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec3 a_color;\n"
    "uniform float u_angle;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    "  float c = cos(u_angle);\n"
    "  float s = sin(u_angle);\n"
    "  vec2 rotated = vec2(a_pos.x * c - a_pos.y * s,\n"
    "                      a_pos.x * s + a_pos.y * c);\n"
    "  gl_Position = vec4(rotated, 0.0, 1.0);\n"
    "  v_color = a_color;\n"
    "}\n";

static const char *FRAGMENT_SHADER_SRC =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  frag_color = vec4(v_color, 1.0);\n"
    "}\n";

static void on_key(Glfw *win, Hash *ev) {
  int key = (int)as(Number, $(ev)->get("key"))->value();
  int pressed = as(Boolean, $(ev)->get("pressed"))->value();
  if (!pressed) {
    return;
  }

  Hash *app = as(Hash, win->state);
  if (key == GLFW_KEY_ESCAPE) {
    $(win)->close();
  } else if (key == GLFW_KEY_SPACE) {
    int paused = as(Boolean, $(app)->get("paused"))->value();
    $(app)->put("paused", create(Boolean, !paused));
  } else if (key == GLFW_KEY_R) {
    $(win)->clear_color(0.24f, 0.06f, 0.08f, 1.0f);
  } else if (key == GLFW_KEY_G) {
    $(win)->clear_color(0.06f, 0.22f, 0.10f, 1.0f);
  } else if (key == GLFW_KEY_B) {
    $(win)->clear_color(0.06f, 0.10f, 0.28f, 1.0f);
  } else if (key == GLFW_KEY_D) {
    $(win)->clear_color(0.08f, 0.10f, 0.14f, 1.0f);
  }
}

static void on_scroll(Glfw *win, Hash *ev) {
  double dy = as(Number, $(ev)->get("yoffset"))->value();
  Hash *app = as(Hash, win->state);
  double speed = as(Number, $(app)->get("speed"))->value() + dy * 15.0;
  if (speed < -360.0) {
    speed = -360.0;
  } else if (speed > 360.0) {
    speed = 360.0;
  }
  $(app)->put("speed", create(Number, speed));
}

static void on_render(Glfw *win, Hash *frame) {
  Hash *app = as(Hash, win->state);
  Gl *gl = as(Gl, $(app)->get("gl"));
  double dt = as(Number, $(frame)->get("dt"))->value();
  int paused = as(Boolean, $(app)->get("paused"))->value();
  double angle = as(Number, $(app)->get("angle"))->value();
  double speed = as(Number, $(app)->get("speed"))->value();

  if (!paused) {
    angle = fmod(angle + speed * (dt > 0.0 ? dt : 0.016), 360.0);
    $(app)->put("angle", create(Number, angle));
  }

  float rad = (float)(angle * 3.14159265358979323846 / 180.0);
  $(gl)->uniform_1f("u_angle", rad)
       ->draw(GL_TRIANGLES, 0, 3);
}

static Glfw* create_app(int width, int height, char *title, int visible) {
  static const GLfloat vertices[] = {
       0.00f,  0.62f,  1.0f, 0.2f, 0.3f,
      -0.58f, -0.45f,  0.2f, 1.0f, 0.4f,
       0.58f, -0.45f,  0.2f, 0.4f, 1.0f,
  };

  Glfw *win = create(Glfw, width, height, title)
      ->gl_version(3, 3, GLFW_OPENGL_CORE_PROFILE)
#ifdef __APPLE__
      ->hint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE)
#endif
      ->resizable(1)
      ->visible(visible)
      ->vsync(visible ? 1 : 0)
      ->clear_color(0.08f, 0.10f, 0.14f, 1.0f)
      ->open()
      ->$$;

  if (win->handle != NULL) {
    Gl *gl = create(Gl, (GlLoader)glfwGetProcAddress)
        ->shader(VERTEX_SHADER_SRC, FRAGMENT_SHADER_SRC)
        ->mesh(vertices, 15, 5)
        ->attr(0, 2, 0)
        ->attr(1, 3, 2);

    $(win)->user_data(create(Hash, 4)
        ->put("angle", create(Number, 0.0))
        ->put("speed", create(Number, 45.0))
        ->put("paused", create(Boolean, 0))
        ->put("gl", gl))
        ->on_key(on_key)
        ->on_scroll(on_scroll)
        ->on_render(on_render);
  }

  return win;
}

static int run_tests(void) {
  Test *t = create(Test, "chain_glfw OpenGL 3.3 Core Tests")->$$;

  Glfw *win = create_app(320, 240, "chain_glfw --test", 0);
  StatusOr *win_st = $(win)->status()->$$;
  $(t)->section("Glfw + Gl OpenGL 3.3 Core lifecycle, shaders & controls")
      ->expect_ok(win_st);

  if ($(win_st)->ok()) {
    Hash *app = as(Hash, win->state);
    Gl *gl = as(Gl, $(app)->get("gl"));
    $(t)->expect_ok($(gl)->status());

    $(win)->max_frames(3)->run();

    TypedIntArray *corner = $(win)->read_pixel(2, 2)->$$;
    TypedIntArray *center = $(win)->read_pixel(160, 120)->$$;

    $(t)->expect_num(create(Number, (double)win->frame_count), 3)
        ->expect_bool(create(Boolean, !$(win)->should_close()), 1)
        ->expect_bool(
            create(Boolean, as(Number, $(app)->get("angle"))->value() > 0.0),
            1)
        ->expect_bool(
            create(Boolean, $(corner)->at(0) < 40 && $(center)->at(1) > 80),
            1);

    release(center);
    release(corner);

    /* Simulate Space pause + scroll + Esc close through fluent events. */
    $(win)->dispatch_event("key", create(Hash, 2)
        ->put("key", create(Number, GLFW_KEY_SPACE))
        ->put("pressed", create(Boolean, 1)))
        ->dispatch_event("scroll", create(Hash, 2)
        ->put("xoffset", create(Number, 0.0))
        ->put("yoffset", create(Number, 2.0)));

    $(t)->expect_bool($(app)->get("paused"), 1)
        ->expect_num($(app)->get("speed"), 75.0);

    $(win)->dispatch_event("key", create(Hash, 2)
        ->put("key", create(Number, GLFW_KEY_ESCAPE))
        ->put("pressed", create(Boolean, 1)));
    $(t)->expect_bool(create(Boolean, $(win)->should_close()), 1);
  }

  release(win_st);
  release(win);

  int rc = $(t)->summary();
  release(t);
  return rc;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--test") == 0) {
    return run_tests();
  }

  Glfw *win = create_app(800, 600, "ISO C Chaining + OpenGL 3.3", 1);
  StatusOr *st = $(win)->status()->$$;
  if (!$(st)->ok()) {
    fprintf(stderr, "Failed to open GLFW OpenGL 3.3 window: %s\n",
            $(st)->error());
    release(st);
    release(win);
    return 1;
  }

  Gl *gl = as(Gl, $(as(Hash, win->state))->get("gl"));
  StatusOr *gl_st = $(gl)->status()->$$;
  if (!$(gl_st)->ok()) {
    fprintf(stderr, "Failed to initialize OpenGL 3.3 pipeline: %s\n",
            $(gl_st)->error());
    release(gl_st);
    release(st);
    release(win);
    return 1;
  }

  $(win)->run();

  StatusOr *final_st = $(win)->status()->$$;
  if ($(final_st)->ok()) {
    puts($(final_st)->value()->to_chars());
  }
  release(final_st);
  release(gl_st);
  release(st);
  release(win);
  return 0;
}
