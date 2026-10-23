#!/usr/bin/env python3
# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Django comparison benchmark matching `benchmarks/server_bench.c`.

Runs the same 5 `/users` REST routes backed by in-memory SQLite (`:memory:`)
using both the Django ORM and raw parameterized SQL via `django.db.connection`.

Usage:
  pip install django
  python3 benchmarks/django_bench.py
"""

import io
import json
import time

import django
from django.conf import settings
from django.core.handlers.wsgi import WSGIHandler
from django.db import connection
from django.db import models
from django.http import JsonResponse
from django.urls import path

settings.configure(
    DEBUG=False,
    ALLOWED_HOSTS=["*"],
    SECRET_KEY="benchmark-secret-key",
    ROOT_URLCONF=__name__,
    MIDDLEWARE=[],
    INSTALLED_APPS=["__main__"],
    DATABASES={
        "default": {
            "ENGINE": "django.db.backends.sqlite3",
            "NAME": ":memory:",
        }
    },
)
django.setup()


class User(models.Model):
  """User record matching the `users` table in `benchmarks/server_bench.c`."""

  name = models.TextField()
  role = models.TextField()
  score = models.FloatField(null=True)

  class Meta:
    app_label = "__main__"
    db_table = "users"


def orm_users(request):
  """Handles `GET /users` and `POST /users` using the Django ORM."""
  if request.method == "GET":
    queryset = User.objects.all()
    role = request.GET.get("role")
    min_score = request.GET.get("min_score")
    if role is not None:
      queryset = queryset.filter(role=role)
    if min_score is not None:
      queryset = queryset.filter(score__gte=float(min_score))
    rows = list(
        queryset.order_by("-score").values("id", "name", "role", "score")
    )
    return JsonResponse(rows, safe=False)

  body = json.loads(request.body.decode("utf-8"))
  user = User.objects.create(
      name=body["name"], role=body["role"], score=body.get("score")
  )
  return JsonResponse({
      "id": user.id,
      "name": user.name,
      "role": user.role,
      "score": user.score,
  })


def orm_user_detail(request, user_id):
  """Handles `GET`, `PUT`, and `DELETE` on `/users/<id>` via the Django ORM."""
  if request.method == "GET":
    user = User.objects.get(id=user_id)
    return JsonResponse({
        "id": user.id,
        "name": user.name,
        "role": user.role,
        "score": user.score,
    })

  if request.method == "PUT":
    body = json.loads(request.body.decode("utf-8"))
    user = User.objects.get(id=user_id)
    if "name" in body:
      user.name = body["name"]
    if "role" in body:
      user.role = body["role"]
    if "score" in body:
      user.score = body["score"]
    user.save()
    return JsonResponse({
        "id": user.id,
        "name": user.name,
        "role": user.role,
        "score": user.score,
    })

  User.objects.filter(id=user_id).delete()
  return JsonResponse({"deleted": True, "id": user_id})


def sql_users(request):
  """Handles `GET /sql/users` and `POST /sql/users` using parameterized SQL."""
  if request.method == "GET":
    role = request.GET.get("role")
    min_score = request.GET.get("min_score")
    min_score_val = float(min_score) if min_score is not None else None
    with connection.cursor() as cursor:
      cursor.execute(
          "SELECT id, name, role, score FROM users "
          "WHERE (? IS NULL OR role = ?) "
          "  AND (? IS NULL OR score >= ?) "
          "ORDER BY score DESC",
          [role, role, min_score_val, min_score_val],
      )
      rows = [
          {"id": row[0], "name": row[1], "role": row[2], "score": row[3]}
          for row in cursor.fetchall()
      ]
    return JsonResponse(rows, safe=False)

  body = json.loads(request.body.decode("utf-8"))
  with connection.cursor() as cursor:
    cursor.execute(
        "INSERT INTO users (name, role, score) VALUES (?, ?, ?)",
        [body["name"], body["role"], body["score"]],
    )
    cursor.execute(
        "SELECT id, name, role, score FROM users "
        "WHERE name = ? ORDER BY id DESC LIMIT 1",
        [body["name"]],
    )
    row = cursor.fetchone()
  return JsonResponse(
      {"id": row[0], "name": row[1], "role": row[2], "score": row[3]}
  )


def sql_user_detail(request, user_id):
  """Handles `GET`, `PUT`, and `DELETE` on `/sql/users/<id>` using raw SQL."""
  if request.method == "GET":
    with connection.cursor() as cursor:
      cursor.execute(
          "SELECT id, name, role, score FROM users WHERE id = ?", [user_id]
      )
      row = cursor.fetchone()
    return JsonResponse(
        {"id": row[0], "name": row[1], "role": row[2], "score": row[3]}
    )

  if request.method == "PUT":
    body = json.loads(request.body.decode("utf-8"))
    with connection.cursor() as cursor:
      cursor.execute(
          "UPDATE users SET "
          "  name = COALESCE(?, name), "
          "  role = COALESCE(?, role), "
          "  score = COALESCE(?, score) "
          "WHERE id = ?",
          [body.get("name"), body.get("role"), body.get("score"), user_id],
      )
      cursor.execute(
          "SELECT id, name, role, score FROM users WHERE id = ?", [user_id]
      )
      row = cursor.fetchone()
    return JsonResponse(
        {"id": row[0], "name": row[1], "role": row[2], "score": row[3]}
    )

  with connection.cursor() as cursor:
    cursor.execute("DELETE FROM users WHERE id = ?", [user_id])
  return JsonResponse({"deleted": True, "id": user_id})


urlpatterns = [
    path("users", orm_users),
    path("users/<int:user_id>", orm_user_detail),
    path("sql/users", sql_users),
    path("sql/users/<int:user_id>", sql_user_detail),
]

WSGI_APP = WSGIHandler()


def call_wsgi(method, path_info, query_string="", body_str=None):
  """Dispatches an in-process WSGI request through Django's `WSGIHandler`."""
  body_bytes = body_str.encode("utf-8") if body_str else b""
  environ = {
      "REQUEST_METHOD": method,
      "PATH_INFO": path_info,
      "QUERY_STRING": query_string,
      "SERVER_NAME": "127.0.0.1",
      "SERVER_PORT": "8080",
      "wsgi.version": (1, 0),
      "wsgi.url_scheme": "http",
      "wsgi.input": io.BytesIO(body_bytes),
      "CONTENT_LENGTH": str(len(body_bytes)),
      "CONTENT_TYPE": "application/json",
  }

  def start_response(status, headers, exc_info=None):
    del status, headers, exc_info

  return b"".join(WSGI_APP(environ, start_response))


