// Public projects: a shared list every account can browse and copy. Only the owner changes a project
// directly; everyone else sends update requests that the owner accepts or declines.
//
// Layout under storagePath/public/<id>/:
//   meta.json                         title, description, tags, owner, version, history
//   versions/<n>.shapezip, <n>.png    every version and its preview
//   requests/<rid>/request.json       one update request
//   requests/<rid>/update.shapezip    its project (removed once declined or withdrawn)
//   requests/<rid>/preview.png
// (Stored files keep the .shapezip ending from when DaedalCAD was ShapeSynth, so existing data stays
// where it is; downloads are named .daedal.)
import crypto from "crypto";
import fsp from "fs/promises";
import path from "path";
import { HttpError, fileSize, readJson, receiveUpload, writeJson } from "./files.js";

export const LIMITS = {
  title: 120,
  description: 4000,
  tags: 10,
  tag: 32,
  message: 2000,
  note: 2000,
  openRequestsPerUser: 20,
  previewBytes: 4 * 1024 * 1024,
};

const STATUSES = new Set(["pending", "open", "accepted", "declined", "withdrawn"]);

function newId() {
  return crypto.randomBytes(8).toString("hex");
}

function checkId(id) {
  const text = String(id ?? "");
  if (!/^[0-9a-f]{16}$/.test(text)) throw new HttpError(404, "Not found");
  return text;
}

function cleanText(value, max, field, required = false) {
  const text = String(value ?? "").replace(/\r\n/g, "\n").trim();
  if (required && !text) throw new HttpError(400, `${field} is required`);
  if (text.length > max) throw new HttpError(400, `${field} is longer than ${max} characters`);
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(text)) throw new HttpError(400, `${field} contains control characters`);
  return text;
}

function cleanTags(value) {
  const list = Array.isArray(value) ? value : String(value ?? "").split(",");
  const tags = [];
  for (const raw of list) {
    const tag = cleanText(raw, LIMITS.tag, "Tag").toLowerCase();
    if (tag && !tags.includes(tag)) tags.push(tag);
  }
  if (tags.length > LIMITS.tags) throw new HttpError(400, `At most ${LIMITS.tags} tags`);
  return tags;
}

