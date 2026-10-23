#!/usr/bin/env node
/**
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
 * Express.js + Node.js `node:sqlite` comparison benchmark matching
 * `benchmarks/server_bench.c`.
 *
 * Runs the same 5 `/users` REST routes backed by in-memory SQLite (`:memory:`),
 * measuring both:
 *   1. `db.prepare(sql)` per query (matching `demos/db.c`'s uncached
 *      `sqlite3_prepare_v2` per query).
 *   2. Pre-compiled `StatementSync` handles prepared once at startup.
 *
 * Usage (Node.js v22.5+):
 *   npm install express
 *   node --no-warnings benchmarks/express_bench.js
 */

'use strict';

const express = require('express');
const http = require('node:http');
const {Socket} = require('node:net');
const {performance} = require('node:perf_hooks');
const {DatabaseSync} = require('node:sqlite');

const db = new DatabaseSync(':memory:');
db.exec(`
  CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    role TEXT NOT NULL,
    score REAL
  )
`);
db.prepare('INSERT INTO users (name, role, score) VALUES (?, ?, ?)')
    .run('Alice', 'admin', 98.25);
db.prepare('INSERT INTO users (name, role, score) VALUES (?, ?, ?)')
    .run('Bob', 'viewer', 82.0);

const cachedStmts = {
  list: db.prepare(
      'SELECT id, name, role, score FROM users ' +
      'WHERE (:role IS NULL OR role = :role) ' +
      '  AND (:min_score IS NULL OR score >= :min_score) ' +
      'ORDER BY score DESC'),
  get: db.prepare('SELECT id, name, role, score FROM users WHERE id = :id'),
  insert: db.prepare(
      'INSERT INTO users (name, role, score) VALUES (:name, :role, :score)'),
  getByName: db.prepare(
      'SELECT id, name, role, score FROM users ' +
      'WHERE name = :name ORDER BY id DESC LIMIT 1'),
  update: db.prepare(
      'UPDATE users SET ' +
      '  name = COALESCE(:name, name), ' +
      '  role = COALESCE(:role, role), ' +
      '  score = COALESCE(:score, score) ' +
      'WHERE id = :id'),
  del: db.prepare('DELETE FROM users WHERE id = :id'),
};

/**
 * Builds an Express application configured with the 5 `/users` routes.
 * @param {boolean} useCachedStmts Whether to reuse pre-compiled statements.
 * @return {!Object} Configured Express application instance.
 */
function buildApp(useCachedStmts) {
  const app = express();
  app.disable('etag');
  app.disable('x-powered-by');

  app.use((req, res, next) => {
    if (req.rawBody) {
      req.body = JSON.parse(req.rawBody);
    }
    next();
  });

  app.get(
         '/users',
         (req, res) => {
           const role = req.query.role ?? null;
           const minScore = req.query.min_score !== undefined ?
               Number(req.query.min_score) :
               null;
           const stmt = useCachedStmts ?
               cachedStmts.list :
               db.prepare(
                   'SELECT id, name, role, score FROM users ' +
                   'WHERE (:role IS NULL OR role = :role) ' +
                   '  AND (:min_score IS NULL OR score >= :min_score) ' +
                   'ORDER BY score DESC');
           res.json(stmt.all({':role': role, ':min_score': minScore}));
         })
      .get(
          '/users/:id',
          (req, res) => {
            const id = Number(req.params.id);
            const stmt = useCachedStmts ?
                cachedStmts.get :
                db.prepare(
                    'SELECT id, name, role, score FROM users WHERE id = :id');
            res.json(stmt.get({':id': id}));
          })
      .post(
          '/users',
          (req, res) => {
            const {name, role, score} = req.body;
            if (useCachedStmts) {
              cachedStmts.insert.run(
                  {':name': name, ':role': role, ':score': score});
              res.json(cachedStmts.getByName.get({':name': name}));
            } else {
              db.prepare(
                    'INSERT INTO users (name, role, score) ' +
                    'VALUES (:name, :role, :score)')
                  .run({':name': name, ':role': role, ':score': score});
              res.json(db.prepare(
                             'SELECT id, name, role, score FROM users ' +
                             'WHERE name = :name ORDER BY id DESC LIMIT 1')
                           .get({':name': name}));
            }
          })
      .put(
          '/users/:id',
          (req, res) => {
            const id = Number(req.params.id);
            const {name = null, role = null, score = null} = req.body;
            if (useCachedStmts) {
              cachedStmts.update.run(
                  {':name': name, ':role': role, ':score': score, ':id': id});
              res.json(cachedStmts.get.get({':id': id}));
            } else {
              db.prepare(
                    'UPDATE users SET ' +
                    '  name = COALESCE(:name, name), ' +
                    '  role = COALESCE(:role, role), ' +
                    '  score = COALESCE(:score, score) ' +
                    'WHERE id = :id')
                  .run({
                    ':name': name,
                    ':role': role,
                    ':score': score,
                    ':id': id
                  });
              res.json(
                  db.prepare(
                        'SELECT id, name, role, score FROM users WHERE id = :id')
                      .get({':id': id}));
            }
          })
      .delete('/users/:id', (req, res) => {
        const id = Number(req.params.id);
        const stmt = useCachedStmts ?
            cachedStmts.del :
            db.prepare('DELETE FROM users WHERE id = :id');
        stmt.run({':id': id});
        res.json({deleted: true, id});
      });

  return app;
}