def run_suite(label, prefix):
  """Runs the 3 benchmark workloads against `prefix` and prints timings."""
  num_reqs = 10000
  crud_cycles = 2000
  total_bytes = 0

  t0 = time.perf_counter()
  for _ in range(num_reqs):
    total_bytes += len(
        call_wsgi("GET", f"{prefix}/users", "role=admin&min_score=90")
    )
  dt_list = time.perf_counter() - t0

  t0 = time.perf_counter()
  for _ in range(num_reqs):
    total_bytes += len(call_wsgi("GET", f"{prefix}/users/1"))
  dt_get = time.perf_counter() - t0

  t0 = time.perf_counter()
  for _ in range(crud_cycles):
    created_bytes = call_wsgi(
        "POST",
        f"{prefix}/users",
        "",
        '{"name":"Charlie","role":"admin","score":95.5}',
    )
    total_bytes += len(created_bytes)
    new_id = json.loads(created_bytes)["id"]
    detail_path = f"{prefix}/users/{new_id}"

    total_bytes += len(
        call_wsgi("GET", f"{prefix}/users", "role=admin&min_score=90")
    )
    total_bytes += len(call_wsgi("PUT", detail_path, "", '{"score":99.75}'))
    total_bytes += len(call_wsgi("GET", detail_path))
    total_bytes += len(call_wsgi("DELETE", detail_path))
  dt_crud = time.perf_counter() - t0

  print(f"{label}:")
  print(
      f"  GET /users?role=admin&min_score=90 : {dt_list * 1000.0:7.2f} ms for"
      f" {num_reqs} reqs ({num_reqs / dt_list:9.0f} req/s,"
      f" {(dt_list * 1e6) / num_reqs:6.2f} us/req)"
  )
  print(
      f"  GET /users/1                       : {dt_get * 1000.0:7.2f} ms for"
      f" {num_reqs} reqs ({num_reqs / dt_get:9.0f} req/s,"
      f" {(dt_get * 1e6) / num_reqs:6.2f} us/req)"
  )
  print(
      f"  5-step CRUD mix (POST/GET/PUT/DEL) : {dt_crud * 1000.0:7.2f} ms for"
      f" {crud_cycles * 5} reqs ({(crud_cycles * 5) / dt_crud:9.0f} req/s,"
      f" {(dt_crud * 1e6) / (crud_cycles * 5):6.2f} us/req)"
  )
  print(f"  (serialized JSON bytes: {total_bytes})")


def main():
  """Initializes the in-memory `users` table and executes both Django suites."""
  with connection.cursor() as cursor:
    cursor.execute(
        "CREATE TABLE users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  role TEXT NOT NULL,"
        "  score REAL"
        ")"
    )
  User.objects.create(name="Alice", role="admin", score=98.25)
  User.objects.create(name="Bob", role="viewer", score=82.0)

  version = django.get_version()
  run_suite(f"Django {version} (WSGI + Django ORM)", "")
  run_suite(f"Django {version} (WSGI + Raw parameterized SQL)", "/sql")


if __name__ == "__main__":
  main()
