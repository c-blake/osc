const char *const use =     // Build: cc -o osc osc.c            vim:set sw=2:
"Usage:\n  osc [-t ms] PAYLOAD1 [PAYLOAD2 ...]\n\n"
"  -t ms  per-query timeout in milliseconds (default: 500)\n"
"  -e ch  end-byte terminating output records (newline by default; ''=NUL)\n"
"  -h     this help\n\n"
"Each PAYLOAD is the body of an OSC sequence (between ESC] and ST). Payloads\n"
"ending in ';?' are queries; Anything else is a set operation. osc auto-detects\n"
"how many replies are expected and enters raw mode only as needed.  E.g.:\n"
"  osc -e ' ' '10;?' '11;?' | read fg bg\n"
"  osc '4;0;?' '4;1;?' '4;2;?' |\n"
"    while read color; do echo $color; done\n"
"  osc '2;My Title'           set window title (no reply expected)\n"
"  osc '10;?' '2;Foo' '11;?'  mix queries & sets\n\n"
"For each query payload, one line is written to stdout containing the inner\n"
"payload from the terminal's OSC reply (framing stripped).  osc aborts if any\n"
"reply times out or seems garbled.\n\n"
"Toward that end, CursorPositionReport (CPR) ESC[6n sentinels trail payloads\n"
"to distinguish terminal answer back from no answer at all / some concurrent\n"
"user input.  Also, a suspended query with a remote terminal still replying\n"
"cannot be soundly resumed.  So, upon SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU the\n"
"terminal is restored and osc exits immediately.\n\n"
"Exits: 0 success, 1 bad command syntax, 2 no ctty, 3 OOM, 4 tcgetattr fail,\n"
"  5 query write fail, 6 stderr fail, 7 time out/garbled reply, 8 stopped\n";

#define _DEFAULT_SOURCE  // cfmakeraw, clock_gettime, sigaction, poll
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

int            fd = -1; // global tty fd & saved termios, but structurally the..
struct termios saved;   //..signal involvement makes refactor into a lib unwise.

double now(void) {      // monotonic seconds as a double
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec*1e-9;
}

void restoreTty(void) { if (fd >= 0) tcsetattr(fd, TCSAFLUSH, &saved); }

void enterRaw(void) {
  struct termios t = saved;
  cfmakeraw(&t);        // Scrolling may still be wonky, but can at least..
  t.c_oflag |= OPOST;   //..restore output \n->\r\n so users see what to parse.
  t.c_cc[VMIN]  = 1;    // Block until a byte arrives..
  t.c_cc[VTIME] = 0;    // ..poll() enforces our timeout
  tcsetattr(fd, TCSAFLUSH, &t);
}

// Fatal signals. Restore tty & re-raise so shells see correct $?
void fatal(int sig) { restoreTty(); signal(sig, SIG_DFL); raise(sig); }

// Stop/background signals. Cannot resume safely => Restore tty & exit.
void stop(int sig) { (void)sig; restoreTty(); _exit(8); }

int fatals[] = { SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS,
  SIGFPE, SIGUSR1, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, SIGSTKFLT, SIGXCPU,
  SIGXFSZ, SIGPOLL, SIGPWR, SIGSYS };
int stops[]  = { SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU };

void installHandlers(void) {
  struct sigaction fatal_sa = { .sa_handler = fatal, .sa_flags = 0 };
  struct sigaction stop_sa  = { .sa_handler = stop,  .sa_flags = 0 };
  sigemptyset(&fatal_sa.sa_mask);
  sigemptyset(&stop_sa.sa_mask);
  for (size_t i = 0; i < sizeof(fatals) / sizeof*fatals; i++)
    sigaction(fatals[i], &fatal_sa, NULL);
  for (size_t i = 0; i < sizeof(stops) / sizeof*stops; i++)
    sigaction(stops[i], &stop_sa, NULL);
}

