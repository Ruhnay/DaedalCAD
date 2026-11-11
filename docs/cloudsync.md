# CloudSync: personal and public projects

Requirements for CloudSync with a **Personal** tab (your own backups, as before) and a **Public** tab (a shared list of designs anyone with an account can copy, and change only through update requests the author accepts or declines).

Server: `webapp/` (DaedalSync, Node 18+, Express), on any machine you host it on. Client: `src/cloudsync.cpp` in DaedalCAD.

## 1. Accounts and roles

- **R1.1** Every user has an account on the server, made by the admin with `node src/cli.js user add <name>`. The account name is the **author name** shown on public projects.
- **R1.2** A user key is the only credential. Every request sends it. Without a valid key nothing is readable, public projects included.
- **R1.3** For each public project, a user is either its **owner** (the account that published it) or a **contributor** (anyone else).
- **R1.4** Only the owner can change a public project directly: new version, details, delete, accept or decline requests.
- **R1.5** A contributor can copy a public project and send update requests. They cannot change it directly.
- **R1.6** The admin can revoke a key. A revoked account's public projects stay listed. Its open requests stay open, and the owner can still decline them.

## 2. Personal projects (the Personal tab)

- **R2.1** Put, Put all, Fetch and Fetch all work as before: send or receive only when the user presses a button, skip unchanged projects, and ask before overwriting either side.
- **R2.2** **Delete from server** removes the selected server copy after a confirmation that names the project. The copy on this machine is never touched.
- **R2.3** Put also uploads a preview image. Selecting a server project shows its preview.
- **R2.4** Personal projects are never visible to other accounts.

## 3. Public projects

