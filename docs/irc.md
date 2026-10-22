# The IRC client

**IRC** (advanced tier) chats through the same WiFi modem as the
[terminal](terminal.md): it dials `ATDT irc.server:6667`, and the adapter's
TCP connection carries IRC. The conversation is drawn on the terminal's
80-column screen, with mIRC colours and the PC character set; the line you
type is below it.

One channel at a time: joining another leaves the one you are in. Private
messages show in the same window, marked.

The modem has no TLS, so this is plain-text IRC on port 6667 (or whatever
the network uses for it), and passwords cross the internet as typed. Most
networks still offer it.

## The list

| Entry | |
| --- | --- |
| A server | Connect, edit it or delete it |
| New server | Add one |
| Password lock | Seal the passwords with a master password (below) |
| Modem settings | Shared with the terminal |
| Save / Save as... / Load from... | As in the terminal: the bookmarks of both live together |

Each server has its own nick, real name, channel to join, server password
(`PASS`), NickServ password, and whether to show joins, parts and quits. A
nick that is taken is retried with `_`, then as its first eight characters
and a digit, for servers that cut nicks to nine.

The NickServ password is sent as `PRIVMSG NickServ :IDENTIFY ...` once the
server welcomes you, which is what the big networks' services expect. On a
network with no services, whoever holds the nick "NickServ" would get it -
so leave it empty there. The channel is joined straight after, so a channel
that wants an identified nick may refuse the first join; `/join` it again.

## In a session

| Pad | |
| --- | --- |
| Triangle | The keyboard: type into the line |
| Cross | Send the line (with nothing typed, the keyboard) |
| Square | Who is in the channel |
| Circle | Back to the newest line |
| Up / Down | Lines sent before |
| Left / Right | Move in the line |
| L1 / R1, L2 / R2 | Scroll back and forward a line, or a page |
| Select | Send a phrase - as if typed, so `/join #x` works as a phrase |
| Start | Join, leave, people, nick, send a phrase, hang up or reconnect, clear, leave |

On the keyboard, Enter sends, **Tab completes a nick** (again for the next
match; at the start of the line it adds `: `), Esc clears the line, and
Circle puts the keyboard away.

The bar under the conversation shows the topic, the line, and the state. It
flashes when a line mentions your nick or someone messages you; those lines
show the nick in yellow.

### Commands

| | |
| --- | --- |
| `/join #chan [key]` | Leave this channel for another (`/join tinrom` means `#tinrom`) |
| `/part [why]` | Leave it |
| `/msg nick text` | A private message; `/r text` answers the last one |
| `/me does something` | An action |
| `/nick name`, `/topic [text]`, `/whois nick`, `/names` | |
| `/notice nick text`, `/ctcp nick VERSION`, `/away [why]` | |
| `/clear` | Wipe the screen here |
| `/quit [why]` | Say goodbye; the session hangs up when the server lets go |
| `/raw LINE` | Sent as it is |
| `//text` | A message that starts with a slash |

Any other `/command` goes to the server as it is - `/mode`, `/kick`,
`/invite`, `/list` (which on a big network is very long).

### What the client answers by itself

`PING` from the server, and the CTCP requests `VERSION`, `PING`, `TIME` (with
"unknown": a PlayStation has no clock) and `CLIENTINFO` - at most one every
second and a half, so a flood of requests cannot get you thrown off for
flooding back.

### What is shown

- UTF-8, or Latin-1 from old clients, in the PC character set; what it has no
  glyph for is `?`. Emoji are `?`.
- mIRC colours 0-15, bold (as bright), reverse and reset. Colours 16-98 show
  as the default; italic, underline and strike-through are dropped.
- Long lines wrap at a space, and the rest is indented under the message.
- Nicks keep one colour each, from a hash of the nick.
- NAMES is listed when the channel has 40 people or fewer; otherwise there is
  a count, and Square has them all. The list holds 400; beyond that it says
  how many it could not keep.

Nothing is logged to the card.

## The password lock

Off, passwords are stored as typed. Anything that reads the card - a PC with
an adapter, a save editor - can read them.

On, they are sealed with a master password:

- The key is derived from it with PBKDF2-HMAC-SHA-256, 4096 iterations, and a
  random 16-byte salt. That takes about a second on the console.
- Each password is encrypted with ChaCha20 under that key and a nonce of its
  own, its length included, so a sealed one does not give away how long it
  is.
- The master password is never stored. A 16-byte check value derived from
  the key tells a right one from a wrong one.

Connecting to a server with sealed passwords asks for the master password
once; it is then kept in RAM until power-off or **Lock now**. **Change master
password** and **Turn the lock off** ask first. **Forget the passwords** is for
a master password nobody remembers: it deletes the sealed passwords and turns
the lock off, without asking for it.

Turning the lock on or off changes what is in memory; **Save** puts it on the
card.

What it is not: a card can be copied, and a copy can be guessed at on a PC as
fast as the PC can go. 4096 iterations is what a 33 MHz console can afford,
which is little to a PC, so a short master password keeps out the casual and
not much more. A long one - a few words - keeps out a great deal more.

A console has no random number generator. The salt and nonces come from a
pool stirred with the root counters' values at every key typed on the
on-screen keyboard - the master password's included - and at button presses
in a session, and hashed with them again when a value is drawn. Each nonce
is half a counter and half that pool; the counter jumps by a random amount
each time the lock is opened, so that a copy loaded from an older save does
not count over values used already.

The ciphertext is not authenticated: anyone able to write to the card could
change a sealed password, though not read it.

## Inside

| | |
| --- | --- |
| `src/comms/irc.c` | The protocol: parsing, registration, the nick list, modes, CTCP, commands, completion |
| `src/comms/irctext.c` | Lines onto the screen: UTF-8 to CP437, mIRC codes, wrapping |
| `src/comms/crypto.c` | SHA-256, HMAC, PBKDF2, ChaCha20 |
| `src/comms/vault.c` | The password lock |
| `src/comms/commsui.c` | The modem settings and saving screens, shared with the terminal |
| `src/modules/irc/` | The server list, its editor, and the session |

`irc.c`, `irctext.c`, `crypto.c` and `vault.c` have no hardware in them;
`scripts/test-irc.sh` tests them on a PC, the cryptography against the
published test vectors. `scripts/render-ui.sh` draws a session against a
scripted server, into `build/render/irc*.png`.

The terminal and the IRC client share one 80x25 screen (`termViewScreen()`),
since only one runs at a time.