export function createPublicStore(config, base) {
  const root = () => path.resolve(config.storagePath, "public");

  function projectDir(id) {
    const dir = path.resolve(root(), checkId(id));
    if (!dir.startsWith(root() + path.sep)) throw new HttpError(404, "Not found");
    return dir;
  }

  function requestDir(id, rid) {
    return path.join(projectDir(id), "requests", checkId(rid));
  }

  async function readMeta(id) {
    const meta = await readJson(path.join(projectDir(id), "meta.json"), null);
    if (!meta || meta.deleted) throw new HttpError(404, "No public project with that id");
    return meta;
  }

  async function writeMeta(meta) {
    await writeJson(path.join(projectDir(meta.id), "meta.json"), meta);
  }

  async function readRequest(id, rid) {
    const request = await readJson(path.join(requestDir(id, rid), "request.json"), null);
    if (!request) throw new HttpError(404, "No request with that id");
    return request;
  }

  async function writeRequest(request) {
    await writeJson(path.join(requestDir(request.projectId, request.id), "request.json"), request);
  }

  async function allIds() {
    try {
      return (await fsp.readdir(root())).filter((name) => /^[0-9a-f]{16}$/.test(name));
    } catch (error) {
      if (error.code === "ENOENT") return [];
      throw error;
    }
  }

  async function allMetas() {
    const out = [];
    for (const id of await allIds()) {
      const meta = await readJson(path.join(root(), id, "meta.json"), null);
      if (meta && !meta.deleted && meta.version > 0) out.push(meta);
    }
    return out;
  }

  async function requestsOf(id) {
    const dir = path.join(projectDir(id), "requests");
    let names = [];
    try {
      names = await fsp.readdir(dir);
    } catch (error) {
      if (error.code === "ENOENT") return [];
      throw error;
    }
    const out = [];
    for (const rid of names) {
      if (!/^[0-9a-f]{16}$/.test(rid)) continue;
      const request = await readJson(path.join(dir, rid, "request.json"), null);
      if (request) out.push(request);
    }
    out.sort((a, b) => String(b.created).localeCompare(String(a.created)));
    return out;
  }

  function summary(meta, user, openCount) {
    const mine = meta.ownerId === user.id;
    return {
      id: meta.id,
      title: meta.title,
      description: meta.description,
      tags: meta.tags,
      author: meta.ownerName,
      mine,
      version: meta.version,
      bytes: meta.bytes,
      sha256: meta.sha256,
      created: meta.created,
      updated: meta.updated,
      downloads: meta.downloads || 0,
      openRequests: mine ? openCount : undefined,
    };
  }

  function requestView(request, meta) {
    return {
      id: request.id,
      projectId: request.projectId,
      projectTitle: meta ? meta.title : request.projectTitle,
      projectVersion: meta ? meta.version : undefined,
      owner: meta ? meta.ownerName : undefined,
      from: request.fromName,
      message: request.message,
      baseVersion: request.baseVersion,
      stale: meta ? request.status === "open" && request.baseVersion < meta.version : false,
      status: request.status,
      created: request.created,
      decided: request.decided || null,
      note: request.note || "",
      resultVersion: request.resultVersion || null,
      bytes: request.bytes || 0,
      hasPreview: Boolean(request.hasPreview),
    };
  }

  function ownerOnly(meta, user) {
    if (meta.ownerId !== user.id) throw new HttpError(403, "Only the author can do that");
  }

  async function roomFor(user) {
    return user.quotaBytes - (await base.bytesUsed(user));
  }

  return {
    LIMITS,

    // Bytes that count against this user: every version of projects they own, and the uploads of
    // their own requests that are still waiting.
    async usage(user) {
      let total = 0;
      for (const id of await allIds()) {
        const meta = await readJson(path.join(root(), id, "meta.json"), null);
        if (!meta || meta.deleted) continue;
        if (meta.ownerId === user.id) {
          for (const version of meta.versions || []) total += (version.bytes || 0) + (version.previewBytes || 0);
        }
        for (const request of await requestsOf(id)) {
          if (request.fromId === user.id && (request.status === "open" || request.status === "pending")) {
            total += (request.bytes || 0) + (request.previewBytes || 0);
          }
        }
      }
      return total;
    },

    async list(user, query) {
      const q = String(query || "").trim().toLowerCase();
      const out = [];
      for (const meta of await allMetas()) {
        if (q) {
          const hay = [meta.title, meta.description, meta.ownerName, ...(meta.tags || [])].join(" ").toLowerCase();
          if (!q.split(/\s+/).every((word) => hay.includes(word))) continue;
        }
        const open = meta.ownerId === user.id ? (await requestsOf(meta.id)).filter((r) => r.status === "open").length : 0;
        out.push(summary(meta, user, open));
      }
      out.sort((a, b) => String(b.updated).localeCompare(String(a.updated)));
      return out;
    },

    async details(user, id) {
      const meta = await readMeta(id);
      const requests = await requestsOf(id);
      const open = requests.filter((r) => r.status === "open").length;
      return {
        ...summary(meta, user, open),
        versions: (meta.versions || []).map((v) => ({
          version: v.version,
          bytes: v.bytes,
          sha256: v.sha256,
          created: v.created,
          author: v.author,
          fromRequest: v.fromRequest || null,
          hasPreview: (v.previewBytes || 0) > 0,
        })),
      };
    },

    // Step one of publishing: the record. It stays hidden until its first file arrives.
    async create(user, body) {
      const title = cleanText(body?.title, LIMITS.title, "Title", true);
      const description = cleanText(body?.description, LIMITS.description, "Description");
      const tags = cleanTags(body?.tags);
      const id = newId();
      const now = new Date().toISOString();
      const meta = {
        id,
        title,
        description,
        tags,
        ownerId: user.id,
        ownerName: user.name,
        version: 0,
        bytes: 0,
        sha256: "",
        created: now,
        updated: now,
        downloads: 0,
        versions: [],
      };
      await fsp.mkdir(path.join(projectDir(id), "versions"), { recursive: true });
      await writeMeta(meta);
      return { id };
    },

    update(user, id, body) {
      return base.locked(`public:${id}`, async () => {
        const meta = await readMeta(id);
        ownerOnly(meta, user);
        if (body?.title !== undefined) meta.title = cleanText(body.title, LIMITS.title, "Title", true);
        if (body?.description !== undefined) meta.description = cleanText(body.description, LIMITS.description, "Description");
        if (body?.tags !== undefined) meta.tags = cleanTags(body.tags);
        meta.updated = new Date().toISOString();
        await writeMeta(meta);
        return summary(meta, user, 0);
      });
    },

    remove(user, id) {
      return base.locked(`public:${id}`, async () => {
        const meta = await readMeta(id);
        ownerOnly(meta, user);
        await fsp.rm(projectDir(id), { recursive: true, force: true });
        return { id, deleted: true };
      });
    },

    // The owner adds a version directly (the first one, or a later one).
    putVersion(user, id, source, contentLength) {
      return base.locked(`public:${id}`, async () => {
        const meta = await readMeta(id);
        ownerOnly(meta, user);
        const next = meta.version + 1;
        const saved = await receiveUpload(source, path.join(projectDir(id), "versions", `${next}.shapezip`), {
          kind: "zip",
          maxBytes: await roomFor(user),
          declaredLength: contentLength,
          storageRoot: config.storagePath,
        });
        const now = new Date().toISOString();
        meta.version = next;
        meta.bytes = saved.bytes;
        meta.sha256 = saved.sha256;
        meta.updated = now;
        meta.versions = [...(meta.versions || []), { version: next, bytes: saved.bytes, sha256: saved.sha256, created: now, author: user.name, previewBytes: 0 }];
        await writeMeta(meta);
        return { id, version: next, bytes: saved.bytes, sha256: saved.sha256 };
      });
    },

    async openVersion(user, id, versionText) {
      const meta = await readMeta(id);
      const version = versionText ? Number(versionText) : meta.version;
      const entry = (meta.versions || []).find((v) => v.version === version);
      if (!entry) throw new HttpError(404, "No such version");
      const file = path.join(projectDir(id), "versions", `${version}.shapezip`);
      // Count copies by other people only; the author bringing their own copy along is not a download.
      if (meta.ownerId !== user.id) {
        await base.locked(`public:${id}`, async () => {
          const fresh = await readMeta(id);
          fresh.downloads = (fresh.downloads || 0) + 1;
          await writeMeta(fresh);
        });
      }
      return { file, bytes: entry.bytes, sha256: entry.sha256, version, title: meta.title };
    },

    putPreview(user, id, source, contentLength) {
      return base.locked(`public:${id}`, async () => {
        const meta = await readMeta(id);
        ownerOnly(meta, user);
        const saved = await receiveUpload(source, path.join(projectDir(id), "versions", `${meta.version}.png`), {
          kind: "png",
          maxBytes: LIMITS.previewBytes,
          declaredLength: contentLength,
          storageRoot: config.storagePath,
        });
        const entry = meta.versions.find((v) => v.version === meta.version);
        if (entry) entry.previewBytes = saved.bytes;
        await writeMeta(meta);
        return { id, version: meta.version, bytes: saved.bytes };
      });
    },

    async previewFile(user, id, versionText) {
      const meta = await readMeta(id);
      const version = versionText ? Number(versionText) : meta.version;
      const file = path.join(projectDir(id), "versions", `${Number.isInteger(version) ? version : 0}.png`);
      const bytes = await fileSize(file);
      if (!bytes) throw new HttpError(404, "No preview");
      return { file, bytes };
    },

    // --- update requests ----------------------------------------------------------------------

    async createRequest(user, id, body) {
      const meta = await readMeta(id);
      if (meta.ownerId === user.id) throw new HttpError(400, "You are the author: publish a new version instead");
      const message = cleanText(body?.message, LIMITS.message, "Message", true);
      const baseVersion = Number(body?.baseVersion);
      if (!Number.isInteger(baseVersion) || baseVersion < 1 || baseVersion > meta.version) {
        throw new HttpError(400, "Base version is not a version of this project");
      }
      let open = 0;
      for (const other of await allIds()) {
        for (const request of await requestsOf(other)) {
          if (request.fromId === user.id && (request.status === "open" || request.status === "pending")) ++open;
        }
      }
      if (open >= LIMITS.openRequestsPerUser) {
        throw new HttpError(429, `You already have ${LIMITS.openRequestsPerUser} open requests`);
      }
      const request = {
        id: newId(),
        projectId: id,
        projectTitle: meta.title,
        fromId: user.id,
        fromName: user.name,
        message,
        baseVersion,
        status: "pending",
        created: new Date().toISOString(),
        bytes: 0,
        sha256: "",
        hasPreview: false,
        previewBytes: 0,
      };
      await fsp.mkdir(requestDir(id, request.id), { recursive: true });
      await writeRequest(request);
      return { id: request.id, status: request.status };
    },

    putRequestFile(user, id, rid, source, contentLength) {
      return base.locked(`public:${id}`, async () => {
        await readMeta(id);
        const request = await readRequest(id, rid);
        if (request.fromId !== user.id) throw new HttpError(403, "Only the sender can upload this request");
        if (request.status !== "pending" && request.status !== "open") throw new HttpError(409, "This request is closed");
        const previous = request.bytes || 0;
        const saved = await receiveUpload(source, path.join(requestDir(id, rid), "update.shapezip"), {
          kind: "zip",
          maxBytes: (await roomFor(user)) + previous,
          declaredLength: contentLength,
          storageRoot: config.storagePath,
        });
        request.bytes = saved.bytes;
        request.sha256 = saved.sha256;
        request.status = "open";
        await writeRequest(request);
        return requestView(request, await readMeta(id));
      });
    },

    putRequestPreview(user, id, rid, source, contentLength) {
      return base.locked(`public:${id}`, async () => {
        const request = await readRequest(id, rid);
        if (request.fromId !== user.id) throw new HttpError(403, "Only the sender can upload this request");
        if (request.status !== "pending" && request.status !== "open") throw new HttpError(409, "This request is closed");
        const saved = await receiveUpload(source, path.join(requestDir(id, rid), "preview.png"), {
          kind: "png",
          maxBytes: LIMITS.previewBytes,
          declaredLength: contentLength,
          storageRoot: config.storagePath,
        });
        request.hasPreview = true;
        request.previewBytes = saved.bytes;
        await writeRequest(request);
        return { id: rid, bytes: saved.bytes };
      });
    },

    async requestFile(user, id, rid, which) {
      const meta = await readMeta(id);
      const request = await readRequest(id, rid);
      if (meta.ownerId !== user.id && request.fromId !== user.id) throw new HttpError(403, "Not your request");
      const file = path.join(requestDir(id, rid), which === "preview" ? "preview.png" : "update.shapezip");
      const bytes = await fileSize(file);
      if (!bytes) throw new HttpError(404, which === "preview" ? "No preview" : "This request has no file");
      return { file, bytes, sha256: which === "preview" ? "" : request.sha256, title: meta.title };
    },

    decide(user, id, rid, action, body) {
      return base.locked(`public:${id}`, async () => {
        const meta = await readMeta(id);
        const request = await readRequest(id, rid);
        const now = new Date().toISOString();
        if (action === "withdraw") {
          if (request.fromId !== user.id) throw new HttpError(403, "Only the sender can withdraw a request");
        } else {
          ownerOnly(meta, user);
        }
        if (request.status !== "open") {
          throw new HttpError(409, request.status === "pending" ? "This request has no file yet" : `This request is already ${request.status}`);
        }
        const dir = requestDir(id, rid);
        if (action === "accept") {
          const next = meta.version + 1;
          const versions = path.join(projectDir(id), "versions");
          await fsp.rename(path.join(dir, "update.shapezip"), path.join(versions, `${next}.shapezip`));
          let previewBytes = 0;
          if (await fileSize(path.join(dir, "preview.png"))) {
            await fsp.copyFile(path.join(dir, "preview.png"), path.join(versions, `${next}.png`));
            previewBytes = await fileSize(path.join(versions, `${next}.png`));
          }
          meta.version = next;
          meta.bytes = request.bytes;
          meta.sha256 = request.sha256;
          meta.updated = now;
          meta.versions.push({
            version: next,
            bytes: request.bytes,
            sha256: request.sha256,
            created: now,
            author: request.fromName,
            fromRequest: request.id,
            previewBytes,
          });
          await writeMeta(meta);
          request.status = "accepted";
          request.resultVersion = next;
        } else if (action === "decline" || action === "withdraw") {
          await fsp.rm(path.join(dir, "update.shapezip"), { force: true });
          request.status = action === "decline" ? "declined" : "withdrawn";
        } else {
          throw new HttpError(400, "Unknown action");
        }
        request.decided = now;
        request.note = action === "withdraw" ? "" : cleanText(body?.note, LIMITS.note, "Note");
        if (!STATUSES.has(request.status)) throw new HttpError(500, "Bad status");
        await writeRequest(request);
        return requestView(request, meta);
      });
    },

    async listIncoming(user, status = "open") {
      const out = [];
      for (const meta of await allMetas()) {
        if (meta.ownerId !== user.id) continue;
        for (const request of await requestsOf(meta.id)) {
          if (request.status === "pending") continue;
          if (status && status !== "all" && request.status !== status) continue;
          out.push(requestView(request, meta));
        }
      }
      out.sort((a, b) => String(b.created).localeCompare(String(a.created)));
      return out;
    },

    async listOutgoing(user) {
      const out = [];
      for (const id of await allIds()) {
        const meta = await readJson(path.join(root(), id, "meta.json"), null);
        if (!meta || meta.deleted) continue;
        for (const request of await requestsOf(id)) {
          if (request.fromId !== user.id || request.status === "pending") continue;
          out.push(requestView(request, meta));
        }
      }
      out.sort((a, b) => String(b.created).localeCompare(String(a.created)));
      return out;
    },
  };
}
