# DaedalSync

Sync store for DaedalCAD. The desktop app stays offline. This service only stores a project (`.daedal`) when a user puts one, and streams it back when they fetch it.

## Configure

Copy `config.example.json` to `config.json`.

| Key | Meaning |
|---|---|
| `port` | Listen port. Default `5002`. |
| `host` | Bind address. Default `127.0.0.1` so only nginx on this machine can reach it. |
| `storagePath` | Folder on disk. Each user gets `users/<id>/` under it. |
| `usersPath` | Optional separate file for the user list. Empty means `storagePath/users.json`. |
| `defaultQuota` | Per-user cap for new keys, such as `5G` or `500M`. |
| `trustProxy` | Which proxy's `X-Forwarded-For` to believe. Default `loopback`; see nginx below. |
| `adminPort` | Admin web page port. Default `5003`; `0` turns it off. |
| `adminHost` | Admin page bind address. Default `0.0.0.0`, so other machines at home can reach it. |
| `adminAllow` | Who the admin page answers: `private` (default: loopback and home-network addresses) or a list such as `"192.168.1.0/24, 127.0.0.1"`. |

Environment overrides: `DAEDALSYNC_PORT`, `DAEDALSYNC_HOST`, `DAEDALSYNC_STORAGE`, `DAEDALSYNC_USERS`, `DAEDALSYNC_QUOTA`, `DAEDALSYNC_CONFIG`.

## Run

```
npm install
node src/cli.js user add alex --quota 5G
node src/cli.js serve
```

`user add` prints the key once. The server stores only its hash. `user key <name>` issues a new key and kills the old one. `user revoke <name>` locks the account and leaves the files in place.

## nginx

Proxy a hostname to `http://127.0.0.1:5002` and set `client_max_body_size 0`. Pass the visitor's
address along, so the download lockout counts per visitor instead of locking everyone out:

```
location / {
    proxy_pass http://127.0.0.1:5002;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    client_max_body_size 0;
}
```

Nginx Proxy Manager already sends `X-Forwarded-For` for every proxy host; only add
`client_max_body_size 0;` under the host's Advanced tab. NPM usually runs in Docker, so its requests
come from a Docker or LAN address, not 127.0.0.1: set `"trustProxy": "loopback, uniquelocal"`.

The server believes that header only from nginx on the same machine (`trustProxy: "loopback"`,
the default). If nginx runs on another machine, set `trustProxy` to its address (for example
`"192.168.1.10"`, or `--trust-proxy` / `DAEDALSYNC_TRUST_PROXY`); `"none"` ignores the header.

## Admin page

`serve` also starts a small web page on `adminPort` (5003) for releases, download logins and sync
users, so you do not need the CLI for them. Open `http://<the server's LAN address>:5003` from a
machine at home. On the first visit you choose the admin password (8+ characters); `node src/cli.js
admin password` sets or resets it from the server.

- **Releases:** drop the zip from package.ps1; the version is read from its name. Upload with a
  progress bar, edit notes, remove versions. "Replace if it exists" swaps a version.
- **Download logins:** add, new password, remove. A made-up password is shown once.
- **Sync users:** add (with quota), new key, quota, revoke. The key is shown once.

It only answers addresses on your own network (`adminAllow`), judged by the connection itself, not by
any header. **Do not add this port to Nginx Proxy Manager or forward it on your router.** NPM runs in
Docker with a private address, so a proxy host pointing at 5003 would let the internet in through
it. If you want it reachable from nowhere but the server itself, set `"adminHost": "127.0.0.1"` and
use an SSH tunnel (`ssh -L 5003:127.0.0.1:5003 you@server`).

## API

Send the key as `Authorization: Bearer <key>`. Every route needs a key, the public list included.

Personal projects (one key sees only its own):

