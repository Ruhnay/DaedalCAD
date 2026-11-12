import assert from "node:assert/strict";
import fs from "fs/promises";
import os from "os";
import path from "path";
import test from "node:test";
import { loadConfig, parseBytes } from "../src/config.js";
import { createApp } from "../src/server.js";
import { createStore } from "../src/store.js";

async function withServer(quota, fn) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "daedalsync-"));
  const config = loadConfig({ storage: root, port: "5002", quota });
  const store = createStore(config);
  await store.ensureLayout();
  const app = createApp(store);
  const server = await new Promise((resolve) => {
    const listening = app.listen(0, "127.0.0.1", () => resolve(listening));
  });
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;
  try {
    await fn(base, store);
  } finally {
    await new Promise((resolve) => server.close(resolve));
    await fs.rm(root, { recursive: true, force: true });
  }
}

async function call(base, method, urlPath, { key, body } = {}) {
  const headers = {};
  if (key) headers.authorization = `Bearer ${key}`;
  if (body) headers["content-type"] = "application/zip";
  const response = await fetch(base + urlPath, { method, headers, body });
  const buffer = Buffer.from(await response.arrayBuffer());
  let json = null;
  const type = response.headers.get("content-type") || "";
  if (type.includes("json")) json = JSON.parse(buffer.toString("utf8"));
  return { status: response.status, json, buffer, headers: response.headers };
}

test("still reads the SHAPESYNC_ settings from before the rename", () => {
  const saved = ["DAEDALSYNC_PORT", "SHAPESYNC_PORT"].map((key) => [key, process.env[key]]);
  delete process.env.DAEDALSYNC_PORT;
  process.env.SHAPESYNC_PORT = "6001";
  try {
    assert.equal(loadConfig().port, 6001);
    process.env.DAEDALSYNC_PORT = "6002";
    assert.equal(loadConfig().port, 6002, "the new name wins");
  } finally {
    for (const [key, value] of saved) {
      if (value === undefined) delete process.env[key];
      else process.env[key] = value;
    }
  }
});

test("defaults to port 5002 and parses sizes", () => {
  const saved = ["PORT", "STORAGE", "CONFIG", "HOST", "QUOTA"]
    .flatMap((name) => [`DAEDALSYNC_${name}`, `SHAPESYNC_${name}`])
    .map((key) => [key, process.env[key]]);
  for (const [key] of saved) delete process.env[key];
  try {
    const config = loadConfig();
    assert.equal(config.port, 5002);
    assert.equal(config.host, "127.0.0.1");
    assert.equal(parseBytes("5G"), 5 * 1024 ** 3);
    assert.equal(parseBytes("500M"), 500 * 1024 ** 2);
  } finally {
    for (const [key, value] of saved) {
      if (value == null) delete process.env[key];
      else process.env[key] = value;
    }
  }
});

test("put, list, fetch, isolate, and quota", async () => {
  await withServer("200", async (base, store) => {
    const alice = await store.addUser("alice");
    const bob = await store.addUser("bob");
    const zip = Buffer.concat([Buffer.from("PK\x03\x04"), Buffer.alloc(32, 7)]);

    const denied = await call(base, "GET", "/v1/me");
    assert.equal(denied.status, 401);

    const saved = await call(base, "PUT", "/v1/projects/Bracket", { key: alice.key, body: zip });
    assert.equal(saved.status, 200);
    assert.equal(saved.json.bytes, zip.length);
    assert.equal(saved.json.sha256.length, 64);

    const mine = await call(base, "GET", "/v1/me", { key: alice.key });
    assert.equal(mine.json.name, "alice");
    assert.equal(mine.json.bytesUsed, zip.length);

    const list = await call(base, "GET", "/v1/projects", { key: alice.key });
    assert.equal(list.json.projects.length, 1);
    assert.equal(list.json.projects[0].name, "Bracket");

    const fetched = await call(base, "GET", "/v1/projects/Bracket", { key: alice.key });
    assert.equal(fetched.status, 200);
    assert.equal(Buffer.compare(fetched.buffer, zip), 0);
    assert.equal(fetched.headers.get("x-sha256"), saved.json.sha256);

    const hidden = await call(base, "GET", "/v1/projects/Bracket", { key: bob.key });
    assert.equal(hidden.status, 404);

    const junk = await call(base, "PUT", "/v1/projects/Nope", { key: alice.key, body: Buffer.from("hello") });
    assert.equal(junk.status, 400);

    const sneaky = await call(base, "PUT", "/v1/projects/..%2Fsecret", { key: alice.key, body: zip });
    assert.equal(sneaky.status, 400);

    const fat = Buffer.concat([Buffer.from("PK\x03\x04"), Buffer.alloc(180, 1)]);
    const tooBig = await call(base, "PUT", "/v1/projects/Wide", { key: alice.key, body: fat });
    assert.equal(tooBig.status, 413);

    await store.revokeUser("alice");
    const revoked = await call(base, "GET", "/v1/me", { key: alice.key });
    assert.equal(revoked.status, 401);
  });
});

