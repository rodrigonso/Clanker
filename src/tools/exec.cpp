#include "tools/exec.h"

#if defined(_WIN32)
#include <windows.h>

ExecResult Exec_Run(Arena* arena, StrView command, u32 timeoutMs, volatile i32* abort) {
  ExecResult res = {};
  res.ExitCode = -1;

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    res.Output = SV("failed to create pipe");
    return res;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  StrBuf cmd = StrBuf_Make(arena, command.Length + 16);
  StrBuf_AppendCStr(&cmd, "cmd.exe /c ");
  StrBuf_Append(&cmd, command);

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pinfo = {};
  BOOL ok = CreateProcessA(nullptr, (LPSTR)StrBuf_CStr(&cmd), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pinfo);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    res.Output = SV("failed to start process");
    return res;
  }
  res.Spawned = true;

  StrBuf out = StrBuf_Make(arena, 4096);
  DWORD start = GetTickCount();
  char buf[4096];
  for (;;) {
    if ((abort && *abort) || (timeoutMs && (GetTickCount() - start) > timeoutMs)) {
      TerminateProcess(pinfo.hProcess, 1);
      res.TimedOut = !(abort && *abort);
      break;
    }
    DWORD avail = 0;
    if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD toRead = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
      DWORD n = 0;
      if (ReadFile(rd, buf, toRead, &n, nullptr) && n > 0)
        StrBuf_Append(&out, StrView{buf, (u32)n});
      else
        break;
    } else if (WaitForSingleObject(pinfo.hProcess, 10) == WAIT_OBJECT_0) {
      for (;;) {
        DWORD a2 = 0;
        if (!(PeekNamedPipe(rd, nullptr, 0, nullptr, &a2, nullptr) && a2 > 0)) break;
        DWORD toRead = a2 > sizeof(buf) ? (DWORD)sizeof(buf) : a2;
        DWORD n = 0;
        if (ReadFile(rd, buf, toRead, &n, nullptr) && n > 0)
          StrBuf_Append(&out, StrView{buf, (u32)n});
        else
          break;
      }
      break;
    }
  }

  DWORD code = 0;
  GetExitCodeProcess(pinfo.hProcess, &code);
  res.ExitCode = (i32)code;
  CloseHandle(rd);
  CloseHandle(pinfo.hProcess);
  CloseHandle(pinfo.hThread);
  res.Output = StrBuf_View(&out);
  return res;
}

#else  // POSIX
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

ExecResult Exec_Run(Arena* arena, StrView command, u32 timeoutMs, volatile i32* abort) {
  ExecResult res = {};
  res.ExitCode = -1;

  int fds[2];
  if (pipe(fds) != 0) {
    res.Output = SV("pipe failed");
    return res;
  }

  StrBuf cmd = StrBuf_Make(arena, command.Length + 1);
  StrBuf_Append(&cmd, command);

  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    res.Output = SV("fork failed");
    return res;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    execl("/bin/sh", "sh", "-c", StrBuf_CStr(&cmd), (char*)nullptr);
    _exit(127);
  }
  close(fds[1]);
  res.Spawned = true;

  StrBuf out = StrBuf_Make(arena, 4096);
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  char buf[4096];
  for (;;) {
    struct pollfd p = {fds[0], POLLIN, 0};
    int pr = poll(&p, 1, 50);
    if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
      ssize_t n = read(fds[0], buf, sizeof(buf));
      if (n > 0)
        StrBuf_Append(&out, StrView{buf, (u32)n});
      else if (n == 0)
        break;
    }
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    u64 elapsed = (u64)(t1.tv_sec - t0.tv_sec) * 1000 + (u64)(t1.tv_nsec - t0.tv_nsec) / 1000000;
    if ((abort && *abort) || (timeoutMs && elapsed > timeoutMs)) {
      kill(pid, SIGKILL);
      res.TimedOut = !(abort && *abort);
      break;
    }
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  res.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  res.Output = StrBuf_View(&out);
  return res;
}

#endif
