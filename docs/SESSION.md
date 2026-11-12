# Session restore

Quill can reopen the buffers you had open at the end of the previous run, each
with its **cursor, anchor, and scroll position** restored. Saving happens once
on exit; restoring happens once on launch. There is **no per-frame cost**, and
nothing runs at all when the feature is disabled.

## Behavior

- On exit (GUI), Quill writes the open buffers to the session file: path, cursor,
  anchor, top line, and horizontal scroll for each, plus which buffer was active.
- On launch **with no file arguments**, Quill restores that session. The active
  buffer and its view come back as you left them.
- On launch **with a file argument**, the file opens normally and the session is
  ignored (but a new session is still saved on exit).
- Buffers whose file no longer exists are skipped. Cursor/anchor past EOF are
  clamped to the restored text length. Tree-sitter highlighting is reattached.
- Only named, on-disk buffers are saved; the in-memory demo buffer is not
  restored. Paths are stored absolute, so restoring works from any directory.
- Headless screenshots never restore or save a session (deterministic CI).

As a side benefit, each buffer now keeps its **own** scroll position when you
switch buffers (previously the scroll leaked across switches).

## Configure

Config file (`mu.conf`/`quill.conf`, see `quill.conf.example`):
```
session_restore = true
session_file = ~/.config/mu/session
```

CLI:
```
mu --no-session        # neither restore nor save this run
```

`session_restore = false` disables both saving and restoring. `~` in
`session_file` expands to `$HOME`; parent directories are created on save.

## File format

Plain text, versioned, one record per buffer:
```
quill-session 1
current /abs/path/to/active.c
begin
path /abs/path/to/file.c
cursor 123
anchor 123
top 40.000000
scrollx 0.000000
end
```
`current` names the active buffer by absolute path (robust to skipped entries).
Unknown lines are ignored; a wrong/missing header restores nothing.

## Implementation notes

- `src/session.{h,c}`: `session_save()` / `session_load()`. Load-time file I/O
  only; the parser tolerates truncated, malformed, and hostile input.
- `Document` gained `view_top` and `scroll_x` (zero-initialized). `app_set_doc()`
  in `src/main.c` flushes the live view into the outgoing buffer and loads the
  incoming buffer's view on every switch; the same flush runs before saving.
- Restore runs in `app_init_doc()` only when there is no file argument and
  `session_restore` is on; it is never reached on the headless path.

## Verification

- `tests/test_session.c`: save/restore round-trip (positions + active buffer +
  syntax reattach), deleted-file skip, missing-session no-op, EOF clamping.
- Fuzzed `session_load()` with 4000+ truncated/malformed/random inputs under
  ASan + UBSan: no crash, no sanitizer report.
- `make check`: 16/16 pass + 3 llvmpipe screenshots.
