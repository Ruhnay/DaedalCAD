// Admin web interface: releases, download logins and sync users, on a port of its own.
//
// Meant for your own network only. It answers loopback and private addresses (192.168.x.x,
// 10.x.x.x, 172.16-31.x.x, fc00::/7, link-local) and nothing else, judged by the address that
// actually connects (X-Forwarded-For is ignored here). Never point nginx / Nginx Proxy Manager or a
// router port forward at this port.
//
// Login: one admin password, set on the first visit (or with `node src/cli.js admin password`),
// stored as an scrypt hash in <storage>/admin.json. A session cookie (HttpOnly, SameSite=Strict)
// keeps you logged in for 12 hours. Every API call must carry the X-DaedalSync-Admin header, which a
// page on another site cannot add, so other sites cannot make your browser act here.
import crypto from "crypto";
import express from "express";
import fsp from "fs/promises";
import http from "http";
import path from "path";
import { formatBytes, parseBytes } from "./config.js";
import { HttpError, freeBytes, readJson, receiveUpload, writeJson } from "./files.js";
import { createReleases, zipFolderVersion } from "./releases.js";

const SESSION_MS = 12 * 60 * 60 * 1000;
const LOCK_AFTER = 8;
const LOCK_MS = 15 * 60 * 1000;
const UPLOAD_MAX = 4 * 1024 ** 3;

function scrypt(password, salt) {
  return new Promise((resolve, reject) => {
    crypto.scrypt(String(password), Buffer.from(salt, "hex"), 32, (error, key) => (error ? reject(error) : resolve(key)));
  });
}

// --- who may connect ------------------------------------------------------------------------------
function ipv4Number(ip) {
  const parts = ip.split(".").map(Number);
  if (parts.length !== 4 || parts.some((p) => !Number.isInteger(p) || p < 0 || p > 255)) return null;
  return ((parts[0] << 24) >>> 0) + (parts[1] << 16) + (parts[2] << 8) + parts[3];
}

function inCidr(ip, cidr) {
  const [base, bitsText] = cidr.split("/");
  const bits = bitsText === undefined ? 32 : Number(bitsText);
  const a = ipv4Number(ip);
  const b = ipv4Number(base);
  if (a === null || b === null || !(bits >= 0 && bits <= 32)) return false;
  const mask = bits === 0 ? 0 : (~0 << (32 - bits)) >>> 0;
  return ((a & mask) >>> 0) === ((b & mask) >>> 0);
}

const PRIVATE_V4 = ["127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16"];

export function plainAddress(raw) {
  const ip = String(raw || "").trim();
  return ip.toLowerCase().startsWith("::ffff:") && ip.includes(".") ? ip.slice(7) : ip.toLowerCase();
}

// allow: "private" (the default), or a comma list of CIDRs / addresses, e.g. "192.168.1.0/24, 127.0.0.1".
export function addressAllowed(raw, allow = "private") {
  const ip = plainAddress(raw);
  const rules = String(allow || "private").split(",").map((r) => r.trim()).filter(Boolean);
  for (const rule of rules) {
    if (rule === "private") {
      if (ip.includes(".")) {
        if (PRIVATE_V4.some((cidr) => inCidr(ip, cidr))) return true;
      } else if (ip === "::1" || /^f[cd][0-9a-f]{2}:/.test(ip) || /^fe[89ab][0-9a-f]:/.test(ip)) {
        return true;
      }
    } else if (ip.includes(".") ? inCidr(ip, rule) : ip === rule.toLowerCase()) {
      return true;
    }
  }
  return false;
}

// --- admin password -------------------------------------------------------------------------------
export function createAdminAuth(config) {
  const file = path.join(config.storagePath, "admin.json");
  return {
    async isSet() {
      const data = await readJson(file, {});
      return Boolean(data.hash && data.salt);
    },
    async setPassword(password) {
      const text = String(password || "");
      if (text.length < 8) throw new HttpError(400, "The admin password needs at least 8 characters");
      const salt = crypto.randomBytes(16).toString("hex");
      const hash = (await scrypt(text, salt)).toString("hex");
      await writeJson(file, { salt, hash, changed: new Date().toISOString() });
    },
    async check(password) {
      const data = await readJson(file, {});
      const salt = data.salt || "00".repeat(16);
      const got = await scrypt(String(password || ""), salt);
      const want = Buffer.from(data.hash || "00".repeat(32), "hex");
      return Boolean(data.hash) && want.length === got.length && crypto.timingSafeEqual(want, got);
    },
  };
}

function cookies(req) {
  const out = {};
  for (const part of String(req.get("cookie") || "").split(";")) {
    const at = part.indexOf("=");
    if (at > 0) out[part.slice(0, at).trim()] = decodeURIComponent(part.slice(at + 1).trim());
  }
  return out;
}

