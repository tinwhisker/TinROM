# The serial terminal

**Terminal** (advanced tier) calls BBSes through a WiFi modem on SIO1: one of
the serial-to-WiFi adapters that pretends to be a Hayes modem and opens a TCP
connection when told `ATDT somebbs.domain:23`. The far end is drawn as a BBS
expects - 80×25, ANSI colours and the PC character set.

The [IRC client](irc.md) uses the same modem, screen and keyboard, and keeps
its servers in the same bookmark list - each client shows its own kind.

## The list

| Entry | |
| --- | --- |
| A bookmark | Connect, edit it or delete it |
| Dial an address... | Type a `host:port` and dial it once |
| Talk to the modem | Type at the adapter yourself: `ATI`, its own setup commands, `ATD` by hand |
| New bookmark | Name, address, and whether to speak telnet |
| Modem settings | Below |

| Save | Bookmarks, phrases and modem settings, to the default place |
| Save as... | The same, to a place you pick |
| Load from... | Read them back from a place you pick |

## Saving

Bookmarks, phrases and the modem profile can live in four places: on either
card, either **with the settings** (inside `tinrom settings`) or **as their own
save** (`tinrom bookmarks`, one more block). See "tinrom's own saves" in
[memory-cards.md](memory-cards.md).

- **Save** goes into the settings file on the card tinrom loaded its settings
  from - card 1 if it had them, otherwise card 2. With no settings saved
  anywhere it uses card 1 if there is one, else card 2, and writes the current
  settings alongside so that card becomes the settings card.
- **Save as...** picks any of the four places. Places on a missing card are
  greyed out.
- **Load from...** lists only the places that hold something.

On entering the terminal for the first time after power-on, tinrom reads all
four places and takes the most recently saved - each save carries a counter,
so this works even with no clock. Nothing is read from a card after that
until you ask, so edits are not overwritten behind your back.

## Connecting the adapter

SIO1 is the serial port on the back of the original PS1; the PSone does not
have one. It works at 3.3V logic levels, not RS-232's, so the adapter has to
as well. It needs TX, RX and ground, and RTS and CTS too for the higher rates
(below).

On the adapter, set:

- the same rate as **Modem settings → Rate** (9600 by default);
- hardware flow control if the cable wires RTS and CTS - `AT&K1` on some
  firmware, `AT&K3` on others;
- `AT&D2` if you want **Hang up: drop DTR** and the cable wires DTR;
- telnet translation off, if it has it (often `ATS15=0` or similar): tinrom
  does telnet itself, per bookmark. With both on, negotiation gets answered
  twice.

**Talk to the modem** is the way to do all of that from the console.

## Speed and flow control

The PS1 keeps eight received bytes and tinrom polls, rather than taking an
interrupt per byte. So while a session is open the port is drained into a 4 KB
buffer while the frame waits for vsync, and again between drawing rows of the
screen.

- **With hardware flow control** (RTS and CTS wired, and on in the adapter)
  tinrom drops RTS whenever the buffer is nearly full, and nothing is lost.
  115200 is fine.
- **Without it**, keep to 9600-19200. Faster, and a busy screen can overrun
  the eight bytes; the status line shows how many were **lost**.

**Wait for CTS** makes tinrom hold each byte it sends until the adapter is
ready. Only turn it on if the cable wires CTS: an unconnected CTS can read as
never ready, and every key then takes a frame.

## Modem settings

| Setting | Default | |
| --- | --- | --- |
| Rate | 9600 | The adapter's rate; set it there to match |
| Init | `ATE0V1` | Sent before every dial: echo off, results as words. Empty sends nothing |
| Dial | `ATDT` | Put in front of the address |
| Hang up | `+++ ATH` | Or **drop DTR**, which is quicker but needs `AT&D2` and a wired DTR |
| Wait for CTS | no | See above |
| Carrier on DSR | no | For a cable that takes the adapter's DCD to the PS1's DSR |

