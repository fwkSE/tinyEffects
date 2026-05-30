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
-T         Terminal mode instead of graphical X11 mode
-F         Start graphical mode fullscreen
-h         Show help
```

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
