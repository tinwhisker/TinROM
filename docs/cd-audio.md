# CD audio

`src/hal/cdrom.c` (the CD-DA half) and `src/hal/spu.c`. For a module that
plays audio discs - an audio CD, or the music tracks on a PlayStation disc.

The drive plays CD-DA on its own once told to, at 1x, with no CPU involved.
Its output goes through a 2x2 volume matrix in the drive, then into the SPU's
CD input, and from there to the speakers. The drive does not region-check
audio, so pressed and burned discs play alike with no unlock.

## The calls

| Call | Commands | |
| --- | --- | --- |
| `tinromCdAudioInit(mono)` | - | SPU on and unmuted, CD input up, the drive's matrix set. Once on entry; again to switch mono |
| `tinromCdReadToc(&toc)` | `GetTN`, `GetTD` per track, `GetTD 0` | Track numbers and start times; `TINROM_ERR_NOT_PRESENT` with no disc |
| `tinromCdTrackLength(&toc, n)` | - | Seconds, from the starts |
| `tinromCdPlayTrack(&toc, n)` | `Setmode`, `SetLoc`, `Play n` | From the track's start, pausing at its end |
| `tinromCdPlayAt(seconds)` | `Setmode`, `SetLoc`, `Play` | From a point on the disc |
| `tinromCdPause()` / `tinromCdResume()` | `Pause` / `Play` | Keeps the place |
| `tinromCdScan(dir)` | `Forward` / `Backward` / `Play` | Audible fast forward or rewind; 0 goes back to playing |
| `tinromCdMute(on)` | `Mute` / `Demute` | Playback carries on |
| `tinromCdGetStatus(&s)` | `GetStat` | Lid, spinning, playing |
| `tinromCdGetPosition(&p)` | `GetlocP` | Track, index, time in the track and on the disc |
| `tinromCdAudioEnded()` | - | True once after a track ran out |
| `tinromCdStop()` | `Stop` | Spins down; call on the way out |
| `tinromSpuSetCdReverb(r)` | - | Studio, hall, church, dome or off, on the CD input |
| `tinromSpuReadCdCapture(l, r)` | - | The last 256 samples each side the SPU played, for a visualiser |

## Things the drive does that a player has to know

**Times are seconds, rounded down.** `GetTD` gives minutes and seconds only
(psx-spx), so a TOC start can be up to 74 frames early. `tinromCdPlayTrack`
therefore seeks to that second *and* passes the track number to `Play`, which
starts exactly at the track; the seek is only there for drives that ignore
the number, which psx-spx says they do shortly after power-up.

**Data tracks.** The TOC does not say which tracks are data. On a PlayStation
disc with music, track 1 is the data track; on an audio CD every track is
audio. `tinromCdIdentify()` says which kind of disc it is. A data track
played as audio is silence, not noise (ps1-bare-metal's example 14 notes
the same).

**The end of a track.** Playback runs with AutoPause on: the drive pauses
itself at the end of the track and raises INT4. Nothing waits for that
interrupt, so the driver notices it the next time it sends any command - a
`tinromCdGetStatus()` poll is enough - and `tinromCdAudioEnded()` reports it
once. The disc then sits at the *end* of that track, so moving on is
`tinromCdPlayTrack(next)`, not `tinromCdResume()`.

**Polling, not reports.** The drive can send its position unprompted (report
mode), but tinrom's driver would take those interrupts for the answer to its
next command. Ask with `tinromCdGetPosition()` instead, a few times a second
- from a grid's `poll` callback, say, not from `describe`, which runs every
frame.

**Data reads end playback.** `tinromCdReadSectors()` puts the drive back in
data mode. A module that plays should stop on its way out, before anything
else reads the disc.

## The SPU

tinrom uses the SPU for nothing else, so `tinromSpuInit()` does only what CD
audio needs: SPU on and unmuted, master volume fixed at 3FFFh, every voice
keyed off, reverb out of the mix - which also cuts off the tail of the BIOS's
startup sound - and the CD input left as `tinromSpuSetCdVolume()` sets it.

