// App downloads: packaged DaedalCAD builds (the zips package.ps1 makes), behind HTTP Basic auth.
//
//   <storage>/releases/index.json          { releases: [{ version, file, bytes, sha256, notes, added }] }
//   <storage>/releases/files/<file>         the zips
//   <storage>/releases/logins.json          { logins: [{ name, salt, hash, created, disabled }] }
//
// Download logins are separate from CloudSync users: someone can get the app without a cloud
// account. Passwords are stored as scrypt hashes only.
import crypto from "crypto";
import fs from "fs";
import fsp from "fs/promises";
import path from "path";
import { HttpError, createLocks, readJson, sha256File, writeJson } from "./files.js";

const VERSION_RE = /^[0-9A-Za-z][0-9A-Za-z._-]{0,39}$/;

function scrypt(password, salt) {
  return new Promise((resolve, reject) => {
    crypto.scrypt(String(password), Buffer.from(salt, "hex"), 32, (error, key) => (error ? reject(error) : resolve(key)));
  });
}

// "1.10.0" after "1.9.2"; anything that is not numbers sorts by text.
export function compareVersions(a, b) {
  const pa = String(a).split(/[.-]/);
  const pb = String(b).split(/[.-]/);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i] ?? "";
    const y = pb[i] ?? "";
    const nx = /^\d+$/.test(x) ? Number(x) : NaN;
    const ny = /^\d+$/.test(y) ? Number(y) : NaN;
    if (!Number.isNaN(nx) && !Number.isNaN(ny)) {
      if (nx !== ny) return nx - ny;
    } else if (x !== y) {
      return x < y ? -1 : 1;
    }
  }
  return 0;
}

// The version a package.ps1 zip carries in its folder name (DaedalCAD-0.7.0/...), read from the
// zip's file list at its end. Null when there is none.
export async function zipFolderVersion(file) {
  const fd = await fsp.open(file, "r");
  try {
    const { size } = await fd.stat();
    const length = Math.min(size, 1024 * 1024);
    const tail = Buffer.alloc(length);
    await fd.read(tail, 0, length, size - length);
    const match = tail.toString("latin1").match(/(?:DaedalCAD|ShapeSynth)-(\d[0-9A-Za-z._-]{0,38})[\/\\]/);
    return match ? match[1].replace(/[._-]+$/, "") : null;
  } finally {
    await fd.close();
  }
}

