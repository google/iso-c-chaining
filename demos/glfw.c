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

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/null.h"
#include "core/number.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/typed_array.h"
#include "core/types.h"

typedef struct {
  int hint;
  int value;
} GlfwHintEntry;

static int glfw_initialized = 0;
static int glfw_open_windows = 0;
static char glfw_last_error_msg[512] = "";

static void native_error_cb(int code, const char *description) {
  snprintf(glfw_last_error_msg, sizeof(glfw_last_error_msg),
           "GLFW error %d: %s", code,
           description != NULL ? description : "unknown error");
}

/**
 * Object handling functions.
 */

static Glfw* self(GenericEl *s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Glfw*)update_self_stack(&top, s, GLFW);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Glfw* bind_glfw(GenericEl *win) {
  return self(win);
}

static void set_error(Glfw *win, const char *msg) {
  if (win->last_err != NULL) {
    release(win->last_err);
    win->last_err = NULL;
  }
  if (msg != NULL && msg[0] != '\0') {
    win->last_err = create(String, (char*)msg)->$$;
    win->last_err->bind((GenericEl*)-1);
  }
}

static void destroy_native_window(Glfw *win) {
  if (win->handle != NULL) {
    if (glfwGetCurrentContext() == win->handle) {
      glFlush();
      glfwMakeContextCurrent(NULL);
    }
    glfwSetWindowUserPointer(win->handle, NULL);
    glfwSetKeyCallback(win->handle, NULL);
    glfwSetCharCallback(win->handle, NULL);
    glfwSetMouseButtonCallback(win->handle, NULL);
    glfwSetCursorPosCallback(win->handle, NULL);
    glfwSetScrollCallback(win->handle, NULL);
    glfwSetWindowSizeCallback(win->handle, NULL);
    glfwSetFramebufferSizeCallback(win->handle, NULL);
    glfwSetWindowCloseCallback(win->handle, NULL);
    glfwDestroyWindow(win->handle);
    win->handle = NULL;
    if (glfw_open_windows > 0) {
      glfw_open_windows--;
    }
    if (glfw_open_windows == 0 && glfw_initialized) {
      glfwTerminate();
      glfw_initialized = 0;
    }
  }
}

static void del(void *obj) {
  Glfw *win = (Glfw*)obj;
  if (win != NULL && self((GenericEl*)win) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      if (slf->state != NULL) {
        if (slf->handle != NULL) {
          glfwMakeContextCurrent(slf->handle);
        }
        release(slf->state);
        slf->state = NULL;
      }
      destroy_native_window(slf);
      if (slf->title != NULL) {
        release(slf->title);
        slf->title = NULL;
      }
      if (slf->last_err != NULL) {
        release(slf->last_err);
        slf->last_err = NULL;
      }
      free(slf->hints);
      slf->hints = NULL;
      Glfw *to_free = slf;
      self((GenericEl*)-1);
      free(to_free);
    } else {
      self((GenericEl*)-1);
    }
  }
}

/**
 * Internal event dispatch and GLFW native callback bridges.
 */

static void invoke_event_cb(Glfw *win, GlfwEventHandler cb, Hash *ev) {
  claim_element(ev, win);
  if (cb != NULL) {
    $(win);
    cb(win, ev != NULL ? $(ev) : NULL);
  }
  release(ev);
}

static void native_key_cb(GLFWwindow *raw, int key, int scancode, int action,
                          int mods) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL || win->key_cb == NULL) {
    return;
  }
  Hash *ev = create(Hash, 8)
      ->put("key", create(Number, (double)key))
      ->put("scancode", create(Number, (double)scancode))
      ->put("action", create(Number, (double)action))
      ->put("mods", create(Number, (double)mods))
      ->put("pressed", create(Boolean, action == GLFW_PRESS))
      ->put("released", create(Boolean, action == GLFW_RELEASE))
      ->put("repeat", create(Boolean, action == GLFW_REPEAT));
  invoke_event_cb(win, win->key_cb, ev);
}

