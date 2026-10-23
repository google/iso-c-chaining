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
 * Example Linux System Inspector Model Context Protocol (MCP) server using
 * fluent C chaining (`Mcp` + `String::split` + `Slice` + `Hash`).
 *
 * Wraps standard Linux CLI tools (`/bin/uname`, `/bin/df`, `/bin/ps`), parses
 * their tabular text output into structured JSON objects, and exposes them over
 * stdio JSON-RPC 2.0:
 *   - `system_info`: kernel name, hostname, release, and architecture (`uname`)
 *   - `disk_usage`: filesystem capacity and free space for a path (`df -P -k`)
 *   - `top_processes`: top CPU-consuming processes (`ps -eo ... --sort=-%cpu`)
 *
 * 1. Build and run the stdio MCP server:
 *
 *      cmake -B build && cmake --build build
 *      ./build/chain_mcp
 *
 * 2. Test from a terminal by piping JSON-RPC 2.0 requests to stdin:
 *
 *      printf '%s\n' \
 *        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
 *        '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 *        '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"system_info","arguments":{}}}' \
 *        '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"disk_usage","arguments":{"path":"/tmp"}}}' \
 *        | ./build/chain_mcp
 *
 * 3. Run the non-interactive self-test suite:
 *
 *      ./build/chain_mcp --test
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/boolean.h"
#include "core/hash.h"
#include "core/number.h"
#include "core/slice.h"
#include "core/status_or.h"
#include "core/string.h"
#include "core/types.h"
#include "demos/mcp.h"
#include "core/test.h"

/*
 * Executes `argv[0]` directly via `fork()` + `execv()` (without invoking a
 * shell) and captures its combined stdout/stderr into a `StatusOr*` wrapping a
 * `String*`.
 */
static StatusOr* run_cmd(char *const argv[]) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return create(StatusOr, NULL, "pipe() failed");
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return create(StatusOr, NULL, "fork() failed");
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execv(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  char buf[16384];
  size_t total = 0;
  ssize_t n;
  while (total < sizeof(buf) - 1 &&
         (n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0) {
    total += (size_t)n;
  }
  buf[total] = '\0';
  close(pipefd[0]);

  while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r')) {
    buf[--total] = '\0';
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return create(StatusOr, NULL, buf[0] != '\0' ? buf : "Command failed");
  }

  return create(StatusOr, create(String, buf), NULL);
}

static char* slice_str(Slice *s, int idx) {
  return (s != NULL && idx >= 0 && idx < s->length &&
          is_type(s->elements[idx], STRING)) ?
      ((String*)s->elements[idx])->chars : "";
}

/* Tool: system_info — wraps `/bin/uname -s -n -r -m` */
static GenericEl* system_info_tool(Hash *args) {
  (void)args;
  char *const argv[] = {"/bin/uname", "-s", "-n", "-r", "-m", NULL};
  StatusOr *cmd = run_cmd(argv)->$$;
  if (!$(cmd)->ok()) {
    StatusOr *err = create(StatusOr, NULL, $(cmd)->error());
    release(cmd);
    return (GenericEl*)$(err);
  }

  Slice *parts = $((String*)cmd->val)->split(" \t\n")->$$;
  Hash *info = create(Hash, 4)
      ->put("sysname", create(String, slice_str(parts, 0)))
      ->put("nodename", create(String, slice_str(parts, 1)))
      ->put("release", create(String, slice_str(parts, 2)))
      ->put("machine", create(String, slice_str(parts, 3)));
  release(parts);
  release(cmd);
  return (GenericEl*)$(info);
}

/* Tool: disk_usage — wraps `/bin/df -P -k -- <path>` */
static GenericEl* disk_usage_tool(Hash *args) {
  GenericEl *path_el = args != NULL ? $(args)->get("path") : NULL;
  if (path_el != NULL && path_el->bind(NULL) == path_el) {
    path_el->bind((GenericEl*)-1);
  }
  if (args != NULL && args->bind(NULL) == args) {
    args->bind((GenericEl*)-1);
  }
  char *path = is_type(path_el, STRING) ? ((String*)path_el)->chars : ".";
  if (path[0] == '\0') {
    path = ".";
  }

  char *const argv[] = {"/bin/df", "-P", "-k", "--", path, NULL};
  StatusOr *cmd = run_cmd(argv)->$$;
  if (!$(cmd)->ok()) {
    StatusOr *err = create(StatusOr, NULL, $(cmd)->error());
    release(cmd);
    return (GenericEl*)$(err);
  }

  Slice *lines = $((String*)cmd->val)->split("\n")->$$;
  if (lines->length < 2) {
    release(lines);
    release(cmd);
    return (GenericEl*)create(StatusOr, NULL, "Unexpected df output");
  }

  Slice *cols = $((String*)lines->elements[1])->split(" \t")->$$;
  Hash *usage = create(Hash, 6)
      ->put("filesystem", create(String, slice_str(cols, 0)))
      ->put("total_kb", create(Number, atof(slice_str(cols, 1))))
      ->put("used_kb", create(Number, atof(slice_str(cols, 2))))
      ->put("available_kb", create(Number, atof(slice_str(cols, 3))))
      ->put("capacity", create(String, slice_str(cols, 4)))
      ->put("mounted_on", create(String, slice_str(cols, 5)));
  release(cols);
  release(lines);
  release(cmd);
  return (GenericEl*)$(usage);
}