async function send(base, method, urlPath, { key, json, body, type } = {}) {
  const headers = {};
  if (key) headers.authorization = `Bearer ${key}`;
  let payload = body;
  if (json !== undefined) {
    headers["content-type"] = "application/json";
    payload = JSON.stringify(json);
  } else if (body) {
    headers["content-type"] = type || "application/octet-stream";
  }
  const response = await fetch(base + urlPath, { method, headers, body: payload });
  const buffer = Buffer.from(await response.arrayBuffer());
  const contentType = response.headers.get("content-type") || "";
  return { status: response.status, json: contentType.includes("json") ? JSON.parse(buffer.toString("utf8")) : null, buffer, headers: response.headers };
}

const ZIP = (n) => Buffer.concat([Buffer.from("PK\x03\x04"), Buffer.alloc(40, n)]);
const PNG = Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), Buffer.alloc(24, 3)]);

test("personal delete and preview", async () => {
  await withServer("10M", async (base, store) => {
    const alice = await store.addUser("alice");
    await send(base, "PUT", "/v1/projects/Box", { key: alice.key, body: ZIP(1) });
    const png = await send(base, "PUT", "/v1/projects/Box/preview", { key: alice.key, body: PNG });
    assert.equal(png.status, 200);
    const notPng = await send(base, "PUT", "/v1/projects/Box/preview", { key: alice.key, body: ZIP(1) });
    assert.equal(notPng.status, 400);
    const list = await send(base, "GET", "/v1/projects", { key: alice.key });
    assert.equal(list.json.projects[0].hasPreview, true);
    const got = await send(base, "GET", "/v1/projects/Box/preview", { key: alice.key });
    assert.equal(Buffer.compare(got.buffer, PNG), 0);
    const gone = await send(base, "DELETE", "/v1/projects/Box", { key: alice.key });
    assert.equal(gone.status, 200);
    assert.equal((await send(base, "GET", "/v1/projects", { key: alice.key })).json.projects.length, 0);
    assert.equal((await send(base, "DELETE", "/v1/projects/Box", { key: alice.key })).status, 404);
  });
});

