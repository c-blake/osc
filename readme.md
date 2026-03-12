# osc — Operating System Command (OSC) escape sequence query & set utility

A small, correct portable C utility for sending terminal OSC (Operating System
Command) escape sequences and reading back replies, suitable for use in shell
scripts & pipelines over high-latency (SSH, long-haul) connections.

<details><summary><strong>The problem `osc` solves</strong>
</summary>
Shell scripts querying terminal properties using `stty raw`/etc. abound in
various Internet spaces, but tend to be fragile & use raw protocols.  E.g. they
do not usually handle `SIGTTIN`, `SIGTTOU` etc. while awaiting terminal replies.
(Running one with `&` can actually trigger SIGTTIN/OU.)  Meanwhile, terminals
continue sending reply data after processes are suspended, making resumption
unsound as well as buffering weird commands to remote shells.  High-latency
networks, meanwhile, compound these problems by pushing for large time outs -
giving user's time to hit keys corrupting the protocol.

`osc` is an attempt to be as careful a little C program as is reasonable, doing
proper signal handling and higher-level framing to avoid as much trouble as
possible.  It aims to be a rendezvous point enabling safer use of these
protocols, a canonical/reference tool to deliver trustable, consistent answers
rather than harder to audit and ad hoc/personal scripting solutions.
</details>
<details><summary><strong>How `osc` solves it</strong>
</summary>
`osc` sends one or more OSC payloads to the terminal via `/dev/tty`, reads back
replies, strips protocol framing, and writes one reply per query line to stdout.
It auto-detects whether any replies are expected and only enters raw/cbreak mode
as needed, making it safe to use for pure set operations (window title, palette
assignment) without any hung terminal/raw mode risk.
</details>

# Usage

```sh
osc [-t ms] PAYLOAD1 [PAYLOAD2 ...]
  -t ms  per-query timeout in milliseconds (default: 500)
  -e ch  end-byte terminating output records (newline by default; ''=NUL)
  -h     this help
```
For high-latency remote connections you may want something like `-t 2000`.

It is designed for use in pipelines:
```sh
osc -e ' ' '10;?' '11;?' | read fg bg
osc '4;0;?' '4;1;?' '4;2;?' |
  while read color; do echo $color; done
```
</details>
<details><summary><strong>Supported protocols (non-exhaustive)</strong>
</summary>
This program is intentionally agnostic to details of the OSC request/reply (as
long as replies do not look like CPRs).  This is to be future proof as terminals
add more OSCs, but for the curious circa 2026, terminal property queries and set
operations include:
  - OSC 10  foreground color query/set
  - OSC 11  background color query/set  ← dark/light theme detection
  - OSC 12  cursor color query/set
  - OSC 4   indexed palette color query/set (256-color palette)
  - OSC 104 reset palette entry
  - OSC 2   window title set
  - OSC 52  clipboard access (base64)
  - OSC 7   current working directory
  - OSC 8   hyperlinks

Reply format is whatever the terminal emits with OSC framing stripped, usually
`rgb:rrrr/gggg/bbbb` (16-bit per channel) for color queries.  This has useful
information: https://wiki.tau.garden/x11-colors/