static void native_char_cb(GLFWwindow *raw, unsigned int codepoint) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL || win->char_cb == NULL) {
    return;
  }
  Hash *ev = create(Hash, 2)
      ->put("codepoint", create(Number, (double)codepoint));
  invoke_event_cb(win, win->char_cb, ev);
}

static void native_mouse_button_cb(GLFWwindow *raw, int button, int action,
                                   int mods) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL || win->mouse_button_cb == NULL) {
    return;
  }
  double x = 0.0;
  double y = 0.0;
  glfwGetCursorPos(raw, &x, &y);
  Hash *ev = create(Hash, 7)
      ->put("button", create(Number, (double)button))
      ->put("action", create(Number, (double)action))
      ->put("mods", create(Number, (double)mods))
      ->put("pressed", create(Boolean, action == GLFW_PRESS))
      ->put("released", create(Boolean, action == GLFW_RELEASE))
      ->put("x", create(Number, x))
      ->put("y", create(Number, y));
  invoke_event_cb(win, win->mouse_button_cb, ev);
}

static void native_cursor_cb(GLFWwindow *raw, double xpos, double ypos) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL) {
    return;
  }
  double dx = win->has_cursor ? (xpos - win->last_cursor_x) : 0.0;
  double dy = win->has_cursor ? (ypos - win->last_cursor_y) : 0.0;
  win->last_cursor_x = xpos;
  win->last_cursor_y = ypos;
  win->has_cursor = 1;
  if (win->cursor_cb == NULL) {
    return;
  }
  Hash *ev = create(Hash, 4)
      ->put("x", create(Number, xpos))
      ->put("y", create(Number, ypos))
      ->put("dx", create(Number, dx))
      ->put("dy", create(Number, dy));
  invoke_event_cb(win, win->cursor_cb, ev);
}

static void native_scroll_cb(GLFWwindow *raw, double xoffset, double yoffset) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL || win->scroll_cb == NULL) {
    return;
  }
  Hash *ev = create(Hash, 2)
      ->put("xoffset", create(Number, xoffset))
      ->put("yoffset", create(Number, yoffset));
  invoke_event_cb(win, win->scroll_cb, ev);
}

static void native_window_size_cb(GLFWwindow *raw, int width, int height) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL) {
    return;
  }
  win->width = width;
  win->height = height;
}

static void native_framebuffer_size_cb(GLFWwindow *raw, int width, int height) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL) {
    return;
  }
  win->fb_width = width;
  win->fb_height = height;
  if (win->resize_cb == NULL) {
    return;
  }
  Hash *ev = create(Hash, 4)
      ->put("width", create(Number, (double)width))
      ->put("height", create(Number, (double)height))
      ->put("win_width", create(Number, (double)win->width))
      ->put("win_height", create(Number, (double)win->height));
  invoke_event_cb(win, win->resize_cb, ev);
}

static void native_close_cb(GLFWwindow *raw) {
  Glfw *win = (Glfw*)glfwGetWindowUserPointer(raw);
  if (win == NULL || win->close_cb == NULL) {
    return;
  }
  Hash *ev = create(Hash, 1)
      ->put("closing", create(Boolean, 1));
  invoke_event_cb(win, win->close_cb, ev);
}