export function createReleases(config) {
  const root = path.join(config.storagePath, "releases");
  const filesDir = path.join(root, "files");
  const indexPath = path.join(root, "index.json");
  const loginsPath = path.join(root, "logins.json");
  const locked = createLocks();

  async function readIndex() {
    const data = await readJson(indexPath, { releases: [] });
    return Array.isArray(data.releases) ? data.releases : [];
  }
  async function readLogins() {
    const data = await readJson(loginsPath, { logins: [] });
    return Array.isArray(data.logins) ? data.logins : [];
  }

  function checkVersion(version) {
    const v = String(version || "").trim();
    if (!VERSION_RE.test(v)) throw new Error("Version must be letters, digits, dots, dashes or underscores, e.g. 0.7.0");
    return v;
  }
  async function checkSignature(file, bytes, message) {
    const head = Buffer.alloc(bytes.length);
    const fd = await fsp.open(path.resolve(file), "r");
    try {
      await fd.read(head, 0, bytes.length, 0);
    } finally {
      await fd.close();
    }
    if (!bytes.every((b, i) => head[i] === b)) throw new Error(message);
  }
  // Puts a file in place (atomically) and returns its size and SHA-256.
  async function takeFile(source, target, move) {
    await fsp.mkdir(filesDir, { recursive: true });
    const tmp = `${target}.${process.pid}.tmp`;
    if (move) {
      await fsp.rename(path.resolve(source), tmp).catch(async () => {
        await fsp.copyFile(path.resolve(source), tmp);
        await fsp.rm(path.resolve(source), { force: true });
      });
    } else {
      await fsp.copyFile(path.resolve(source), tmp);
    }
    await fsp.rename(tmp, target);
    return { bytes: (await fsp.stat(target)).size, sha256: await sha256File(target) };
  }

  // Too many wrong passwords from one address: wait a while.
  const failures = new Map();
  const LOCK_AFTER = 10;
  const LOCK_MS = 15 * 60 * 1000;

  return {
    // --- logins (CLI) ------------------------------------------------------------------------
    async addLogin(name, password) {
      const display = String(name || "").trim();
      if (!display || display.length > 80 || display.includes(":")) throw new Error("Login name is missing, too long or has a ':'");
      return locked("logins", async () => {
        const logins = await readLogins();
        if (logins.some((l) => l.name.toLowerCase() === display.toLowerCase())) throw new Error(`Login ${display} already exists`);
        const secret = password || crypto.randomBytes(12).toString("base64url");
        const salt = crypto.randomBytes(16).toString("hex");
        const hash = (await scrypt(secret, salt)).toString("hex");
        logins.push({ name: display, salt, hash, created: new Date().toISOString(), disabled: false });
        await writeJson(loginsPath, { logins });
        return { name: display, password: secret };
      });
    },
    async setPassword(name, password) {
      return locked("logins", async () => {
        const logins = await readLogins();
        const login = logins.find((l) => l.name.toLowerCase() === String(name).toLowerCase());
        if (!login) throw new Error(`No login named ${name}`);
        const secret = password || crypto.randomBytes(12).toString("base64url");
        login.salt = crypto.randomBytes(16).toString("hex");
        login.hash = (await scrypt(secret, login.salt)).toString("hex");
        login.disabled = false;
        await writeJson(loginsPath, { logins });
        return { name: login.name, password: secret };
      });
    },
    async removeLogin(name) {
      return locked("logins", async () => {
        const logins = await readLogins();
        const kept = logins.filter((l) => l.name.toLowerCase() !== String(name).toLowerCase());
        if (kept.length === logins.length) throw new Error(`No login named ${name}`);
        await writeJson(loginsPath, { logins: kept });
      });
    },
    async listLogins() {
      return (await readLogins()).map(({ name, created, disabled }) => ({ name, created, disabled: Boolean(disabled) }));
    },

    // --- releases -----------------------------------------------------------------------------
    // A version holds the portable zip, the installer (setup.exe), or both:
    //   { version, notes, added, file, bytes, sha256, installer: { file, bytes, sha256 } }
    // options.move: take the file over instead of copying it (an upload already on this disk).
    async addRelease(zipPath, version, notes, options = {}) {
      const v = checkVersion(version);
      await checkSignature(zipPath, [0x50, 0x4b], `${zipPath} is not a zip file`);
      return locked("releases", async () => {
        const releases = await readIndex();
        let entry = releases.find((r) => r.version === v);
        if (entry?.file && !options.replace) throw new Error(`Version ${v} is already there. Remove it first to replace it.`);
        const file = `DaedalCAD-${v}-windows-x64.zip`;
        const { bytes, sha256 } = await takeFile(zipPath, path.join(filesDir, file), options.move);
        if (!entry) {
          entry = { version: v, notes: String(notes || ""), added: new Date().toISOString() };
          releases.push(entry);
        } else if (notes) {
          entry.notes = String(notes);
        }
        Object.assign(entry, { file, bytes, sha256 });
        await writeJson(indexPath, { releases });
        return entry;
      });
    },
    // The installer for a version; options.replace swaps an installer that is already there.
    async addInstaller(exePath, version, notes, options = {}) {
      const v = checkVersion(version);
      await checkSignature(exePath, [0x4d, 0x5a], `${exePath} is not a Windows program`);
      return locked("releases", async () => {
        const releases = await readIndex();
        let entry = releases.find((r) => r.version === v);
        if (entry?.installer && !options.replace) throw new Error(`Version ${v} already has an installer.`);
        const file = `DaedalCAD-${v}-setup.exe`;
        const { bytes, sha256 } = await takeFile(exePath, path.join(filesDir, file), options.move);
        if (!entry) {
          entry = { version: v, notes: String(notes || ""), added: new Date().toISOString() };
          releases.push(entry);
        } else if (notes) {
          entry.notes = String(notes);
        }
        entry.installer = { file, bytes, sha256 };
        await writeJson(indexPath, { releases });
        return entry;
      });
    },
    async removeRelease(version) {
      return locked("releases", async () => {
        const releases = await readIndex();
        const found = releases.find((r) => r.version === String(version));
        if (!found) throw new Error(`No version ${version}`);
        await writeJson(indexPath, { releases: releases.filter((r) => r !== found) });
        if (found.file) await fsp.rm(path.join(filesDir, path.basename(found.file)), { force: true });
        if (found.installer?.file) await fsp.rm(path.join(filesDir, path.basename(found.installer.file)), { force: true });
      });
    },
    async setNotes(version, notes) {
      return locked("releases", async () => {
        const releases = await readIndex();
        const found = releases.find((r) => r.version === String(version));
        if (!found) throw new Error(`No version ${version}`);
        found.notes = String(notes || "").slice(0, 2000);
        await writeJson(indexPath, { releases });
        return found;
      });
    },
    async listReleases() {
      const releases = await readIndex();
      return releases.slice().sort((a, b) => compareVersions(b.version, a.version));
    },

    // --- HTTP --------------------------------------------------------------------------------
    // Express middleware: HTTP Basic auth against the download logins.
    requireLogin() {
      return async (req, res, next) => {
        const ip = req.ip || req.socket.remoteAddress || "?";
        const state = failures.get(ip);
        if (state && state.count >= LOCK_AFTER && Date.now() - state.last < LOCK_MS) {
          res.status(429).json({ error: "Too many wrong passwords. Try again later." });
          return;
        }
        const challenge = () => {
          res.setHeader("WWW-Authenticate", 'Basic realm="DaedalCAD downloads", charset="UTF-8"');
          res.status(401).json({ error: "Log in to download DaedalCAD" });
        };
        const header = req.get("authorization") || "";
        if (!header.toLowerCase().startsWith("basic ")) return challenge();
        const decoded = Buffer.from(header.slice(6).trim(), "base64").toString("utf8");
        const colon = decoded.indexOf(":");
        if (colon < 0) return challenge();
        const name = decoded.slice(0, colon);
        const password = decoded.slice(colon + 1);
        try {
          const logins = await readLogins();
          const login = logins.find((l) => l.name.toLowerCase() === name.toLowerCase() && !l.disabled);
          // Hash even for an unknown name, so the answer takes the same time either way.
          const salt = login ? login.salt : "00".repeat(16);
          const got = await scrypt(password, salt);
          const want = Buffer.from(login ? login.hash : "00".repeat(32), "hex");
          if (login && want.length === got.length && crypto.timingSafeEqual(want, got)) {
            failures.delete(ip);
            req.downloadLogin = login.name;
            return next();
          }
          const next_ = failures.get(ip) || { count: 0, last: 0 };
          next_.count = Date.now() - next_.last > LOCK_MS ? 1 : next_.count + 1;
          next_.last = Date.now();
          failures.set(ip, next_);
          // A public page gets scanned: forget addresses whose lock has run out, so the list stays small.
          if (failures.size > 1000) {
            const now = Date.now();
            for (const [key, state] of failures) if (now - state.last > LOCK_MS) failures.delete(key);
          }
          return challenge();
        } catch (error) {
          return next(error);
        }
      };
    },

    // What one version offers, for programs (the updater, /downloads JSON).
    describe(r) {
      const q = `version=${encodeURIComponent(r.version)}`;
      return {
        version: r.version,
        notes: r.notes || "",
        added: r.added,
        zip: r.file ? { bytes: r.bytes, sha256: r.sha256, url: `download?${q}` } : null,
        installer: r.installer ? { bytes: r.installer.bytes, sha256: r.installer.sha256, url: `download?${q}&kind=installer` } : null,
        // older clients read these
        bytes: r.bytes ?? r.installer?.bytes ?? 0,
        sha256: r.sha256 ?? null,
        url: r.file ? `download?${q}` : `download?${q}&kind=installer`,
      };
    },

    // GET /downloads: JSON for programs, a small page with links for a browser.
    async listHandler(req, res) {
      const rows = (await this.listReleases()).map((r) => this.describe(r));
      if (req.accepts(["json", "html"]) === "html") {
        const esc = (t) => String(t).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
        const mb = (n) => `${(n / 1048576).toFixed(1)} MB`;
        const items = rows.length
          ? rows
              .map((r, i) => {
                const links = [];
                if (r.installer) links.push(`<a href="${esc(r.installer.url)}">Installer</a> <span>${mb(r.installer.bytes)}</span>`);
                if (r.zip) links.push(`<a href="${esc(r.zip.url)}">Portable zip</a> <span>${mb(r.zip.bytes)}</span>`);
                return (
                  `<li><b>DaedalCAD ${esc(r.version)}</b>${i === 0 ? " (latest)" : ""} <span>${esc(String(r.added).slice(0, 10))}</span>` +
                  `<div>${links.join(" &nbsp;·&nbsp; ")}</div>` +
                  (r.notes ? `<p>${esc(r.notes)}</p>` : "") +
                  "</li>"
                );
              })
              .join("")
          : "<li>No versions yet.</li>";
        res.type("html").send(
          `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">` +
            `<title>DaedalCAD downloads</title><style>body{font:16px system-ui,sans-serif;max-width:640px;margin:40px auto;padding:0 16px;` +
            `background:#12151a;color:#ece6da}a{color:#ff9a2e}li{margin:14px 0}span{color:#99a5b3;font-size:14px}p{margin:4px 0;color:#c9c2b6}div{margin-top:4px}</style>` +
            `<h1>DaedalCAD</h1><p>The installer sets DaedalCAD up and keeps it up to date. The portable zip runs from any folder: unzip it and start DaedalCAD.exe.</p><ul>${items}</ul>`,
        );
        return;
      }
      res.json({ latest: rows[0]?.version ?? null, versions: rows });
    },

    // GET .../latest for the updater: the newest version and what it offers.
    async latestHandler(_req, res) {
      const newest = (await this.listReleases())[0];
      res.json(newest ? this.describe(newest) : { version: null });
    },

    // GET /download?version=0.7.0[&kind=installer] (no version, or "latest": the newest that has one).
    async downloadHandler(req, res) {
      const wanted = String(req.query.version || "latest");
      const installer = req.query.kind === "installer";
      const releases = (await this.listReleases()).filter((r) => (installer ? r.installer : r.file));
      const found = wanted === "latest" ? releases[0] : releases.find((r) => r.version === wanted);
      const what = installer ? "installer" : "zip";
      if (!found) throw new HttpError(404, wanted === "latest" ? `No ${what} yet` : `No ${what} for version ${wanted}`);
      const part = installer ? found.installer : found;
      const file = path.join(filesDir, path.basename(part.file));
      const stat = await fsp.stat(file).catch(() => null);
      if (!stat) throw new HttpError(404, `The ${what} for ${found.version} is missing on the server`);
      res.setHeader("Content-Type", installer ? "application/vnd.microsoft.portable-executable" : "application/zip");
      res.setHeader("Content-Length", String(stat.size));
      res.setHeader("Content-Disposition", `attachment; filename="${path.basename(part.file)}"`);
      res.setHeader("X-Sha256", part.sha256);
      res.setHeader("X-Version", found.version);
      fs.createReadStream(file).pipe(res);
    },
  };
}
