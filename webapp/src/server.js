import express from "express";
import fs from "fs";
import http from "http";
import { createReleases } from "./releases.js";

function sendError(res, error) {
  const status = error.status || (error.type === "entity.parse.failed" ? 400 : 500);
  if (status >= 500) console.error(error);
  res.status(status).json({ error: status >= 500 ? "Server error" : error.message });
}

function presentedKey(req) {
  const header = req.get("authorization") || "";
  if (header.toLowerCase().startsWith("bearer ")) return header.slice(7).trim();
  return (req.get("x-api-key") || "").trim();
}

function streamFile(res, file, bytes, type, extra = {}) {
  res.setHeader("Content-Type", type);
  res.setHeader("Content-Length", String(bytes));
  for (const [key, value] of Object.entries(extra)) if (value) res.setHeader(key, value);
  fs.createReadStream(file).pipe(res);
}

export function createApp(store) {
  const app = express();
  app.disable("x-powered-by");
  // Behind nginx every request comes from nginx; trust its X-Forwarded-For so req.ip is the real
  // visitor (the download lockout counts wrong passwords per visitor). Only the proxies named in
  // the config are believed, so a visitor cannot fake the header when connecting directly.
  const trust = store.config.trustProxy ?? "loopback";
  app.set("trust proxy", trust === "none" ? false : trust.split(",").map((t) => t.trim()).filter(Boolean));
  const json = express.json({ limit: "64kb" });

  app.get("/", (_req, res) => {
    res.json({ service: "daedalsync", ok: true, public: true });
  });

  async function requireUser(req, res, next) {
    try {
      req.user = await store.authenticate(presentedKey(req));
      next();
    } catch (error) {
      sendError(res, error);
    }
  }

  // Wraps an async handler so every failure becomes a JSON error.
  const route = (fn) => async (req, res) => {
    try {
      await fn(req, res);
    } catch (error) {
      sendError(res, error);
    }
  };

  // App downloads (packaged DaedalCAD builds), HTTP Basic auth with download logins.
  const releases = store.releases || createReleases(store.config);
  const login = releases.requireLogin();
  app.get("/downloads", login, route(async (req, res) => releases.listHandler(req, res)));
  app.get("/download", login, route(async (req, res) => releases.downloadHandler(req, res)));
  app.get("/downloads/latest", login, route(async (req, res) => releases.latestHandler(req, res)));

  app.use("/v1", requireUser);

  // --- app updates with the sync key (DaedalCAD's updater) -----------------------------------
  app.get("/v1/app/latest", route(async (req, res) => releases.latestHandler(req, res)));
  app.get("/v1/app/download", route(async (req, res) => releases.downloadHandler(req, res)));

  // --- account and personal projects ---------------------------------------------------------
  app.get("/v1/me", route(async (req, res) => res.json(await store.profile(req.user))));

  app.get("/v1/projects", route(async (req, res) => res.json({ projects: await store.listProjects(req.user) })));

  app.get("/v1/projects/:name", route(async (req, res) => {
    const project = await store.openProject(req.user, req.params.name);
    streamFile(res, project.file, project.bytes, "application/zip", {
      "X-Sha256": project.sha256,
      "Content-Disposition": `attachment; filename="${encodeURIComponent(project.name)}.daedal"`,
    });
  }));

  app.put("/v1/projects/:name", route(async (req, res) => {
    res.json(await store.saveProject(req.user, req.params.name, req, req.get("content-length")));
  }));

  app.delete("/v1/projects/:name", route(async (req, res) => res.json(await store.deleteProject(req.user, req.params.name))));

  app.get("/v1/projects/:name/preview", route(async (req, res) => {
    const preview = await store.previewFile(req.user, req.params.name);
    streamFile(res, preview.file, preview.bytes, "image/png");
  }));

  app.put("/v1/projects/:name/preview", route(async (req, res) => {
    res.json(await store.savePreview(req.user, req.params.name, req, req.get("content-length")));
  }));

  // --- public projects -----------------------------------------------------------------------
  const pub = store.public;

  app.get("/v1/public", route(async (req, res) => res.json({ projects: await pub.list(req.user, req.query.q) })));
  app.post("/v1/public", json, route(async (req, res) => res.status(201).json(await pub.create(req.user, req.body))));
  app.get("/v1/public/:id", route(async (req, res) => res.json(await pub.details(req.user, req.params.id))));
  app.patch("/v1/public/:id", json, route(async (req, res) => res.json(await pub.update(req.user, req.params.id, req.body))));
  app.delete("/v1/public/:id", route(async (req, res) => res.json(await pub.remove(req.user, req.params.id))));

  app.put("/v1/public/:id/file", route(async (req, res) => {
    res.json(await pub.putVersion(req.user, req.params.id, req, req.get("content-length")));
  }));
  app.get("/v1/public/:id/file", route(async (req, res) => {
    const found = await pub.openVersion(req.user, req.params.id, req.query.version);
    streamFile(res, found.file, found.bytes, "application/zip", {
      "X-Sha256": found.sha256,
      "X-Version": String(found.version),
      "Content-Disposition": `attachment; filename="${encodeURIComponent(found.title)}.daedal"`,
    });
  }));
  app.put("/v1/public/:id/preview", route(async (req, res) => {
    res.json(await pub.putPreview(req.user, req.params.id, req, req.get("content-length")));
  }));
  app.get("/v1/public/:id/preview", route(async (req, res) => {
    const preview = await pub.previewFile(req.user, req.params.id, req.query.version);
    streamFile(res, preview.file, preview.bytes, "image/png");
  }));

  // --- update requests -----------------------------------------------------------------------
  app.post("/v1/public/:id/requests", json, route(async (req, res) => {
    res.status(201).json(await pub.createRequest(req.user, req.params.id, req.body));
  }));
  app.put("/v1/public/:id/requests/:rid/file", route(async (req, res) => {
    res.json(await pub.putRequestFile(req.user, req.params.id, req.params.rid, req, req.get("content-length")));
  }));
  app.put("/v1/public/:id/requests/:rid/preview", route(async (req, res) => {
    res.json(await pub.putRequestPreview(req.user, req.params.id, req.params.rid, req, req.get("content-length")));
  }));
  app.get("/v1/public/:id/requests/:rid/file", route(async (req, res) => {
    const found = await pub.requestFile(req.user, req.params.id, req.params.rid, "file");
    streamFile(res, found.file, found.bytes, "application/zip", { "X-Sha256": found.sha256 });
  }));
  app.get("/v1/public/:id/requests/:rid/preview", route(async (req, res) => {
    const found = await pub.requestFile(req.user, req.params.id, req.params.rid, "preview");
    streamFile(res, found.file, found.bytes, "image/png");
  }));
  for (const action of ["accept", "decline", "withdraw"]) {
    app.post(`/v1/public/:id/requests/:rid/${action}`, json, route(async (req, res) => {
      res.json(await pub.decide(req.user, req.params.id, req.params.rid, action, req.body || {}));
    }));
  }
  app.get("/v1/requests/incoming", route(async (req, res) => {
    res.json({ requests: await pub.listIncoming(req.user, req.query.status || "open") });
  }));
  app.get("/v1/requests/outgoing", route(async (req, res) => res.json({ requests: await pub.listOutgoing(req.user) })));

  app.use((error, _req, res, _next) => sendError(res, error));
  return app;
}

export function listen(store) {
  const app = createApp(store);
  const server = http.createServer(app);
  server.requestTimeout = 0;
  server.timeout = 0;
  server.headersTimeout = 60_000;
  return new Promise((resolve) => {
    server.listen(store.config.port, store.config.host, () => resolve(server));
  });
}