// --- the app --------------------------------------------------------------------------------------
export function createAdminApp(store, options = {}) {
  const config = store.config;
  const allow = options.allow ?? config.adminAllow ?? "private";
  const auth = createAdminAuth(config);
  const releases = store.releases || createReleases(config);
  const sessions = new Map();
  const failures = new Map();
  const app = express();
  app.disable("x-powered-by");
  app.set("trust proxy", false);

  // Home network only, by the address that really connects.
  app.use((req, res, next) => {
    if (addressAllowed(req.socket.remoteAddress, allow)) return next();
    res.status(403).type("text").send("The admin page only answers on your own network.");
  });
  app.use((_req, res, next) => {
    res.setHeader("X-Frame-Options", "DENY");
    res.setHeader("Referrer-Policy", "no-referrer");
    res.setHeader("Cache-Control", "no-store");
    next();
  });

  const sendError = (res, error) => {
    const status = error.status || (error.type === "entity.parse.failed" ? 400 : 500);
    if (status >= 500) console.error(error);
    res.status(status).json({ error: status >= 500 ? error.message || "Server error" : error.message });
  };
  const route = (fn) => async (req, res) => {
    try {
      await fn(req, res);
    } catch (error) {
      // CLI-style errors from the stores ("No user named x") are the user's mistake, not a crash.
      if (!error.status && !error.code && error instanceof Error && !(error instanceof TypeError)) error.status = 400;
      sendError(res, error);
    }
  };
  const json = express.json({ limit: "64kb" });

  function sessionOf(req) {
    const token = cookies(req).daedalsync_admin;
    if (!token) return null;
    const expires = sessions.get(token);
    if (!expires || expires < Date.now()) {
      sessions.delete(token);
      return null;
    }
    return token;
  }
  function startSession(res) {
    const token = crypto.randomBytes(32).toString("base64url");
    sessions.set(token, Date.now() + SESSION_MS);
    res.setHeader("Set-Cookie", `daedalsync_admin=${token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${SESSION_MS / 1000}`);
  }
  function lockedOut(req) {
    const state = failures.get(plainAddress(req.socket.remoteAddress));
    return state && state.count >= LOCK_AFTER && Date.now() - state.last < LOCK_MS;
  }
  function failed(req) {
    const ip = plainAddress(req.socket.remoteAddress);
    const state = failures.get(ip) || { count: 0, last: 0 };
    state.count = Date.now() - state.last > LOCK_MS ? 1 : state.count + 1;
    state.last = Date.now();
    failures.set(ip, state);
    if (failures.size > 1000) {
      const now = Date.now();
      for (const [key, old] of failures) if (now - old.last > LOCK_MS) failures.delete(key);
    }
  }

  // Every API call: the custom header (blocks cross-site requests).
  app.use("/api", (req, res, next) => {
    if (req.get("x-daedalsync-admin") !== "1") return sendError(res, new HttpError(403, "Missing admin header"));
    next();
  });

  app.get("/api/state", route(async (req, res) => {
    res.json({ setup: !(await auth.isSet()), loggedIn: Boolean(sessionOf(req)) });
  }));
  app.post("/api/setup", json, route(async (req, res) => {
    if (await auth.isSet()) throw new HttpError(409, "The admin password is already set");
    await auth.setPassword(req.body?.password);
    startSession(res);
    res.json({ ok: true });
  }));
  app.post("/api/login", json, route(async (req, res) => {
    if (lockedOut(req)) throw new HttpError(429, "Too many wrong passwords. Try again in 15 minutes.");
    if (!(await auth.check(req.body?.password))) {
      failed(req);
      throw new HttpError(401, "Wrong password");
    }
    failures.delete(plainAddress(req.socket.remoteAddress));
    startSession(res);
    res.json({ ok: true });
  }));
  app.post("/api/logout", route(async (req, res) => {
    const token = sessionOf(req);
    if (token) sessions.delete(token);
    res.setHeader("Set-Cookie", "daedalsync_admin=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    res.json({ ok: true });
  }));

  // Everything below needs a session.
  app.use("/api", (req, res, next) => {
    if (!sessionOf(req)) return sendError(res, new HttpError(401, "Log in first"));
    next();
  });

  app.post("/api/password", json, route(async (req, res) => {
    if (!(await auth.check(req.body?.current))) throw new HttpError(401, "The current password is wrong");
    await auth.setPassword(req.body?.password);
    res.json({ ok: true });
  }));

  app.get("/api/overview", route(async (_req, res) => {
    let free = null;
    try {
      free = await freeBytes(config.storagePath);
    } catch {}
    res.json({ storage: config.storagePath, freeBytes: free, defaultQuota: formatBytes(config.defaultQuotaBytes) });
  }));

  // --- releases ---
  app.get("/api/releases", route(async (_req, res) => res.json({ releases: await releases.listReleases() })));
  // Raw file in the body: POST /api/releases?version=0.7.0&notes=...&replace=1[&kind=installer]
  // The zip without a version: the one in its folder name (DaedalCAD-0.7.0/) is used.
  app.post("/api/releases", route(async (req, res) => {
    const installer = req.query.kind === "installer";
    let version = String(req.query.version || "").trim();
    const notes = String(req.query.notes || "");
    const replace = req.query.replace === "1";
    const taken = (found) => Boolean(found && (installer ? found.installer : found.file));
    const what = installer ? "an installer" : "a zip";
    if (version && !replace && taken((await releases.listReleases()).find((r) => r.version === version)))
      throw new HttpError(409, `Version ${version} already has ${what}`);
    if (installer && !version) throw new HttpError(400, "Fill in the version for the installer");
    const incoming = path.join(config.storagePath, "releases", "incoming");
    await fsp.mkdir(incoming, { recursive: true });
    const tmp = path.join(incoming, `${crypto.randomBytes(8).toString("hex")}.${installer ? "exe" : "zip"}`);
    try {
      await receiveUpload(req, tmp, {
        kind: installer ? "exe" : "zip",
        maxBytes: UPLOAD_MAX,
        declaredLength: req.get("content-length"),
        storageRoot: config.storagePath,
      });
      if (!version) version = (await zipFolderVersion(tmp)) || "";
      if (!version) throw new HttpError(400, "No version in that zip. Fill in the version.");
      if (!replace && taken((await releases.listReleases()).find((r) => r.version === version)))
        throw new HttpError(409, `Version ${version} already has ${what}`);
      const entry = installer
        ? await releases.addInstaller(tmp, version, notes, { move: true, replace })
        : await releases.addRelease(tmp, version, notes, { move: true, replace });
      res.status(201).json(entry);
    } finally {
      await fsp.rm(tmp, { force: true });
    }
  }));
  app.patch("/api/releases/:version", json, route(async (req, res) => {
    res.json(await releases.setNotes(req.params.version, String(req.body?.notes ?? "")));
  }));
  app.delete("/api/releases/:version", route(async (req, res) => {
    await releases.removeRelease(req.params.version);
    res.json({ ok: true });
  }));

  // --- download logins ---
  app.get("/api/logins", route(async (_req, res) => res.json({ logins: await releases.listLogins() })));
  app.post("/api/logins", json, route(async (req, res) => {
    res.status(201).json(await releases.addLogin(req.body?.name, String(req.body?.password || "")));
  }));
  app.post("/api/logins/:name/password", json, route(async (req, res) => {
    res.json(await releases.setPassword(req.params.name, String(req.body?.password || "")));
  }));
  app.delete("/api/logins/:name", route(async (req, res) => {
    await releases.removeLogin(req.params.name);
    res.json({ ok: true });
  }));

  // --- sync users ---
  app.get("/api/users", route(async (_req, res) => {
    const users = await store.listUsers();
    res.json({ users: users.map(({ folder, ...rest }) => rest) });
  }));
  app.post("/api/users", json, route(async (req, res) => {
    const quota = req.body?.quota ? parseBytes(req.body.quota) : config.defaultQuotaBytes;
    const { user, key } = await store.addUser(req.body?.name, quota);
    res.status(201).json({ name: user.name, quotaBytes: user.quotaBytes, key });
  }));
  app.post("/api/users/:name/key", route(async (req, res) => {
    const { user, key } = await store.rotateKey(req.params.name);
    res.json({ name: user.name, key });
  }));
  app.post("/api/users/:name/quota", json, route(async (req, res) => {
    const user = await store.setQuota(req.params.name, parseBytes(req.body?.quota));
    res.json({ name: user.name, quotaBytes: user.quotaBytes });
  }));
  app.post("/api/users/:name/revoke", route(async (req, res) => {
    await store.revokeUser(req.params.name);
    res.json({ ok: true });
  }));

  app.get("/", (_req, res) => {
    res.setHeader("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    res.type("html").send(PAGE);
  });
  app.use((error, _req, res, _next) => sendError(res, error));
  return app;
}

export function listenAdmin(store) {
  const app = createAdminApp(store);
  const server = http.createServer(app);
  server.requestTimeout = 0;
  server.timeout = 0;
  server.headersTimeout = 60_000;
  return new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(store.config.adminPort, store.config.adminHost, () => resolve(server));
  });
}