test("public projects: publish, copy, request, accept, decline, withdraw", async () => {
  await withServer("10M", async (base, store) => {
    const alice = await store.addUser("alice");
    const bob = await store.addUser("bob");
    const carol = await store.addUser("carol");

    // Publish: record, then file and preview. Hidden until the first file arrives.
    const created = await send(base, "POST", "/v1/public", {
      key: alice.key,
      json: { title: "Pi case", description: "Case for a Pi 5", tags: "pi, case,Case" },
    });
    assert.equal(created.status, 201);
    const id = created.json.id;
    assert.equal((await send(base, "GET", "/v1/public", { key: bob.key })).json.projects.length, 0);
    assert.equal((await send(base, "PUT", `/v1/public/${id}/file`, { key: bob.key, body: ZIP(2) })).status, 403);
    const v1 = await send(base, "PUT", `/v1/public/${id}/file`, { key: alice.key, body: ZIP(2) });
    assert.equal(v1.json.version, 1);
    assert.equal((await send(base, "PUT", `/v1/public/${id}/preview`, { key: alice.key, body: PNG })).status, 200);

    // Everyone sees it; tags are cleaned; search works.
    const seen = await send(base, "GET", "/v1/public?q=pi%20alice", { key: bob.key });
    assert.equal(seen.json.projects.length, 1);
    assert.deepEqual(seen.json.projects[0].tags, ["pi", "case"]);
    assert.equal(seen.json.projects[0].author, "alice");
    assert.equal(seen.json.projects[0].mine, false);
    assert.equal((await send(base, "GET", "/v1/public?q=nothing", { key: bob.key })).json.projects.length, 0);

    // Copy: download checks out against the hash.
    const copy = await send(base, "GET", `/v1/public/${id}/file`, { key: bob.key });
    assert.equal(copy.status, 200);
    assert.equal(copy.headers.get("x-version"), "1");
    assert.equal(Buffer.compare(copy.buffer, ZIP(2)), 0);
    assert.equal((await send(base, "GET", `/v1/public/${id}`, { key: bob.key })).json.downloads, 1);

    // Only the owner edits or deletes.
    assert.equal((await send(base, "PATCH", `/v1/public/${id}`, { key: bob.key, json: { title: "Mine now" } })).status, 403);
    assert.equal((await send(base, "PATCH", `/v1/public/${id}`, { key: alice.key, json: { title: "Pi 5 case" } })).json.title, "Pi 5 case");
    assert.equal((await send(base, "DELETE", `/v1/public/${id}`, { key: bob.key })).status, 403);

    // Bob sends an update request.
    assert.equal((await send(base, "POST", `/v1/public/${id}/requests`, { key: alice.key, json: { message: "x", baseVersion: 1 } })).status, 400);
    assert.equal((await send(base, "POST", `/v1/public/${id}/requests`, { key: bob.key, json: { message: "", baseVersion: 1 } })).status, 400);
    const req = await send(base, "POST", `/v1/public/${id}/requests`, { key: bob.key, json: { message: "Added vents", baseVersion: 1 } });
    assert.equal(req.status, 201);
    const rid = req.json.id;
    // Not visible to the owner until it has a file.
    assert.equal((await send(base, "GET", "/v1/requests/incoming", { key: alice.key })).json.requests.length, 0);
    assert.equal((await send(base, "PUT", `/v1/public/${id}/requests/${rid}/file`, { key: carol.key, body: ZIP(3) })).status, 403);
    const up = await send(base, "PUT", `/v1/public/${id}/requests/${rid}/file`, { key: bob.key, body: ZIP(3) });
    assert.equal(up.json.status, "open");
    await send(base, "PUT", `/v1/public/${id}/requests/${rid}/preview`, { key: bob.key, body: PNG });
    const incoming = await send(base, "GET", "/v1/requests/incoming", { key: alice.key });
    assert.equal(incoming.json.requests.length, 1);
    assert.equal(incoming.json.requests[0].from, "bob");
    assert.equal(incoming.json.requests[0].message, "Added vents");
    assert.equal(incoming.json.requests[0].hasPreview, true);
    assert.equal((await send(base, "GET", "/v1/me", { key: alice.key })).json.openIncoming, 1);
    // Owner and sender can download it; nobody else.
    assert.equal((await send(base, "GET", `/v1/public/${id}/requests/${rid}/file`, { key: alice.key })).status, 200);
    assert.equal((await send(base, "GET", `/v1/public/${id}/requests/${rid}/file`, { key: carol.key })).status, 403);
    // Only the owner decides.
    assert.equal((await send(base, "POST", `/v1/public/${id}/requests/${rid}/accept`, { key: bob.key, json: {} })).status, 403);
    const accepted = await send(base, "POST", `/v1/public/${id}/requests/${rid}/accept`, { key: alice.key, json: { note: "Thanks!" } });
    assert.equal(accepted.json.status, "accepted");
    assert.equal(accepted.json.resultVersion, 2);
    assert.equal((await send(base, "POST", `/v1/public/${id}/requests/${rid}/accept`, { key: alice.key, json: {} })).status, 409);
    const details = await send(base, "GET", `/v1/public/${id}`, { key: carol.key });
    assert.equal(details.json.version, 2);
    assert.equal(details.json.versions[1].author, "bob");
    assert.equal(details.json.versions[1].hasPreview, true);
    assert.equal(Buffer.compare((await send(base, "GET", `/v1/public/${id}/file`, { key: carol.key })).buffer, ZIP(3)), 0);
    assert.equal(Buffer.compare((await send(base, "GET", `/v1/public/${id}/file?version=1`, { key: carol.key })).buffer, ZIP(2)), 0);
    const outgoing = await send(base, "GET", "/v1/requests/outgoing", { key: bob.key });
    assert.equal(outgoing.json.requests[0].status, "accepted");
    assert.equal(outgoing.json.requests[0].note, "Thanks!");

    // A request on an old version is flagged; decline removes its file.
    const old = await send(base, "POST", `/v1/public/${id}/requests`, { key: carol.key, json: { message: "Old idea", baseVersion: 1 } });
    await send(base, "PUT", `/v1/public/${id}/requests/${old.json.id}/file`, { key: carol.key, body: ZIP(4) });
    const stale = (await send(base, "GET", "/v1/requests/incoming", { key: alice.key })).json.requests;
    assert.equal(stale[0].stale, true);
    const declined = await send(base, "POST", `/v1/public/${id}/requests/${old.json.id}/decline`, { key: alice.key, json: { note: "No" } });
    assert.equal(declined.json.status, "declined");
    assert.equal((await send(base, "GET", `/v1/public/${id}/requests/${old.json.id}/file`, { key: carol.key })).status, 404);

    // Withdraw: only by the sender.
    const third = await send(base, "POST", `/v1/public/${id}/requests`, { key: carol.key, json: { message: "Again", baseVersion: 2 } });
    await send(base, "PUT", `/v1/public/${id}/requests/${third.json.id}/file`, { key: carol.key, body: ZIP(5) });
    assert.equal((await send(base, "POST", `/v1/public/${id}/requests/${third.json.id}/withdraw`, { key: alice.key, json: {} })).status, 403);
    assert.equal((await send(base, "POST", `/v1/public/${id}/requests/${third.json.id}/withdraw`, { key: carol.key, json: {} })).json.status, "withdrawn");
    assert.equal((await send(base, "GET", "/v1/requests/incoming", { key: alice.key })).json.requests.length, 0);

    // Bad ids and bad bodies.
    assert.equal((await send(base, "GET", "/v1/public/..%2F..%2Fusers", { key: bob.key })).status, 404);
    assert.equal((await send(base, "POST", "/v1/public", { key: bob.key, json: { title: "" } })).status, 400);
    const broken = await fetch(base + "/v1/public", { method: "POST", headers: { authorization: `Bearer ${bob.key}`, "content-type": "application/json" }, body: "{nope" });
    assert.equal(broken.status, 400);

    // Owner deletes: gone for everyone, with its requests.
    assert.equal((await send(base, "DELETE", `/v1/public/${id}`, { key: alice.key })).status, 200);
    assert.equal((await send(base, "GET", "/v1/public", { key: bob.key })).json.projects.length, 0);
    assert.equal((await send(base, "GET", "/v1/requests/outgoing", { key: bob.key })).json.requests.length, 0);
  });
});