tinrom understands the result words (`OK`, `CONNECT`, `NO CARRIER`, `BUSY`,
`NO ANSWER`, `NO DIALTONE`, `ERROR`) and their numbers, ignores what it does
not recognise, and shows it while dialling, which is how an adapter's own
`CONNECTING...` reaches the status line. A call ends when the adapter says `NO
CARRIER`, or when DSR drops if you have said it carries the carrier.

## In a session

| Pad | |
| --- | --- |
| D-pad | The arrow keys |
| Cross | Enter |
| Square | Backspace |
| Circle | Esc |
| Triangle | The on-screen keyboard |
| Select | Send a saved phrase, followed by Enter |
| L1 / R1 | Scroll back / forward a line |
| L2 / R2 | A screen back / forward |
| Start | Hang up, dial again, send a phrase or Ctrl-C, clear the screen, leave |

Leaving hangs up first. The last 200 lines that scrolled off the top can be
scrolled back to; typing returns to the live screen.

### The on-screen keyboard

A US layout in five rows, drawn over the bottom of the screen; the screen
moves up when the cursor would be hidden.

| Pad | |
| --- | --- |
| D-pad | Move between keys |
| Cross | Press the key |
| Square | Backspace |
| Triangle | Shift, for the next key |
| L1 | Space |
| Start | Enter |
| Circle | Put the keyboard away |

**Shift** and **Ctrl** on the keyboard also hold for one key. Ctrl turns a
letter into its control code, so Ctrl then C sends 03h.

The same keyboard edits bookmarks and the modem settings. The menu font has
no lower case, so text there shows in capitals whatever it is; the terminal
font does, and shows it properly.

## What the screen understands

ANSI-BBS: the ECMA-48 / VT100 sequences in the dialect DOS's ANSI.SYS spoke.

- Cursor movement and positioning, save and restore (`ESC [ s` / `u` and
  `ESC 7` / `8`), the scroll region, insert and delete of lines and
  characters, erasing in the current background colour.
- `ESC [ 2 J` homes the cursor, and form feed clears the screen, as BBSes
  expect.
- Colours 30-37 and 40-47 with bold as bright, 90-97 and 100-107, reverse and
  conceal. 256-colour and true-colour codes are taken to the nearest of the
  sixteen. Underline and blink are not shown.
- The cursor position and device attribute queries are answered, which is how
  many BBSes detect an ANSI terminal.
- The full CP437 set, box drawing included, in a 4×8 font drawn for tinrom
  (`src/ui/fonts/term4x8.txt`, editable as text).

### Telnet

Most BBSes are telnet servers, and send option negotiation before anything
else. A bookmark's **Telnet** setting decides whether tinrom answers it:
**auto** (the default) means yes for port 23 or no port, no otherwise. tinrom
lets the server echo and suppress go-ahead, tells it the terminal is `ANSI`
and 80×25, and refuses the rest. With telnet on, Enter goes as CR NUL and 0FFh
is doubled, as the protocol requires.

## How it fits together

| Piece | |
| --- | --- |
| `src/hal/uart_sio.c` | Buffered receive, RTS flow control, DTR, the port claim |
| `src/comms/modem.c` | Dialling and hanging up, as state machines polled once a frame |
| `src/comms/telnet.c` | Negotiation, filtered out of the received stream |
| `src/comms/term.c` | The 80×25 screen and the ANSI parser, with no hardware in it |
| `src/comms/termview.c` | Draws the screen into the text canvas's VRAM slot, only the rows that changed |
| `src/ui/keyboard.c` | The on-screen keyboard and the line editor |
| `src/modules/terminal/` | The list, the settings and the session |

While a session has the port, the serial console's server and the log mirror
stay off it, since anything they sent would go to the modem.

## Tests

`sh scripts/test-terminal.sh` builds the screen, telnet and the modem for the
host and checks them:

- the screen against ANSI sequences: positioning, colours, wrapping, scrolling
  into history, the scroll region, insert and delete, queries, CP437;
- telnet against a negotiating server, including sequences split across reads
  and refusals that must not repeat;
- the modem against a scripted adapter: dialling with and without echo, BUSY,
  numeric results, NO CARRIER split across reads, both ways of hanging up,
  cancelling, and dialling by hand.

`sh scripts/render-ui.sh` draws a session, its keyboard and its menu.
