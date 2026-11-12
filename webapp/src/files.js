// Shared helpers for the stores: errors, JSON files, and checked uploads.
import crypto from "crypto";
import fs from "fs";
import fsp from "fs/promises";
import path from "path";
import { pipeline } from "stream/promises";
import { Transform } from "stream";

export class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

export async function readJson(file, fallback) {
  try {
    return JSON.parse(await fsp.readFile(file, "utf8"));
  } catch (error) {
    if (error.code === "ENOENT") return fallback;
    throw error;
  }
}

export async function writeJson(file, value) {
  await fsp.mkdir(path.dirname(file), { recursive: true });
  const tmp = `${file}.${process.pid}.${crypto.randomBytes(4).toString("hex")}.tmp`;
  await fsp.writeFile(tmp, JSON.stringify(value, null, 2));
  await fsp.rename(tmp, file);
}

export async function fileSize(file) {
  try {
    return (await fsp.stat(file)).size;
  } catch (error) {
    if (error.code === "ENOENT") return 0;
    throw error;
  }
}

export async function freeBytes(dir) {
  const stat = await fsp.statfs(dir);
  return Number(stat.bavail) * Number(stat.bsize);
}

// One lock chain per key: work queued on the same key runs one at a time.
export function createLocks() {
  const chains = new Map();
  return function locked(key, fn) {
    const prev = chains.get(key) || Promise.resolve();
    const run = prev.then(fn, fn);
    chains.set(key, run.then(() => {}, () => {}));
    return run;
  };
}

const SIGNATURES = {
  zip: { bytes: [0x50, 0x4b], message: "Expected a zip file" },
  png: { bytes: [0x89, 0x50, 0x4e, 0x47], message: "Expected a PNG image" },
};

// Streams a request body into `finalPath`, checking the file signature, the size limit and the
// free disk space, and returns { bytes, sha256 }. Nothing is left behind when it fails.
export async function receiveUpload(source, finalPath, { kind, maxBytes, declaredLength, storageRoot }) {
  const signature = SIGNATURES[kind];
  if (maxBytes < 1) throw new HttpError(413, "Quota exceeded");
  const declared = Number(declaredLength);
  if (Number.isFinite(declared) && declared > maxBytes) throw new HttpError(413, kind === "png" ? "Image too large" : "Quota exceeded");
  if (Number.isFinite(declared) && declared > (await freeBytes(storageRoot))) {
    throw new HttpError(507, "Not enough free disk space");
  }
  await fsp.mkdir(path.dirname(finalPath), { recursive: true });
  const tmp = path.join(path.dirname(finalPath), `.upload-${crypto.randomBytes(6).toString("hex")}.tmp`);
  const checker = new Transform({
    construct(done) {
      this.hash = crypto.createHash("sha256");
      this.size = 0;
      this.header = Buffer.alloc(0);
      this.checked = false;
      done();
    },
    transform(chunk, _enc, done) {
      this.size += chunk.length;
      if (this.size > maxBytes) {
        done(new HttpError(413, kind === "png" ? "Image too large" : "Quota exceeded"));
        return;
      }
      if (!this.checked) {
        this.header = Buffer.concat([this.header, chunk]).subarray(0, signature.bytes.length);
        if (this.header.length >= signature.bytes.length) {
          this.checked = true;
          if (!signature.bytes.every((b, i) => this.header[i] === b)) {
            done(new HttpError(400, signature.message));
            return;
          }
        }
      }
      this.hash.update(chunk);
      done(null, chunk);
    },
  });
  try {
    await pipeline(source, checker, fs.createWriteStream(tmp));
    if (!checker.size) throw new HttpError(400, "Empty upload");
    if (!checker.checked) throw new HttpError(400, signature.message);
    await fsp.rename(tmp, finalPath);
    return { bytes: checker.size, sha256: checker.hash.digest("hex") };
  } catch (error) {
    await fsp.rm(tmp, { force: true });
    throw error;
  }
}
