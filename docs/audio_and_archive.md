# The audio archive, and why it moves the heap

This note exists because a savestate question turned into an audio question. It
records what the game's asset archive is, what is inside it, and the one property
of that content that perturbs the allocator. Nothing here needs the game running.

No game content is committed. The tools below read the installed files in place
and write their output to `.csv`, which `.gitignore` excludes.

## The short version

Two runs of the game that should have allocated identically diverged at a single
`calloc`, one asking for 0x25 bytes and the other for 0x2E. That block turned out
to be a Vorbis vendor string, and the size difference is not randomness: the
game's music was not all encoded with the same version of libVorbis, so **which
track loads determines the shape of the allocation stream**. Nine of the sixty
tracks allocate 37 bytes and the other fifty-one allocate 46.

That reframes the determinism problem. The divergence is game state, reproducible
given the same route, rather than a clock or a seed that has to be pinned.

## pack.kanobi

`pack.kanobi` is a DxLib **DXA version 4** archive with every byte XORed against a
repeating twelve-byte key:

```
BE 43 BD 5A A5 F6 B4 D7 89 4E C5 FD
```

The key was recovered without touching the game or the executable. An index of
coincidence over the file peaks at lags 12, 24, 36 and 48 in both the trailing
tables and the payload, which fixes the period at twelve. The plaintext then
carries enough zero padding that the most common byte at each of the twelve
positions *is* the key byte; over 16 MB every position came out with a 3.4x margin
over the runner-up. Decrypted, offset 0 reads `DX\x04\x00`.

`kanobi.py key` re-runs both steps against your own copy and says whether the
result matches the key the tool ships with, so none of the above has to be taken
on trust:

```
  lag 12    4.99%      <- noise is about 0.39%
  lag 36    3.89%
recovered key: BE 43 BD 5A A5 F6 B4 D7 89 4E C5 FD
MATCHES the key this tool ships with
```

The header is `<HHIIIII`: magic, version, header size, data start, then the
absolute address of the name table and the file and directory tables relative to
it. File table entries are 44 bytes - name address, attributes, three `FILETIME`s,
then data address, size and compressed size, with `0xFFFFFFFF` meaning stored. A
name table entry is a four byte head, the upper-cased name, then the real name,
each NUL padded to a four byte boundary.

The archive holds 1326 files: 967 `.png`, 237 `.wav`, 60 `.ogg`, 35 `.rbrb`,
26 `.jpg` and one `.tga`.

## The music

Sixty tracks, `bgm0.ogg` through `bgm59.ogg`. These nine were encoded with
libVorbis 1.3.5, whose vendor string is 36 characters and so allocates 37 bytes:

| file | length | stream serial |
| --- | --- | --- |
| `bgm0.ogg` | 17.152 s | 11262 |
| `bgm34.ogg` | 454.097 s | 13105 |
| `bgm40.ogg` | 24.629 s | 4622 |
| `bgm46.ogg` | 94.030 s | 25771 |
| `bgm52.ogg` | 67.200 s | 3475 |
| `bgm56.ogg` | 237.244 s | 1383 |
| `bgm57.ogg` | 39.321 s | 31210 |
| `bgm58.ogg` | 202.319 s | 23532 |
| `bgm59.ogg` | 149.277 s | 15007 |

The other fifty-one are libVorbis 1.3.4, whose vendor string is
`Xiph.Org libVorbis I 20140122 (Turpak<e4>r<e4>jiin)` - the codename is
*Turpakäräjiin*, and both of its non-ASCII characters are stored as a single
Latin-1 byte rather than as two bytes of UTF-8. That makes the string 45 bytes
and the allocation 46.

Write the codename out in ASCII as "Turpakaerajiin" and you get 47, which is
wrong; the byte count only works in the encoding the file actually uses. Two
further traps sit next to this one. The 20101101 build's vendor string,
`Xiph.Org libVorbis I 20101101 (Schaufenugget)`, is *also* 45 bytes, so the size
alone cannot distinguish the two builds and only the logged string settles it.
And 1.3.5's codename is four snowmen, which the log renders as `(????)`; they
occupy four bytes here, not the twelve that four UTF-8 snowmen would.

