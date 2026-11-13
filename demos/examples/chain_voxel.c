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
 * Continuous 3x3-chunk Minecraft-style 3D voxel sandbox demonstrating how
 * `Glfw`, `Gl` (with `share_program()` + `mesh_buf(TypedF32Array*)`),
 * `Slice::foreach_ctx()`, and a custom `VoxelWorld` chaining class fit together.
 *
 * Controls:
 *   W / A / S / D  Walk along the surface (auto-steps 1-block slopes)
 *   Mouse          Look around (captured via `cursor_mode(GLFW_CURSOR_DISABLED)`)
 *   Space          Jump (or fly up in Fly mode)
 *   Left Shift     Fly down (in Fly mode)
 *   F              Toggle surface walking vs. Fly mode
 *   Left Click     Break targeted voxel block (raycast)
 *   Right Click    Place selected voxel block on hit face
 *   Scroll / 1..6  Cycle or pick block type (Grass, Dirt, Stone, Sand, Wood,
 *                  Leaves)
 *   Tab            Toggle mouse cursor capture
 *   Esc            Quit
 *
 * Build & run with CMake (requires GLFW3 + OpenGL development headers):
 *   cmake -B build && cmake --build build
 *   ./build/chain_voxel
 *   ./build/chain_voxel --test
 */

#include <stdio.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/test.h"
#include "core/typed_array.h"
#include "core/types.h"
#include "demos/gl.h"
#include "demos/glfw.h"
#include "demos/voxel.h"

static void on_cursor(Glfw *win, Hash *ev) {
  VoxelWorld *world = as(VoxelWorld, win->state);
  if (world->captured) {
    $(world)->look(as(Number, $(ev)->get("dx"))->value(),
                   as(Number, $(ev)->get("dy"))->value());
  }
}

static void on_mouse_button(Glfw *win, Hash *ev) {
  if (!as(Boolean, $(ev)->get("pressed"))->value()) {
    return;
  }
  VoxelWorld *world = as(VoxelWorld, win->state);
  int btn = (int)as(Number, $(ev)->get("button"))->value();
  if (!world->captured) {
    $(world)->set_captured(1);
    $(win)->cursor_mode(GLFW_CURSOR_DISABLED);
    return;
  }
  if (btn == GLFW_MOUSE_BUTTON_LEFT) {
    $(world)->raycast_break();
  } else if (btn == GLFW_MOUSE_BUTTON_RIGHT) {
    $(world)->raycast_place();
  }
}

static void on_scroll(Glfw *win, Hash *ev) {
  double dy = as(Number, $(ev)->get("yoffset"))->value();
  if (dy != 0.0) {
    $(as(VoxelWorld, win->state))->cycle_block(dy > 0.0 ? 1 : -1);
  }
}

static void on_key(Glfw *win, Hash *ev) {
  if (!as(Boolean, $(ev)->get("pressed"))->value()) {
    return;
  }
  VoxelWorld *world = as(VoxelWorld, win->state);
  int key = (int)as(Number, $(ev)->get("key"))->value();
  if (key == GLFW_KEY_ESCAPE) {
    $(win)->close();
  } else if (key == GLFW_KEY_TAB) {
    $(world)->set_captured(!world->captured);
    $(win)->cursor_mode(world->captured ? GLFW_CURSOR_DISABLED
                                        : GLFW_CURSOR_NORMAL);
  } else if (key == GLFW_KEY_F) {
    $(world)->toggle_fly();
  } else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_6) {
    $(world)->select_block(key - GLFW_KEY_0);
  }
}

static void on_render(Glfw *win, Hash *frame) {
  float fwd = (float)($(win)->key_down(GLFW_KEY_W) -
                      $(win)->key_down(GLFW_KEY_S));
  float right = (float)($(win)->key_down(GLFW_KEY_D) -
                        $(win)->key_down(GLFW_KEY_A));
  float up = (float)($(win)->key_down(GLFW_KEY_SPACE) -
                     $(win)->key_down(GLFW_KEY_LEFT_SHIFT));

  $(as(VoxelWorld, win->state))
      ->move(fwd, right, up, $(win)->key_down(GLFW_KEY_SPACE),
             (float)as(Number, $(frame)->get("dt"))->value())
      ->render((float)as(Number, $(frame)->get("width"))->value(),
               (float)as(Number, $(frame)->get("height"))->value());
}

static Glfw* create_voxel_app(int width, int height, char *title, int visible) {
  Glfw *win = create(Glfw, width, height, title)
      ->gl_version(3, 3, GLFW_OPENGL_CORE_PROFILE)
#ifdef __APPLE__
      ->hint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE)
#endif
      ->resizable(1)
      ->visible(visible)
      ->cursor_mode(GLFW_CURSOR_DISABLED)
      ->vsync(visible ? 1 : 0)
      ->clear_color(VOXEL_SKY_R, VOXEL_SKY_G, VOXEL_SKY_B, 1.0f)
      ->open()
      ->$$;

  if (win->handle != NULL) {
    $(win)
        ->user_data(create(VoxelWorld, (GlLoader)glfwGetProcAddress))
        ->on_key(on_key)
        ->on_cursor(on_cursor)
        ->on_mouse_button(on_mouse_button)
        ->on_scroll(on_scroll)
        ->on_render(on_render);
  }

  return win;
}

