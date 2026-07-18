# Hum

Minimal TUI music player. Searches YouTube, plays with mpv, downloads tracks to a local library. No accounts, no bloat.

![hum](hum.jpg)

## Dependencies

- [mpv](https://mpv.io) for playback
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) for search and download
- ncurses

## Building

```
make
make install   # /usr/local/bin
```

## Usage

```
hum            # home screen
hum -p         # playlists
hum -b         # library
hum -s         # search
hum -q         # queue
```

Search for something on YouTube, play it, and hum downloads it to `~/Music/` in the background. Next time it plays the local file instead. Playlists are plain text files in `~/Music/playlists/`.

Search results show songs and playlists. You can expand a playlist to see its tracks, batch download everything, or just queue individual songs. Use `/` in any view to filter. Queue is saved between sessions.

## Keys

Vim-style. `?` in the app shows everything.

```
j/k         move
l, Enter    play / expand
Space       pause
n/p         next / prev
, .         seek 5s
+/-         volume
m           mute
r           repeat mode
a           add to queue
A           add to playlist
d           delete
c           clear queue
V           visual select
J/K         reorder queue
S           shuffle
s           save queue as playlist
R           rename playlist
D           batch download
/           filter
q, Esc      back
```

## Config

Edit `config.h` and rebuild. All keybinds, all colors, library path, search count, volume and seek step.

Build flags in `config.mk`.

## License

[MIT](LICENSE)