`bgm56` through `bgm59` being contiguous is consistent with tracks added later and
encoded with a newer library. Two tracks are licensed rather than original; their
comment headers carry `ALBUM=MusMus` and `ARTIST=watson`.

### There are no loop points

The tracks are pre-trimmed to loop seamlessly, so the game stores no loop
metadata. Five of the sixty have sample counts that are exact multiples of 44100 -
`bgm3` at 144 x 44100, `bgm16` at 90 x, `bgm19` and `bgm20` at 80 x, `bgm31` at
76 x - which masters never are. Three places were checked and all are empty:

- no `LOOPSTART`, `LOOPLENGTH` or `LOOPEND` tag anywhere in the archive, which is
  the usual DxLib convention
- no 60 entry table in the unpacked executable, at any stride from 4 to 16, where
  every value lands meaningfully inside its own track
- `data\misc\loop.dat` is not music metadata: 100 slots, five of them non-zero.
  `loop1.wav` through `loop4.wav` in the archive are looping sound effects

### The comment header is what reaches our allocator

The decoded audio never passes through the private heap. Every one of nineteen
track loads in a session was checked for a large allocation within 120 operations
either side and none had one; the oversize allocations in the trace are graphics,
including 1280x720x4 framebuffers. What does reach the heap is the Ogg comment
header - the vendor string and the tags, which are also variable length
(`SOFTWARE=FL Studio 12`, `SOFTWARE=FL Studio 10`, `DATE=2014`).

A room transition costs a remarkably constant number of allocator operations,
1284 to 1394 with most between 1291 and 1298.

## Tools

All under `tools/`, none of which need the game running.

- `kanobi.py` - decrypt the archive and read it. `names` lists file names,
  `files` lists them with offsets and sizes, `streams` enumerates the Ogg streams,
  `match` writes `tracks.csv` joining every stream to its file name, serial, sample
  count and vendor string length.
- `ostlen.py` - read FLAC `STREAMINFO` from the store soundtrack to build a
  duration table. Useful only for attaching human track titles to `bgmN`: the
  in-game files are loop-edited, so their durations do not match the masters. Only
  15 of 60 matched within 300 ms, and two different streams matched the same title.
  `kanobi.py` does not read it; the archive supplies its own sample counts.
- `trackmatch.py` - an earlier attempt to name tracks by matching allocation sizes
  to durations. Superseded; kept because the negative result is informative.

## What here is measurement and what is testimony

Worth stating, because the archive is not committed and `.gitignore` excludes
`.csv`, `.txt` and `.log`, so most numbers above are reports of runs that cannot
be re-read from this repository.

Reproducible from a checkout plus an installed copy of the game: the key, via
`kanobi.py key`, which re-derives it rather than asserting it; the archive
inventory, the file table, and the per-stream serials, sample counts and vendor
strings, via `names`, `files`, `streams` and `match`.

Testimony, in that the instrument is committed but the run is not: the two
sessions agreeing through operation 6816, the nineteen track loads with no large
allocation within 120 operations either side, the 1284 to 1394 operations a room
transition costs, and the absence of a sixty-entry loop table at strides 4 to 16.
All of those are re-runnable with `D3D9SW_GHVORBIS=1` and `D3D9SW_GHTRACE`, and
none of them should be believed harder than that.

## The knob

`D3D9SW_GHVORBIS=1` makes the game heap report Ogg comment headers and comment
tags as they are freed, each keyed to the allocator operation number so a track
load becomes a landmark two runs can be aligned against:

```
vorbis: comment header freed at allocator operation 4553 - 37 byte(s), 0x25, vendor "Xiph.Org libVorbis I 20150105 (????)"
vorbis: tag freed at allocator operation 4550 - "SOFTWARE=FL Studio 12"
```

It recognises headers by content rather than by address, so it survives the heap
moving, reads only, and stops after 96 lines so a long session cannot bury the log.

Two separate sessions produced identical operation numbers and sizes through
operation 6816 before diverging, and the divergence was the third track load,
which is the route differing rather than the allocator.

## Open thread

The Ogg comments carry no `TITLE`, so a track load cannot name itself from
metadata. The remaining step is a hook on the reads against `pack.kanobi` that
reports the offset being read; `tracks.csv` already maps every offset and stream
serial to a file name, so one offset names the track outright.
