#!/usr/bin/env node
import { formatBytes, loadConfig, parseBytes } from "./config.js";
import { listen } from "./server.js";
import { createStore } from "./store.js";
import { createReleases, zipFolderVersion } from "./releases.js";
import { createAdminAuth, listenAdmin } from "./admin.js";
import readline from "readline";

// Reads a password from the terminal without showing it.
function askHidden(question) {
  return new Promise((resolve) => {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout, terminal: true });
    rl._writeToOutput = (text) => {
      if (text.includes(question)) process.stdout.write(text);
    };
    rl.question(question, (answer) => {
      rl.close();
      process.stdout.write("\n");
      resolve(answer);
    });
  });
}

function parseArgs(argv) {
  const flags = {};
  const pos = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (!arg.startsWith("--")) {
      pos.push(arg);
      continue;
    }
    const key = arg.slice(2);
    const next = argv[i + 1];
    if (next && !next.startsWith("--")) {
      flags[key] = next;
      i++;
    } else {
      flags[key] = true;
    }
  }
  return { flags, pos };
}

function die(error) {
  console.error(error.message || error);
  process.exit(1);
}

const { flags, pos } = parseArgs(process.argv.slice(2));
const [group, action, ...rest] = pos;

try {
  const config = loadConfig(flags);
  const store = createStore(config);

  if (group === "serve" || (!group && !action)) {
    await store.ensureLayout();
    const server = await listen(store);
    const address = server.address();
    console.log(`DaedalSync listening on http://${config.host}:${address.port}`);
    console.log(`Storage ${config.storagePath}`);
    console.log(`Users   ${config.usersPath}`);
    if (config.adminPort) {
      try {
        const admin = await listenAdmin(store);
        const shown = config.adminHost === "0.0.0.0" || config.adminHost === "::" ? "<this machine's address>" : config.adminHost;
        console.log(`Admin   http://${shown}:${admin.address().port}  (answers ${config.adminAllow} addresses only)`);
      } catch (error) {
        console.error(`Admin page not started: ${error.message}`);
      }
    }
  } else if (group === "user" && action === "add") {
    const name = rest.join(" ").trim();
    const quota = flags.quota ? parseBytes(flags.quota) : config.defaultQuotaBytes;
    const { user, key } = await store.addUser(name, quota);
    console.log(`User  ${user.name}`);
    console.log(`Key   ${key}`);
    console.log(`Quota ${formatBytes(user.quotaBytes)}`);
    console.log(`Folder ${config.storagePath}/users/${user.id}`);
    console.log("Give this key to that person once. It is not shown again.");
  } else if (group === "user" && action === "list") {
    const users = await store.listUsers();
    if (!users.length) console.log("No users.");
    for (const user of users) {
      const state = user.disabled ? "revoked" : "active";
      console.log(`${user.name}  ${state}  ${formatBytes(user.bytesUsed)} / ${formatBytes(user.quotaBytes)}`);
    }
  } else if (group === "user" && action === "quota") {
    const quotaText = rest[rest.length - 1];
    const name = rest.slice(0, -1).join(" ").trim();
    const user = await store.setQuota(name, parseBytes(quotaText));
    console.log(`${user.name} quota is ${formatBytes(user.quotaBytes)}`);
  } else if (group === "user" && action === "revoke") {
    const name = rest.join(" ").trim();
    await store.revokeUser(name);
    console.log(`${name} can no longer sync. Their files are still on disk.`);
  } else if (group === "user" && action === "key") {
    const name = rest.join(" ").trim();
    const { user, key } = await store.rotateKey(name);
    console.log(`User ${user.name}`);
    console.log(`Key  ${key}`);
    console.log("The previous key no longer works.");
  } else if (group === "admin" && action === "password") {
    await store.ensureLayout();
    const password = typeof flags.password === "string" ? flags.password : await askHidden("New admin password (8+ characters): ");
    await createAdminAuth(config).setPassword(password);
    console.log("Admin password set. Running servers use it straight away.");
  } else if (group === "release" || group === "login") {
    const releases = createReleases(config);
    await store.ensureLayout();
    if (group === "release" && action === "add") {
      const zip = rest[0];
      if (!zip) throw new Error("Give the file: node src/cli.js release add <zip or setup.exe> [--version 0.7.0] [--notes \"...\"]");
      const installer = /\.exe$/i.test(zip);
      const guessed = (zip.match(/(?:DaedalCAD|ShapeSynth)-([0-9A-Za-z._-]+?)-(?:windows|setup)/i) || [])[1];
      const version = flags.version || guessed || (installer ? null : await zipFolderVersion(zip).catch(() => null));
      if (!version) throw new Error("Could not read the version from the file. Add --version 0.7.0");
      const notes = typeof flags.notes === "string" ? flags.notes : "";
      const entry = installer
        ? await releases.addInstaller(zip, version, notes, { replace: Boolean(flags.replace) })
        : await releases.addRelease(zip, version, notes);
      console.log(`Added the ${installer ? "installer" : "zip"} of DaedalCAD ${entry.version}.`);
    } else if (group === "release" && action === "list") {
      const rows = await releases.listReleases();
      if (!rows.length) console.log("No versions yet.");
      for (const r of rows) {
        const parts = [r.file ? `zip ${formatBytes(r.bytes)}` : null, r.installer ? `installer ${formatBytes(r.installer.bytes)}` : null].filter(Boolean);
        console.log(`${r.version}  ${parts.join(", ")}  ${r.added.slice(0, 10)}${r.notes ? "  " + r.notes : ""}`);
      }
    } else if (group === "release" && action === "remove") {
      await releases.removeRelease(rest.join(" ").trim());
      console.log("Removed.");
    } else if (group === "login" && action === "add") {
      const { name, password } = await releases.addLogin(rest.join(" ").trim(), typeof flags.password === "string" ? flags.password : "");
      console.log(`Download login ${name}`);
      console.log(`Password ${password}`);
      console.log("Give these to that person. The password is not shown again.");
    } else if (group === "login" && action === "password") {
      const { name, password } = await releases.setPassword(rest.join(" ").trim(), typeof flags.password === "string" ? flags.password : "");
      console.log(`Download login ${name}`);
      console.log(`Password ${password}`);
    } else if (group === "login" && action === "remove") {
      await releases.removeLogin(rest.join(" ").trim());
      console.log("Removed. That login can no longer download.");
    } else if (group === "login" && action === "list") {
      const rows = await releases.listLogins();
      if (!rows.length) console.log("No download logins.");
      for (const r of rows) console.log(`${r.name}  added ${r.created.slice(0, 10)}`);
    } else {
      throw new Error(`Unknown command: ${group} ${action || ""}`);
    }
  } else {
    console.log(`Usage:
  node src/cli.js serve [--config config.json]
  node src/cli.js user add <name> [--quota 5G]
  node src/cli.js user list
  node src/cli.js user quota <name> <size>
  node src/cli.js user key <name>
  node src/cli.js user revoke <name>

  node src/cli.js release add <zip or setup.exe> [--version 0.7.0] [--notes "text"] [--replace]
  node src/cli.js release list
  node src/cli.js release remove <version>
  node src/cli.js login add <name> [--password secret]
  node src/cli.js login password <name> [--password secret]
  node src/cli.js login list
  node src/cli.js login remove <name>
  node src/cli.js admin password [--password secret]

The admin web page (releases, download logins, sync users) runs on adminPort (5003) next to
serve, for your own network only. Set its password on the first visit or with "admin password".

Config keys: port (5002), host, storagePath, usersPath, defaultQuota, trustProxy,
             adminPort (5003, 0 = off), adminHost (0.0.0.0), adminAllow ("private" or CIDRs).
Env overrides: DAEDALSYNC_PORT, DAEDALSYNC_HOST, DAEDALSYNC_STORAGE, DAEDALSYNC_USERS, DAEDALSYNC_QUOTA.`);
    process.exit(group ? 1 : 0);
  }
} catch (error) {
  die(error);
}