// --- the page -------------------------------------------------------------------------------------
const PAGE = String.raw`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DaedalSync admin</title>
<style>
  :root { --bg:#12151a; --panel:#1a1f26; --line:#2a3340; --text:#ece6da; --dim:#99a5b3; --accent:#ff9a2e; --bad:#ff6b6b; --good:#5fd38d; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font:15px/1.45 system-ui, "Segoe UI", sans-serif; }
  header { display:flex; align-items:center; gap:16px; padding:14px 20px; border-bottom:1px solid var(--line); }
  header h1 { margin:0; font-size:18px; font-weight:600; }
  header h1 span { color:var(--accent); }
  header .grow { flex:1; }
  header small { color:var(--dim); }
  nav { display:flex; gap:4px; padding:12px 20px 0; flex-wrap:wrap; }
  nav button { background:none; border:1px solid transparent; border-bottom:none; color:var(--dim); padding:8px 14px; border-radius:6px 6px 0 0; cursor:pointer; font:inherit; }
  nav button.on { color:var(--text); background:var(--panel); border-color:var(--line); }
  main { padding:0 20px 40px; max-width:1100px; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:0 8px 8px 8px; padding:18px; }
  h2 { font-size:15px; margin:0 0 12px; color:var(--accent); font-weight:600; }
  h2 + p { margin-top:-6px; color:var(--dim); font-size:13px; }
  table { width:100%; border-collapse:collapse; margin-top:8px; }
  th, td { text-align:left; padding:8px 10px; border-bottom:1px solid var(--line); vertical-align:middle; }
  th { color:var(--dim); font-weight:500; font-size:13px; }
  td.num { font-variant-numeric:tabular-nums; white-space:nowrap; }
  .row { display:flex; gap:10px; align-items:end; flex-wrap:wrap; margin:6px 0 14px; }
  label { display:flex; flex-direction:column; gap:4px; font-size:13px; color:var(--dim); }
  input[type=text], input[type=password] { background:#0f1318; border:1px solid var(--line); color:var(--text); border-radius:6px; padding:8px 10px; font:inherit; min-width:0; }
  input:focus { outline:2px solid var(--accent); outline-offset:-1px; }
  button.btn { background:#232a33; border:1px solid var(--line); color:var(--text); border-radius:6px; padding:8px 12px; font:inherit; cursor:pointer; white-space:nowrap; }
  button.btn:hover { border-color:var(--accent); }
  button.primary { background:var(--accent); border-color:var(--accent); color:#24180c; font-weight:600; }
  button.danger:hover { border-color:var(--bad); color:var(--bad); }
  button:disabled { opacity:.5; cursor:default; }
  .actions { display:flex; gap:6px; flex-wrap:wrap; justify-content:flex-end; }
  .drop { border:2px dashed var(--line); border-radius:8px; padding:22px; text-align:center; color:var(--dim); cursor:pointer; }
  .drop.over { border-color:var(--accent); color:var(--text); }
  .drop b { color:var(--text); }
  progress { width:100%; height:10px; accent-color:var(--accent); }
  .secret { background:#0f1318; border:1px solid var(--accent); border-radius:8px; padding:12px 14px; margin:10px 0 16px; }
  .secret code { display:block; font:15px ui-monospace, Consolas, monospace; color:var(--accent); word-break:break-all; margin:6px 0; user-select:all; }
  .secret small { color:var(--dim); }
  .secret .title { display:block; margin-bottom:6px; }
  .tag { font-size:12px; padding:2px 8px; border-radius:10px; border:1px solid var(--line); color:var(--dim); }
  .tag.ok { color:var(--good); border-color:#2e5b40; }
  .tag.off { color:var(--bad); border-color:#5b2e2e; }
  .msg { min-height:20px; margin:8px 0 0; font-size:13px; }
  .msg.bad { color:var(--bad); } .msg.good { color:var(--good); }
  .muted { color:var(--dim); }
  .login { max-width:360px; margin:12vh auto; background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:24px; }
  .login h1 { margin:0 0 6px; font-size:20px; } .login p { color:var(--dim); margin:0 0 16px; font-size:14px; }
  .login label { margin-bottom:12px; } .login button { width:100%; }
  .hidden { display:none !important; }
  @media (max-width: 700px) { th.hide, td.hide { display:none; } .actions { justify-content:flex-start; } th, td { padding:8px 5px; } main, nav, header { padding-left:16px; padding-right:16px; } section { padding:14px; } }
</style>
</head>
<body>
<div id="gate" class="login hidden">
  <h1>DaedalSync admin</h1>
  <p id="gateText">Log in to manage releases and users.</p>
  <label>Password <input id="gatePass" type="password" autocomplete="current-password"></label>
  <label id="gateRepeatWrap" class="hidden">Repeat password <input id="gateRepeat" type="password" autocomplete="new-password"></label>
  <button id="gateGo" class="btn primary">Log in</button>
  <div id="gateMsg" class="msg bad"></div>
</div>

<div id="app" class="hidden">
  <header>
    <h1>Shape<span>Sync</span> admin</h1>
    <div class="grow"></div>
    <small id="disk"></small>
    <button class="btn" id="logout">Log out</button>
  </header>
  <nav>
    <button data-tab="releases" class="on">Releases</button>
    <button data-tab="logins">Download logins</button>
    <button data-tab="users">Sync users</button>
    <button data-tab="settings">Admin password</button>
  </nav>
  <main>
    <section id="tab-releases">
      <h2>Upload a release</h2>
      <p>What package.ps1 makes: the portable zip and the installer (DaedalCAD-0.7.0-setup.exe). Drop one or both. The version is read from the names; you can change it before uploading. Installed copies update themselves from the installer.</p>
      <div class="drop" id="drop"><span id="dropText"><b>Drop the zip and/or setup.exe here</b> or click to pick them</span></div>
      <input id="file" type="file" accept=".zip,.exe" multiple class="hidden">
      <div class="row" id="uploadForm" style="margin-top:12px">
        <label>Version <input id="version" type="text" placeholder="0.7.0" size="10"></label>
        <label style="flex:1; min-width:220px">Notes <input id="notes" type="text" placeholder="What changed (optional)"></label>
        <label style="flex-direction:row; align-items:center; gap:6px"><input id="replace" type="checkbox"> Replace if it exists</label>
        <button class="btn primary" id="upload" disabled>Upload</button>
      </div>
      <progress id="progress" class="hidden" max="100" value="0"></progress>
      <div class="msg" id="releaseMsg"></div>
      <h2 style="margin-top:18px">Versions</h2>
      <table><thead><tr><th>Version</th><th class="hide">Added</th><th>Files</th><th>Notes</th><th></th></tr></thead><tbody id="releaseRows"></tbody></table>
    </section>

    <section id="tab-logins" class="hidden">
      <h2>Download logins</h2>
      <p>Name and password people use on /downloads. Separate from sync users. Leave the password empty to have one made up.</p>
      <div class="row">
        <label>Name <input id="loginName" type="text" placeholder="name"></label>
        <label>Password <input id="loginPass" type="text" placeholder="(made up when empty)"></label>
        <button class="btn primary" id="loginAdd">Add login</button>
      </div>
      <div id="loginSecret"></div>
      <div class="msg" id="loginMsg"></div>
      <table><thead><tr><th>Name</th><th class="hide">Added</th><th></th></tr></thead><tbody id="loginRows"></tbody></table>
    </section>

    <section id="tab-users" class="hidden">
      <h2>Sync users</h2>
      <p>People who sync projects from DaedalCAD. Each gets a key to paste into CloudSync; it is shown once.</p>
      <div class="row">
        <label>Name <input id="userName" type="text" placeholder="name"></label>
        <label>Quota <input id="userQuota" type="text" placeholder="5G" size="8"></label>
        <button class="btn primary" id="userAdd">Add user</button>
      </div>
      <div id="userSecret"></div>
      <div class="msg" id="userMsg"></div>
      <table><thead><tr><th>Name</th><th>State</th><th>Used</th><th></th></tr></thead><tbody id="userRows"></tbody></table>
    </section>

    <section id="tab-settings" class="hidden">
      <h2>Admin password</h2>
      <div class="row">
        <label>Current <input id="pwOld" type="password" autocomplete="current-password"></label>
        <label>New (8+ characters) <input id="pwNew" type="password" autocomplete="new-password"></label>
        <button class="btn primary" id="pwSave">Change</button>
      </div>
      <div class="msg" id="pwMsg"></div>
      <p class="muted">Forgot it? On the server: <code>node src/cli.js admin password</code></p>
    </section>
  </main>
</div>

<script>
const $ = (id) => document.getElementById(id);
const esc = (t) => String(t ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const size = (n) => { n = Number(n) || 0; if (n < 1024) return n + " B"; const u = ["KiB","MiB","GiB","TiB"]; let v = n / 1024, i = 0; while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; } return v.toFixed(v >= 10 ? 0 : 1) + " " + u[i]; };

async function api(method, url, body, raw) {
  const headers = { "X-DaedalSync-Admin": "1" };
  if (body !== undefined && !raw) headers["Content-Type"] = "application/json";
  const res = await fetch(url, { method, headers, body: body === undefined ? undefined : raw ? body : JSON.stringify(body), credentials: "same-origin" });
  const data = await res.json().catch(() => ({}));
  if (res.status === 401 && url !== "/api/login" && url !== "/api/password") { showGate(false); throw new Error("Log in again"); }
  if (!res.ok) throw new Error(data.error || res.statusText);
  return data;
}
function say(id, text, good) { const el = $(id); el.textContent = text || ""; el.className = "msg " + (good ? "good" : "bad"); }
function secret(id, title, lines) {
  $(id).innerHTML = '<div class="secret"><b class="title">' + esc(title) + "</b>" + lines.map(([k, v]) => '<small>' + esc(k) + '</small><code>' + esc(v) + "</code>").join("") +
    '<button class="btn" data-copy>Copy</button> <button class="btn" data-close>Done</button> <small>Shown once. Copy it now.</small></div>';
  const box = $(id).firstChild;
  box.querySelector("[data-copy]").onclick = () => navigator.clipboard?.writeText(lines.map(([k, v]) => k + ": " + v).join("\n"));
  box.querySelector("[data-close]").onclick = () => { $(id).innerHTML = ""; };
}

// --- gate ---
let setupMode = false;
function showGate(setup) {
  setupMode = setup;
  $("app").classList.add("hidden");
  $("gate").classList.remove("hidden");
  $("gateText").textContent = setup ? "First visit: choose the admin password (8+ characters)." : "Log in to manage releases and users.";
  $("gateRepeatWrap").classList.toggle("hidden", !setup);
  $("gateGo").textContent = setup ? "Set password" : "Log in";
  $("gatePass").value = ""; $("gateRepeat").value = ""; $("gatePass").focus();
}
async function gateGo() {
  try {
    if (setupMode) {
      if ($("gatePass").value !== $("gateRepeat").value) throw new Error("The two passwords differ");
      await api("POST", "/api/setup", { password: $("gatePass").value });
    } else {
      await api("POST", "/api/login", { password: $("gatePass").value });
    }
    say("gateMsg", "");
    openApp();
  } catch (e) { say("gateMsg", e.message); }
}
$("gateGo").onclick = gateGo;
for (const id of ["gatePass", "gateRepeat"]) $(id).addEventListener("keydown", (e) => { if (e.key === "Enter") gateGo(); });
$("logout").onclick = async () => { await api("POST", "/api/logout").catch(() => {}); showGate(false); };

// --- tabs ---
let tab = "releases";
try { tab = localStorage.getItem("daedalsync-admin-tab") || tab; } catch {}
function showTab(name) {
  tab = name;
  try { localStorage.setItem("daedalsync-admin-tab", name); } catch {}
  for (const b of document.querySelectorAll("nav button")) b.classList.toggle("on", b.dataset.tab === name);
  for (const s of document.querySelectorAll("main section")) s.classList.toggle("hidden", s.id !== "tab-" + name);
  ({ releases: loadReleases, logins: loadLogins, users: loadUsers, settings: () => {} })[name]?.();
}
for (const b of document.querySelectorAll("nav button")) b.onclick = () => showTab(b.dataset.tab);

async function openApp() {
  $("gate").classList.add("hidden");
  $("app").classList.remove("hidden");
  showTab(document.querySelector('nav button[data-tab="' + tab + '"]') ? tab : "releases");
  try {
    const o = await api("GET", "/api/overview");
    $("disk").textContent = o.freeBytes == null ? "" : size(o.freeBytes) + " free";
  } catch {}
}

// --- releases ---
let picked = [];  // [{ file, kind: "zip" | "installer" }]
const DROP_HINT = "<b>Drop the zip and/or setup.exe here</b> or click to pick them";
function pick(files) {
  const list = Array.from(files || []).filter((f) => /\.(zip|exe)$/i.test(f.name));
  if (!list.length) return say("releaseMsg", "Pick the .zip and/or the setup .exe");
  picked = list.map((file) => ({ file, kind: /\.exe$/i.test(file.name) ? "installer" : "zip" }));
  picked.sort((a, b) => (a.kind === b.kind ? 0 : a.kind === "zip" ? -1 : 1));
  for (const { file } of picked) {
    const m = file.name.match(/(?:DaedalCAD|ShapeSynth)-([0-9A-Za-z._-]+?)-(?:windows|setup)/i);
    if (m && !$("version").value) $("version").value = m[1];
  }
  const zip = picked.find((p) => p.kind === "zip");
  if (zip && !$("version").value) {
    // A renamed zip: the version is still in the folder name inside (DaedalCAD-0.7.0/).
    const file = zip.file;
    file.slice(Math.max(0, file.size - 1048576)).arrayBuffer().then((buf) => {
      let text = "";
      const bytes = new Uint8Array(buf);
      for (let i = 0; i < bytes.length; i += 8192) text += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
      const inside = text.match(/(?:DaedalCAD|ShapeSynth)-(\d[0-9A-Za-z._-]{0,38})[\/\\]/);
      if (inside && picked.some((p) => p.file === file) && !$("version").value) $("version").value = inside[1].replace(/[._-]+$/, "");
    }).catch(() => {});
  }
  $("dropText").innerHTML = picked.map((p) => "<b>" + esc(p.file.name) + "</b> · " + size(p.file.size)).join("<br>") +
    ' <span class="muted">(click to pick others)</span>';
  $("upload").disabled = false;
  say("releaseMsg", "");
}
$("drop").onclick = () => $("file").click();
$("file").onchange = () => pick($("file").files);
$("drop").addEventListener("dragover", (e) => { e.preventDefault(); $("drop").classList.add("over"); });
$("drop").addEventListener("dragleave", () => $("drop").classList.remove("over"));
$("drop").addEventListener("drop", (e) => { e.preventDefault(); $("drop").classList.remove("over"); pick(e.dataTransfer.files); });
function sendOne({ file, kind }, version) {
  return new Promise((resolve, reject) => {
    const q = new URLSearchParams({ version, notes: $("notes").value, replace: $("replace").checked ? "1" : "0", kind });
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/releases?" + q);
    xhr.setRequestHeader("X-DaedalSync-Admin", "1");
    xhr.setRequestHeader("Content-Type", kind === "installer" ? "application/octet-stream" : "application/zip");
    $("progress").value = 0;
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) $("progress").value = (100 * e.loaded) / e.total; };
    xhr.onload = () => {
      let data = {}; try { data = JSON.parse(xhr.responseText); } catch {}
      if (xhr.status === 401) { showGate(false); return reject(new Error("Log in again")); }
      if (xhr.status >= 300) return reject(new Error(file.name + ": " + (data.error || "upload failed")));
      resolve(data);
    };
    xhr.onerror = () => reject(new Error(file.name + ": upload failed (connection)"));
    xhr.send(file);
  });
}
$("upload").onclick = async () => {
  if (!picked.length) return say("releaseMsg", "Pick the files first");
  let version = $("version").value.trim();
  if (!version && picked.some((p) => p.kind === "installer")) return say("releaseMsg", "Fill in the version");
  $("progress").classList.remove("hidden"); $("upload").disabled = true;
  try {
    let entry = null;
    for (const p of picked) {
      say("releaseMsg", "Uploading " + p.file.name + " …", true);
      entry = await sendOne(p, version);
      version = entry.version;  // a zip may have told us the version
    }
    say("releaseMsg", "Added DaedalCAD " + entry.version + ": " + picked.map((p) => p.kind).join(" and ") + ".", true);
    picked = []; $("version").value = ""; $("notes").value = ""; $("replace").checked = false;
    $("dropText").innerHTML = DROP_HINT; $("file").value = "";
  } catch (e) {
    say("releaseMsg", e.message);
    $("upload").disabled = false;
  } finally {
    $("progress").classList.add("hidden");
    if (!picked.length) $("upload").disabled = true;
    loadReleases();
  }
};
async function loadReleases() {
  try {
    const { releases } = await api("GET", "/api/releases");
    $("releaseRows").innerHTML = releases.length ? releases.map((r, i) =>
      "<tr><td><b>" + esc(r.version) + "</b>" + (i === 0 ? ' <span class="tag ok">latest</span>' : "") + '</td><td class="hide num">' + esc(r.added.slice(0, 10)) +
      '</td><td class="num">' + [r.file ? "zip " + size(r.bytes) : "", r.installer ? "installer " + size(r.installer.bytes) : ""].filter(Boolean).join("<br>") +
      "</td><td>" + esc(r.notes) + '</td><td><div class="actions"><button class="btn" data-notes="' + esc(r.version) +
      '">Notes</button><button class="btn danger" data-del="' + esc(r.version) + '">Remove</button></div></td></tr>').join("")
      : '<tr><td colspan="5" class="muted">No versions yet.</td></tr>';
    for (const b of document.querySelectorAll("[data-del]")) b.onclick = async () => {
      if (!confirm("Remove DaedalCAD " + b.dataset.del + "? People can no longer download it.")) return;
      try { await api("DELETE", "/api/releases/" + encodeURIComponent(b.dataset.del)); loadReleases(); } catch (e) { say("releaseMsg", e.message); }
    };
    for (const b of document.querySelectorAll("[data-notes]")) b.onclick = async () => {
      const current = releases.find((r) => r.version === b.dataset.notes)?.notes || "";
      const notes = prompt("Notes for " + b.dataset.notes, current);
      if (notes === null) return;
      try { await api("PATCH", "/api/releases/" + encodeURIComponent(b.dataset.notes), { notes }); loadReleases(); } catch (e) { say("releaseMsg", e.message); }
    };
  } catch (e) { say("releaseMsg", e.message); }
}

// --- download logins ---
$("loginAdd").onclick = async () => {
  try {
    const r = await api("POST", "/api/logins", { name: $("loginName").value, password: $("loginPass").value });
    secret("loginSecret", "Download login added", [["Name", r.name], ["Password", r.password]]);
    $("loginName").value = ""; $("loginPass").value = ""; say("loginMsg", ""); loadLogins();
  } catch (e) { say("loginMsg", e.message); }
};
async function loadLogins() {
  try {
    const { logins } = await api("GET", "/api/logins");
    $("loginRows").innerHTML = logins.length ? logins.map((l) =>
      "<tr><td><b>" + esc(l.name) + '</b></td><td class="hide num">' + esc(l.created.slice(0, 10)) + '</td><td><div class="actions"><button class="btn" data-pw="' +
      esc(l.name) + '">New password</button><button class="btn danger" data-rm="' + esc(l.name) + '">Remove</button></div></td></tr>').join("")
      : '<tr><td colspan="3" class="muted">No download logins.</td></tr>';
    for (const b of document.querySelectorAll("[data-pw]")) b.onclick = async () => {
      if (!confirm("Make a new password for " + b.dataset.pw + "? The old one stops working.")) return;
      try { const r = await api("POST", "/api/logins/" + encodeURIComponent(b.dataset.pw) + "/password", {}); secret("loginSecret", "New password", [["Name", r.name], ["Password", r.password]]); }
      catch (e) { say("loginMsg", e.message); }
    };
    for (const b of document.querySelectorAll("[data-rm]")) b.onclick = async () => {
      if (!confirm("Remove the download login " + b.dataset.rm + "?")) return;
      try { await api("DELETE", "/api/logins/" + encodeURIComponent(b.dataset.rm)); loadLogins(); } catch (e) { say("loginMsg", e.message); }
    };
  } catch (e) { say("loginMsg", e.message); }
}

// --- sync users ---
$("userAdd").onclick = async () => {
  try {
    const r = await api("POST", "/api/users", { name: $("userName").value, quota: $("userQuota").value.trim() || undefined });
    secret("userSecret", "Sync user added", [["User", r.name], ["Key", r.key]]);
    $("userName").value = ""; $("userQuota").value = ""; say("userMsg", ""); loadUsers();
  } catch (e) { say("userMsg", e.message); }
};
async function loadUsers() {
  try {
    const { users } = await api("GET", "/api/users");
    $("userRows").innerHTML = users.length ? users.map((u) =>
      "<tr><td><b>" + esc(u.name) + "</b></td><td>" + (u.disabled ? '<span class="tag off">revoked</span>' : '<span class="tag ok">active</span>') +
      '</td><td class="num">' + size(u.bytesUsed) + " / " + size(u.quotaBytes) + '</td><td><div class="actions"><button class="btn" data-key="' + esc(u.name) + '">' +
      (u.disabled ? "Give a key again" : "New key") + '</button><button class="btn" data-quota="' + esc(u.name) + '">Quota</button>' +
      (u.disabled ? "" : '<button class="btn danger" data-revoke="' + esc(u.name) + '">Revoke</button>') + "</div></td></tr>").join("")
      : '<tr><td colspan="4" class="muted">No sync users.</td></tr>';
    for (const b of document.querySelectorAll("[data-key]")) b.onclick = async () => {
      if (!confirm("Make a new key for " + b.dataset.key + "? The old key stops working on every machine.")) return;
      try { const r = await api("POST", "/api/users/" + encodeURIComponent(b.dataset.key) + "/key"); secret("userSecret", "New key", [["User", r.name], ["Key", r.key]]); loadUsers(); }
      catch (e) { say("userMsg", e.message); }
    };
    for (const b of document.querySelectorAll("[data-quota]")) b.onclick = async () => {
      const quota = prompt("New quota for " + b.dataset.quota + " (e.g. 5G, 500M)");
      if (!quota) return;
      try { await api("POST", "/api/users/" + encodeURIComponent(b.dataset.quota) + "/quota", { quota }); loadUsers(); } catch (e) { say("userMsg", e.message); }
    };
    for (const b of document.querySelectorAll("[data-revoke]")) b.onclick = async () => {
      if (!confirm("Revoke " + b.dataset.revoke + "? They can no longer sync. Their files stay on disk.")) return;
      try { await api("POST", "/api/users/" + encodeURIComponent(b.dataset.revoke) + "/revoke"); loadUsers(); } catch (e) { say("userMsg", e.message); }
    };
  } catch (e) { say("userMsg", e.message); }
}

// --- admin password ---
$("pwSave").onclick = async () => {
  try { await api("POST", "/api/password", { current: $("pwOld").value, password: $("pwNew").value }); $("pwOld").value = ""; $("pwNew").value = ""; say("pwMsg", "Changed.", true); }
  catch (e) { say("pwMsg", e.message); }
};

(async () => {
  try {
    const s = await api("GET", "/api/state");
    if (s.loggedIn) openApp(); else showGate(s.setup);
  } catch (e) { showGate(false); say("gateMsg", e.message); }
})();
</script>
</body>
</html>`;