test("public versions count against the owner's quota", async () => {
  await withServer("100", async (base, store) => {
    const alice = await store.addUser("alice");
    const id = (await send(base, "POST", "/v1/public", { key: alice.key, json: { title: "T" } })).json.id;
    assert.equal((await send(base, "PUT", `/v1/public/${id}/file`, { key: alice.key, body: ZIP(1) })).status, 200);
    // 44 bytes used; another 44 fits, a third does not.
    assert.equal((await send(base, "PUT", `/v1/public/${id}/file`, { key: alice.key, body: ZIP(1) })).status, 200);
    assert.equal((await send(base, "PUT", `/v1/public/${id}/file`, { key: alice.key, body: ZIP(1) })).status, 413);
    assert.equal((await send(base, "GET", "/v1/me", { key: alice.key })).json.bytesUsed, 88);
  });
});

test("admin page: only private addresses", async () => {
  const { addressAllowed } = await import("../src/admin.js");
  for (const ip of ["127.0.0.1", "::1", "::ffff:192.168.1.20", "10.1.2.3", "172.16.0.9", "172.31.255.1", "fd12:3456::1", "fe80::1"])
    assert.equal(addressAllowed(ip), true, ip);
  for (const ip of ["8.8.8.8", "::ffff:84.10.1.1", "172.32.0.1", "2a02:a46::1", "100.64.0.1", ""]) assert.equal(addressAllowed(ip), false, ip);
  assert.equal(addressAllowed("100.64.3.4", "private, 100.64.0.0/10"), true);
  assert.equal(addressAllowed("192.168.2.5", "192.168.1.0/24"), false);
  assert.equal(addressAllowed("192.168.1.5", "192.168.1.0/24"), true);
});

