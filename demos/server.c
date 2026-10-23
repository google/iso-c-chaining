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

#include "demos/server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/json.h"
#include "core/null.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"

static volatile sig_atomic_t keep_running = 1;

static void handle_stop_signal(int sig) {
  (void)sig;
  keep_running = 0;
}

typedef struct {
  String *method;
  String *pattern;
  RouteHandler handler;
} RouteEntry;

/**
 * Object handling functions.
 */

static Server* self(GenericEl* s) {
  static CH_THREAD_LOCAL GenericEl *top = NULL;
  return (Server*)update_self_stack(&top, s, SERVER);
}

static void* save(void) {
  slf->generic.refs++;
  return slf;
}

static Server* bind_server(GenericEl* server) {
  return self(server);
}

static void stop_server(void) {
  Server *server = slf;
  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }
}

static void del(void* obj) {
  Server* server = (Server*)obj;
  /*
   * `self((GenericEl*)server)` verifies `obj` is a `Server` and pushes it onto
   * the `Server` `self` stack so `slf` resolves to `server` during cleanup.
   */
  if (server != NULL && self((GenericEl*)server) != NULL) {
    slf->generic.refs--;
    if (slf->generic.refs <= 0) {
      stop_server();
      RouteEntry *entries = (RouteEntry*)slf->routes;
      int i;
      for (i = 0; i < slf->route_count; i++) {
        release(entries[i].method);
        release(entries[i].pattern);
      }
      free(entries);
      Server *to_free = slf;
      /* Pop `server` off the `self` stack (`-1` sentinel) before `free()`. */
      self((GenericEl*)-1);
      free(to_free);
    } else {
      /* Pop the temporary `self` stack entry pushed by the `if` condition. */
      self((GenericEl*)-1);
    }
  }
}

/**
 * Native routing and parameter parsing helpers.
 */

static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static void url_decode(char *s) {
  if (s == NULL) {
    return;
  }
  /*
   * Decode in place: since `%XX` (3 bytes -> 1 byte) and `+` (1 byte -> 1 byte)
   * never expand the string, `dst` safely trails `src` in the same buffer.
   */
  char *src = s;
  char *dst = s;
  while (*src != '\0') {
    if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
      int hi = hex_val(src[1]);
      int lo = hex_val(src[2]);
      if (hi >= 0 && lo >= 0) {
        *dst++ = (char)((hi << 4) | lo);
        src += 3;
        continue;
      }
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
      continue;
    }
    *dst++ = *src++;
  }
  *dst = '\0';
}

static GenericEl* parse_scalar_param(const char *s) {
  if (s == NULL) {
    return (GenericEl*)create(Null);
  }
  /* Coerce `"true"` / `"false"` literals into `Boolean` instances. */
  if (strcmp(s, "true") == 0) {
    return (GenericEl*)create(Boolean, 1);
  }
  if (strcmp(s, "false") == 0) {
    return (GenericEl*)create(Boolean, 0);
  }
  /*
   * Coerce strings that parse completely as numbers into `Number` instances so
   * route/query parameters (e.g. `:id` or `?min_score=90`) bind with numeric
   * affinity in SQLite queries.
   */
  if (s[0] != '\0') {
    char *endptr = NULL;
    double num = strtod(s, &endptr);
    if (endptr != s && *endptr == '\0') {
      return (GenericEl*)create(Number, num);
    }
  }
  return (GenericEl*)create(String, (char*)s);
}

static int match_route_pattern(const char *pattern, const char *path,
                               Hash *params) {
  /* Split both the route pattern and request path on `'/'` segments. */
  Slice *pat_parts = create(String, (char*)pattern)->split("/")->$$;
  Slice *path_parts = create(String, (char*)path)->split("/")->$$;

  if (pat_parts->length != path_parts->length) {
    release(pat_parts);
    release(path_parts);
    return 0;
  }

  /*
   * Pass 1: verify that every static (non-`:param`) segment matches before
   * mutating `params`. Because `dispatch_request` reuses a single `params`
   * `Hash` across candidate routes, a single-pass loop would leave partial
   * `:param` captures in `params` if a route matched `:param` early in the path
   * (e.g. `/users/:user_id/posts`) and then failed on a later literal segment.
   */
  int i;
  for (i = 0; i < pat_parts->length; i++) {
    char *p_seg = ((String*)pat_parts->elements[i])->chars;
    char *u_seg = ((String*)path_parts->elements[i])->chars;
    if (p_seg[0] != ':' && strcmp(p_seg, u_seg) != 0) {
      release(pat_parts);
      release(path_parts);
      return 0;
    }
  }

  /*
   * Pass 2: now that the entire route pattern is confirmed to match, URL-decode
   * each `:param` segment and populate its coerced value into `params`.
   */
  for (i = 0; i < pat_parts->length; i++) {
    char *p_seg = ((String*)pat_parts->elements[i])->chars;
    char *u_seg = ((String*)path_parts->elements[i])->chars;
    if (p_seg[0] == ':') {
      url_decode(u_seg);
      $(params)->put(p_seg + 1, parse_scalar_param(u_seg));
      params->bind((GenericEl*)-1);
    }
  }

  release(pat_parts);
  release(path_parts);
  return 1;
}

