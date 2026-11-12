#!/usr/bin/env node
import { formatBytes, loadConfig, parseBytes } from "./config.js";
import { listen } from "./server.js";
import { createStore } from "./store.js";
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
  } else {
    console.log(`Usage:
  node src/cli.js serve [--config config.json]
  node src/cli.js user add <name> [--quota 5G]
  node src/cli.js user list
  node src/cli.js user quota <name> <size>
  node src/cli.js user key <name>
  node src/cli.js user revoke <name>
  node src/cli.js admin password [--password secret]

The admin web page (sync users) runs on adminPort (5003) next to serve, for your own network
only. Set its password on the first visit or with "admin password".

Config keys: port (5002), host, storagePath, usersPath, defaultQuota,
             adminPort (5003, 0 = off), adminHost (0.0.0.0), adminAllow ("private" or CIDRs).
Env overrides: DAEDALSYNC_PORT, DAEDALSYNC_HOST, DAEDALSYNC_STORAGE, DAEDALSYNC_USERS, DAEDALSYNC_QUOTA.`);
    process.exit(group ? 1 : 0);
  }
} catch (error) {
  die(error);
}
