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

#include "demos/glfw.h"

#include <stddef.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/test.h"
#include "core/typed_array.h"
#include "core/types.h"

static void render_solid_quad(Glfw *win, Hash *frame) {
  Slice *log = as(Slice, win->state);
  $(log)->add(create(Hash, 3)
      ->put("frame", create(Number, as(Number, $(frame)->get("frame"))->value()))
      ->put("width", create(Number, as(Number, $(frame)->get("width"))->value()))
      ->put("height",
            create(Number, as(Number, $(frame)->get("height"))->value())));

  /* Paint a bright magenta center quad over the dark clear background. */
  glColor3f(1.0f, 0.0f, 1.0f);
  glBegin(GL_QUADS);
  glVertex2f(-0.5f, -0.5f);
  glVertex2f(0.5f, -0.5f);
  glVertex2f(0.5f, 0.5f);
  glVertex2f(-0.5f, 0.5f);
  glEnd();
}

static void record_key_event(Glfw *win, Hash *ev) {
  Hash *captured = as(Hash, win->state);
  $(captured)->put("last_key", $(ev)->get("key"))
             ->put("last_pressed", $(ev)->get("pressed"));
  if ((int)as(Number, $(ev)->get("key"))->value() == GLFW_KEY_ESCAPE &&
      as(Boolean, $(ev)->get("pressed"))->value()) {
    $(win)->close();
  }
}

static void record_mouse_event(Glfw *win, Hash *ev) {
  Hash *captured = as(Hash, win->state);
  $(captured)->put("mouse_btn", $(ev)->get("button"))
             ->put("mouse_x", $(ev)->get("x"))
             ->put("mouse_y", $(ev)->get("y"));
}

static void record_cursor_event(Glfw *win, Hash *ev) {
  Hash *captured = as(Hash, win->state);
  $(captured)->put("cursor_dx", $(ev)->get("dx"))
             ->put("cursor_dy", $(ev)->get("dy"));
}

static void record_scroll_event(Glfw *win, Hash *ev) {
  Hash *captured = as(Hash, win->state);
  $(captured)->put("scroll_y", $(ev)->get("yoffset"));
}

static void record_resize_event(Glfw *win, Hash *ev) {
  Hash *captured = as(Hash, win->state);
  $(captured)->put("resize_w", $(ev)->get("width"))
             ->put("resize_h", $(ev)->get("height"));
}

static void record_close_event(Glfw *win, Hash *ev) {
  Hash *captured = as(Hash, win->state);
  $(captured)->put("closing_event", $(ev)->get("closing"));
}

static void test_glfw_rendering_and_lifecycle(Test *t) {
  Slice *frame_log = create(Slice, 4)->$$;

  /* Replace a queued hint cleanly before open(), then run 3 frames. */
  Glfw *win = create(Glfw, 64, 48, "Headless GL")
      ->hint(GLFW_RESIZABLE, GLFW_FALSE)
      ->hint(GLFW_RESIZABLE, GLFW_TRUE)
      ->visible(0)
      ->cursor_mode(GLFW_CURSOR_DISABLED)
      ->samples(0)
      ->vsync(0)
      ->clear_color(0.0f, 1.0f, 0.0f, 1.0f)
      ->user_data(frame_log)
      ->on_render(render_solid_quad)
      ->max_frames(3)
      ->$$;

  $(t)->section("Glfw pre-open configuration & lazy open")
      ->expect_str(win, "<Glfw 64x48 closed frame=0>")
      ->expect_bool(create(Boolean, $(win)->should_close()), 1);

  $(win)->open();
  $(t)->expect_ok($(win)->status())
      ->expect_str(win, "<Glfw 64x48 open frame=0>")
      ->expect_bool(create(Boolean, !$(win)->should_close()), 1)
      ->expect_num($(win)->info()->get("width"), 64)
      ->expect_num($(win)->info()->get("height"), 48)
      ->expect_bool($(win)->info()->get("open"), 1);

  /* Run bounded render loop (`max_frames == 3`). */
  $(win)->run();

  $(t)->section("Glfw OpenGL rendering, pixel readback & resize")
      ->expect_str(win, "<Glfw 64x48 open frame=3>")
      ->expect_num(create(Number, (double)frame_log->length), 3);

  if (frame_log->length >= 3) {
    $(t)->expect_num(as(Hash, $(frame_log)->get(0))->get("frame"), 0)
        ->expect_num(as(Hash, $(frame_log)->get(2))->get("frame"), 2)
        /* Clear background at bottom-left corner `(2, 2)` should be pure green. */
        ->expect_str($(win)->read_pixel(2, 2), "[0,255,0,255]")
        /* Center quad at `(32, 24)` should be magenta `[255,0,255,255]`. */
        ->expect_str($(win)->read_pixel(32, 24), "[255,0,255,255]")
        /* Out-of-bounds read_pixel safely returns an empty TypedIntArray. */
        ->expect_str($(win)->read_pixel(-1, 0), "[]")
        ->expect_str($(win)->read_pixel(64, 48), "[]");
  }

  /* Resize window, clear with solid blue, and verify readback at new bounds. */
  $(win)->set_title("Resized GL")
      ->set_size(80, 60)
      ->clear_color(0.0f, 0.0f, 1.0f, 1.0f)
      ->clear();

  $(t)->expect_str($(win)->info()->get("title"), "Resized GL")
      ->expect_num($(win)->info()->get("fb_width"), 80)
      ->expect_num($(win)->info()->get("fb_height"), 60)
      ->expect_str($(win)->read_pixel(75, 55), "[0,0,255,255]");

  release(win);
  release(frame_log);
}