static void parse_query_string(const char *query, Hash *query_hash) {
  if (query == NULL || query[0] == '\0') {
    return;
  }
  Slice *pairs = create(String, (char*)query)->split("&")->$$;

  int i;
  for (i = 0; i < pairs->length; i++) {
    char *pair = ((String*)pairs->elements[i])->chars;
    char *eq = strchr(pair, '=');
    if (eq != NULL) {
      /* `key=value` pair: split at `'='`, URL-decode both, and coerce value. */
      *eq = '\0';
      char *val = eq + 1;
      url_decode(pair);
      url_decode(val);
      $(query_hash)->put(pair, parse_scalar_param(val));
      query_hash->bind((GenericEl*)-1);
    } else if (pair[0] != '\0') {
      /* Bare flag without `'='` (e.g. `?verbose`): default to `true`. */
      url_decode(pair);
      $(query_hash)->put(pair, create(Boolean, 1));
      query_hash->bind((GenericEl*)-1);
    }
  }
  release(pairs);
}

static Server* add_route(char *method, char *pattern, RouteHandler handler) {
  Server *server = slf;
  if (server->route_count >= server->route_cap) {
    server->route_cap *= 2;
    server->routes = realloc(
        server->routes, sizeof(RouteEntry) * server->route_cap);
  }
  /*
   * Store owned `String` copies of `method` and `pattern` detached from the
   * `String` `self` stack so route registration does not disturb caller chains.
   */
  RouteEntry *entries = (RouteEntry*)server->routes;
  entries[server->route_count].method = create(String, method)->$$;
  entries[server->route_count].method->bind((GenericEl*)-1);
  entries[server->route_count].pattern = create(String, pattern)->$$;
  entries[server->route_count].pattern->bind((GenericEl*)-1);
  entries[server->route_count].handler = handler;
  server->route_count++;
  return $(server);
}

static Server* get_route(char *pattern, RouteHandler handler) {
  return add_route("GET", pattern, handler);
}

static Server* post_route(char *pattern, RouteHandler handler) {
  return add_route("POST", pattern, handler);
}

static Server* put_route(char *pattern, RouteHandler handler) {
  return add_route("PUT", pattern, handler);
}

static Server* delete_route(char *pattern, RouteHandler handler) {
  return add_route("DELETE", pattern, handler);
}

static StatusOr* dispatch_request(char *method, char *path, char *query,
                                  char *body) {
  Server *server = slf;
  const char *req_method = method != NULL ? method : "GET";
  const char *req_path = path != NULL ? path : "/";

  /*
   * Step 1: scan registered routes in order for a matching HTTP method and
   * path pattern, populating any `:param` captures into `params`.
   */
  RouteEntry *entries = (RouteEntry*)server->routes;
  RouteHandler matched_handler = NULL;
  Hash *params = create(Hash, 4)->$$;
  params->bind((GenericEl*)-1);

  int i;
  for (i = 0; i < server->route_count; i++) {
    if (strcmp(entries[i].method->chars, req_method) == 0 &&
        match_route_pattern(entries[i].pattern->chars, req_path, params)) {
      matched_handler = entries[i].handler;
      break;
    }
  }

  if (matched_handler == NULL) {
    release(params);
    return create(StatusOr, NULL, "Not found");
  }

  /*
   * Step 2: parse a non-empty request body as JSON (returning any syntax error
   * immediately as a failed `StatusOr`), or use `Null` when the body is empty.
   */
  GenericEl *body_el = NULL;
  if (body != NULL && body[0] != '\0') {
    StatusOr *parsed_body = from_json(body)->$$;
    if (!$(parsed_body)->ok()) {
      release(params);
      parsed_body->generic.refs--;
      return $(parsed_body);
    }
    body_el = $(parsed_body)->value()->save();
    body_el->bind((GenericEl*)-1);
    release(parsed_body);
  } else {
    body_el = (GenericEl*)create(Null)->$$;
    body_el->bind((GenericEl*)-1);
  }

  /*
   * Step 3: parse URL query parameters and assemble the `req` `Hash` passed to
   * the route handler (`method`, `path`, `params`, `query`, and `body`).
   */
  Hash *query_hash = create(Hash, 4)->$$;
  query_hash->bind((GenericEl*)-1);
  parse_query_string(query, query_hash);

  Hash *req = create(Hash, 8)
      ->put("method", create(String, (char*)req_method))
      ->put("path", create(String, (char*)req_path))
      ->put("params", params)
      ->put("query", query_hash)
      ->put("body", body_el)->$$;
  req->bind((GenericEl*)-1);

  /* Drop local refs now that `req` owns `params`, `query_hash`, `body_el`. */
  release(params);
  release(query_hash);
  release(body_el);

  /*
   * Step 4: invoke the route handler and normalize its return value (`NULL`,
   * an existing `StatusOr*`, or a raw `GenericEl*`) into a `StatusOr*`.
   */
  GenericEl *res = matched_handler(req);
  StatusOr *status_res = NULL;
  if (res == NULL) {
    status_res = create(StatusOr, create(Null), NULL)->$$;
  } else if (res->type_val == STATUS_OR) {
    status_res = ((StatusOr*)res)->$$;
  } else {
    status_res = create(StatusOr, res, NULL)->$$;
  }
  /*
   * Step 5: hold a temporary ref on `status_res` across `release(req)` in case
   * `res` was a sub-element borrowed from `req`, then drop the temporary ref
   * and re-bind `status_res` to `self` as an unowned return value.
   */
  status_res->bind((GenericEl*)-1);

  release(req);

  status_res->generic.refs--;
  return $(status_res);
}