It does not use ps1-bare-metal's `initSPU()`, because that also writes the
bus's common delay register, which the EXP1 cart tinrom may be running from
shares. It does write the SPU's own bus timing, asserted at compile time to be
the value the BIOS programs, because a start from the cart's pre-boot hook
may come before the BIOS has set it.

## Reverb

The SPU's reverb unit can take the CD input (SPUCNT bit 2) as well as the
voices, so the player's effects - studio, hall, church, dome, and direct
for none - are that, nothing more. The names are the console's own CD
player's; the settings are not. They are tinrom's, in `rooms[]` in
`src/hal/spu.c`, written as a room - how far away the walls are, how much
comes back, how dark the surfaces - and laid out into the 32 registers by
`layoutReverb()`.

They were tuned on a host model of psx-spx's reverb formula, for the
length of the tail and so that full-scale noise does not clip the reverb's
16-bit work area. A reflection that feeds itself builds up at its own
frequency, which is why the input volumes are low and the output ones high.
Approximate tails: studio 0.6 s, hall 1.6 s, church 2.9 s, dome 3.7 s.

The work area is the top 24 KB of sound RAM. Choosing a setting turns the
reverb off, clears the area by manual writes (so neither the last setting's
tail nor whatever was there before is heard), then turns it back on. Off
leaves EVOL at 0, because the unit plays its area whether or not it is
writing it.

## The visualiser

The player draws a visualiser behind its track grid, in place of the
spinning chip: a ring of sixteen spectrum bars, a stereo level meter, or a
ring of the waveform, stepped through with L1 and R1.

Its sound comes from the SPU's capture buffers (psx-spx): the SPU writes
what it plays from the CD input to the bottom of sound RAM, left at 0..3FFh
and right at 400h..7FFh, 512 samples each round and round, and SPUSTAT bit
11 says which half it is on. `tinromSpuReadCdCapture()` DMAs the other
half of each - the last 256 samples, 5.8 ms - once a frame while a track
plays. Sound RAM reads need two extra DMA cycles on the bus, which the
BIOS's timing does not give, so they are raised for the read and put back.

`src/modules/cdplay/visual.c` turns that into what is drawn, in 32-bit
integer maths throughout (64-bit pulls kilobytes of helper code into the
image):

- **Spectrum**: one DFT bin per band, at sixteen frequencies log-spaced
  from 100 Hz to 12 kHz, over a Hann-windowed eight-bit mix of both sides.
  256 samples resolve 172 Hz, so the lowest bands overlap.
- **Level**: each side's peak, 40 dB of it, with the peak held for half a
  second.
- **Waveform**: 24 points from a rising zero crossing, so a steady tone
  stands still.

Everything rises at once and falls back slowly. `src/modules/cdplay/scene.c`
draws it as boxes with the GTE - the model's transform, cull and depth sort,
with flat quads shaded by face - inside the UI's background hook
(`uiSetBackground()`), so it carries on under the menu and message boxes.

Whether an emulator fills the capture buffers is up to the emulator; if
the bars stay flat while music plays, that is where to look first.

## Testing

`scripts/test-cdaudio.sh` runs every call above against a fake drive behind
the registers, which answers as psx-spx documents: INT3 and the status byte,
INT2 as Pause's second answer, INT5 for refusals, BCD throughout, and
`GetlocP` with no status byte in front. It checks the commands and parameters
sent, that every interrupt is acknowledged, and the SPU registers left behind -
the reverb's included, with a fake sound RAM to check that exactly the work
area is cleared, and a fake DMA to check the capture read takes the right
halves with the bus set up for it. `scripts/test-visual.sh` feeds the
visualiser's analysis tones and silence.

On an emulator, pcsx-redux plays an audio disc image loaded the usual way.