static void test_glfw_events_and_reuse(Test *t) {
  Hash *captured = create(Hash, 12)->$$;

  Glfw *win = create(Glfw, 100, 100, "Events")
      ->visible(0)
      ->vsync(0)
      ->user_data(captured)
      ->on_key(record_key_event)
      ->on_mouse_button(record_mouse_event)
      ->on_cursor(record_cursor_event)
      ->on_scroll(record_scroll_event)
      ->on_resize(record_resize_event)
      ->on_close(record_close_event)
      ->open()
      ->$$;

  $(win)->dispatch_event("key", create(Hash, 2)
      ->put("key", create(Number, GLFW_KEY_A))
      ->put("pressed", create(Boolean, 1)))
      ->dispatch_event("mouse_button", create(Hash, 3)
      ->put("button", create(Number, GLFW_MOUSE_BUTTON_LEFT))
      ->put("x", create(Number, 12.5))
      ->put("y", create(Number, 34.5)))
      ->dispatch_event("cursor", create(Hash, 2)
      ->put("dx", create(Number, -3.0))
      ->put("dy", create(Number, 4.5)))
      ->dispatch_event("scroll", create(Hash, 2)
      ->put("xoffset", create(Number, 0.0))
      ->put("yoffset", create(Number, 1.5)))
      ->dispatch_event("resize", create(Hash, 2)
      ->put("width", create(Number, 320))
      ->put("height", create(Number, 240)))
      ->dispatch_event("close", create(Hash, 1)
      ->put("closing", create(Boolean, 1)));

  $(t)->section("Glfw event callbacks & re-initialization across instances")
      ->expect_num($(captured)->get("last_key"), GLFW_KEY_A)
      ->expect_bool($(captured)->get("last_pressed"), 1)
      ->expect_num($(captured)->get("mouse_btn"), GLFW_MOUSE_BUTTON_LEFT)
      ->expect_num($(captured)->get("mouse_x"), 12.5)
      ->expect_num($(captured)->get("cursor_dx"), -3.0)
      ->expect_num($(captured)->get("scroll_y"), 1.5)
      ->expect_num($(captured)->get("resize_w"), 320)
      ->expect_num($(captured)->get("resize_h"), 240)
      ->expect_bool($(captured)->get("closing_event"), 1);

  /* Key press with Escape triggers win->close() from inside callback. */
  $(t)->expect_bool(create(Boolean, $(win)->should_close()), 0);
  $(win)->dispatch_event("key", create(Hash, 2)
      ->put("key", create(Number, GLFW_KEY_ESCAPE))
      ->put("pressed", create(Boolean, 1)));
  $(t)->expect_bool(create(Boolean, $(win)->should_close()), 1);

  /* Unknown or NULL events are safely ignored without leaking. */
  $(win)->dispatch_event("unknown_event", create(Hash, 1)
      ->put("unused", create(Number, 1)));
  $(win)->dispatch_event(NULL, NULL);

  /* Destroying the sole window cleanly calls glfwTerminate(). */
  release(win);
  release(captured);

  /* Creating and running a fresh window re-initializes GLFW seamlessly. */
  Glfw *win2 = create(Glfw, 32, 32, "Second Window")
      ->visible(0)
      ->clear_color(1.0f, 1.0f, 1.0f, 1.0f)
      ->step()
      ->$$;
  $(t)->expect_ok($(win2)->status())
      ->expect_str($(win2)->read_pixel(16, 16), "[255,255,255,255]");
  release(win2);
}

int main(void) {
  Test *t = create(Test, "GLFW Wrapper Extension Unit Tests")->$$;

  test_glfw_rendering_and_lifecycle(t);
  test_glfw_events_and_reuse(t);

  int rc = $(t)->summary();
  release(t);
  return rc;
}