test("admin page: setup, login, users", async () => {
  const { createAdminApp } = await import("../src/admin.js");
  await withServer("5G", async (base, store) => {
    const admin = createAdminApp(store);
    const server = await new Promise((resolve) => {
      const s = admin.listen(0, "127.0.0.1", () => resolve(s));
    });
    const adminBase = `http://127.0.0.1:${server.address().port}`;
    let cookie = "";
    const ask = async (method, url, { body, raw, header = true, type } = {}) => {
      const headers = {};
      if (header) headers["x-daedalsync-admin"] = "1";
      if (cookie) headers.cookie = cookie;
      if (body !== undefined) headers["content-type"] = type || (raw ? "application/zip" : "application/json");
      const res = await fetch(adminBase + url, { method, headers, body: body === undefined ? undefined : raw ? body : JSON.stringify(body) });
      const set = res.headers.get("set-cookie");
      if (set) cookie = set.split(";")[0];
      const text = await res.text();
      let json = null;
      try {
        json = JSON.parse(text);
      } catch {}
      return { status: res.status, json, text };
    };
    try {
      const page = await fetch(adminBase + "/");
      assert.equal(page.status, 200);
      assert.match(await page.text(), /DaedalSync admin/);
      assert.equal((await ask("GET", "/api/state", { header: false })).status, 403, "API needs the admin header");
      assert.deepEqual((await ask("GET", "/api/state")).json, { setup: true, loggedIn: false });
      assert.equal((await ask("GET", "/api/users")).status, 401, "nothing before logging in");
      assert.equal((await ask("POST", "/api/setup", { body: { password: "short" } })).status, 400);
      assert.equal((await ask("POST", "/api/setup", { body: { password: "correct horse" } })).status, 200);
      assert.equal((await ask("POST", "/api/setup", { body: { password: "another one" } })).status, 409, "setup only once");
      cookie = "";
      assert.equal((await ask("POST", "/api/login", { body: { password: "wrong password" } })).status, 401);
      assert.equal((await ask("POST", "/api/login", { body: { password: "correct horse" } })).status, 200);
      assert.equal((await ask("GET", "/api/state")).json.loggedIn, true);

      // Sync users: the key works on the public API.
      const added = await ask("POST", "/api/users", { body: { name: "Bob", quota: "1G" } });
      assert.equal(added.status, 201);
      assert.equal((await call(base, "GET", "/v1/me", { key: added.json.key })).status, 200);
      assert.equal((await ask("POST", "/api/users", { body: { name: "bob" } })).status, 400, "duplicate user is a 400");
      const rotated = await ask("POST", "/api/users/Bob/key");
      assert.equal((await call(base, "GET", "/v1/me", { key: added.json.key })).status, 401, "old key stops");
      assert.equal((await call(base, "GET", "/v1/me", { key: rotated.json.key })).status, 200);
      assert.equal((await ask("POST", "/api/users/Bob/quota", { body: { quota: "2G" } })).json.quotaBytes, 2 * 1024 ** 3);
      const users = (await ask("GET", "/api/users")).json.users;
      assert.equal(users.length, 1);
      assert.equal(users[0].folder, undefined, "no server paths in the list");
      await ask("POST", "/api/users/Bob/revoke");
      assert.equal((await call(base, "GET", "/v1/me", { key: rotated.json.key })).status, 401, "revoked");

      // No download center any more: those routes are gone, on both ports.
      assert.equal((await ask("GET", "/api/releases")).status, 404);
      assert.equal((await ask("GET", "/api/logins")).status, 404);
      for (const gone of ["/downloads", "/download", "/downloads/latest"]) assert.equal((await fetch(base + gone)).status, 404, gone);
      assert.equal((await call(base, "GET", "/v1/app/latest", { key: (await ask("POST", "/api/users", { body: { name: "Cy" } })).json.key })).status, 404);
      await ask("POST", "/api/logout");
      assert.equal((await ask("GET", "/api/users")).status, 401, "logged out");
    } finally {
      await new Promise((resolve) => server.close(resolve));
    }
    // A page that only allows another network refuses this one.
    const closed = createAdminApp(store, { allow: "10.0.0.0/8" });
    const s2 = await new Promise((resolve) => {
      const s = closed.listen(0, "127.0.0.1", () => resolve(s));
    });
    try {
      assert.equal((await fetch(`http://127.0.0.1:${s2.address().port}/`)).status, 403);
    } finally {
      await new Promise((resolve) => s2.close(resolve));
    }
  });
});