/* Tool: top_processes — wraps `/bin/ps -eo pid,comm,%cpu,%mem --sort=-%cpu` */
static GenericEl* top_processes_tool(Hash *args) {
  GenericEl *limit_el = args != NULL ? $(args)->get("limit") : NULL;
  if (limit_el != NULL && limit_el->bind(NULL) == limit_el) {
    limit_el->bind((GenericEl*)-1);
  }
  if (args != NULL && args->bind(NULL) == args) {
    args->bind((GenericEl*)-1);
  }
  int limit = is_type(limit_el, NUMBER) ?
      (int)as(Number, limit_el)->value() : 5;
  if (limit_el != NULL && limit_el->bind(NULL) == limit_el) {
    limit_el->bind((GenericEl*)-1);
  }
  if (limit < 1) {
    limit = 1;
  }
  if (limit > 50) {
    limit = 50;
  }

  char *const argv[] = {
      "/bin/ps", "-eo", "pid,comm,%cpu,%mem", "--sort=-%cpu", NULL};
  StatusOr *cmd = run_cmd(argv)->$$;
  if (!$(cmd)->ok()) {
    StatusOr *err = create(StatusOr, NULL, $(cmd)->error());
    release(cmd);
    return (GenericEl*)$(err);
  }

  Slice *lines = $((String*)cmd->val)->split("\n")->$$;
  Slice *procs = create(Slice, limit);
  int i;
  for (i = 1; i < lines->length && (i - 1) < limit; i++) {
    Slice *cols = $((String*)lines->elements[i])->split(" \t")->$$;
    if (cols->length >= 4) {
      $(procs)->add(create(Hash, 4)
          ->put("pid", create(Number, atof(slice_str(cols, 0))))
          ->put("command", create(String, slice_str(cols, 1)))
          ->put("cpu_percent", create(Number, atof(slice_str(cols, 2))))
          ->put("mem_percent", create(Number, atof(slice_str(cols, 3)))));
    }
    release(cols);
  }
  release(lines);
  release(cmd);
  return (GenericEl*)$(procs);
}

static int run_self_test(Mcp *mcp) {
  StatusOr *sys_so = $(mcp)->call_tool("system_info", NULL)->$$;
  Hash *sys_hash = as(Hash, $(sys_so)->value());
  GenericEl *sysname = $(sys_hash)->get("sysname");
  if (sys_hash->bind(NULL) == sys_hash) {
    sys_hash->bind((GenericEl*)-1);
  }

  StatusOr *df_so = $(mcp)->call_tool(
      "disk_usage",
      create(Hash, 1)->put("path", create(String, "/tmp")))->$$;

  StatusOr *ps_so = $(mcp)->call_tool(
      "top_processes",
      create(Hash, 1)->put("limit", create(Number, 3)))->$$;
  Slice *ps_slice = as(Slice, $(ps_so)->value());
  int ps_count = ps_slice->length;
  if (ps_slice->bind(NULL) == ps_slice) {
    ps_slice->bind((GenericEl*)-1);
  }

  int rc = create(Test, "MCP System Inspector Self-Test")
      ->section("initialize handshake")
      ->expect_str(
          $(mcp)->dispatch(
              "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}"),
          "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
          "\"protocolVersion\":\"2024-11-05\","
          "\"capabilities\":{\"tools\":{}},"
          "\"serverInfo\":{\"name\":\"sys-inspector\",\"version\":\"1.0.0\"}}}")
      ->section("system_info tool (uname)")
      ->expect_ok(sys_so)
      ->expect_str(sysname, "Linux")
      ->section("disk_usage tool (df)")
      ->expect_ok(df_so)
      ->expect_err(
          $(mcp)->call_tool(
              "disk_usage",
              create(Hash, 1)->put(
                  "path", create(String, "/nonexistent_chain_mcp_path"))),
          "df:")
      ->section("top_processes tool (ps)")
      ->expect_ok(ps_so)
      ->expect_bool(create(Boolean, ps_count >= 1 && ps_count <= 3), true)
      ->summary();

  release(ps_so);
  release(df_so);
  release(sys_so);
  return rc;
}

int main(int argc, char **argv) {
  int test_mode = (argc > 1 && strcmp(argv[1], "--test") == 0);

  Mcp *mcp = create(Mcp, "sys-inspector", "1.0.0")
      ->tool(
          "system_info",
          "Return kernel name, hostname, kernel release, and architecture "
          "(wraps /bin/uname)",
          NULL,
          system_info_tool)
      ->tool(
          "disk_usage",
          "Return filesystem capacity and available space in KB for a path "
          "(wraps /bin/df -P -k)",
          create(Hash, 2)
              ->put("type", create(String, "object"))
              ->put("properties", create(Hash, 1)
                  ->put("path", create(Hash, 2)
                      ->put("type", create(String, "string"))
                      ->put("description",
                            create(String, "Directory or file path (default '.')")))),
          disk_usage_tool)
      ->tool(
          "top_processes",
          "Return the top CPU-consuming processes on the host "
          "(wraps /bin/ps --sort=-%cpu)",
          create(Hash, 2)
              ->put("type", create(String, "object"))
              ->put("properties", create(Hash, 1)
                  ->put("limit", create(Hash, 2)
                      ->put("type", create(String, "integer"))
                      ->put("description",
                            create(String, "Max processes to return (default 5)")))),
          top_processes_tool)->$$;

  int rc = 0;
  if (test_mode) {
    rc = run_self_test(mcp);
  } else {
    $(mcp)->serve_stdio();
  }

  release(mcp);
  return rc;
}
