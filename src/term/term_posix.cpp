#include "term/term.h"

#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static_assert(sizeof(struct termios) <= 64, "termios storage too small");

bool Term_IsInteractive() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

bool Term_Enter(Term* t) {
  memset(t, 0, sizeof(*t));
  if (!Term_IsInteractive()) return false;

  t->InFd = STDIN_FILENO;
  t->OutFd = STDOUT_FILENO;

  struct termios* saved = (struct termios*)t->SavedTermios;
  if (tcgetattr(t->InFd, saved) != 0) return false;

  struct termios raw = *saved;
  raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
  raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(tcflag_t)(OPOST);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(t->InFd, TCSAFLUSH, &raw);

  t->Active = true;
  Term_TrackForRestore(t);
  // Alt screen, clear, home, bracketed paste, SGR mouse (button + wheel) tracking.
  Term_WriteStr("\x1b[?1049h\x1b[2J\x1b[H\x1b[?2004h\x1b[?1000h\x1b[?1006h");
  return true;
}

void Term_Exit(Term* t) {
  if (!t->Active) return;
  Term_WriteStr("\x1b[?1006l\x1b[?1000l\x1b[?2004l\x1b[?25h\x1b[?1049l");
  tcsetattr(t->InFd, TCSAFLUSH, (struct termios*)t->SavedTermios);
  t->Active = false;
}

void Term_Size(const Term* t, u16* cols, u16* rows) {
  struct winsize ws;
  if (ioctl(t->OutFd, TIOCGWINSZ, &ws) == 0 && ws.ws_col != 0) {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
  } else {
    *cols = 80;
    *rows = 24;
  }
}

int Term_InFd(const Term* t) { return t->InFd; }

u32 Term_ReadInput(const Term* t, char* buf, u32 cap) {
  ssize_t n = read(t->InFd, buf, cap);
  if (n <= 0) return 0;
  return (u32)n;
}
