import fs from "fs";
import path from "path";

export function parseBytes(text) {
  const raw = String(text).trim();
  const match = raw.match(/^(\d+(?:\.\d+)?)\s*([KMGT]?)(?:i?B)?$/i);
  if (!match) throw new Error("Size must look like 5G, 500M, or a byte count");
  const value = Number(match[1]);
  const unit = (match[2] || "").toUpperCase();
  const scale = { "": 1, K: 1024, M: 1024 ** 2, G: 1024 ** 3, T: 1024 ** 4 }[unit];
  const bytes = Math.floor(value * scale);
  if (!Number.isFinite(bytes) || bytes < 1) throw new Error("Size must be at least 1 byte");
  return bytes;
}

export function formatBytes(bytes) {
  const n = Number(bytes) || 0;
  if (n < 1024) return `${n} B`;
  const units = ["KiB", "MiB", "GiB", "TiB"];
  let value = n / 1024;
  let unit = units[0];
  for (let i = 0; i < units.length; i++) {
    unit = units[i];
    if (value < 1024 || i === units.length - 1) break;
    value /= 1024;
  }
  return `${value.toFixed(value >= 10 ? 0 : 1)} ${unit}`;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, "utf8"));
}

// DAEDALSYNC_<NAME>, or SHAPESYNC_<NAME> from when the server was called ShapeSync.
function env(name) {
  return process.env[`DAEDALSYNC_${name}`] ?? process.env[`SHAPESYNC_${name}`];
}

export function loadConfig(flags = {}) {
  const fromFlag = flags.config;
  const fromEnv = env("CONFIG");
  const local = path.resolve("config.json");
  const file = fromFlag || fromEnv || (fs.existsSync(local) ? local : "");
  const json = file ? readJson(file) : {};
  const storagePath = path.resolve(flags.storage || env("STORAGE") || json.storagePath || "./data");
  const usersSetting = flags.users || env("USERS") || json.usersPath || "";
  const usersPath = usersSetting ? path.resolve(usersSetting) : path.join(storagePath, "users.json");
  const port = Number(flags.port || env("PORT") || json.port || 5002);
  const host = String(flags.host || env("HOST") || json.host || "127.0.0.1");
  const quotaText = flags.quota || env("QUOTA") || json.defaultQuota || "5G";
  // Admin web page: its own port, for your own network only (0 turns it off). It listens on all
  // interfaces by default so other machines at home can reach it, and only answers private addresses.
  const adminPort = Number(flags["admin-port"] ?? env("ADMIN_PORT") ?? json.adminPort ?? 5003);
  const adminHost = String(flags["admin-host"] || env("ADMIN_HOST") || json.adminHost || "0.0.0.0");
  const adminAllow = String(flags["admin-allow"] || env("ADMIN_ALLOW") || json.adminAllow || "private");
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("Port must be 1-65535");
  if (!Number.isInteger(adminPort) || adminPort < 0 || adminPort > 65535) throw new Error("adminPort must be 0-65535 (0 = off)");
  if (adminPort && adminPort === port) throw new Error("adminPort must differ from port");
  return {
    port,
    host,
    storagePath,
    usersPath,
    defaultQuotaBytes: typeof quotaText === "number" ? quotaText : parseBytes(quotaText),
    adminPort,
    adminHost,
    adminAllow,
    configFile: file || null,
  };
}
