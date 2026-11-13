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

#ifndef CHAIN_GLFW_H_
#define CHAIN_GLFW_H_

#include <GLFW/glfw3.h>

#include "core/hash.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Glfw, GLFW, (int, int, char*))

/*
 * Callback invoked once per frame during `step()` or `run()`. Receives the
 * bound `Glfw *win` and a `Hash *frame` containing `"frame"`, `"time"`, `"dt"`,
 * `"width"`, `"height"`, `"win_width"`, and `"win_height"`.
 */
typedef void (*ChGlfwFrameHandler)(ChGlfw *win, ChHash *frame);

/*
 * Callback invoked on input or window events (`on_key`, `on_char`,
 * `on_mouse_button`, `on_cursor`, `on_scroll`, `on_resize`, `on_close`).
 * Receives the bound `Glfw *win` and an event-specific `Hash *event`.
 */
typedef void (*ChGlfwEventHandler)(ChGlfw *win, ChHash *event);

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChGlfwFrameHandler GlfwFrameHandler;
typedef ChGlfwEventHandler GlfwEventHandler;
#endif

/*
 * Fluent GLFW3 window, input, and OpenGL context wrapper supporting pre-open
 * `hint()` configuration, automatic reference-counted `glfwInit()` /
 * `glfwTerminate()`, `Hash`-based event/frame callbacks, framebuffer pixel
 * readback (`read_pixel`), and `Test`-friendly `step()` / `max_frames()`.
 */
struct GLFW {
  ChGenericEl generic;
  GLFWwindow *handle;
  ChString *title;
  int width;
  int height;
  int fb_width;
  int fb_height;
  int swap_interval;
  int cursor_input_mode;
  int has_clear_color;
  float clear_rgba[4];
  int auto_viewport;
  long frame_count;
  long max_frame_count;
  double start_time;
  double last_frame_time;
  double last_cursor_x;
  double last_cursor_y;
  int has_cursor;
  void *hints;
  int hint_count;
  int hint_cap;
  ChGenericEl *state;
  ChString *last_err;
  char repr[128];

  ChGlfwFrameHandler render_cb;
  ChGlfwEventHandler key_cb;
  ChGlfwEventHandler char_cb;
  ChGlfwEventHandler mouse_button_cb;
  ChGlfwEventHandler cursor_cb;
  ChGlfwEventHandler scroll_cb;
  ChGlfwEventHandler resize_cb;
  ChGlfwEventHandler close_cb;

  /* Binds this `Glfw` as the active instance on the `Glfw` `self` stack. */
  ChGlfw* (*bind)(ChGenericEl*);

  /*
   * Queues a `glfwWindowHint(hint, value)` setting to be applied when `open()`
   * creates the native window (replacing any earlier value for `hint`).
   */
  ChGlfw* (*hint)(int, int);

  /*
   * Convenience helper queueing `GLFW_CONTEXT_VERSION_MAJOR`,
   * `GLFW_CONTEXT_VERSION_MINOR`, and (when non-zero) `GLFW_OPENGL_PROFILE`.
   */
  ChGlfw* (*gl_version)(int, int, int);

  /* Queues `GLFW_RESIZABLE` (before `open()`) or toggles live attribute. */
  ChGlfw* (*resizable)(int);

  /* Queues `GLFW_VISIBLE` (before `open()`) or shows/hides if open. */
  ChGlfw* (*visible)(int);

  /* Queues `GLFW_SAMPLES` MSAA sample count before `open()`. */
  ChGlfw* (*samples)(int);

  /*
   * Sets the GLFW cursor mode (`GLFW_CURSOR_NORMAL`, `GLFW_CURSOR_HIDDEN`, or
   * `GLFW_CURSOR_DISABLED` for FPS mouse-look), applying immediately if open.
   */
  ChGlfw* (*cursor_mode)(int);

  /* Sets the swap interval (`glfwSwapInterval`) for vsync (`1` = on, `0` = off). */
  ChGlfw* (*vsync)(int);

  /*
   * Configures the RGBA clear color and enables automatic `glClear()` before
   * each `render_cb` invocation in `step()` / `run()`.
   */
  ChGlfw* (*clear_color)(float, float, float, float);

  /*
   * Limits `run()` to at most `count` frames (`<= 0` runs until closed).
   * Useful for deterministic animations, benchmarks, and automated smoke tests.
   */
  ChGlfw* (*max_frames)(long);

  /*
   * Attaches a `ChGenericEl*` state object (`win->state`) to this window,
   * claiming ownership and releasing any previous state object automatically.
   */
  ChGlfw* (*user_data)(void*);

