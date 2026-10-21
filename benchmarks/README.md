# REST Server & SQLite Benchmark (`iso-c-chaining` vs. Express.js & Django)

This directory benchmarks the `Server` + `Db` stack from
`examples/chain_server.c` against equivalent **Node.js + Express.js** and
**Python + Django** applications backed by an in-memory SQLite database
(`:memory:`).

All three implementations initialize the same `users` table (seeded with `Alice`
and `Bob`), expose the same 5 REST endpoints (`GET /users`, `GET /users/:id`,
`POST /users`, `PUT /users/:id`, `DELETE /users/:id`), and measure in-process
request dispatch (`server->dispatch()` vs. Express's `app.handle()` vs. Django's
`WSGIHandler`) across 10,000 requests per workload:

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

Normally, benchmarking C against JavaScript (Express.js) or Python (Django) web
frameworks compares two completely different programming models: hundreds of
lines of stack-allocated C structs, manual `sqlite3_bind_*` calls, and
hand-rolled `snprintf` buffers against a 10-line dynamic handler.

With `iso-c-chaining`, however, the C code, the Express.js code, and the Django
code are written at nearly the **exact same level of abstraction** (see the
[side-by-side comparison below](#side-by-side-code-comparison)):

-   **Declarative chained route registration**: `server->get("/users/:id",
    get_user)->post("/users", create_user)` is a 1:1 mirror of Express's
    `app.get('/users/:id', getUser).post('/users', createUser)` and Django's
    `urlpatterns`.
-   **6-to-8-line handlers**: Route handlers receive a pre-parsed request map
    (`req` with `params`, `query`, and parsed JSON `body`), pass that map
    directly into parameterized queries (`$(db)->params(...)->query(...)`), and
    return a dynamic collection that the server serializes to JSON and frees
    automatically.
-   **Dynamic boxing and reference counting**: Rather than using fixed C
    structs, `iso-c-chaining` dynamically boxes every string, number, boolean,
    array (`Slice`), and map (`Hash`) on the heap with runtime type tags
    (`ChGenericEl`) and reference counting (`refs`)—the same conceptual
    machinery that dynamic languages use under the hood.

Because all three implementations express the endpoints in the same style and
perform dynamic object creation per request, it makes sense to ask: *how much
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

### 2. Node.js + Express (`benchmarks/express_bench.js`)

Using Node.js v22.5+ (which includes the built-in `node:sqlite` module), install
`express` and run `express_bench.js`:

```bash
npm install express
node --no-warnings benchmarks/express_bench.js
```

### 3. Python + Django (`benchmarks/django_bench.py`)

Install Django (if not already present in your Python environment) and run
`django_bench.py`:

```bash
pip install django
python3 benchmarks/django_bench.py
```

## Sample Results

Measured on a single core (`gcc -O3` vs. Node.js v24.15.0 + Express 4.20 vs.
Django 3.2 with `DEBUG=False` and `MIDDLEWARE=[]`):

| Workload (10,000 requests)            | `iso-c-chaining` | Express 4.20  | Express 4.20     | Django (Raw     | Django (Django  |
:                                       : (`-O3`, uncached : (`db.prepare` : (Pre-compiled    : SQL +           : ORM +           :
:                                       : SQL)             : per query)    : `StatementSync`) : `JsonResponse`) : `JsonResponse`) :
| :------------------------------------ | ---------------: | ------------: | ---------------: | --------------: | --------------: |
| **`GET                                | **58.2 ms**      | **166.3 ms**  | **74.3 ms**      | **518.0 ms**    | **2,021.4 ms**  |
: /users?role=admin&min_score=90`**     : (`171,741        : (`60,122      : (`134,514        : (`19,304        : (`4,947 req/s`, :
:                                       : req/s`, `5.82    : req/s`,       : req/s`, `7.43    : req/s`, `51.80  : `202.14         :
:                                       : µs/req`)         : `16.63        : µs/req`)         : µs/req`)        : µs/req`)        :
:                                       :                  : µs/req`)      :                  :                 :                 :
| **`GET /users/1`**                    | **37.5 ms**      | **75.0 ms**   | **47.5 ms**      | **378.0 ms**    | **1,315.3 ms**  |
:                                       : (`266,732        : (`133,274     : (`210,546        : (`26,453        : (`7,603 req/s`, :
:                                       : req/s`, `3.75    : req/s`, `7.50 : req/s`, `4.75    : req/s`, `37.80  : `131.53         :
:                                       : µs/req`)         : µs/req`)      : µs/req`)         : µs/req`)        : µs/req`)        :
| **5-Step CRUD Mix                     | **57.6 ms**      | **132.1 ms**  | **101.7 ms**     | **499.6 ms**    | **1,735.0 ms**  |
: (`POST`/`GET`/`PUT`/`GET`/`DELETE`)** : (`173,683        : (`75,687      : (`98,329 req/s`, : (`20,016        : (`5,764 req/s`, :
:                                       : req/s`, `5.76    : req/s`,       : `10.17 µs/req`)  : req/s`, `49.96  : `173.49         :
:                                       : µs/req`)         : `13.21        :                  : µs/req`)        : µs/req`)        :
:                                       :                  : µs/req`)      :                  :                 :                 :
| **Relative Speed (`iso-c-chaining`    | **1.0x           | **2.0x to     | **1.3x to 1.8x   | **8.7x to 10.1x | **30.1x to      |
: speedup)**                            : (baseline)**     : 2.9x faster** : faster**         : faster**        : 35.1x faster**  :

Even though `iso-c-chaining` dynamically boxes route parameters, query
parameters, JSON payloads, and SQLite result rows into reference-counted `Hash`,
`Slice`, `String`, and `Number` objects (and prepares/finalizes every SQL
statement without a statement cache), each end-to-end request completes in
**3.75–5.82 µs**.

## What This Does (and Doesn't) Measure

This benchmark is an **ergonomic equivalence comparison**, not a production web
server benchmark.

### What it measures

-   **In-process request dispatch (`server->dispatch()` vs. `app.handle()` vs.
    `WSGIHandler`)**: Route matching, URL path/query parameter extraction, JSON
    request body parsing, parameterized SQLite execution, dynamic row
    construction, JSON response serialization, and per-request memory cleanup,
    without kernel TCP socket handshake noise.
-   **Both uncached and cached SQLite statements**: In Express.js, we measure
    both `db.prepare(sql)` per query (matching `ext/db.c`'s uncached
    `sqlite3_prepare_v2()` per query) and pre-compiled `StatementSync` objects.
    In Django, we measure both Raw SQL (8.7x to 10.1x gap) and the Django ORM
    (30.1x to 35.1x gap).
-   **Stripped middleware**: Express has `etag` and `x-powered-by` disabled, and
    Django is configured with `DEBUG=False` and `MIDDLEWARE=[]` so default CSRF,
    session, authentication, and security middleware do not skew the comparison.

### Where the comparison favors `iso-c-chaining` (what Express & Django do that `Server` doesn't)

-   **Full HTTP/WSGI request & response lifecycle**: Express's `app.handle()`
    and Django's `WSGIHandler` construct full `IncomingMessage`/`ServerResponse`
    or `WSGIRequest`/`JsonResponse` objects (supporting headers, cookies,
    content negotiation, middleware stacks, and streaming responses).
    `Server::dispatch()` takes `(method, path, query, body)` directly without
    parsing HTTP headers, cookies, or charsets.
-   **Router features**: Express (`path-to-regexp`) and Django (`URLResolver`)
    support sub-routers, regexes, and middleware pipelines, whereas `Server`
    scans a flat route table and splits path segments on `'/'`.
-   **ORM query generation (Django ORM column)**: In the Django ORM column,
    `User.objects.filter(...)` dynamically builds a query AST and compiles SQL
    on every call, whereas `Db::query()` takes a SQL string directly.

### Where the comparison handicaps `iso-c-chaining` (what Node.js & Python optimize that `Server` + `Db` doesn't)

-   **No SQLite prepared-statement cache in `ext/db.c`**: Both Python's built-in
    `_sqlite3` driver (`cached_statements=128` LRU cache) and the pre-compiled
    Express `StatementSync` suite compile each SQL query once and reuse the
    compiled SQLite bytecode for all 10,000 requests. `ext/db.c` has no
    statement cache—it calls `sqlite3_prepare_v2()` (lexing, parsing, and
    query-planning the SQL string from scratch) and `sqlite3_finalize()` on
    **every single query**, yet still outperforms pre-compiled `StatementSync`
    in Node.js by **1.3x–1.8x** and cached raw SQL in Django by **8.7x–10.1x**.
-   **Uncompiled route patterns and raw `malloc`/`free`**: `Server`'s
    `match_route_pattern()` re-splits both the route pattern and the request
    path on `'/'` into freshly `malloc`'d `Slice` and `String` objects on every
    candidate route check (whereas Express and Django pre-compile their route
    patterns once at startup), and `iso-c-chaining` calls raw libc
    `malloc()`/`free()` for every boxed primitive rather than using V8's bump
    nursery or CPython's `pymalloc` arena allocator.

--------------------------------------------------------------------------------

## Side-by-Side Code Comparison

Because `Server` automatically parses JSON request bodies, coerces URL/query
parameters into a `Hash`, and serializes returned objects to JSON—and `Db` binds
`:named` SQL parameters directly from a `Hash`—the C handlers look almost
identical to Express.js and are shorter than Django's.

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

**Express.js (`benchmarks/express_bench.js`)**

```javascript
app.get('/users', listUsers)
   .get('/users/:id', getUser)
   .post('/users', createUser)
   .put('/users/:id', updateUser)
   .delete('/users/:id', deleteUser);
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

**Express.js + `node:sqlite`**

```javascript
app.get('/users', (req, res) => {
  const role = req.query.role ?? null;
  const minScore = req.query.min_score !== undefined ? Number(req.query.min_score) : null;
  res.json(
    db.prepare(
      'SELECT id, name, role, score FROM users ' +
      'WHERE (:role IS NULL OR role = :role) ' +
      '  AND (:min_score IS NULL OR score >= :min_score) ' +
      'ORDER BY score DESC'
    ).all({ ':role': role, ':min_score': minScore })
  );
});
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

**Express.js + `node:sqlite`**

```javascript
app.get('/users/:id', (req, res) => {
  res.json(
    db.prepare('SELECT id, name, role, score FROM users WHERE id = :id')
      .get({ ':id': Number(req.params.id) })
  );
});
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

**Express.js + `node:sqlite`**

```javascript
app.post('/users', (req, res) => {
  const { name, role, score } = req.body;
  db.prepare('INSERT INTO users (name, role, score) VALUES (:name, :role, :score)')
    .run({ ':name': name, ':role': role, ':score': score });
  res.json(
    db.prepare('SELECT id, name, role, score FROM users WHERE name = :name ORDER BY id DESC LIMIT 1')
      .get({ ':name': name })
  );
});
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
