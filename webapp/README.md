# DaedalSync

The project sync server for DaedalCAD's **CloudSync**. Run it on a machine at home or a small
server, give each person a key, and they can back up their projects, publish designs to the group,
copy each other's designs and send update requests. DaedalCAD itself stays offline: it only talks to
this server when someone presses a button in the CloudSync window.

DaedalSync does not hand out DaedalCAD itself: new versions are on the GitHub releases page, and
DaedalCAD updates itself from there.

## 1. Start it

You need Node.js 18 or newer.

```
npm install
node src/cli.js serve
```

It stores everything in `./data` and listens on `127.0.0.1:5002`. To change that, copy
`config.example.json` to `config.json` and edit it (see section 5). Keep it running with
whatever you like (a systemd service, pm2, Task Scheduler).

## 2. Add people

Open the **admin page** on `http://<the server's address on your network>:5003`. On the first visit
you choose the admin password. Under **Sync users**, add a person with a quota (for example `5G`):
the page shows their key once. Send them the key; you can always make a new one, which stops the
old one.

The same from the command line, on the server:

```
node src/cli.js user add Alex --quota 5G   # prints the key once
node src/cli.js user list
node src/cli.js user quota Alex 10G
node src/cli.js user key Alex              # a new key; the old one stops working
node src/cli.js user revoke Alex           # locks the account, keeps the files
node src/cli.js admin password             # sets or resets the admin page password
```

The admin page only answers on your own network (loopback and private addresses, judged by the
connection itself). **Never put port 5003 in Nginx Proxy Manager or forward it on your router.**
For a server you only reach over SSH: set `"adminHost": "127.0.0.1"` and use
`ssh -L 5003:127.0.0.1:5003 you@server`.

## 3. Make it reachable with HTTPS

Put a proxy with an SSL certificate in front of port 5002, so people outside your home can sync and
keys never travel unencrypted.

**Nginx Proxy Manager:** add a proxy host for your hostname pointing at `http://<server>:5002`,
request an SSL certificate, and under the host's **Advanced** tab add:

```
client_max_body_size 0;
```

**nginx:**

```
location / {
    proxy_pass http://127.0.0.1:5002;
    proxy_set_header Host $host;
    client_max_body_size 0;
}
```

If the proxy runs on another machine (or in Docker), set `"host": "0.0.0.0"` so it can reach the
server, and keep port 5002 closed on your router.

## 4. Connect DaedalCAD

In DaedalCAD, open **CloudSync**, fill in **Host** (`https://your-hostname`) and the **User key**,
and save. The **Personal** tab is your own backups; the **Public** tab is what everyone on this
server has published. `docs/cloudsync.md` in the DaedalCAD source describes exactly how publishing,
copying and update requests behave.

## 5. Settings

`config.json`, all optional:

| Key | Meaning |
|---|---|
| `port` | Port for DaedalCAD (behind the proxy). Default `5002`. |
| `host` | Address to listen on. Default `127.0.0.1`: only a proxy on this machine can reach it. |
| `storagePath` | Folder for everything stored. Default `./data`. |
| `usersPath` | Separate file for the user list. Empty: `storagePath/users.json`. |
| `defaultQuota` | Quota for new users, such as `5G` or `500M`. Default `5G`. |
| `adminPort` | Admin page port. Default `5003`; `0` turns the page off. |
| `adminHost` | Admin page address. Default `0.0.0.0`, so other machines at home can open it. |
| `adminAllow` | Who the admin page answers: `private` (default) or a list such as `"192.168.1.0/24, 127.0.0.1"`. |

Environment variables override the file: `DAEDALSYNC_PORT`, `DAEDALSYNC_HOST`,
`DAEDALSYNC_STORAGE`, `DAEDALSYNC_USERS`, `DAEDALSYNC_QUOTA`, `DAEDALSYNC_CONFIG` (another
config file), `DAEDALSYNC_ADMIN_PORT`, `DAEDALSYNC_ADMIN_HOST`, `DAEDALSYNC_ADMIN_ALLOW`.

What is on disk: `users/<id>/` holds each person's projects, `public/<id>/` each published project
with every version, `users.json` the accounts (only a hash of each key) and `admin.json` the admin
password hash. Back up the whole `storagePath` folder.

## 6. API

Every route needs a key: `Authorization: Bearer <key>`.

Personal projects (one key sees only its own):

- `GET /v1/me`: `name`, `bytesUsed`, `quotaBytes`, `projects`, `openIncoming` (open update requests on your public projects).
- `GET /v1/projects`: `name`, `bytes`, `sha256`, `updated`, `hasPreview`.
- `PUT /v1/projects/:name` stores a zip body, replacing that name. `413` over quota, `400` if the body is not a zip.
- `GET /v1/projects/:name` streams the zip; `X-Sha256` is the stored hash.
- `DELETE /v1/projects/:name` removes the server copy and its preview.
- `PUT/GET /v1/projects/:name/preview`: a PNG preview (4 MB max).

Public projects and update requests:

- `GET /v1/public?q=`, `POST /v1/public` (JSON title, description, tags), `GET/PATCH/DELETE /v1/public/:id`
- `PUT /v1/public/:id/file` (owner adds a version), `GET /v1/public/:id/file?version=n`, `PUT/GET /v1/public/:id/preview`
- `POST /v1/public/:id/requests` (JSON message, baseVersion), then `PUT .../requests/:rid/file` and `.../preview`
- `POST .../requests/:rid/accept|decline` (owner, JSON note), `POST .../requests/:rid/withdraw` (sender)
- `GET /v1/requests/incoming?status=open|all`, `GET /v1/requests/outgoing`

Public versions count against the owner's quota; waiting request uploads against the sender's.

## Upgrading from an older DaedalSync

Copy the new files over the old ones and start it again; the stored projects and users are read as
they are.

- **App downloads are gone.** The `/downloads`, `/download` and `/v1/app/…` routes, the download
  logins and the Releases tab are removed. The `releases` folder in your `storagePath` is no longer
  used and can be deleted. `trustProxy` in `config.json` is ignored now and can go too, as can the
  `X-Forwarded-…` lines in an nginx config.
- **From ShapeSync** (the name before DaedalSync): the `SHAPESYNC_…` environment variables still
  work (a `DAEDALSYNC_…` one wins when both are set) and the stored `.shapezip` files are read as
  they are.

## Tests

```
npm install
npm test
```