const dummySocket = new Socket();

/**
 * Dispatches an in-process request through `app.handle(req, res)`.
 * @param {!Object} app Express application instance.
 * @param {string} method HTTP method name.
 * @param {string} url Request path and query string.
 * @param {?string} rawBody Optional raw JSON request body.
 * @return {string} Serialized JSON response body.
 */
function dispatchExpress(app, method, url, rawBody) {
  const req = new http.IncomingMessage(dummySocket);
  req.method = method;
  req.url = url;
  req.headers = {host: '127.0.0.1:8080', 'content-type': 'application/json'};
  req.rawBody = rawBody;

  const res = new http.ServerResponse(req);
  let outBody = '';
  res.send = function(body) {
    outBody = typeof body === 'string' ? body : JSON.stringify(body);
    return this;
  };
  app.handle(req, res);
  return outBody;
}

/**
 * Runs the 3 benchmark workloads against an Express app and prints timings.
 * @param {string} label Suite display label.
 * @param {boolean} useCachedStmts Whether to reuse pre-compiled statements.
 */
function runSuite(label, useCachedStmts) {
  const app = buildApp(useCachedStmts);
  const numReqs = 10000;
  const crudCycles = 2000;
  let totalBytes = 0;

  for (let i = 0; i < 500; i++) {
    dispatchExpress(app, 'GET', '/users?role=admin&min_score=90', null);
    dispatchExpress(app, 'GET', '/users/1', null);
  }

  let t0 = performance.now();
  for (let i = 0; i < numReqs; i++) {
    totalBytes +=
        dispatchExpress(app, 'GET', '/users?role=admin&min_score=90', null)
            .length;
  }
  const dtList = (performance.now() - t0) / 1000.0;

  t0 = performance.now();
  for (let i = 0; i < numReqs; i++) {
    totalBytes += dispatchExpress(app, 'GET', '/users/1', null).length;
  }
  const dtGet = (performance.now() - t0) / 1000.0;

  t0 = performance.now();
  for (let i = 0; i < crudCycles; i++) {
    const createdStr = dispatchExpress(
        app, 'POST', '/users',
        '{"name":"Charlie","role":"admin","score":95.5}');
    totalBytes += createdStr.length;
    const newId = JSON.parse(createdStr).id;
    const detailUrl = `/users/${newId}`;

    totalBytes +=
        dispatchExpress(app, 'GET', '/users?role=admin&min_score=90', null)
            .length;
    totalBytes +=
        dispatchExpress(app, 'PUT', detailUrl, '{"score":99.75}').length;
    totalBytes += dispatchExpress(app, 'GET', detailUrl, null).length;
    totalBytes += dispatchExpress(app, 'DELETE', detailUrl, null).length;
  }
  const dtCrud = (performance.now() - t0) / 1000.0;

  console.log(`${label}:`);
  console.log(
      `  GET /users?role=admin&min_score=90 : ` +
      `${(dtList * 1000).toFixed(2).padStart(7)} ms for ${numReqs} reqs ` +
      `(${Math.round(numReqs / dtList).toString().padStart(9)} req/s, ` +
      `${((dtList * 1e6) / numReqs).toFixed(2).padStart(6)} us/req)`);
  console.log(
      `  GET /users/1                       : ` +
      `${(dtGet * 1000).toFixed(2).padStart(7)} ms for ${numReqs} reqs ` +
      `(${Math.round(numReqs / dtGet).toString().padStart(9)} req/s, ` +
      `${((dtGet * 1e6) / numReqs).toFixed(2).padStart(6)} us/req)`);
  console.log(
      `  5-step CRUD mix (POST/GET/PUT/DEL) : ` +
      `${(dtCrud * 1000).toFixed(2).padStart(7)} ms for ${
          crudCycles * 5} reqs ` +
      `(${
          Math.round((crudCycles * 5) / dtCrud)
              .toString()
              .padStart(9)} req/s, ` +
      `${((dtCrud * 1e6) / (crudCycles * 5)).toFixed(2).padStart(6)} us/req)`);
  console.log(`  (serialized JSON bytes: ${totalBytes})`);
}

runSuite(
    `Node.js ${
        process.version} + Express 4.20 (db.prepare per query, matching C)`,
    false);
runSuite(
    `Node.js ${process.version} + Express 4.20 (pre-compiled StatementSync)`,
    true);
