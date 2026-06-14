# Hum

A minimal TUI music player. Searches YouTube, plays with mpv, and downloads tracks to a local library. No accounts and no bloat :)

## Dependencies

- [mpv](https://mpv.io) - audio playback
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) - YouTube search and download
- ncurses

## Building

```
make
make install   # installs to /usr/local/bin
```

Or copy the binary wherever you want:

```
cp hum ~/.local/bin/
```

## How It Works

Search for music on YouTube, play it, and Hum automatically downloads it to `~/Music/hum` in the background. Next time you can play it straight from your library without needing internet.

Playlists are stored as simple text files in `~/Music/hum/playlists/`. You can build them up over time by adding tracks from search results, your library, or the queue.

## Keys

Hum uses vim-style navigation. Press `?` in the app for the full list, but here's the overview:

**Navigation:**
- `j/k` to move up and down
- `Esc /` to search YouTube
- `Esc p` to open playlists
- `v` to view the queue, `b` for library
- `q` or `Esc` to go back

**Playback:**
- `l` or `Enter` to play
- `Space` to pause
- `n/p` for next/previous track
- `,` and `.` to seek 5 seconds
- `+/-` for volume
- `r` to cycle repeat mode

**Managing Tracks:**
- `a` to add to queue
- `A` to add to a playlist
- `d` to delete, `c` to clear queue
- `V` for visual mode (multi-select)
- `s` to save the queue as a playlist
- `R` to rename a playlist

## Configuration

Edit `config.h` and rebuild. You can change keybinds, library path, search result count, volume step, seek step, and more.

## License

[MIT](LICENSE)