  /*
   * Initializes GLFW (if needed), applies queued hints, creates the
   * `GLFWwindow*`, makes its OpenGL context current, and binds native callbacks.
   */
  ChGlfw* (*open)(void);

  /* Opens the window if needed and binds its OpenGL context current. */
  ChGlfw* (*make_current)(void);

  /* Updates the window title (and `glfwSetWindowTitle` if already open). */
  ChGlfw* (*set_title)(char*);

  /* Updates the window size (and `glfwSetWindowSize` if already open). */
  ChGlfw* (*set_size)(int, int);

  /* Sets the window top-left screen position (`glfwSetWindowPos`). */
  ChGlfw* (*set_pos)(int, int);

  /* Shows the native window (`glfwShowWindow`). */
  ChGlfw* (*show)(void);

  /* Hides the native window (`glfwHideWindow`). */
  ChGlfw* (*hide)(void);

  /* Brings the native window to front and gives it input focus. */
  ChGlfw* (*focus)(void);

  /* Marks the window for closing (`glfwSetWindowShouldClose(handle, GLFW_TRUE)`). */
  ChGlfw* (*close)(void);

  /* Returns non-zero if closed, uninitialized, or close was requested. */
  int (*should_close)(void);

  /* Registers the per-frame render callback. */
  ChGlfw* (*on_render)(ChGlfwFrameHandler);

  /* Registers the keyboard callback (`key`, `scancode`, `action`, `mods`, ...). */
  ChGlfw* (*on_key)(ChGlfwEventHandler);

  /* Registers the Unicode character input callback (`codepoint`). */
  ChGlfw* (*on_char)(ChGlfwEventHandler);

  /* Registers the mouse button callback (`button`, `action`, `mods`, `x`, `y`). */
  ChGlfw* (*on_mouse_button)(ChGlfwEventHandler);

  /* Registers the cursor movement callback (`x`, `y`, `dx`, `dy`). */
  ChGlfw* (*on_cursor)(ChGlfwEventHandler);

  /* Registers the scroll wheel callback (`xoffset`, `yoffset`). */
  ChGlfw* (*on_scroll)(ChGlfwEventHandler);

  /* Registers the window/framebuffer resize callback (`width`, `height`, ...). */
  ChGlfw* (*on_resize)(ChGlfwEventHandler);

  /* Registers the window close notification callback (`closing`). */
  ChGlfw* (*on_close)(ChGlfwEventHandler);

  /* Sets `glClearColor` and clears color + depth buffers on the current context. */
  ChGlfw* (*clear)(void);

  /* Processes pending window/input events (`glfwPollEvents`). */
  ChGlfw* (*poll)(void);

  /* Waits up to `timeout_sec` (`< 0` waits indefinitely) for events. */
  ChGlfw* (*wait_events)(double);

  /* Swaps front and back framebuffers (`glfwSwapBuffers`). */
  ChGlfw* (*swap)(void);

  /*
   * Runs one complete frame: opens if needed, makes current, updates viewport
   * and auto-clears (when `clear_color` is set), invokes `on_render`, swaps
   * buffers, polls events, and increments `frame_count`.
   */
  ChGlfw* (*step)(void);

  /*
   * Opens if needed and runs `step()` in a loop until `should_close()` or
   * `max_frames` is reached. Returns this `Glfw` for cleanup or inspection.
   */
  ChGlfw* (*run)(void);

  /*
   * Programmatically dispatches an event (`"key"`, `"char"`, `"mouse_button"`,
   * `"cursor"`, `"scroll"`, `"resize"`, or `"close"`) to the registered handler.
   */
  ChGlfw* (*dispatch_event)(char*, ChHash*);

  /* Returns non-zero if `key` is currently pressed (`GLFW_PRESS`). */
  int (*key_down)(int);

  /* Returns non-zero if mouse `button` is currently pressed (`GLFW_PRESS`). */
  int (*mouse_down)(int);

  /*
   * Reads a single RGBA pixel at framebuffer coordinates `(x, y)` (OpenGL
   * bottom-left origin) into a 4-element `[r, g, b, a]` `TypedIntArray*`.
   */
  ChTypedIntArray* (*read_pixel)(int, int);

  /*
   * Returns a fluent `Hash*` snapshot of window dimensions, OpenGL version,
   * frame count, vsync, and error state.
   */
  ChHash* (*info)(void);

  /*
   * Returns a `StatusOr*` wrapping `info()` when healthy, or `last_err` on
   * initialization/window failure.
   */
  ChStatusOr* (*status)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_GLFW_H_ */
