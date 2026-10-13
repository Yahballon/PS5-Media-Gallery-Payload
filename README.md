# PS5 Media Gallery Payload

[![Build](https://github.com/Yahballon/PS5-Media-Gallery-Payload/actions/workflows/build.yml/badge.svg)](https://github.com/Yahballon/PS5-Media-Gallery-Payload/actions/workflows/build.yml)

Browse your PS5 screenshots and video clips from your phone or PC. A lightweight local payload by **YAHBALLON**, with an embedded web interface and no external frontend dependencies.

## Features

- Screenshots and videos grouped by app/game ID, including nested folders beneath `NPXS40087`.
- App names and icons when available on the console.
- Screenshot previews and full-size viewing.
- Video thumbnails, playback and seeking for browser-supported codecs.
- Automatic refresh every 10 seconds without closing the current viewer.
- Delete individual captures directly from their tiles or viewer.
- Delete an app's captures, or the entire gallery, with confirmation.
- Startup notification showing version, local IP and port.
- Buffered streaming and handling of cancelled or parallel transfers.

## Run

1. Download `ps5-media-gallery.elf` from [Releases](https://github.com/Yahballon/PS5-Media-Gallery-Payload/releases).
2. Send it to your jailbroken PS5 using your ELF loader (commonly port `9021`).
3. Open **`http://YOUR_PS5_IP:3737/`** on a device on the same network.

The payload must remain running. Stop the previous payload before starting a newer version, then refresh the browser page.

Media is scanned recursively beneath `/user/av_contents/photo/` and `/user/av_contents/video/`. For example, `photo/NPXS40087/PPSA01234/035/` is grouped under `PPSA01234`.

## Delete captures

Use **Delete** on a tile or **Delete this capture** in the viewer. **Delete app captures** removes both photos and videos for that app, including captures hidden by filters. **Delete all captures** requires confirmation and typing `DELETE ALL`.

Deletion is permanent. Only supported media files are removed; app files, metadata sidecars (`.dat`, `.meta`) and folders are preserved. The local server has no login or TLS, so devices able to reach its port can view or delete captures. Keep it on your trusted LAN.

## Build

Requires Linux, Python 3, Make, Clang/LLD and the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk).

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make all
```

Output: `ps5-media-gallery.elf`. Run `make test` for host HTTP and filesystem tests. Tests cover grouping, icons, media/range transfers, parallel and cancelled transfers, deletion scopes and path restrictions. The project owner also tested the payload on PS5; other firmware versions may behave differently.

## Automatic builds and releases

Every push to `main` runs tests and builds the ELF. Successful builds upload an artifact with the ELF and SHA-256 checksum. An unpublished version in `VERSION` creates a matching `vVERSION` GitHub release automatically. Existing releases are kept unchanged: bump the version for a new release. You can also run the workflow manually from Actions.

The workflow uses SDK v0.44 and checks its published archive checksum. Release publishing uses GitHub's built-in workflow token; no personal token or repository secrets are required.

## API

| Request | Result |
| --- | --- |
| `GET /` | Gallery page |
| `GET /api/gallery` | Captures and app information |
| `GET /icon/<APP_ID>` | App icon |
| `GET /media/photo/<PATH>` | JPEG, PNG or WebP |
| `GET /media/video/<PATH>` | MP4, M4V, MOV or WebM, with HTTP Range |
| `HEAD` on a GET endpoint | Headers only |
| `DELETE /api/delete/media/<TYPE>/<PATH>` | One capture |
| `DELETE /api/delete/app/<APP_ID>` | An app's captures |
| `DELETE /api/delete/all` | All supported captures |

DELETE requests require `X-Gallery-Confirm: delete`. The server grants no cross-origin CORS permission. This header blocks ordinary cross-origin browser deletion; it is not authentication.

## Limitations

- Video playback/previews depend on browser codec support; no transcoding is performed.
- Listings are limited to 12,000 media files and nine nested directory levels.
- App names and icons are best effort.
- Thumbnail frames are generated on the viewing device and kept in page memory.

## Credits and license

Inspired by [BFpilot](https://github.com/ItsBlurf/BFpilot)'s browser-based payload architecture. Built with [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk).

The repository's existing GPL-3.0 license is preserved in `LICENSE`. The original gallery source's MIT notices are retained in `LICENSE-MIT` and its source files.