/**
 * POSIX Socket HTTP/1.1 transport adapter.
 */

static void write_all(int fd, const char *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, buf + sent, len - sent);
    if (n <= 0) {
      break;
    }
    sent += (size_t)n;
  }
}

static void send_http_json(int fd, int code, const char *status_text,
                           const char *json) {
  char header[256];
  size_t body_len = strlen(json) + 1;
  int hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %lu\r\n"
                      "Connection: close\r\n\r\n",
                      code, status_text, (unsigned long)body_len);
  if (hlen > 0) {
    write_all(fd, header, (size_t)hlen);
  }
  write_all(fd, json, strlen(json));
  write_all(fd, "\n", 1);
}

static int parse_content_length(const char *headers, size_t headers_len) {
  const char *p = headers;
  const char *end = headers + headers_len;
  while (p < end) {
    const char *line_end = strstr(p, "\r\n");
    if (line_end == NULL || line_end > end) {
      break;
    }
    if (line_end - p >= 15 &&
        strncasecmp(p, "Content-Length:", 15) == 0) {
      return atoi(p + 15);
    }
    p = line_end + 2;
  }
  return 0;
}

static int has_expect_continue(const char *headers, size_t headers_len) {
  const char *p = headers;
  const char *end = headers + headers_len;
  while (p < end) {
    const char *line_end = strstr(p, "\r\n");
    if (line_end == NULL || line_end > end) {
      break;
    }
    if (line_end - p >= 7 && strncasecmp(p, "Expect:", 7) == 0) {
      const char *val = p + 7;
      while (val < line_end && isspace((unsigned char)*val)) {
        val++;
      }
      if (line_end - val >= 12 &&
          strncasecmp(val, "100-continue", 12) == 0) {
        return 1;
      }
    }
    p = line_end + 2;
  }
  return 0;
}

