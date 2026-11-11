import crypto from "crypto";
import fsp from "fs/promises";
import path from "path";
import { HttpError, createLocks, fileSize, readJson, receiveUpload, writeJson } from "./files.js";
import { createPublicStore } from "./public.js";

export { HttpError };

export const PREVIEW_MAX_BYTES = 4 * 1024 * 1024;

function hashKey(key) {
  return crypto.createHash("sha256").update(String(key), "utf8").digest("hex");
}

function newId() {
  return crypto.randomBytes(8).toString("hex");
}

function newKey() {
  return crypto.randomBytes(24).toString("base64url");
}

function safeName(name) {
  const text = String(name ?? "").trim();
  if (!text || text.length > 120) throw new HttpError(400, "Project name is missing or too long");
  if (text === "." || text === ".." || /[\\/\u0000-\u001f]/.test(text)) {
    throw new HttpError(400, "Project name cannot contain a path");
  }
  return text;
}

function fileId(name) {
  return crypto.createHash("sha256").update(name, "utf8").digest("hex");
}

export function createStore(config) {
  const locked = createLocks();

  async function ensureLayout() {
    await fsp.mkdir(path.join(config.storagePath, "users"), { recursive: true });
    await fsp.mkdir(path.join(config.storagePath, "public"), { recursive: true });
    await fsp.mkdir(path.dirname(config.usersPath), { recursive: true });
    try {
      await fsp.access(config.usersPath);
    } catch {
      await writeJson(config.usersPath, { users: [] });
    }
  }

  async function readUsers() {
    const data = await readJson(config.usersPath, { users: [] });
    return Array.isArray(data.users) ? data.users : [];
  }

  async function writeUsers(users) {
    await writeJson(config.usersPath, { users });
  }

  function userDir(user) {
    const root = path.resolve(config.storagePath);
    const dir = path.resolve(root, "users", user.id);
    if (dir !== root && !dir.startsWith(root + path.sep)) throw new HttpError(400, "Bad user folder");
    return dir;
  }

  async function metas(user) {
    const dir = userDir(user);
    let names = [];
    try {
      names = await fsp.readdir(dir);
    } catch (error) {
      if (error.code === "ENOENT") return [];
      throw error;
    }
    const projects = [];
    for (const name of names) {
      if (!name.endsWith(".meta.json")) continue;
      const meta = await readJson(path.join(dir, name), null);
      if (!meta || !meta.name) continue;
      const zip = path.join(dir, `${fileId(meta.name)}.shapezip`);
      try {
        const stat = await fsp.stat(zip);
        const previewBytes = await fileSize(path.join(dir, `${fileId(meta.name)}.png`));
        projects.push({
          name: meta.name,
          bytes: stat.size,
          sha256: meta.sha256,
          updated: meta.updated,
          hasPreview: previewBytes > 0,
          previewBytes,
        });
      } catch (error) {
        if (error.code !== "ENOENT") throw error;
      }
    }
    projects.sort((a, b) => a.name.localeCompare(b.name));
    return projects;
  }

  async function privateBytes(user) {
    const projects = await metas(user);
    return projects.reduce((sum, project) => sum + project.bytes + project.previewBytes, 0);
  }

  // Everything that counts against a user's quota: personal projects, owned public versions and
  // their open request uploads.
  async function bytesUsed(user) {
    return (await privateBytes(user)) + (await publicStore.usage(user));
  }

  const store = {
    config,
    ensureLayout,
    locked,
    bytesUsed,

    async addUser(name, quotaBytes) {
      const display = String(name || "").trim();
      if (!display || display.length > 80) throw new Error("User name is missing or too long");
      return locked("users", async () => {
        await ensureLayout();
        const users = await readUsers();
        if (users.some((user) => user.name.toLowerCase() === display.toLowerCase())) {
          throw new Error(`User ${display} already exists`);
        }
        const key = newKey();
        const user = {
          id: newId(),
          name: display,
          keyHash: hashKey(key),
          quotaBytes: quotaBytes || config.defaultQuotaBytes,
          created: new Date().toISOString(),
          disabled: false,
        };
        users.push(user);
        await writeUsers(users);
        await fsp.mkdir(userDir(user), { recursive: true });
        return { user, key };
      });
    },

    async listUsers() {
      await ensureLayout();
      const users = await readUsers();
      const rows = [];
      for (const user of users) {
        rows.push({
          name: user.name,
          disabled: Boolean(user.disabled),
          quotaBytes: user.quotaBytes,
          bytesUsed: await bytesUsed(user),
          folder: userDir(user),
        });
      }
      return rows;
    },

    async setQuota(name, quotaBytes) {
      return locked("users", async () => {
        const users = await readUsers();
        const user = users.find((item) => item.name.toLowerCase() === String(name).toLowerCase());
        if (!user) throw new Error(`No user named ${name}`);
        user.quotaBytes = quotaBytes;
        await writeUsers(users);
        return user;
      });
    },

    async revokeUser(name) {
      return locked("users", async () => {
        const users = await readUsers();
        const user = users.find((item) => item.name.toLowerCase() === String(name).toLowerCase());
        if (!user) throw new Error(`No user named ${name}`);
        user.disabled = true;
        user.keyHash = hashKey(newKey());
        await writeUsers(users);
      });
    },

    async rotateKey(name) {
      return locked("users", async () => {
        const users = await readUsers();
        const user = users.find((item) => item.name.toLowerCase() === String(name).toLowerCase());
        if (!user) throw new Error(`No user named ${name}`);
        const key = newKey();
        user.disabled = false;
        user.keyHash = hashKey(key);
        await writeUsers(users);
        return { user, key };
      });
    },

    async authenticate(presented) {
      if (!presented) throw new HttpError(401, "Bad key");
      const users = await readUsers();
      const digest = Buffer.from(hashKey(presented), "hex");
      for (const user of users) {
        if (!user.keyHash || user.disabled) continue;
        const want = Buffer.from(user.keyHash, "hex");
        if (want.length === digest.length && crypto.timingSafeEqual(want, digest)) return user;
      }
      throw new HttpError(401, "Bad key");
    },

    async profile(user) {
      const projects = await metas(user);
      const incoming = await publicStore.listIncoming(user, "open");
      return {
        name: user.name,
        bytesUsed: await bytesUsed(user),
        quotaBytes: user.quotaBytes,
        projects: projects.length,
        openIncoming: incoming.length,
      };
    },

    async listProjects(user) {
      return (await metas(user)).map(({ previewBytes, ...rest }) => rest);
    },

    async openProject(user, rawName) {
      const name = safeName(rawName);
      const zip = path.join(userDir(user), `${fileId(name)}.shapezip`);
      try {
        await fsp.access(zip);
      } catch {
        throw new HttpError(404, "No project with that name");
      }
      const meta = await readJson(path.join(userDir(user), `${fileId(name)}.meta.json`), null);
      return { name, file: zip, sha256: meta?.sha256 || "", bytes: (await fsp.stat(zip)).size };
    },

    saveProject(user, rawName, source, contentLength) {
      const name = safeName(rawName);
      return locked(user.id, async () => {
        await fsp.mkdir(userDir(user), { recursive: true });
        const projects = await metas(user);
        const previous = projects.find((project) => project.name === name);
        const room = user.quotaBytes - (await bytesUsed(user)) + (previous ? previous.bytes : 0);
        const dir = userDir(user);
        const saved = await receiveUpload(source, path.join(dir, `${fileId(name)}.shapezip`), {
          kind: "zip",
          maxBytes: room,
          declaredLength: contentLength,
          storageRoot: config.storagePath,
        });
        const meta = { name, bytes: saved.bytes, sha256: saved.sha256, updated: new Date().toISOString() };
        await writeJson(path.join(dir, `${fileId(name)}.meta.json`), meta);
        return { name: meta.name, bytes: meta.bytes, sha256: meta.sha256, updated: meta.updated };
      });
    },

    // Removes the server copy of a personal project and its preview.
    deleteProject(user, rawName) {
      const name = safeName(rawName);
      return locked(user.id, async () => {
        const dir = userDir(user);
        const zip = path.join(dir, `${fileId(name)}.shapezip`);
        try {
          await fsp.access(zip);
        } catch {
          throw new HttpError(404, "No project with that name");
        }
        await fsp.rm(zip, { force: true });
        await fsp.rm(path.join(dir, `${fileId(name)}.meta.json`), { force: true });
        await fsp.rm(path.join(dir, `${fileId(name)}.png`), { force: true });
        return { name, deleted: true };
      });
    },

    savePreview(user, rawName, source, contentLength) {
      const name = safeName(rawName);
      return locked(user.id, async () => {
        const dir = userDir(user);
        try {
          await fsp.access(path.join(dir, `${fileId(name)}.shapezip`));
        } catch {
          throw new HttpError(404, "No project with that name");
        }
        const saved = await receiveUpload(source, path.join(dir, `${fileId(name)}.png`), {
          kind: "png",
          maxBytes: PREVIEW_MAX_BYTES,
          declaredLength: contentLength,
          storageRoot: config.storagePath,
        });
        return { name, bytes: saved.bytes };
      });
    },

    async previewFile(user, rawName) {
      const name = safeName(rawName);
      const file = path.join(userDir(user), `${fileId(name)}.png`);
      const bytes = await fileSize(file);
      if (!bytes) throw new HttpError(404, "No preview");
      return { file, bytes };
    },

    async userById(id) {
      const users = await readUsers();
      return users.find((user) => user.id === id) || null;
    },
  };

  const publicStore = createPublicStore(config, store);
  store.public = publicStore;
  return store;
}