- `GET /v1/me` returns `name`, `bytesUsed`, `quotaBytes`, `projects`, `openIncoming` (open update requests on your public projects).
- `GET /v1/projects` returns `name`, `bytes`, `sha256`, `updated`, `hasPreview`.
- `PUT /v1/projects/:name` stores a zip body. Overwrites that name. `413` over quota, `400` if the body is not a zip.
- `GET /v1/projects/:name` streams the zip. `X-Sha256` is the stored hash.
- `DELETE /v1/projects/:name` removes the server copy and its preview.
- `PUT/GET /v1/projects/:name/preview` stores or returns a PNG preview (4 MB max).

Public projects and update requests: see `docs/cloudsync.md` in the DaedalCAD source for the requirements and the full route table. In short:

- `GET /v1/public?q=`, `POST /v1/public` (JSON title, description, tags), `GET/PATCH/DELETE /v1/public/:id`
- `PUT /v1/public/:id/file` (owner adds a version), `GET /v1/public/:id/file?version=n`, `PUT/GET /v1/public/:id/preview`
- `POST /v1/public/:id/requests` (JSON message, baseVersion), then `PUT .../requests/:rid/file` and `.../preview`
- `POST .../requests/:rid/accept|decline` (owner, JSON note), `POST .../requests/:rid/withdraw` (sender)
- `GET /v1/requests/incoming?status=open|all`, `GET /v1/requests/outgoing`

Public versions count against the owner's quota; waiting request uploads count against the sender's. Every version is kept under `public/<id>/versions/`.

## App downloads and updates

The server also hands out DaedalCAD itself: what `package.ps1` makes, the portable zip
(`DaedalCAD-0.7.0-windows-x64.zip`) and the installer (`DaedalCAD-0.7.0-setup.exe`). A version can
have one or both. Upload them on the admin page, or:

```
node src/cli.js release add DaedalCAD-0.7.0-windows-x64.zip [--notes "what changed"]
node src/cli.js release add DaedalCAD-0.7.0-setup.exe [--replace]
node src/cli.js release list
node src/cli.js release remove 0.7.0                      # both files
node src/cli.js login add <name> [--password secret]     # prints the password once
node src/cli.js login password <name> [--password secret]
node src/cli.js login list
node src/cli.js login remove <name>
```

The version is read from the file name (or the folder inside the zip; or give `--version`).
Download passwords are stored as scrypt hashes; ten wrong passwords from one address lock it out
for 15 minutes.

| Endpoint | Sign in | Answer |
|---|---|---|
| `GET /downloads` | download login | the versions, newest first: JSON `{latest, versions:[{version, notes, added, zip, installer}]}` (`zip` / `installer`: `{bytes, sha256, url}` or null), or a page with links in a browser |
| `GET /downloads/latest` | download login | the newest version, same fields |
| `GET /download?version=0.7.0[&kind=installer]` | download login | the zip (or the installer); without `version` the newest that has one |
| `GET /v1/app/latest` | sync key | the newest version, same fields: what DaedalCAD's updater asks |
| `GET /v1/app/download?version=0.7.0&kind=installer` | sync key | the file; `X-Sha256` and `X-Version` headers |

DaedalCAD checks for updates a few seconds after it starts (and from File → Check for updates).
It signs in with its CloudSync key, or else with a download login filled in there. An installed copy
downloads the setup.exe, checks its SHA-256 and runs it silently; a portable copy only says a new
version is out and opens `/downloads`. Upload the installer for a version, or installed copies are
not offered it.

In a browser, `https://your-host/downloads` asks for the login and shows the links. From a script:
`curl -u name:password -O -J "https://your-host/download?version=0.7.0&kind=installer"`.

Use HTTPS (nginx in front, as above): Basic auth sends the password with every request.

## Upgrading

The new code reads existing `users/` folders as they are. It adds a `public/` folder next to `users/` on first start.

DaedalSync was called ShapeSync, for ShapeSynth. Nothing needs to change on a running server: the
`SHAPESYNC_…` environment variables still work (a `DAEDALSYNC_…` one wins when both are set), the
stored projects keep their `.shapezip` files on disk, and releases uploaded as `ShapeSynth-…` stay
downloadable. New uploads may be named `DaedalCAD-…` or `ShapeSynth-…`. Admin page sessions start
over once, because the login cookie got the new name.

## Tests

```
npm install
npm test
```