static int run_tests(void) {
  Test *t = create(Test, "chain_voxel Continuous 3x3 Chunk Tests")->$$;

  Glfw *win = create_voxel_app(320, 240, "chain_voxel --test", 0);
  StatusOr *win_st = $(win)->status()->$$;
  $(t)->section("3x3 chunk initialization, shared shader & 3D rendering")
      ->expect_ok(win_st);

  if ($(win_st)->ok()) {
    VoxelWorld *world = as(VoxelWorld, win->state);
    Gl *center_gl = as(Gl, $($(world)->chunk_slot(1, 1))->get("gl"));
    int initial_verts = $(world)->total_vertices();

    $(t)->expect_ok($(world)->status())
        ->expect_num(create(Number, (double)world->center_cx), 0)
        ->expect_num(create(Number, (double)world->center_cz), 0)
        ->expect_bool(create(Boolean, world->fly), 0)
        ->expect_bool(
            create(Boolean,
                   center_gl->program == world->world_shader->program &&
                   center_gl->program_owner == world->world_shader),
            1)
        ->expect_bool(
            create(Boolean,
                   initial_verts > 2000 &&
                   initial_verts <
                       VOXEL_GRID_SLOTS * VOXEL_CHUNK_VOLUME * 36),
            1);

    $(win)->max_frames(2)->run();

    /* Top sky `(160, 220)` matches sky; lower `(160, 35)` hits surface terrain. */
    TypedIntArray *sky_px = $(win)->read_pixel(160, 220)->$$;
    TypedIntArray *ground_px = $(win)->read_pixel(160, 35)->$$;
    $(t)->expect_bool(
            create(Boolean, $(sky_px)->at(2) > 200 && $(sky_px)->at(0) < 170),
            1)
        ->expect_bool(
            create(Boolean, $(ground_px)->at(1) != $(sky_px)->at(1) ||
                            $(ground_px)->at(2) < 200),
            1);
    release(ground_px);
    release(sky_px);

    /* Entering East chunk slides it into center `[1][1]` and reuses its VBO. */
    $(t)->section("Continuous 3x3 chunk recentering & mesh reuse");
    Hash *east_before = $(world)->chunk_slot(2, 1);
    Gl *east_gl_before = as(Gl, $(east_before)->get("gl"));

    int target_wx = VOXEL_CHUNK_X + 1;
    int target_wy = $(world)->surface_height(target_wx, 8);
    $(world)->teleport((float)target_wx + 0.5f, (float)(target_wy + 1) + 0.02f,
                       8.5f);
    $(win)->step();

    Hash *new_center = $(world)->chunk_slot(1, 1);
    $(t)->expect_num(create(Number, (double)world->center_cx), 1)
        ->expect_num(create(Number, (double)world->center_cz), 0)
        ->expect_num(create(Number, (double)world->chunk_shifts), 1)
        ->expect_num($(new_center)->get("cx"), 1)
        ->expect_num($(new_center)->get("cz"), 0)
        ->expect_bool(create(Boolean, new_center == east_before), 1)
        ->expect_bool(
            create(Boolean, as(Gl, $(new_center)->get("gl")) == east_gl_before),
            1);

    /* Break & place directly below player, then evict/reload chunk to verify edit. */
    $(t)->section("Voxel raycast break/place & persistent edits across shifts");
    $(world)->set_angles(world->yaw, -89.0f);
    int before_verts = $(world)->total_vertices();
    float before_yaw = world->yaw;

    $(win)->dispatch_event("cursor", create(Hash, 2)
        ->put("dx", create(Number, 10.0))
        ->put("dy", create(Number, 0.0)))
        ->dispatch_event("scroll", create(Hash, 2)
        ->put("xoffset", create(Number, 0.0))
        ->put("yoffset", create(Number, -1.0)))
        ->dispatch_event("mouse_button", create(Hash, 2)
        ->put("button", create(Number, GLFW_MOUSE_BUTTON_LEFT))
        ->put("pressed", create(Boolean, 1)));

    $(t)->expect_bool(create(Boolean, world->yaw > before_yaw), 1)
        ->expect_num(create(Number, (double)world->selected_block), BLOCK_SAND)
        ->expect_num(
            create(Number, (double)$(world)->block_at(target_wx, target_wy, 8)),
            BLOCK_AIR)
        ->expect_bool(
            create(Boolean, $(world)->total_vertices() != before_verts), 1);

    int after_break_verts = $(world)->total_vertices();
    $(win)->dispatch_event("mouse_button", create(Hash, 2)
        ->put("button", create(Number, GLFW_MOUSE_BUTTON_RIGHT))
        ->put("pressed", create(Boolean, 1)));
    $(t)->expect_num(
            create(Number, (double)$(world)->block_at(target_wx, target_wy, 8)),
            BLOCK_SAND)
        ->expect_bool(
            create(Boolean, $(world)->total_vertices() != after_break_verts),
            1);

    $(world)->teleport((float)(VOXEL_CHUNK_X * 4 + 2), world->y, world->z);
    $(win)->step();
    $(world)->teleport((float)target_wx + 0.5f, world->y, 8.5f);
    $(win)->step();
    $(t)->expect_num(
        create(Number, (double)$(world)->block_at(target_wx, target_wy, 8)),
        BLOCK_SAND);

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

  Glfw *win = create_voxel_app(1024, 720, "ISO C Chaining + Voxel Sandbox", 1);
  StatusOr *st = $(win)->status()->$$;
  if (!$(st)->ok()) {
    fprintf(stderr, "Failed to open GLFW OpenGL 3.3 window: %s\n",
            $(st)->error());
    release(st);
    release(win);
    return 1;
  }

  $(win)->run();
  release(st);
  release(win);
  return 0;
}
