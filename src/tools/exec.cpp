#include "tools/exec.h"

#if defined(_WIN32)
#include <windows.h>

static bool Exec_WindowsArgNeedsQuotes(StrView arg) {
  if (arg.Length == 0) return true;
  for (u32 i = 0; i < arg.Length; i++) {
    char c = arg.Data[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') return true;
  }
  return false;
}

static bool Exec_IsAbsoluteWindowsPath(StrView path) {
  if (path.Length >= 3 && path.Data[1] == ':' &&
      (path.Data[2] == '\\' || path.Data[2] == '/'))
    return true;
  return path.Length >= 2 && (path.Data[0] == '\\' || path.Data[0] == '/') &&
         (path.Data[1] == '\\' || path.Data[1] == '/');
}

static void Exec_AppendWindowsArg(StrBuf* commandLine, StrView arg) {
  if (!Exec_WindowsArgNeedsQuotes(arg)) {
    StrBuf_Append(commandLine, arg);
    return;
  }

  StrBuf_AppendChar(commandLine, '"');
  u32 slashes = 0;
  for (u32 i = 0; i < arg.Length; i++) {
    char c = arg.Data[i];
    if (c == '\\') {
      slashes++;
      continue;
    }
    if (c == '"') {
      for (u32 j = 0; j < slashes * 2u + 1u; j++) StrBuf_AppendChar(commandLine, '\\');
      StrBuf_AppendChar(commandLine, '"');
      slashes = 0;
      continue;
    }
    for (u32 j = 0; j < slashes; j++) StrBuf_AppendChar(commandLine, '\\');
    slashes = 0;
    StrBuf_AppendChar(commandLine, c);
  }
  for (u32 j = 0; j < slashes * 2u; j++) StrBuf_AppendChar(commandLine, '\\');
  StrBuf_AppendChar(commandLine, '"');
}

static bool Exec_ReadWindowsPipe(HANDLE pipe, ExecChunkFn sink, void* user, bool* hadData) {
  char buf[4096];
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) return true;
    DWORD toRead = available > sizeof(buf) ? (DWORD)sizeof(buf) : available;
    DWORD read = 0;
    if (!ReadFile(pipe, buf, toRead, &read, nullptr) || read == 0) return true;
    *hadData = true;
    if (sink && !sink(user, StrView{buf, (u32)read})) return false;
  }
}

ExecArgvResult Exec_RunArgv(Arena* arena, StrView program, const StrView* args, u32 argCount,
                            u32 timeoutMs, volatile i32* abort, ExecArgvSink sink) {
  ExecArgvResult res = {};
  res.ExitCode = -1;
  if (!Exec_IsAbsoluteWindowsPath(program)) return res;

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE outRead = nullptr, outWrite = nullptr;
  HANDLE errRead = nullptr, errWrite = nullptr;
  if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
    if (outRead) CloseHandle(outRead);
    if (outWrite) CloseHandle(outWrite);
    if (errRead) CloseHandle(errRead);
    if (errWrite) CloseHandle(errWrite);
    return res;
  }
  SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

  StrBuf commandLine = StrBuf_Make(arena, program.Length + argCount * 16u + 16u);
  Exec_AppendWindowsArg(&commandLine, program);
  for (u32 i = 0; i < argCount; i++) {
    StrBuf_AppendChar(&commandLine, ' ');
    Exec_AppendWindowsArg(&commandLine, args[i]);
  }

  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (!job) {
    CloseHandle(outRead);
    CloseHandle(outWrite);
    CloseHandle(errRead);
    CloseHandle(errWrite);
    return res;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job);
    CloseHandle(outRead);
    CloseHandle(outWrite);
    CloseHandle(errRead);
    CloseHandle(errWrite);
    return res;
  }

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = outWrite;
  si.hStdError = errWrite;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process = {};
  BOOL ok = CreateProcessA(nullptr, commandLine.Data, nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &process);
  CloseHandle(outWrite);
  CloseHandle(errWrite);
  if (!ok) {
    CloseHandle(job);
    CloseHandle(outRead);
    CloseHandle(errRead);
    return res;
  }
  if (!AssignProcessToJobObject(job, process.hProcess) ||
      ResumeThread(process.hThread) == (DWORD)-1) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(job);
    CloseHandle(outRead);
    CloseHandle(errRead);
    return res;
  }
  res.Spawned = true;

  DWORD start = GetTickCount();
  bool terminated = false;
  for (;;) {
    bool hadData = false;
    if (!Exec_ReadWindowsPipe(outRead, sink.OnStdout, sink.User, &hadData) ||
        !Exec_ReadWindowsPipe(errRead, sink.OnStderr, sink.User, &hadData)) {
      TerminateJobObject(job, 1);
      terminated = true;
    }

    if (!terminated && abort && *abort) {
      TerminateJobObject(job, 1);
      terminated = true;
    } else if (!terminated && timeoutMs && GetTickCount() - start > timeoutMs) {
      res.TimedOut = true;
      TerminateJobObject(job, 1);
      terminated = true;
    }

    if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
      bool ignored = false;
      Exec_ReadWindowsPipe(outRead, sink.OnStdout, sink.User, &ignored);
      Exec_ReadWindowsPipe(errRead, sink.OnStderr, sink.User, &ignored);
      break;
    }
    if (!hadData) Sleep(10);
  }

  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  res.ExitCode = (i32)code;
  CloseHandle(outRead);
  CloseHandle(errRead);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  CloseHandle(job);
  return res;
}

