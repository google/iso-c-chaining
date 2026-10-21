# REST Server & SQLite Benchmark (`iso-c-chaining` vs. Django)

This directory benchmarks the `Server` + `Db` stack from
`examples/chain_server.c` against an equivalent Django application backed by an
in-memory SQLite database (`:memory:`).

Both benchmarks initialize the same `users` table (seeded with `Alice` and
`Bob`), expose the same 5 REST endpoints (`GET /users`, `GET /users/:id`, `POST
/users`, `PUT /users/:id`, `DELETE /users/:id`), and measure in-process request
dispatch (`server->dispatch()` vs. Django's `WSGIHandler`) across 10,000
requests per workload:

1.  **`GET /users?role=admin&min_score=90`** (10,000 requests): URL query
    parsing, numeric coercion, parameterized `SELECT` with filtering and `ORDER
    BY`, and JSON array serialization.
2.  **`GET /users/1`** (10,000 requests): URL `:id` segment capture,
    parameterized single-row `SELECT`, and JSON object serialization.
3.  **5-Step CRUD Mix** (2,000 full cycles = 10,000 requests): `POST /users`
    (JSON body parse + `INSERT` + `SELECT`), `GET
    /users?role=admin&min_score=90`, `PUT /users/:id` (JSON body parse +
    `UPDATE` + `SELECT`), `GET /users/:id`, and `DELETE /users/:id`.

## Why Did We Do This?

Normally, benchmarking C against a high-level Python web framework like Django
is comparing two completely different programming models: hundreds of lines of
stack-allocated C structs, manual `sqlite3_bind_*` calls, and hand-rolled
`snprintf` buffers against a 10-line Python view.

With `iso-c-chaining`, however, the C code and the Python/Django code are
written at nearly the **exact same level of abstraction** (see the
[side-by-side comparison below](#side-by-side-code-comparison)):

-   **Declarative route registration**: `server->get("/users/:id", get_user)`
    mirrors `path("users/<int:user_id>", user_detail)`.
-   **6-to-8-line handlers**: Route handlers receive a pre-parsed request map
    (`req` with `params`, `query`, and parsed JSON `body`), pass that map
    directly into parameterized queries (`$(db)->params(...)->query(...)`), and
    return a dynamic collection that the server serializes to JSON and frees
    automatically.
-   **Dynamic boxing and reference counting on both sides**: Rather than using
    fixed C structs, `iso-c-chaining` dynamically boxes every string, number,
    boolean, array (`Slice`), and map (`Hash`) on the heap with runtime type
    tags (`ChGenericEl`) and reference counting (`refs`)—the same conceptual
    machinery (`PyObject`, `dict`, `list`, `ob_refcnt`) that Python uses under
    the hood.

Because both implementations express the endpoints in the same style and do the
same kind of dynamic object boxing per request, it makes sense to ask: *how much
throughput do you get when that high-level, fluent programming model is
implemented directly in C?*

## Running the Benchmarks

### 1. `iso-c-chaining` (`benchmarks/server_bench.c`)

Build in `Release` mode (`-O3`) with CMake and run `server_bench`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target server_bench
./build/server_bench
```

### 2. Django (`benchmarks/django_bench.py`)

Install Django (if not already present in your Python environment) and run
`django_bench.py`:

```bash
pip install django
python3 benchmarks/django_bench.py
```

## Sample Results

Measured on a single core (`gcc -O3` vs. Django 3.2 with `DEBUG=False` and
`MIDDLEWARE=[]`):

| Workload (10,000 requests)            | `iso-c-chaining` | Django (Raw     | Django (Django  | Speedup   | Speedup   |
:                                       : (`-O3`)          : SQL +           : ORM +           : vs.       : vs.       :
:                                       :                  : `JsonResponse`) : `JsonResponse`) : Django    : Django    :
:                                       :                  :                 :                 : Raw SQL   : ORM       :
| :------------------------------------ | ---------------: | --------------: | --------------: | --------: | --------: |
| **`GET                                | **58.2 ms**      | **518.0 ms**    | **2,021.4 ms**  | **8.9x**  | **34.7x** |
: /users?role=admin&min_score=90`**     : (`171,741        : (`19,304        : (`4,947 req/s`, :           :           :
:                                       : req/s`, `5.82    : req/s`, `51.80  : `202.14         :           :           :
:                                       : µs/req`)         : µs/req`)        : µs/req`)        :           :           :
| **`GET /users/1`**                    | **37.5 ms**      | **378.0 ms**    | **1,315.3 ms**  | **10.1x** | **35.1x** |
:                                       : (`266,732        : (`26,453        : (`7,603 req/s`, :           :           :
:                                       : req/s`, `3.75    : req/s`, `37.80  : `131.53         :           :           :
:                                       : µs/req`)         : µs/req`)        : µs/req`)        :           :           :
| **5-Step CRUD Mix                     | **57.6 ms**      | **499.6 ms**    | **1,735.0 ms**  | **8.7x**  | **30.1x** |
: (`POST`/`GET`/`PUT`/`GET`/`DELETE`)** : (`173,683        : (`20,016        : (`5,764 req/s`, :           :           :
:                                       : req/s`, `5.76    : req/s`, `49.96  : `173.49         :           :           :
:                                       : µs/req`)         : µs/req`)        : µs/req`)        :           :           :

Even though `iso-c-chaining` dynamically boxes route parameters, query
parameters, JSON payloads, and SQLite result rows into reference-counted `Hash`,
`Slice`, `String`, and `Number` objects (and prepares/finalizes every SQL
statement without a statement cache), each end-to-end request completes in
**3.75–5.82 µs**.

## What This Does (and Doesn't) Measure

This benchmark is an **ergonomic equivalence comparison**, not a production web
server benchmark.

### What it measures

-   **In-process request dispatch (`server->dispatch()` vs. `WSGIHandler`)**:
    Route matching, URL path/query parameter extraction, JSON request body
    parsing, parameterized SQLite execution, dynamic row construction, JSON
    response serialization, and per-request memory cleanup, without kernel TCP
    socket handshake noise.
-   **Both ORM and Raw SQL in Django**: Including **Django Raw SQL (`8.7x–10.1x`
    gap)** alongside **Django ORM (`30.1x–35.1x` gap)** separates the cost of
    Django's `QuerySet` AST construction and SQL compiler from the cost of
    routing, parameter binding, SQLite execution, and JSON serialization.
-   **Stripped middleware**: Django is configured with `DEBUG=False` and
    `MIDDLEWARE=[]` so default CSRF, session, authentication, and security
    middleware do not skew the comparison.

### Where the comparison favors `iso-c-chaining` (what Django does that `Server` doesn't)

-   **Full HTTP/WSGI request & response lifecycle**: Even with `MIDDLEWARE=[]`,
    Django's `WSGIHandler` constructs a full `WSGIRequest` (parsing headers,
    `META`, content-type charsets, and multi-valued `QueryDict`s), fires
    `request_started`/`request_finished` signals (including database connection
    health checks), and builds a full `JsonResponse` with HTTP header validation
    and streaming iterators. `Server::dispatch()` takes `(method, path, query,
    body)` directly without parsing HTTP headers, cookies, or charsets.
-   **Router features**: Django's `URLResolver` supports nested `include()`
    trees, regex/type converters (`<int:user_id>`), and named URL reversal,
    whereas `Server` scans a flat route table and splits path segments on `'/'`.
-   **ORM query generation**: In the ORM column, `User.objects.filter(...)`
    dynamically builds a query AST and compiles SQL on every call, whereas
    `Db::query()` takes a SQL string directly (making the **Django Raw SQL**
    column the closer baseline to `Db::query()`).

### Where the comparison handicaps `iso-c-chaining` (what Python/Django optimizes that `Server` + `Db` doesn't)

-   **No SQLite prepared-statement cache in `ext/db.c`**: Python's built-in
    `_sqlite3` C driver caches compiled `sqlite3_stmt*` handles in an LRU cache
    (`cached_statements=128` by default), so Django only parses and plans each
    SQL query once and reuses the compiled bytecode for the remaining 9,999
    requests. `ext/db.c` has no statement cache—it calls `sqlite3_prepare_v2()`
    (lexing, parsing, and query-planning the SQL string from scratch) and
    `sqlite3_finalize()` on **every single query**.
-   **Uncompiled route patterns and raw `malloc`/`free`**: `Server`'s
    `match_route_pattern()` re-splits both the route pattern and the request
    path on `'/'` into freshly `malloc`'d `Slice` and `String` objects on every
    candidate route check (whereas Django pre-compiles its URL patterns once at
    startup), and `iso-c-chaining` calls raw libc `malloc()`/`free()` for every
    boxed primitive rather than using a pooled small-object allocator like
    CPython's `pymalloc`.

--------------------------------------------------------------------------------

## Side-by-Side Code Comparison

Because `Server` automatically parses JSON request bodies, coerces URL/query
parameters into a `Hash`, and serializes returned objects to JSON—and `Db` binds
`:named` SQL parameters directly from a `Hash`—the C handlers are as concise as
(or shorter than) their Django counterparts.

### Route Registration

**`iso-c-chaining` (`benchmarks/server_bench.c`)**

```c
Server *server = create(Server, 8080)
    ->get("/users", list_users)
    ->get("/users/:id", get_user)
    ->post("/users", create_user)
    ->put("/users/:id", update_user)
    ->delete_route("/users/:id", delete_user)->$$;
```

**Django (`benchmarks/django_bench.py`)**

```python
urlpatterns = [
    path("users", orm_users),
    path("users/<int:user_id>", orm_user_detail),
]
```

### `GET /users?role=admin&min_score=90`

**`iso-c-chaining`**

```c
static GenericEl* list_users(Hash *req) {
  return (GenericEl*)$(db)
      ->params($(req)->get("query"))
      ->query("SELECT id, name, role, score FROM users "
              "WHERE (:role IS NULL OR role = :role) "
              "  AND (:min_score IS NULL OR score >= :min_score) "
              "ORDER BY score DESC");
}
```

**Django (Raw SQL)**

```python
def sql_users(request):
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
```

**Django (ORM)**

```python
def orm_users(request):
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
```

### `GET /users/:id`

**`iso-c-chaining`**

```c
static GenericEl* get_user(Hash *req) {
  return (GenericEl*)$(db)
      ->params($(req)->get("params"))
      ->query_one("SELECT id, name, role, score FROM users "
                  "WHERE id = :id");
}
```

**Django (Raw SQL)**

```python
def sql_user_detail(request, user_id):
  with connection.cursor() as cursor:
    cursor.execute(
        "SELECT id, name, role, score FROM users WHERE id = ?", [user_id]
    )
    row = cursor.fetchone()
  return JsonResponse(
      {"id": row[0], "name": row[1], "role": row[2], "score": row[3]}
  )
```

**Django (ORM)**

```python
def orm_user_detail(request, user_id):
  user = User.objects.get(id=user_id)
  return JsonResponse({
      "id": user.id,
      "name": user.name,
      "role": user.role,
      "score": user.score,
  })
```

### `POST /users`

**`iso-c-chaining`**

```c
static GenericEl* create_user(Hash *req) {
  Hash *body = as(Hash, $(req)->get("body"));
  return (GenericEl*)$(db)
      ->insert("users", body)
      ->params(body)
      ->query_one("SELECT id, name, role, score FROM users "
                  "WHERE name = :name ORDER BY id DESC");
}
```

**Django (Raw SQL)**

```python
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
```

**Django (ORM)**

```python
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
```
