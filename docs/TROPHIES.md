# Local trophies

[← Documentation](README.md)

Trophies are recorded locally, per launcher profile. Nothing is sent to PSN, with or without the
online module. Two games have trophies: Bloodborne (CUSA03173) and 13 Sentinels: Aegis Rim
(CUSA19610); the log, the edited-save rule and the notices work the same for both.

## Recording

When the game unlocks a trophy, the runtime appends a line to `trophies-v1.log` in the active
`BB_USER_DIR` (next to `savedata/`, apart from shared caches):

```
v1 UNIX_SECONDS TITLE_ID TROPHY_ID
```

Writes are serialized, appended and synced; links and non-regular files are refused; a failed
write is logged and the game goes on. Valid ids are 0–127. The runtime prints
`trophy N unlocked`, and the launcher imports the log into its database (profile, title and id
unique; the earliest timestamp kept). Incomplete trailing lines are retried later; malformed
ones are ignored.

The platinum is awarded locally, as trophy 0, and reported back to the game once the trophies
below it are recorded. The rules are a table in `src/runtime_trophies.c` (`trophy_rules`):

| Title | Platinum | Needs | Catalog |
|---|---|---|---|
| CUSA03173 (Bloodborne) | 0 | ids 1–33; The Old Hunters' six (34–39) are separate | `trophy_catalog.json`, 40 trophies |
| CUSA19610 (13 Sentinels) | 0 | ids 1–55 | `trophy_catalog_13s.json`, 56 trophies |

Old unlocks are not inferred from a save.

## 13 Sentinels

