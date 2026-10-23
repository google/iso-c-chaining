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

#ifndef CHAIN_SERVER_H_
#define CHAIN_SERVER_H_

#include "core/hash.h"
#include "core/status_or.h"
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ch_use(Server, SERVER, (int))

/*
 * Callback signature for HTTP route handlers. Receives a `Hash *req` containing
 * `"method"` (`String`), `"path"` (`String`), `"params"` (`Hash` of `:param`
 * path segments), `"query"` (`Hash` of query string parameters), and `"body"`
 * (parsed JSON `GenericEl*`, or `Null` if empty). May return any `GenericEl*`
 * (serialized as JSON `200 OK`) or a `StatusOr*` (serialized as `200 OK` if
 * `ok()`, or `400`/`404` `{"error": ...}` on failure).
 */
typedef ChGenericEl* (*ChRouteHandler)(ChHash *req);

#ifndef CHAIN_NO_SHORT_NAMES
typedef ChRouteHandler RouteHandler;
#endif

/*
 * Zero-dependency POSIX HTTP/1.1 JSON REST server supporting `:param` route
 * matching, query/body parsing, in-memory `dispatch()`, loopback socket
 * `request()` testing, and interactive `listen()`.
 */
struct SERVER {
  ChGenericEl generic;
  int port;
  int listen_fd;
  void *routes;
  int route_count;
  int route_cap;

  /* Binds this `Server` as the active instance on the `Server` `self` stack. */
  ChServer* (*bind)(ChGenericEl*);

  /* Registers a `GET` route handler for `pattern` (e.g. `"/users/:id"`). */
  ChServer* (*get)(char*, ChRouteHandler);

  /* Registers a `POST` route handler for `pattern`. */
  ChServer* (*post)(char*, ChRouteHandler);

  /* Registers a `PUT` route handler for `pattern`. */
  ChServer* (*put)(char*, ChRouteHandler);

  /* Registers a `DELETE` route handler for `pattern`. */
  ChServer* (*delete_route)(char*, ChRouteHandler);

  /*
   * Dispatches a request in-memory (`method`, `path`, `query`, `body`) without
   * opening a socket, returning the handler result wrapped in a `StatusOr*`.
   */
  ChStatusOr* (*dispatch)(char*, char*, char*, char*);

  /*
   * Sends a real HTTP/1.1 request (`method`, `uri`, `body`) over a loopback
   * TCP socket to this `Server` (requires `start()` first) and parses the JSON
   * response back into a `StatusOr*`.
   */
  ChStatusOr* (*request)(char*, char*, char*);

  /*
   * Binds and listens on `127.0.0.1:port` (if `port == 0`, resolves the
   * OS-assigned ephemeral port into `server->port`) without blocking.
   */
  ChServer* (*start)(void);

  /* Closes the listening socket if open. */
  void (*stop)(void);

  /*
   * Starts the server (if not already started) and runs a blocking `poll()`
   * loop accepting HTTP/1.1 connections until `SIGINT` or `SIGTERM`.
   */
  void (*listen)(void);
};

#ifdef __cplusplus
}
#endif

#endif  /* CHAIN_SERVER_H_ */