static int ensure_open(Glfw *win) {
  if (win->handle != NULL) {
    return 1;
  }
  if (win->last_err != NULL) {
    return 0;
  }
  glfw_last_error_msg[0] = '\0';
  if (!glfw_initialized) {
    glfwSetErrorCallback(native_error_cb);
    if (!glfwInit()) {
      set_error(win, glfw_last_error_msg[0] != '\0'
                         ? glfw_last_error_msg
                         : "glfwInit() failed");
      return 0;
    }
    glfw_initialized = 1;
  }

  glfwDefaultWindowHints();
  GlfwHintEntry *entries = (GlfwHintEntry*)win->hints;
  int i;
  for (i = 0; i < win->hint_count; i++) {
    glfwWindowHint(entries[i].hint, entries[i].value);
  }

  const char *title = (win->title != NULL && win->title->chars != NULL)
                          ? win->title->chars
                          : "iso-c-chaining";
  win->handle = glfwCreateWindow(win->width, win->height, title, NULL, NULL);
  if (win->handle == NULL) {
    set_error(win, glfw_last_error_msg[0] != '\0'
                       ? glfw_last_error_msg
                       : "glfwCreateWindow() failed");
    if (glfw_open_windows == 0 && glfw_initialized) {
      glfwTerminate();
      glfw_initialized = 0;
    }
    return 0;
  }

  glfw_open_windows++;
  glfwSetWindowUserPointer(win->handle, win);
  glfwSetKeyCallback(win->handle, native_key_cb);
  glfwSetCharCallback(win->handle, native_char_cb);
  glfwSetMouseButtonCallback(win->handle, native_mouse_button_cb);
  glfwSetCursorPosCallback(win->handle, native_cursor_cb);
  glfwSetScrollCallback(win->handle, native_scroll_cb);
  glfwSetWindowSizeCallback(win->handle, native_window_size_cb);
  glfwSetFramebufferSizeCallback(win->handle, native_framebuffer_size_cb);
  glfwSetWindowCloseCallback(win->handle, native_close_cb);

  glfwMakeContextCurrent(win->handle);
  if (win->cursor_input_mode != GLFW_CURSOR_NORMAL) {
    glfwSetInputMode(win->handle, GLFW_CURSOR, win->cursor_input_mode);
  }
  if (win->swap_interval >= 0) {
    glfwSwapInterval(win->swap_interval);
  }
  glfwGetWindowSize(win->handle, &win->width, &win->height);
  glfwGetFramebufferSize(win->handle, &win->fb_width, &win->fb_height);
  win->start_time = glfwGetTime();
  win->last_frame_time = win->start_time;
  return 1;
}

/**
 * Fluent configuration methods.
 */

static Glfw* set_hint(int hint, int value) {
  Glfw *win = slf;
  GlfwHintEntry *entries = (GlfwHintEntry*)win->hints;
  int i;
  for (i = 0; i < win->hint_count; i++) {
    if (entries[i].hint == hint) {
      entries[i].value = value;
      return $(win);
    }
  }
  if (win->hint_count >= win->hint_cap) {
    win->hint_cap *= 2;
    entries = realloc(entries, sizeof(GlfwHintEntry) * win->hint_cap);
    win->hints = entries;
  }
  entries[win->hint_count].hint = hint;
  entries[win->hint_count].value = value;
  win->hint_count++;
  return $(win);
}

static Glfw* set_gl_version(int major, int minor, int profile) {
  Glfw *win = slf;
  $(win)->hint(GLFW_CONTEXT_VERSION_MAJOR, major)
        ->hint(GLFW_CONTEXT_VERSION_MINOR, minor);
  if (profile != 0) {
    $(win)->hint(GLFW_OPENGL_PROFILE, profile);
  }
  return $(win);
}

static Glfw* set_resizable(int enabled) {
  Glfw *win = slf;
  int flag = enabled ? GLFW_TRUE : GLFW_FALSE;
  $(win)->hint(GLFW_RESIZABLE, flag);
  if (win->handle != NULL) {
    glfwSetWindowAttrib(win->handle, GLFW_RESIZABLE, flag);
  }
  return $(win);
}

static Glfw* set_visible(int enabled) {
  Glfw *win = slf;
  int flag = enabled ? GLFW_TRUE : GLFW_FALSE;
  $(win)->hint(GLFW_VISIBLE, flag);
  if (win->handle != NULL) {
    if (flag) {
      glfwShowWindow(win->handle);
    } else {
      glfwHideWindow(win->handle);
    }
  }
  return $(win);
}

static Glfw* set_samples(int msaa_samples) {
  return $(slf)->hint(GLFW_SAMPLES, msaa_samples);
}

static Glfw* set_cursor_mode(int mode) {
  Glfw *win = slf;
  win->cursor_input_mode = mode;
  win->has_cursor = 0;
  if (win->handle != NULL) {
    glfwSetInputMode(win->handle, GLFW_CURSOR, mode);
  }
  return $(win);
}