(One might also say "past proof" per an old `rxvt` ESC[7n request for `DISPLAY`
setting in the innocent pre-ssh/DISPLAY-forwarding days of the Internet.)
</details>
<details><summary><strong>Signal Handling Details</strong>
</summary>
Mostly already said, but:
  - SIGHUP, SIGINT, ..: restore terminal & re-raise (shell sees correct $? /
    128+signum exit status).

  - SIGTSTP, SIGTTIN, SIGTTOU, SIGCONT: restore terminal then exit immediately.
    A suspended OSC query cannot be safely resumed since remote terminals may
    continue sending reply data during local process suspension.

  - `SIGCONT` is subtle.  `SIGSTOP` is uncatchable, but if it *had* happened and
    the process was then continued with `SIGCONT` then the same problems as the
    above suspension happens.  Probably the line editor/shell reads the reply.
    From within the signal handler, there is no portable/reliable way to know if
    a process moved from `SIGSTOP`'d to continued, but the handler is called
    regardless.  So, this program essentially converts that kind of "useless but
    harmless `SIGCONT` to an already running program" into termination, but that
    still seems the safest solution.

Concurrent `osc` instances against the same terminal produce undefined results,
but probably failure.  Use flock(1) or similar if shell parallelism is in play.
</details>
<details><summary><strong>Sentinel / framing</strong>
</summary>
CPR (Cursor Position Report, `ESC[6n`) sentinels are sent after each query.
This serves two purposes:

1. Fail fast on terminals that do not support OSC (Linux virtual console, old
   VT100/VT102 terminals, HP terminals): the CPR reply arrives immediately with
   no preceding OSC reply.  So, `osc` exits in under 1 ms rather than waiting
   for the full timeout.

2. Distinguishes terminal answer back from concurrent user keyboard input.  This
   is imperfect, but something is better than nothing.
</details>
<details><summary><strong>Why not a shell script?</strong>
</summary>
All of this, including the CPR termination, *can* be done in shell (with slower
execution and more propensity to lose races), but for various reasons *generally
isn't*.  E.g., `SIGTTIN` *can* be handled in *sub*-shells (not your main line-
editor shell) with `trap`, yet I could not find even one example of an OSC
protocol driver doing so - only dozens not doing so.  The culture of this space
is "sloppy, works for me on my terminal fast enough with nothing weird afoot".
This compounds the already bad reputation of an intrinsically racy protocol.
`osc` tries to raise the bar a little.
</details>
<details><summary><strong>Why not xtermcontrol?</strong>
</summary>
`xtermcontrol` covers the xterm-era OSC subset (colors, title, font, geometry)
but sounds xterm-centric (even if most modern emulators imitate xterm), does not
support general payload passthrough, and has limited timeout configurability for
high-latency links.
</details>

# Build

```sh
cc -o osc osc.c
```
Single file, no dependencies beyond a POSIX.1-2008 + `cfmakeraw` libc (Linux
glibc, musl, macOS, FreeBSD, OpenBSD, NetBSD).

[`osc` also works with `cosmocc` for an actually portable executable (APE) about
350 KiB in my tests](github.com/c-blake/osc/releases/latest/download/osc) (with
`cosmocc -Os -mtiny -o osc osc.c`).  A statically linked Linux musl-gcc is about
40 KiB.
<details><summary><strong>Related concepts</strong>
</summary>
Terminal escape sequences, terminal emulator control sequences, ANSI SGR escape
codes, xterm control sequences, OSC sequences, Operating System Commands,
terminal color query, background color detection, dark mode detection, light
mode detection, dark-mode detection, light-mode detection, terminal palette
query, dynamic colors, terminal property query, raw mode terminal, cbreak mode,
termios, VMIN VTIME, terminal SSH pipeline, terminal job control, terminal
signal handling, signal safety, ctlseqs, XTerm Control Sequences, invisible
character framing, VT100 VT220 xterm st kitty foot ghostty iTerm2 WezTerm
alacritty Konsole GNOME Terminal color scheme detection
</details>
<details><summary><strong>Appendix: Parsing Colors</strong>
</summary>
Users of this utility may also find this code handy.  You might pipe `osc
'4;0;?' '4;1;?' '4;2;?' '4;3;?' ..` to `gawk`:
```awk
BEGIN { FS = ";" } {    # Targeted at the 4;<idx>;? form only
  idx = $2
  rgb = $3              # $3 is "rgb:hhhh/hhhh/hhhh"
  sub(/^rgb:/, "", rgb)
  split(rgb, chans, "/")
  r = strtonum("0x" chans[1]) / 65535   # gawk only
  g = strtonum("0x" chans[2]) / 65535
  b = strtonum("0x" chans[3]) / 65535
  printf "%s %.5f %.5f %.5f\n", idx, r, g, b
}
```
or you could be slightly more general (any number of hex digits) like this Zsh:
```zsh
parseColor () {  # Zsh color parser - strip "4;1;rbg:" before this call!
  local cs=("${(@s:/:)1}") 
  local digits=${#cs[1]} 
  local mx=$((16**digits - 1.0))  # Zsh specific FP arithmetic
  printf "r=$((16#${cs[1]}/mx)); g=$((16#${cs[2]}/mx)); b=$((16#${cs[3]}/mx))"
}
# E.g.Use: a=${rec#4;}; eval `parseColor ${a#*;}`; echo ${a%%;*} $r $g $b
```
</details>