static void handle_client_fd(Server *server, int client_fd) {
  /*
   * Step 1: set a 5-second receive timeout so a slow or stalled client cannot
   * block the single-threaded accept loop indefinitely.
   */
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  size_t cap = 8192;
  size_t len = 0;
  char *buf = malloc(cap);
  char *body_start = NULL;
  size_t header_bytes = 0;
  int content_length = 0;
  int sent_continue = 0;

  /*
   * Step 2: read from `client_fd` until the `\r\n\r\n` header terminator and
   * all `Content-Length` body bytes have been received.
   */
  while (1) {
    if (len + 1 >= cap) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
    ssize_t n = read(client_fd, buf + len, cap - 1 - len);
    if (n <= 0) {
      break;
    }
    len += (size_t)n;
    buf[len] = '\0';

    /*
     * Once the end of the HTTP header block (`\r\n\r\n`) arrives, extract
     * `Content-Length` and answer `Expect: 100-continue` so clients waiting
     * before sending a request body proceed immediately.
     */
    if (body_start == NULL) {
      char *sep = strstr(buf, "\r\n\r\n");
      if (sep != NULL) {
        header_bytes = (size_t)(sep - buf) + 4;
        body_start = sep + 4;
        content_length = parse_content_length(buf, header_bytes);
        if (has_expect_continue(buf, header_bytes) && !sent_continue) {
          const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
          write_all(client_fd, cont, strlen(cont));
          sent_continue = 1;
        }
      }
    }

    if (body_start != NULL) {
      size_t body_read = len - header_bytes;
      if ((int)body_read >= content_length) {
        break;
      }
    }
  }

  if (body_start == NULL) {
    free(buf);
    close(client_fd);
    return;
  }

  /* Truncate any bytes read past `Content-Length`. */
  if ((int)(len - header_bytes) > content_length) {
    body_start[content_length] = '\0';
  }

  /*
   * Step 3: parse the HTTP request line (`METHOD URI HTTP/1.x`) in place by
   * splitting on `\r\n` and spaces, then split `?query` off `uri`.
   */
  char *req_line_end = strstr(buf, "\r\n");
  if (req_line_end == NULL) {
    free(buf);
    close(client_fd);
    return;
  }
  *req_line_end = '\0';

  char *method = buf;
  char *sp1 = strchr(method, ' ');
  if (sp1 == NULL) {
    free(buf);
    close(client_fd);
    return;
  }
  *sp1 = '\0';
  char *uri = sp1 + 1;
  char *sp2 = strchr(uri, ' ');
  if (sp2 != NULL) {
    *sp2 = '\0';
  }

  char *query = strchr(uri, '?');
  if (query != NULL) {
    *query = '\0';
    query++;
  }

  printf("[HTTP] %s %s%s%s\n",
         method,
         uri,
         query != NULL ? "?" : "",
         query != NULL ? query : "");
  fflush(stdout);

  /*
   * Step 4: dispatch the parsed request to the matched route handler and write
   * back either a `200 OK` JSON payload or a `400`/`404` `{"error": ...}` JSON
   * object.
   */
  StatusOr *res = $(server)->dispatch(method, uri, query, body_start)->$$;
  if ($(res)->ok()) {
    char *json = $(res)->chars();
    send_http_json(client_fd, 200, "OK", json);
  } else {
    char *err_msg = $(res)->error();
    int code = 400;
    const char *status_text = "Bad Request";
    if (strcmp(err_msg, "Not found") == 0 ||
        strcmp(err_msg, "No rows returned") == 0) {
      code = 404;
      status_text = "Not Found";
    }
    Hash *err_obj = create(Hash, 2)
        ->put("error", create(String, err_msg))->$$;
    char *json = $(err_obj)->chars();
    send_http_json(client_fd, code, status_text, json);
    release(err_obj);
  }

  release(res);
  free(buf);
  close(client_fd);
}

static Server* start_server(void) {
  Server *server = slf;
  if (server->listen_fd >= 0) {
    return $(server);
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return $(server);
  }

  /* Allow immediate re-binding to the port after a server restart. */
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  /* Bind to loopback (`127.0.0.1`) on `server->port` and start listening. */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)server->port);

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      listen(fd, 64) < 0) {
    close(fd);
    return $(server);
  }

  /* If bound to port 0, record the OS-assigned ephemeral port on `server`. */
  if (server->port == 0) {
    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &alen) == 0) {
      server->port = (int)ntohs(addr.sin_port);
    }
  }

  server->listen_fd = fd;
  return $(server);
}