// Reply read/parse: accumulate 1 OSC reply into buf including prefix.  Discard
// CSI & other seqs.  Return when CPR sentinel (ESC[...R) arrives or deadline is
// reached.  Returns 1 if an OSC payload was captured, <= 0 otherwise.
int readReply(char *rBuf, int nBuf, double deadline) {
  enum { S_IDLE, S_ESC, S_OSC, S_OSC_ESC, S_CSI } state = S_IDLE;
  int nOSC = 0, got_osc = 0;
  while (1) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rem_ms = (int)((deadline - now())*1000);
    if (rem_ms <= 0) break;
    if (poll(&pfd, 1, rem_ms) <= 0) // <=0 covers tmOut(0)&-1 EINTR from signals
        break;                      // Signal handlers re-raise/exit; So done.
    unsigned char c;
    if (read(fd, &c, 1) != 1) continue;
    switch (state) {
    case S_IDLE:
      if (c==0x1b) state = S_ESC;
      break;
    case S_ESC:
      if (c < 0x20) return -1;      // Unprintable ASCII is supposedly illegal
      if      (c==']') { state = S_OSC; nOSC = 0; }
      else if (c=='[')   state = S_CSI;
      else               state = S_IDLE;
      break;
    case S_OSC:
      if      (c==0x07) { got_osc=1; rBuf[nOSC]='\0'; state=S_IDLE; }   //BEL
      else if (c==0x1b)   state = S_OSC_ESC;
      else if (nOSC < nBuf - 1) rBuf[nOSC++] = (char)c;
      else { fprintf(stderr, "osc: reply too long\n"); return -2; }
      break;
    case S_OSC_ESC:
      if (c < 0x20) return -1;      // Unprintable ASCII is supposedly illegal
      if (c=='\\') { got_osc=1; rBuf[nOSC] = '\0'; state=S_IDLE; }      //ST
      else { state = c==']' ? S_OSC : (c=='[' ? S_CSI : S_IDLE); nOSC = 0; }
      break;
    case S_CSI:
      if (c >= 0x40 && c <= 0x7e) {
        if (c == 'R') { rBuf[nOSC] = '\0'; return got_osc; } // CPR sentinel
        state = S_IDLE;
      }
      break;
    }
  }
  rBuf[nOSC] = '\0';
  return got_osc;
}

int main(int ac, char *av[]) {  // a[cvnq] = count, vector, le(n)gth, is(q)uery
  int  an[ac], opt, timeout_ms = 500, total = 0, nQ = 0, nB = 0, i;
  char aq[ac], *qBuf, rBuf[4096], eor = '\n';  // 4096 always plenty, AFAIK
  while ((opt = getopt(ac, av, "t:e:h")) != -1) switch (opt) { // for each opt
    case 't': if ((timeout_ms = atoi(optarg)) <= 0) timeout_ms = 500; break;
    case 'e': eor = *optarg; break;
    case 'h': fputs(use, stdout); return 0;
    default:  fputs(use, stderr); return 1;
  }
  if (optind >= ac) { fputs(use, stderr); return 1; }
  if ((fd = open("/dev/tty", O_RDWR)) < 0) return perror("osc: /dev/tty"), 2;
  for (i = optind; i < ac; i++) {
    an[i] = strlen(av[i]);
    aq[i] = an[i] >= 2 && av[i][an[i] - 2] == ';' && av[i][an[i] - 1] == '?';
    nQ += aq[i];
    total += 4 + an[i] + 4*aq[i];       // Queries get a footer/tail/terminator
  }     // ^^\e[ST      ^^\e[6n
  if (!(qBuf = malloc(++total)))        // ++ for snprintf NUL term
    _exit(3);
  for (i = optind; i < ac; i++) {       // Build up query to write
    nB += snprintf(qBuf + nB, total - nB, "\x1b]%s\x1b\\", av[i]);
    if (aq[i])                          // 1-per-query CPR sentinel
      nB += snprintf(qBuf + nB, total - nB, "\x1b[6n");
  }
  if (nQ > 0) {                         // Raw mode is needed
    if (tcgetattr(fd, &saved) < 0) return perror("osc: tcgetattr"), 4;
    installHandlers();                  // Now sound restoration is possible
    enterRaw();                         // Enter raw
  }
  if (write(fd, qBuf, nB) != nB) {
    if (write(2, "osc: failed terminal query write\n", 33) != 33) _exit(6);
    _exit(5);
  }
  if (nQ > 0) {                         // Raw mode was needed; read replies
    double deadline = now() + timeout_ms*1e-3;
    for (i = optind; i < ac; i++) if (aq[i]) {           // for each query
      if (readReply(rBuf, sizeof rBuf, deadline) <= 0 || // bad read || ..
          strncmp(rBuf, av[i], an[i] - 1) != 0)          // prefix mismatch
        break;
      fputs(rBuf, stdout);  // write output record..
      fputc(eor, stdout);   //..and its terminator.
    }
    restoreTty();       // Restore terminal, only if we altered it
    if (i < ac)
      return fprintf(stderr, "osc: garbled response\n"), 7;
  }
  return 0;
}