static Glfw* set_vsync(int interval) {
  Glfw *win = slf;
  win->swap_interval = interval;
  if (win->handle != NULL) {
    glfwMakeContextCurrent(win->handle);
    glfwSwapInterval(interval);
  }
  return $(win);
}

static Glfw* set_clear_color(float r, float g, float b, float a) {
  Glfw *win = slf;
  win->has_clear_color = 1;
  win->clear_rgba[0] = r;
  win->clear_rgba[1] = g;
  win->clear_rgba[2] = b;
  win->clear_rgba[3] = a;
  return $(win);
}

static Glfw* set_max_frames(long count) {
  Glfw *win = slf;
  win->max_frame_count = count;
  return $(win);
}

static Glfw* set_user_data(void *elem) {
  Glfw *win = slf;
  claim_element(elem, win);
  if (win->state != NULL) {
    release(win->state);
  }
  win->state = (GenericEl*)elem;
  return $(win);
}

/**
 * Window lifecycle and attribute setters.
 */

static Glfw* open_window(void) {
  Glfw *win = slf;
  ensure_open(win);
  return $(win);
}

static Glfw* make_current_ctx(void) {
  Glfw *win = slf;
  if (ensure_open(win)) {
    glfwMakeContextCurrent(win->handle);
  }
  return $(win);
}

static Glfw* set_title_str(char *title) {
  Glfw *win = slf;
  if (win->title != NULL) {
    release(win->title);
  }
  win->title = create(String, title != NULL ? title : "")->$$;
  win->title->bind((GenericEl*)-1);
  if (win->handle != NULL) {
    glfwSetWindowTitle(win->handle, win->title->chars);
  }
  return $(win);
}

static Glfw* set_window_size(int width, int height) {
  Glfw *win = slf;
  win->width = width > 0 ? width : 1;
  win->height = height > 0 ? height : 1;
  win->fb_width = win->width;
  win->fb_height = win->height;
  if (win->handle != NULL) {
    glfwSetWindowSize(win->handle, win->width, win->height);
    glfwGetWindowSize(win->handle, &win->width, &win->height);
    glfwGetFramebufferSize(win->handle, &win->fb_width, &win->fb_height);
    glfwMakeContextCurrent(win->handle);
  }
  return $(win);
}

static Glfw* set_window_pos(int x, int y) {
  Glfw *win = slf;
  if (win->handle != NULL) {
    glfwSetWindowPos(win->handle, x, y);
  }
  return $(win);
}

static Glfw* show_window(void) {
  Glfw *win = slf;
  if (ensure_open(win)) {
    glfwShowWindow(win->handle);
  }
  return $(win);
}

static Glfw* hide_window(void) {
  Glfw *win = slf;
  if (win->handle != NULL) {
    glfwHideWindow(win->handle);
  }
  return $(win);
}

static Glfw* focus_window(void) {
  Glfw *win = slf;
  if (win->handle != NULL) {
    glfwFocusWindow(win->handle);
  }
  return $(win);
}

static Glfw* close_window(void) {
  Glfw *win = slf;
  if (win->handle != NULL) {
    glfwSetWindowShouldClose(win->handle, GLFW_TRUE);
  }
  return $(win);
}

static int should_close_window(void) {
  Glfw *win = slf;
  if (win->last_err != NULL || win->handle == NULL) {
    return 1;
  }
  return glfwWindowShouldClose(win->handle);
}

/**
 * Callback registration setters.
 */

static Glfw* on_render(GlfwFrameHandler handler) {
  slf->render_cb = handler;
  return slf;
}

static Glfw* on_key(GlfwEventHandler handler) {
  slf->key_cb = handler;
  return slf;
}

static Glfw* on_char(GlfwEventHandler handler) {
  slf->char_cb = handler;
  return slf;
}

static Glfw* on_mouse_button(GlfwEventHandler handler) {
  slf->mouse_button_cb = handler;
  return slf;
}

