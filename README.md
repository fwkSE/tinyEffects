# tinyEffects

`tinyfx` is a small Linux EQ visualizer aimed at low-resource machines. It
captures mono audio through ALSA, analyzes a small set of frequency bands, and
draws a lightweight graphical X11 window by default. A terminal renderer is
still available with `-T`.

The first target is antiX on netbook-class hardware, so the defaults favor
low sample rates, capped redraws, fixed buffers, and no terminal UI framework.

## Requirements

- C compiler and `make`
- X11 development headers/library
- ALSA runtime library (`libasound.so.2`)

On Debian/antiX:

```sh
sudo apt install build-essential libx11-dev libasound2
```

## Build

```sh
make
```

This creates `./tinyfx`.

## Usage

Capture from the default ALSA input:

```sh
./tinyfx -d default
```

Render a graphical window with the text as large as possible while keeping its
proportions, and pulse its letters with EQ-driven colors:

```sh
./tinyfx -d default -t "tinyEffects"
```

Start in fullscreen:

```sh
./tinyfx -F -d default -t "tinyEffects"
```

Run without music or an audio device using demo mode:

```sh
./tinyfx -m -t "tinyEffects"
```

Use a built-in color palette:

```sh
./tinyfx -m -t "tinyEffects" --palette neon
```

Use custom color stops:

```sh
./tinyfx -m -t "tinyEffects" --colors '#001122,#44ccff,#ffffff'
```

Low frequencies drive the left side of the display. Higher frequencies drive
progressively farther-right characters or bars.

Press `q`, `Esc`, or `Ctrl-C` to quit.
In graphical mode, press `f` or `F11` to toggle fullscreen.

## Options

```text
-d DEVICE  ALSA capture device (default: default)
-t TEXT    Render this exact text once with EQ color pulsing
-r RATE    Sample rate, Hz (default: 11025)
-f FPS     Max redraws per second, 1..60 (default: 15)
-b BANDS   EQ bands, 4..64 (default: 24)
-m         Demo mode: animate without ALSA or music
-M         Poll MOC (mocp) for "Artist - Song" text
-T         Terminal mode instead of graphical X11 mode
-F         Start graphical mode fullscreen
-p NAME    Palette: classic, fire, ice, matrix, mono, rainbow, neon
-C COLORS  Custom #RRGGBB colors, comma-separated
-h         Show help
```

Long forms are also available for palettes:

```sh
--palette fire
--palette neon
--colors '#000000,#00ff00,#ffffff'
```

Palette colors are mapped left-to-right by frequency. Audio energy controls
brightness, so quiet sections use a dim version of the same palette color.
The `neon` palette is inspired by cyan, pink, magenta, purple, and white neon
sign colors. `soundsvall` is accepted as an alias for `neon`.

## Capturing System Audio

For microphone or line-in, use the normal capture device, often `default` or
`hw:0,0`.

For system output, ALSA needs a capture-capable monitor device. A common
approach is `snd-aloop`:

```sh
sudo modprobe snd-aloop
aplay -l
arecord -l
```

Then route your player to the loopback playback side and run `tinyfx` against
the matching capture side, for example:

```sh
./tinyfx -d hw:Loopback,1,0 -t "Now Playing"
```

For the old terminal renderer, add `-T`:

```sh
./tinyfx -T -d hw:Loopback,1,0 -t "Now Playing"
```

Exact loopback device numbers vary by machine and ALSA configuration.

## MOC (Music On Console)

If you use [MOC](https://moc.daper.net/) as your player, `tinyfx` can poll the
current track with `-M` and display `Artist - Song` instead of fixed `-t` text:

```sh
./tinyfx -M -d hw:Loopback,1,0 --palette neon
```

`mocp` must be installed and the MOC server should already be running. Metadata
is refreshed about once per second. When nothing is playing, `-M` falls back to
`-t` text if you provided it, otherwise bar mode is shown.

Route MOC playback through the same ALSA loopback or monitor device you use for
the EQ capture side.

## Low-Resource Tuning

The defaults are intentionally modest:

- `11025` Hz mono capture
- `24` frequency bands
- `15` FPS redraw cap
- `512` sample processing windows
- X11 software rendering with a single pixel buffer
- terminal fallback bar rendering capped to `120x40` cells on large terminals

On very slow machines, try lowering bands or FPS:

```sh
./tinyfx -b 12 -f 10 -t "tinyEffects"
```
