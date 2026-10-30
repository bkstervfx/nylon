# Nylon Viewer (lightweight web edition)

Nylon Viewer is a local, read-only browser UI for Nylon schema-v2 `image.db` audit databases.

It deliberately avoids the Tauri/Node/Rust toolchain. The viewer is a small native C++ executable using the same pinned SQLite source and LLVM-MinGW toolchain already cached by Nylon. It binds only to `127.0.0.1`, opens the default browser, and serves a self-contained HTML/Canvas UI and bounded JSON API.

## Build

Run the normal Nylon build once so the portable LLVM-MinGW and SQLite sources exist:

```cmd
build.cmd
```

Then build the viewer:

```cmd
build-viewer.cmd
```

Or simply run:

```cmd
view.cmd
```

`view.cmd` builds the viewer automatically if needed and opens the default database:

```text
output\image.db
```

To inspect another Nylon database:

```cmd
view.cmd D:\audits\image.db
```

## Behavior

The viewer:

- opens the source SQLite database with `SQLITE_OPEN_READONLY`;
- enables `PRAGMA query_only=ON`;
- binds its HTTP listener to loopback (`127.0.0.1`) only;
- does not modify or add indexes to `image.db`;
- does not use a CDN or make application network requests;
- caps graph responses at 500 nodes so a large audit cannot accidentally create a million-node browser graph.

The browser UI includes:

- whole-database summary counts;
- registry hive navigation;
- registry key/value search;
- local graph neighborhoods;
- pan/zoom and graph fitting;
- drag interactions with spring-like connected-node response;
- focus dimming for unrelated visible nodes;
- registry value visibility toggle;
- node inspector;
- registry type names;
- decoded text preview;
- first 256 raw bytes as hex preview;
- copy-path action;
- navigation back to previous focused nodes.

Double-click a graph node to focus its local neighborhood. `/` focuses search. `F` centers the selected node.

## Scope

This is intentionally lighter than a Tauri/Sigma viewer. Canvas physics run in the browser and the visible graph is deliberately bounded. The database itself can still contain hundreds of thousands of keys and values because only local neighborhoods are queried and rendered.

Keep the console window open while using the viewer. Press `Ctrl+C` there to stop the localhost server.