static Glfw* on_cursor(GlfwEventHandler handler) {
  slf->cursor_cb = handler;
  return slf;
}

static Glfw* on_scroll(GlfwEventHandler handler) {
  slf->scroll_cb = handler;
  return slf;
}

static Glfw* on_resize(GlfwEventHandler handler) {
  slf->resize_cb = handler;
  return slf;
}

static Glfw* on_close(GlfwEventHandler handler) {
  slf->close_cb = handler;
  return slf;
}

/**
 * Rendering, event loop, and OpenGL inspection.
 */

static Glfw* clear_buffers(void) {
  Glfw *win = slf;
  if (ensure_open(win)) {
    glfwMakeContextCurrent(win->handle);
    if (win->has_clear_color) {
      glClearColor(win->clear_rgba[0], win->clear_rgba[1],
                   win->clear_rgba[2], win->clear_rgba[3]);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
  return $(win);
}

static Glfw* poll_events(void) {
  Glfw *win = slf;
  if (glfw_initialized) {
    glfwPollEvents();
  }
  return $(win);
}

static Glfw* wait_events_timeout(double timeout_sec) {
  Glfw *win = slf;
  if (glfw_initialized) {
    if (timeout_sec < 0.0) {
      glfwWaitEvents();
    } else {
      glfwWaitEventsTimeout(timeout_sec);
    }
  }
  return $(win);
}

static Glfw* swap_buffers(void) {
  Glfw *win = slf;
  if (win->handle != NULL) {
    glFlush();
    glfwSwapBuffers(win->handle);
  }
  return $(win);
}

static Glfw* step_frame(void) {
  Glfw *win = slf;
  if (!ensure_open(win)) {
    return $(win);
  }
  glfwMakeContextCurrent(win->handle);
  glfwGetWindowSize(win->handle, &win->width, &win->height);
  glfwGetFramebufferSize(win->handle, &win->fb_width, &win->fb_height);
  if (win->auto_viewport && win->fb_width > 0 && win->fb_height > 0) {
    glViewport(0, 0, win->fb_width, win->fb_height);
  }
  if (win->has_clear_color) {
    glClearColor(win->clear_rgba[0], win->clear_rgba[1],
                 win->clear_rgba[2], win->clear_rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  double now = glfwGetTime();
  double dt = now - win->last_frame_time;
  if (dt < 0.0) {
    dt = 0.0;
  }
  win->last_frame_time = now;

  if (win->render_cb != NULL) {
    Hash *frame = create(Hash, 8)
        ->put("frame", create(Number, (double)win->frame_count))
        ->put("time", create(Number, now - win->start_time))
        ->put("dt", create(Number, dt))
        ->put("width", create(Number, (double)win->fb_width))
        ->put("height", create(Number, (double)win->fb_height))
        ->put("win_width", create(Number, (double)win->width))
        ->put("win_height", create(Number, (double)win->height))
        ->$$;
    frame->bind((GenericEl*)-1);
    $(win);
    win->render_cb(win, $(frame));
    release(frame);
  }

  if (win->handle != NULL) {
    glFlush();
    glfwSwapBuffers(win->handle);
  }
  glfwPollEvents();
  win->frame_count++;
  return $(win);
}

static Glfw* run_loop(void) {
  Glfw *win = slf;
  if (!ensure_open(win)) {
    return $(win);
  }
  long executed = 0;
  while (win->handle != NULL && !glfwWindowShouldClose(win->handle)) {
    if (win->max_frame_count > 0 && executed >= win->max_frame_count) {
      break;
    }
    $(win)->step();
    executed++;
  }
  return $(win);
}

static Glfw* dispatch_event(char *event_type, Hash *event) {
  Glfw *win = slf;
  Hash *ev = event != NULL ? event : create(Hash, 1);
  if (event_type == NULL) {
    claim_element(ev, win);
    release(ev);
    return $(win);
  }
  if (strcmp(event_type, "key") == 0) {
    invoke_event_cb(win, win->key_cb, ev);
  } else if (strcmp(event_type, "char") == 0) {
    invoke_event_cb(win, win->char_cb, ev);
  } else if (strcmp(event_type, "mouse_button") == 0) {
    invoke_event_cb(win, win->mouse_button_cb, ev);
  } else if (strcmp(event_type, "cursor") == 0) {
    invoke_event_cb(win, win->cursor_cb, ev);
  } else if (strcmp(event_type, "scroll") == 0) {
    invoke_event_cb(win, win->scroll_cb, ev);
  } else if (strcmp(event_type, "resize") == 0) {
    GenericEl *w_el = $(ev)->get("width");
    GenericEl *h_el = $(ev)->get("height");
    if (is_type(w_el, NUMBER) && is_type(h_el, NUMBER)) {
      int new_w = (int)((Number*)w_el)->number;
      int new_h = (int)((Number*)h_el)->number;
      if (new_w > 0 && new_h > 0) {
        win->fb_width = new_w;
        win->fb_height = new_h;
      }
    }
    invoke_event_cb(win, win->resize_cb, ev);
  } else if (strcmp(event_type, "close") == 0) {
    invoke_event_cb(win, win->close_cb, ev);
  } else {
    claim_element(ev, win);
    release(ev);
  }
  return $(win);
}

static int key_down(int key) {
  Glfw *win = slf;
  if (win->handle == NULL) {
    return 0;
  }
  return glfwGetKey(win->handle, key) == GLFW_PRESS;
}

static int mouse_down(int button) {
  Glfw *win = slf;
  if (win->handle == NULL) {
    return 0;
  }
  return glfwGetMouseButton(win->handle, button) == GLFW_PRESS;
}

static TypedIntArray* read_pixel(int x, int y) {
  Glfw *win = slf;
  if (win->handle == NULL) {
    return create(TypedIntArray, 0);
  }
  glfwMakeContextCurrent(win->handle);
  glfwGetFramebufferSize(win->handle, &win->fb_width, &win->fb_height);
  if (x < 0 || y < 0 || x >= win->fb_width || y >= win->fb_height) {
    return create(TypedIntArray, 0);
  }
  unsigned char px[4] = {0, 0, 0, 0};
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  return create(TypedIntArray, 4)
      ->push_n(4, (int)px[0], (int)px[1], (int)px[2], (int)px[3]);
}

static Hash* window_info(void) {
  Glfw *win = slf;
  const char *gl_ver = NULL;
  const char *gl_rend = NULL;
  if (win->handle != NULL) {
    glfwMakeContextCurrent(win->handle);
    glfwGetWindowSize(win->handle, &win->width, &win->height);
    glfwGetFramebufferSize(win->handle, &win->fb_width, &win->fb_height);
    gl_ver = (const char*)glGetString(GL_VERSION);
    gl_rend = (const char*)glGetString(GL_RENDERER);
  }
  return create(Hash, 12)
      ->put("title", create(String,
                            win->title != NULL ? win->title->chars : ""))
      ->put("width", create(Number, (double)win->width))
      ->put("height", create(Number, (double)win->height))
      ->put("fb_width", create(Number, (double)win->fb_width))
      ->put("fb_height", create(Number, (double)win->fb_height))
      ->put("open", create(Boolean, win->handle != NULL))
      ->put("should_close",
            create(Boolean, win->handle != NULL
                                ? glfwWindowShouldClose(win->handle)
                                : 1))
      ->put("frame", create(Number, (double)win->frame_count))
      ->put("vsync", create(Number, (double)win->swap_interval))
      ->put("gl_version",
            gl_ver != NULL ? (GenericEl*)create(String, (char*)gl_ver)
                           : (GenericEl*)create(Null))
      ->put("gl_renderer",
            gl_rend != NULL ? (GenericEl*)create(String, (char*)gl_rend)
                            : (GenericEl*)create(Null))
      ->put("error",
            win->last_err != NULL
                ? (GenericEl*)create(String, win->last_err->chars)
                : (GenericEl*)create(Null));
}

static StatusOr* window_status(void) {
  Glfw *win = slf;
  if (win->last_err != NULL) {
    return create(StatusOr, NULL, win->last_err->chars);
  }
  return create(StatusOr, $(win)->info(), NULL);
}

static char* to_chars(void) {
  Glfw *win = slf;
  if (win->last_err != NULL) {
    return win->last_err->chars;
  }
  snprintf(win->repr, sizeof(win->repr), "<Glfw %dx%d %s frame=%ld>",
           win->width, win->height,
           win->handle != NULL ? "open" : "closed", win->frame_count);
  return win->repr;
}

/**
 * Constructor.
 */

Glfw* ch_new_Glfw(int width, int height, char *title) {
  Glfw *win = malloc(sizeof(Glfw));
  win->generic.magic = CH_MAGIC;
  win->generic.type_val = GLFW;
  win->generic.prev_self = NULL;
  win->generic.next_self = NULL;
  win->generic.refs = 0;

  win->handle = NULL;
  win->title = create(String, title != NULL ? title : "iso-c-chaining")->$$;
  win->title->bind((GenericEl*)-1);
  win->width = width > 0 ? width : 640;
  win->height = height > 0 ? height : 480;
  win->fb_width = win->width;
  win->fb_height = win->height;
  win->swap_interval = 1;
  win->cursor_input_mode = GLFW_CURSOR_NORMAL;
  win->has_clear_color = 0;
  win->clear_rgba[0] = 0.0f;
  win->clear_rgba[1] = 0.0f;
  win->clear_rgba[2] = 0.0f;
  win->clear_rgba[3] = 1.0f;
  win->auto_viewport = 1;
  win->frame_count = 0;
  win->max_frame_count = 0;
  win->start_time = 0.0;
  win->last_frame_time = 0.0;
  win->last_cursor_x = 0.0;
  win->last_cursor_y = 0.0;
  win->has_cursor = 0;
  win->hint_count = 0;
  win->hint_cap = 8;
  win->hints = malloc(sizeof(GlfwHintEntry) * win->hint_cap);
  win->state = NULL;
  win->last_err = NULL;
  win->repr[0] = '\0';

  win->render_cb = NULL;
  win->key_cb = NULL;
  win->char_cb = NULL;
  win->mouse_button_cb = NULL;
  win->cursor_cb = NULL;
  win->scroll_cb = NULL;
  win->resize_cb = NULL;
  win->close_cb = NULL;

  win->bind = bind_glfw;
  win->generic.bind = (void* (*)(GenericEl*))bind_glfw;
  win->generic.save = save;
  win->generic.del = del;
  win->generic.to_chars = to_chars;

  win->hint = set_hint;
  win->gl_version = set_gl_version;
  win->resizable = set_resizable;
  win->visible = set_visible;
  win->samples = set_samples;
  win->cursor_mode = set_cursor_mode;
  win->vsync = set_vsync;
  win->clear_color = set_clear_color;
  win->max_frames = set_max_frames;
  win->user_data = set_user_data;

  win->open = open_window;
  win->make_current = make_current_ctx;
  win->set_title = set_title_str;
  win->set_size = set_window_size;
  win->set_pos = set_window_pos;
  win->show = show_window;
  win->hide = hide_window;
  win->focus = focus_window;
  win->close = close_window;
  win->should_close = should_close_window;

  win->on_render = on_render;
  win->on_key = on_key;
  win->on_char = on_char;
  win->on_mouse_button = on_mouse_button;
  win->on_cursor = on_cursor;
  win->on_scroll = on_scroll;
  win->on_resize = on_resize;
  win->on_close = on_close;

  win->clear = clear_buffers;
  win->poll = poll_events;
  win->wait_events = wait_events_timeout;
  win->swap = swap_buffers;
  win->step = step_frame;
  win->run = run_loop;
  win->dispatch_event = dispatch_event;

  win->key_down = key_down;
  win->mouse_down = mouse_down;
  win->read_pixel = read_pixel;
  win->info = window_info;
  win->status = window_status;

  self((GenericEl*)win);

  return win;
}
