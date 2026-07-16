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

## Usage

```
hum                 # start at home screen
hum -p, --playlists # open playlists
hum -b, --browse    # browse library
hum -s, --search    # search youtube
hum -q, --queue     # view queue
hum -h, --help      # show help
```

## How It Works

Hum starts with a home screen where you can jump to playlists, library, search, or queue. Search for music on YouTube, play it, and Hum automatically downloads it to `~/Music/` in the background. Next time you can play it straight from your library without needing internet.

Search results show both songs (`s`) and playlists (`p`) from YouTube. Select a playlist to expand it and see its tracks, with a "download all" option to batch download and create a local playlist.

Playlists are stored as simple text files in `~/Music/playlists/`. You can build them up over time by adding tracks from search results, your library, or the queue.

## Keys

Hum uses vim-style navigation. Press `?` in the app for the full list, but here's the overview:

**Navigation:**
- `j/k` to move up and down
- `Esc /` to search YouTube
- `Esc p` to open playlists
- `v` to view the queue, `b` for library
- `q` to go back (quit from home screen)

**Playback:**
- `l` or `Enter` to play
- `Space` to pause
- `n/p` for next/previous track
- `,` and `.` to seek 5 seconds
- `+/-` for volume
- `m` to mute/unmute
- `r` to cycle repeat mode

**Managing Tracks:**
- `a` to add to queue
- `A` to add to a playlist
- `d` to delete, `c` to clear queue
- `V` for visual mode (multi-select)
- `J/K` to reorder tracks in the queue
- `S` to shuffle the queue
- `s` to save the queue as a playlist
- `R` to rename a playlist
- `D` to batch download all search results

## Configuration

Edit `config.h` and rebuild. You can change:

- All keybinds
- All colors (headers, numbers, playing indicator, visual selection, search bar, progress bar, home screen, song/playlist markers, and more)
- Library path
- Search result count (songs and playlists separately)
- Volume step and seek step

Build options (compiler, flags, install prefix) are in `config.mk`.

See `man hum` after installing for the full reference.

## License

[MIT](LICENSE)