- **R3.1** A public project has:
  - an id
  - a title (required, up to 120 characters)
  - a description (up to 4000 characters)
  - tags (up to 10, each up to 32 characters)
  - the owner's id and author name
  - a version number, starting at 1
  - size and SHA-256 of the current version
  - created and updated times
  - a download count (copies by other people; the author's own downloads are not counted)
  - a preview image
- **R3.2** **Publish** turns a local project into a new public project at version 1. The author fills in title, description and tags. The local project is then linked to the public one, see R5.
- **R3.3** Every version is kept: the zip and its preview. The current version is the highest number. The details view lists all versions with their date and who made each one: the owner, or the contributor whose request was accepted.
- **R3.4** The owner can **Publish new version** from the linked local project. This skips the request flow.
- **R3.5** The owner can edit the title, description and tags without making a new version.
- **R3.6** The owner can **delete** the public project. This removes all versions and all its requests, after a confirmation that names it. Copies already made on other machines stay theirs.
- **R3.7** The public list shows the preview, title, author, version, last update, size, and the number of open requests (only for your own projects). It can be filtered by text in the title, author, description and tags.

## 4. Copying

- **R4.1** **Copy project locally** downloads the current version into a new local project. It is a plain copy with no link, and nothing ever flows back. If the name is taken, a number is added to it; nothing is overwritten.
- **R4.2** **Copy project for update** does the same, but links the local copy to the public project and records the version it was copied from (the base version). Only a linked copy can send an update request.
- **R4.3** Both downloads are checked against the server's SHA-256. A mismatch is discarded and reported.

## 5. The link between a local and a public project

- **R5.1** A link is stored in the local project folder as `cloudsync.json`. It holds:
  - the server address
  - the public project's id and title
  - the base version
  - the role: `owner` for a published project, `update` for a copy made for an update
- **R5.2** `cloudsync.json` is not uploaded. Packing a project includes only `daedalcad.json` and its STL and SVG files.
- **R5.3** A link belongs to one server. With another server address entered, the link is shown as belonging to another server and cannot be used.

## 6. Update requests

- **R6.1** From a linked copy, **Send update request** uploads the project, its preview, a message (required, up to 2000 characters) and the base version. The request starts as **open**.
- **R6.2** A request is always one of: **open**, **accepted**, **declined**, **withdrawn**. Only open requests can change. Each request belongs to one public project.
- **R6.3** The owner sees each open request with:
  - the contributor
  - the message
  - its preview next to the current preview
  - the base version against the current version. A request made on an older version is marked **made on an older version**, because accepting it replaces whatever changed since.
- **R6.4** **Download to look at it** copies the request into a new unlinked local project, so the owner can open and inspect it before deciding.
- **R6.5** **Accept** makes the request the next version. The contributor is recorded as the author of that version. An optional note goes back to the contributor.
- **R6.6** **Decline** closes the request with an optional note. The project does not change.
- **R6.7** The contributor can **withdraw** their own open request.
- **R6.8** The contributor sees all their requests and their status, notes included.
- **R6.9** The zip of a declined or withdrawn request is deleted from the server, but the record stays. An accepted request's zip becomes the new version.
- **R6.10** One contributor can have at most 20 open requests at a time.

## 7. Notifications when DaedalCAD starts

- **R7.1** The CloudSync window has an **Enable CloudSync** setting, on by default once a host and key have been saved.
- **R7.2** When DaedalCAD starts with CloudSync enabled, it checks the server in the background. It never blocks start-up, and it waits at most 10 seconds.
- **R7.3** If the server cannot be reached (no internet, server down, bad key), nothing is shown.
- **R7.4** A popup appears when there are:
  - open requests on your projects that you have not reviewed yet
  - decisions on your own requests that you have not seen yet
- **R7.5** The popup lists each item with:
  - its preview
  - the project title
  - who sent it, or what was decided
  - the message or note
- **R7.6** **Review** opens CloudSync on the requests page and marks everything shown as seen. **Later** leaves it for the next start.

## 8. Previews

- **R8.1** A preview is a PNG of the whole project, about 640×480, made by DaedalCAD at upload time from the project file itself. The project does not need to be open. It is a fixed three-quarter view with part colours, and holes are not drawn.
- **R8.2** The server accepts PNG only (checked by signature), up to 4 MB.
- **R8.3** If no preview exists, the window shows "No preview".

## 9. Limits and storage

- **R9.1** Public versions count against the owner's quota, including versions that came from accepted requests. Open request zips count against the contributor's quota. Over quota answers `413` and is shown as such.
- **R9.2** Names, ids and file paths are checked on the server. No request can reach a path outside the storage folder.
- **R9.3** Writes to one project or one request are serialized on the server, so two accepts at once cannot both win.
- **R9.4** Storage layout:
  - `users/<id>/` holds personal projects
  - `public/<id>/meta.json`
  - `public/<id>/versions/<n>.shapezip` and `<n>.png`
  - `public/<id>/requests/<rid>/request.json`, `update.shapezip` and `preview.png`

## 10. Server API (all under `/v1`, `Authorization: Bearer <key>`)

| Method and path | Who | Does |
|---|---|---|
| `GET /me` | any | name, bytes used, quota, open incoming requests |
| `GET /projects`, `GET/PUT /projects/:name` | any | personal projects, as before |
| `DELETE /projects/:name` | any | delete own personal project |
| `GET/PUT /projects/:name/preview` | any | personal preview |
| `GET /public?q=` | any | public list |
| `POST /public` | any | create (JSON title, description, tags) and return the id; then upload the file |
| `GET /public/:id` | any | details and version list |
| `PATCH /public/:id` | owner | edit title, description, tags |
| `DELETE /public/:id` | owner | delete with all versions and requests |
| `PUT /public/:id/file` | owner | new version (zip body) |
| `GET /public/:id/file?version=n` | any | download (current when no version is given) |
| `GET/PUT /public/:id/preview?version=n` | any / owner | preview |
| `POST /public/:id/requests` | contributor | create (JSON message, baseVersion) and return the request id |
| `PUT /public/:id/requests/:rid/file` | that contributor | upload the zip; the request becomes open |
| `PUT /public/:id/requests/:rid/preview` | that contributor | upload the preview |
| `GET /public/:id/requests/:rid/file`, `/preview` | owner or contributor | download |
| `POST /public/:id/requests/:rid/accept` | owner | accept (JSON note) |
| `POST /public/:id/requests/:rid/decline` | owner | decline (JSON note) |
| `POST /public/:id/requests/:rid/withdraw` | that contributor | withdraw |
| `GET /requests/incoming` | any | requests on your projects (`?status=open` by default) |
| `GET /requests/outgoing` | any | your own requests, every status |

## 11. Not in this version

- A web page to browse public projects without DaedalCAD.
- Comments or discussion on a request, and merging two requests together.
- Licences per project, and an attribution chain when a copy is published as a new project.
- Rolling back to an older version as the current one. Older versions can be downloaded, but not promoted.