ExecResult Exec_RunStreaming(Arena* arena, StrView command, u32 timeoutMs,
                             volatile i32* abort, ExecChunkFn onData,
                             void* user, StrBuf* output) {
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

  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (!job) {
    CloseHandle(rd);
    CloseHandle(wr);
    res.Output = SV("failed to create job object");
    return res;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job);
    CloseHandle(rd);
    CloseHandle(wr);
    res.Output = SV("failed to configure job object");
    return res;
  }

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pinfo = {};
  BOOL ok = CreateProcessA(nullptr, (LPSTR)StrBuf_CStr(&cmd), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pinfo);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(job);
    CloseHandle(rd);
    res.Output = SV("failed to start process");
    return res;
  }
  if (!AssignProcessToJobObject(job, pinfo.hProcess) || ResumeThread(pinfo.hThread) == (DWORD)-1) {
    TerminateProcess(pinfo.hProcess, 1);
    CloseHandle(pinfo.hProcess);
    CloseHandle(pinfo.hThread);
    CloseHandle(job);
    CloseHandle(rd);
    res.Output = SV("failed to start process in job object");
    return res;
  }
  res.Spawned = true;

  StrBuf localOutput = {};
  if (!output) {
    localOutput = StrBuf_Make(arena, 4096);
    output = &localOutput;
  }
  DWORD start = GetTickCount();
  char buf[4096];
  for (;;) {
    if ((abort && *abort) || (timeoutMs && (GetTickCount() - start) > timeoutMs)) {
      TerminateJobObject(job, 1);
      res.TimedOut = !(abort && *abort);
      break;
    }
    DWORD avail = 0;
    if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD toRead = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
      DWORD n = 0;
      if (ReadFile(rd, buf, toRead, &n, nullptr) && n > 0) {
        StrBuf_Append(output, StrView{buf, (u32)n});
        if (onData && !onData(user, StrView{buf, (u32)n})) {
          TerminateJobObject(job, 1);
          break;
        }
      } else
        break;
    } else if (WaitForSingleObject(pinfo.hProcess, 10) == WAIT_OBJECT_0) {
      for (;;) {
        DWORD a2 = 0;
        if (!(PeekNamedPipe(rd, nullptr, 0, nullptr, &a2, nullptr) && a2 > 0)) break;
        DWORD toRead = a2 > sizeof(buf) ? (DWORD)sizeof(buf) : a2;
        DWORD n = 0;
        if (ReadFile(rd, buf, toRead, &n, nullptr) && n > 0) {
          StrBuf_Append(output, StrView{buf, (u32)n});
          if (onData && !onData(user, StrView{buf, (u32)n})) {
            TerminateJobObject(job, 1);
            break;
          }
        } else
          break;
      }
      break;
    }
  }

  WaitForSingleObject(pinfo.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pinfo.hProcess, &code);
  res.ExitCode = (i32)code;
  CloseHandle(rd);
  CloseHandle(pinfo.hProcess);
  CloseHandle(pinfo.hThread);
  CloseHandle(job);
  res.Output = StrBuf_View(output);
  return res;
}

#else  // POSIX
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char* Exec_DupPosixArg(Arena* arena, StrView arg) {
  char* result = (char*)ArenaAlloc(arena, arg.Length + 1u, 1);
  if (arg.Length) memcpy(result, arg.Data, arg.Length);
  result[arg.Length] = '\0';
  return result;
}

static bool Exec_ReadPosixPipe(int fd, ExecChunkFn sink, void* user, bool* open) {
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (sink && !sink(user, StrView{buf, (u32)n})) return false;
      continue;
    }
    if (n == 0) {
      close(fd);
      *open = false;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      close(fd);
      *open = false;
    }
    return true;
  }
}