static StatusOr* http_request(char *method, char *uri, char *body) {
  Server *server = slf;
  if (server->listen_fd < 0 || server->port <= 0) {
    return create(StatusOr, NULL, "Server is not running");
  }

  /*
   * Step 1: open a TCP client socket and connect to the server's loopback
   * listener (`127.0.0.1:server->port`). Because `listen()` is already active,
   * the kernel completes the loopback handshake in the listen backlog without
   * needing a separate server thread.
   */
  int c_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c_fd < 0) {
    return create(StatusOr, NULL, "Failed to create client socket");
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)server->port);

  if (connect(c_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(c_fd);
    return create(StatusOr, NULL, "HTTP connection failed");
  }

  /*
   * Step 2: format the HTTP/1.1 request (including `Content-Length` and JSON
   * body when non-empty) and write it into the client socket buffer.
   */
  char req_buf[4096];
  int req_len = 0;
  if (body != NULL && body[0] != '\0') {
    req_len = snprintf(
        req_buf, sizeof(req_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        method, uri, server->port, (unsigned long)strlen(body), body);
  } else {
    req_len = snprintf(
        req_buf, sizeof(req_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Connection: close\r\n\r\n",
        method, uri, server->port);
  }
  if (req_len > 0) {
    write_all(c_fd, req_buf, (size_t)req_len);
  }

  /*
   * Step 3: accept the queued loopback connection on the server socket and
   * process it synchronously via `handle_client_fd()`, which writes the HTTP
   * response into the kernel socket buffer and closes the server-side fd.
   */
  int s_fd = accept(server->listen_fd, NULL, NULL);
  if (s_fd >= 0) {
    handle_client_fd(server, s_fd);
  }

  /* Step 4: read the complete raw HTTP response from the client socket. */
  char raw_buf[8192];
  size_t total = 0;
  ssize_t n;
  while ((n = read(c_fd, raw_buf + total,
                   sizeof(raw_buf) - 1 - total)) > 0) {
    total += (size_t)n;
    if (total >= sizeof(raw_buf) - 1) {
      break;
    }
  }
  raw_buf[total] = '\0';
  close(c_fd);

  /*
   * Step 5: parse the HTTP status code from `"HTTP/1.1 <code> ..."` and locate
   * the JSON payload after the `\r\n\r\n` header separator.
   */
  int status_code = 500;
  char *sp = strchr(raw_buf, ' ');
  if (sp != NULL) {
    status_code = atoi(sp + 1);
  }

  char *resp_body = strstr(raw_buf, "\r\n\r\n");
  if (resp_body != NULL) {
    resp_body += 4;
  } else {
    resp_body = raw_buf;
  }

  /*
   * Step 6: parse the JSON response body. For HTTP error statuses (`>= 400`),
   * unwrap `{"error": "<msg>"}` into a failed `StatusOr`; otherwise return the
   * parsed response `StatusOr`.
   */
  StatusOr *parsed = from_json(resp_body)->$$;
  if (status_code >= 400) {
    if ($(parsed)->ok() && is_type($(parsed)->value(), HASH)) {
      Hash *err_hash = as(Hash, $(parsed)->value());
      GenericEl *err_val = $(err_hash)->get("error");
      char *msg = err_val != NULL ? err_val->to_chars() : "HTTP error";
      StatusOr *err_res = create(StatusOr, NULL, msg)->$$;
      release(parsed);
      err_res->generic.refs--;
      return $(err_res);
    }
    release(parsed);
    return create(StatusOr, NULL, resp_body);
  }
  parsed->generic.refs--;
  return $(parsed);
}

static void listen_forever(void) {
  Server *server = slf;
  keep_running = 1;
  /* Trap `SIGINT` / `SIGTERM` so Ctrl-C exits the poll loop cleanly. */
  signal(SIGINT, handle_stop_signal);
  signal(SIGTERM, handle_stop_signal);
  start_server();
  if (server->listen_fd < 0) {
    fprintf(stderr, "Failed to bind port %d\n", server->port);
    return;
  }
  printf("Server listening on http://127.0.0.1:%d (Ctrl-C to stop)\n",
         server->port);
  fflush(stdout);

  /*
   * Poll `listen_fd` with a 100ms timeout so `keep_running` is re-checked
   * promptly after a stop signal arrives.
   */
  while (keep_running && server->listen_fd >= 0) {
    struct pollfd pfd;
    pfd.fd = server->listen_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = poll(&pfd, 1, 100);
    if (ready > 0 && (pfd.revents & POLLIN)) {
      int client_fd = accept(server->listen_fd, NULL, NULL);
      if (client_fd >= 0) {
        handle_client_fd(server, client_fd);
      }
    }
  }

  $(server);
  stop_server();
}

static char* to_chars(void) {
  return "<Server>";
}

/**
 * Constructor.
 */
Server* ch_new_Server(int port) {
  Server *server = malloc(sizeof(Server));
  server->generic.magic = CH_MAGIC;
  server->generic.type_val = SERVER;
  server->generic.prev_self = NULL;
  server->generic.next_self = NULL;
  server->generic.refs = 0;
  server->port = port;
  server->listen_fd = -1;
  server->route_count = 0;
  server->route_cap = 8;
  server->routes = malloc(sizeof(RouteEntry) * server->route_cap);

  server->bind = bind_server;
  server->generic.bind = (void* (*)(GenericEl*))bind_server;
  server->generic.save = save;
  server->generic.del = del;
  server->generic.to_chars = to_chars;
  server->get = get_route;
  server->post = post_route;
  server->put = put_route;
  server->delete_route = delete_route;
  server->dispatch = dispatch_request;
  server->request = http_request;
  server->start = start_server;
  server->stop = stop_server;
  server->listen = listen_forever;

  self((GenericEl*)server);

  return server;
}
