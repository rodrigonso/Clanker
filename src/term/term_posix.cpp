#include "term/term.h"

#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static_assert(sizeof(struct termios) <= 64, "termios storage too small");

bool Term_IsInteractive() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

bool Term_Enter(Term* t) {
  memset(t, 0, sizeof(*t));
  t->WakeReadFd = -1;
  t->WakeWriteFd = -1;
  if (!Term_IsInteractive()) return false;

  t->InFd = STDIN_FILENO;
  t->OutFd = STDOUT_FILENO;

  int wake[2];
  if (pipe(wake) != 0) return false;
  t->WakeReadFd = wake[0];
  t->WakeWriteFd = wake[1];
  fcntl(t->WakeReadFd, F_SETFL, fcntl(t->WakeReadFd, F_GETFL, 0) | O_NONBLOCK);
  fcntl(t->WakeWriteFd, F_SETFL, fcntl(t->WakeWriteFd, F_GETFL, 0) | O_NONBLOCK);

  struct termios* saved = (struct termios*)t->SavedTermios;
  if (tcgetattr(t->InFd, saved) != 0) {
    close(t->WakeReadFd);
    close(t->WakeWriteFd);
    t->WakeReadFd = -1;
    t->WakeWriteFd = -1;
    return false;
  }

  struct termios raw = *saved;
  raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
  raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(tcflag_t)(OPOST);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(t->InFd, TCSAFLUSH, &raw);

  t->Active = true;
  Term_TrackForRestore(t);
  // Alt screen, clear, home, bracketed paste, and SGR all-motion mouse tracking.
  Term_WriteStr("\x1b[?1049h\x1b[2J\x1b[H\x1b[?2004h\x1b[?1003h\x1b[?1006h");
  return true;
}

void Term_Exit(Term* t) {
  if (!t->Active) return;
  Term_WriteStr("\x1b[?1006l\x1b[?1003l\x1b[?2004l\x1b[?25h\x1b[?1049l");
  tcsetattr(t->InFd, TCSAFLUSH, (struct termios*)t->SavedTermios);
  if (t->WakeReadFd >= 0) close(t->WakeReadFd);
  if (t->WakeWriteFd >= 0) close(t->WakeWriteFd);
  t->WakeReadFd = -1;
  t->WakeWriteFd = -1;
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

u32 Term_ReadInput(const Term* t, char* buf, u32 cap) {
  ssize_t n = read(t->InFd, buf, cap);
  if (n <= 0) return 0;
  return (u32)n;
}

bool Term_WaitInput(const Term* t, u32 timeoutMs) {
  struct pollfd fds[2] = {};
  fds[0].fd = t->InFd;
  fds[0].events = POLLIN;
  fds[1].fd = t->WakeReadFd;
  fds[1].events = POLLIN;
  int timeout = timeoutMs > 0x7fffffffu ? 0x7fffffff : (int)timeoutMs;
  int result = poll(fds, 2, timeout);
  if (result <= 0) return false;
  if ((fds[1].revents & POLLIN) != 0) {
    char wake[16];
    while (read(t->WakeReadFd, wake, sizeof(wake)) > 0) {}
    return false;
  }
  return (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

void Term_WakeInput(const Term* t) {
  if (!t->Active || t->WakeWriteFd < 0) return;
  char wake = 1;
  (void)write(t->WakeWriteFd, &wake, 1);
}