The catalog (`gpu/shim/trophy_catalog_13s.json`) has 56 trophies: the platinum (0), 2 gold, 5
silver and 48 bronze, one group, none hidden. Names and grades are the public ones (PSNProfiles
and Platinum Pursuit agree on every row); the descriptions are written for the catalog. Each trophy
has an `icon_url` (Sony's public image CDN) that only the packaging script reads.

**The numbering is unverified.** The game's own list is encrypted with a console key and is not in
the dump, so the ids are inferred from the order of the public lists, the platinum as 0. They stay
unverified until a real unlock confirms them (the count, the platinum as 0, the first trophies
earned in the prologue); an unlock shows the number the game reported in `trophies-v1.log`.

**The ledger.** `achievements.txt` in the same user directory as `trophies-v1.log` is what the
Wayfarer launcher reads (Bloodborne has none). It is written whole, to a temporary file that is
renamed over it, when the game starts (so all 56 show before the first unlock) and after each
recorded unlock, the platinum's included. Lines are `key = value`, `;` starts a comment line, the
keys are the trophy numbers:

```
[unlocked]
0 = 2026-11-11 21:05          local time, yyyy-MM-dd HH:mm, the earliest record in the log
[grades]
0 = platinum                  platinum | gold | silver | bronze, every trophy
[list]
0 = name | description         every trophy, in id order
```

(The annotations are not in the file.) The catalog text reaches the runtime through
`bbgpu_trophy_catalog_text` (`gpu/bbgpu.h`), a lookup into the same table the notices use; the
generator refuses a `|` or a line break in a name or description.

**Unlock state.** `sceNpTrophyGetTrophyUnlockState` reports the earned set to the game as a
`SceNpTrophyFlagArray` (four uint32s, bit `id & 31` of word `id >> 5`) and the number of trophies in
the set, as shadPS4 does. Bloodborne's answer is unchanged: no flags, count 0.

**The icons.** `scripts/package-13s-app.sh` downloads each `icon_url` to
`Contents/Resources/achievements/<id>.png` in the app. Downloads are kept in `build/trophy-icons`
(`--icon-cache DIR`), so a repackage fetches nothing; a failed download is a warning and the app
goes on without that icon. The pictures never go into git (`*.png` is ignored).

## Edited saves

A file `savedata/.bb-edited` (any file there, even an invalid one) marks an edited save set.
While it is there the runtime records no trophies and the launcher imports none.
`BB_TROPHIES_ELIGIBLE=0` stops recording too; `=1` cannot override the marker. This is not
anti-cheat: it does not detect saves edited elsewhere. The launcher's save editor writes the
marker before it publishes an edited save, and nothing removes it; backups copy it, so restoring
a clean backup from before the first edit restores eligibility. Earned history stays in the
launcher's database through restores.

An edited save set also plays **offline only** (the maintainer's decision): the launcher refuses
online play for it, and `src/runtime_net.c` refuses it again when the marker exists or cannot be
checked.

## Notifications

The game draws newly recorded trophies in its own ImGui overlay, above the presented frame.
Events enter a 16-entry queue only after the log line is synced; entries older than 30 seconds
are dropped. Notices last five seconds (platinum seven), with a one-second gap. They appear
only over the game because they are part of its frame, and do not change window focus. Already
recorded trophies are not shown again.

**Placement.** A table in `gpu/shim/bbport_trophy_style.h` (taken by `kTrophyTitles` in
`bbport_overlay.cpp`) gives each title its corner, whether it chimes and whether it shows the
trophy's icon: Bloodborne top right, silent, text only; 13 Sentinels bottom right, with chimes and
icons. The bottom-right margins mirror the top-right ones
(14 px, scaled). The notice keeps clear of what else is in its corner: below the FPS counter when
that is top right (Bloodborne), above it when it is bottom right (13 Sentinels, Show FPS counter
at the bottom right), and over the touch controls when they are shown and would reach it (at 16:9
the face buttons and R3 end well above the notice, so this acts on unusual window shapes only). The
online status line and the text prompt are centered and are not avoided.

**Icons (13 Sentinels).** The notice shows the trophy's own picture, 60 px (scaled) at its left
with the text to its right, the platinum's included, in a thin frame of the grade's colour.
Bloodborne's notice stays text only with the cup badge: the runtime carries
no game artwork, and the pictures are not in git. They come from the packaged app,
`<App>.app/Contents/Resources/achievements/<trophy id>.png` (240x240, put there by
`scripts/package-13s-app.sh`, see The icons above). The runtime finds the folder next to its
executable (`SDL_GetBasePath()`, then `../Resources/achievements` from `Contents/MacOS`).
`BB_TROPHY_ICON_DIR=/some/folder` overrides it (checks, and `scripts/run-13s.sh` runs outside the
app) and is used alone: a folder that does not exist means no icons. Outside the app with no
override there is simply no icon and the notice looks as for Bloodborne, with one log line
(`trophy icons: no icon folder ...`). A missing, unreadable, truncated or non-PNG file does the
same for that trophy, one log line per id (`trophy icon N: ...`). `BB_TROPHY_OVERLAY_PREVIEW=1`
shows the icons too.

How it works (`gpu/shim/bbport_trophy_icons.h`, `bbport_overlay.cpp`): when a notice is queued, a
worker thread reads the file and decodes it with SDL's PNG loader (SDL 3.4, no extra dependency;
no icon on an SDL without it), shrinking anything over 128 px with an area average; the present
thread never touches the disk. The cache holds what is loading, at most 8 decoded pictures waiting
for their notice, and a mark per failed id. When the notice appears, the present thread copies the
picture into ImGui's font atlas as a custom rect (one at a time; the next icon replaces it) and
draws it with the rect's UVs. ImGui's Metal and Vulkan backends both set
`ImGuiBackendFlags_RendererHasTextures`, so ImGui uploads the atlas change itself and there is no
texture code per renderer. While an icon is still loading, its room is kept in the layout so the
text does not jump. `tests/test_trophy_icons.cpp` (`trophy-icons-test`) checks the folder lookup, a
good, a big, a truncated and a non-PNG file, and the cache, on PNGs it writes itself.

**Chimes (13 Sentinels).** As a notice appears on screen (when the queue starts showing it, not
when it is queued) a chime plays: "B", a bell-like ding, for a regular trophy; "P2", a short
fanfare, for the platinum. Both are synthesized at runtime from the recipes in
`gpu/shim/bbport_trophy_chime.h` (no audio file ships), once, on a worker thread that starts when the
first notice is queued, normalized to a peak of 0.7, mono at 44.1 kHz. The present thread only puts
the samples on an SDL audio stream on the default playback device, so they mix over the game's
audio and nothing waits; a notice shown before the chimes are ready (the preview) chimes on the
next frame. If audio cannot open, one line is logged (`trophy chime: ...`) and the notices go on
silently. `tests/test_trophy_chime.cpp` checks lengths, the peak and the per-title table, and
compares the output with the WAVs approved by ear when they exist on the machine
(`BB_CHIME_WAV_DIR`; they are not in git).

`BB_TROPHY_SOUND=0` silences the chimes only. `BB_TROPHY_OVERLAY=0` turns notices off (Advanced →
Launcher → Trophy notifications), and with them the chimes, without affecting local recording or the
launcher's trophy view. `BB_TROPHY_OVERLAY_PREVIEW=1` plays the chimes too, and draws
labelled previews without recording anything, on the Vulkan and the Metal renderer: five for
Bloodborne (ids 21, 7, 1, 0, 35), five for 13 Sentinels (1, 40, 45, 49, 0). A notice is shown for
the running game only, by title and id, for the ids its catalog has (Bloodborne 0–39, 13 Sentinels
0–55). The runtime embeds a copy of the reviewed public trophy catalogs for display
(`gpu/shim/gen_trophy_catalog.py` makes the table at build time and refuses a catalog whose ids are
not 0..n-1); the launcher keeps its own copy of Bloodborne's for the trophy list. The catalogs use
public names and original summaries, never game trophy files, artwork or keys.