ExecArgvResult Exec_RunArgv(Arena* arena, StrView program, const StrView* args, u32 argCount,
                            u32 timeoutMs, volatile i32* abort, ExecArgvSink sink) {
  ExecArgvResult res = {};
  res.ExitCode = -1;

  int outPipe[2];
  int errPipe[2];
  int execPipe[2];
  if (pipe(outPipe) != 0) return res;
  if (pipe(errPipe) != 0) {
    close(outPipe[0]);
    close(outPipe[1]);
    return res;
  }
  if (pipe(execPipe) != 0) {
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);
    return res;
  }
  fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);

  char** argv = ArenaPushArray(arena, char*, argCount + 2u);
  argv[0] = Exec_DupPosixArg(arena, program);
  for (u32 i = 0; i < argCount; i++) argv[i + 1u] = Exec_DupPosixArg(arena, args[i]);
  argv[argCount + 1u] = nullptr;

  pid_t pid = fork();
  if (pid < 0) {
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);
    close(execPipe[0]);
    close(execPipe[1]);
    return res;
  }
  if (pid == 0) {
    close(outPipe[0]);
    close(errPipe[0]);
    close(execPipe[0]);
    setpgid(0, 0);
    dup2(outPipe[1], STDOUT_FILENO);
    dup2(errPipe[1], STDERR_FILENO);
    int nullFd = open("/dev/null", O_RDONLY);
    if (nullFd >= 0) {
      dup2(nullFd, STDIN_FILENO);
      close(nullFd);
    }
    close(outPipe[1]);
    close(errPipe[1]);
    execvp(argv[0], argv);
    int execErr = errno;
    (void)write(execPipe[1], &execErr, sizeof(execErr));
    close(execPipe[1]);
    _exit(127);
  }

  close(outPipe[1]);
  close(errPipe[1]);
  close(execPipe[1]);
  setpgid(pid, pid);

  int execErr = 0;
  ssize_t execRead = read(execPipe[0], &execErr, sizeof(execErr));
  close(execPipe[0]);
  if (execRead > 0) {
    close(outPipe[0]);
    close(errPipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return res;
  }
  res.Spawned = true;

  fcntl(outPipe[0], F_SETFL, fcntl(outPipe[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(errPipe[0], F_SETFL, fcntl(errPipe[0], F_GETFL, 0) | O_NONBLOCK);
  bool outOpen = true;
  bool errOpen = true;
  bool childDone = false;
  bool terminated = false;
  int status = 0;
  u64 start = 0;
  struct timespec t0 = {};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  start = (u64)t0.tv_sec * 1000u + (u64)t0.tv_nsec / 1000000u;

  while (outOpen || errOpen || !childDone) {
    struct pollfd fds[2] = {};
    nfds_t count = 0;
    i32 outIndex = -1;
    i32 errIndex = -1;
    if (outOpen) {
      outIndex = (i32)count;
      fds[count++] = pollfd{outPipe[0], POLLIN | POLLHUP, 0};
    }
    if (errOpen) {
      errIndex = (i32)count;
      fds[count++] = pollfd{errPipe[0], POLLIN | POLLHUP, 0};
    }
    if (count) poll(fds, count, 50);

    bool keepGoing = true;
    if (outIndex >= 0 && fds[outIndex].revents)
      keepGoing = Exec_ReadPosixPipe(outPipe[0], sink.OnStdout, sink.User, &outOpen);
    if (keepGoing && errIndex >= 0 && fds[errIndex].revents)
      keepGoing = Exec_ReadPosixPipe(errPipe[0], sink.OnStderr, sink.User, &errOpen);
    if (!keepGoing && !terminated) {
      kill(-pid, SIGKILL);
      terminated = true;
    }

    if (!terminated && abort && *abort) {
      kill(-pid, SIGKILL);
      terminated = true;
    } else if (!terminated && timeoutMs) {
      struct timespec now = {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      u64 current = (u64)now.tv_sec * 1000u + (u64)now.tv_nsec / 1000000u;
      if (current - start > timeoutMs) {
        res.TimedOut = true;
        kill(-pid, SIGKILL);
        terminated = true;
      }
    }

    if (!childDone) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      childDone = waited == pid;
    }
  }

  if (!childDone) waitpid(pid, &status, 0);
  res.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return res;
}

ExecResult Exec_RunStreaming(Arena* arena, StrView command, u32 timeoutMs,
                             volatile i32* abort, ExecChunkFn onData,
                             void* user, StrBuf* output) {
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
    setpgid(0, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    execl("/bin/sh", "sh", "-c", StrBuf_CStr(&cmd), (char*)nullptr);
    _exit(127);
  }
  close(fds[1]);
  setpgid(pid, pid);
  res.Spawned = true;

  StrBuf localOutput = {};
  if (!output) {
    localOutput = StrBuf_Make(arena, 4096);
    output = &localOutput;
  }
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  char buf[4096];
  for (;;) {
    struct pollfd p = {fds[0], POLLIN, 0};
    int pr = poll(&p, 1, 50);
    if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
      ssize_t n = read(fds[0], buf, sizeof(buf));
      if (n > 0) {
        StrBuf_Append(output, StrView{buf, (u32)n});
        if (onData && !onData(user, StrView{buf, (u32)n})) {
          kill(-pid, SIGKILL);
          break;
        }
      } else if (n == 0)
        break;
    }
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    u64 elapsed = (u64)(t1.tv_sec - t0.tv_sec) * 1000 + (u64)(t1.tv_nsec - t0.tv_nsec) / 1000000;
    if ((abort && *abort) || (timeoutMs && elapsed > timeoutMs)) {
      kill(-pid, SIGKILL);
      res.TimedOut = !(abort && *abort);
      break;
    }
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  res.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  res.Output = StrBuf_View(output);
  return res;
}

#endif

ExecResult Exec_Run(Arena* arena, StrView command, u32 timeoutMs,
                    volatile i32* abort) {
  return Exec_RunStreaming(arena, command, timeoutMs, abort, nullptr, nullptr,
                           nullptr);
}
