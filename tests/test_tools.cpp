#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/arena.h"
#include "base/file_io.h"
#include "base/str.h"
#include "base/sysinfo.h"
#include "base/thread.h"
#include "base/win32_utf8.h"
#include "check.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/imagemime.h"
#include "tools/search_regex.h"
#include "tools/shell_capture.h"
#include "tools/tools.h"
#include "tools/truncate.h"
#include "tests.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define CLANKER_RMDIR _rmdir
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#define CLANKER_RMDIR rmdir
#endif

// ---- helpers -------------------------------------------------------------

static void MakeFile(const char* path, const char* content) {
  FILE* f = fopen(path, "wb");
  if (f) {
    fwrite(content, 1, strlen(content), f);
    fclose(f);
  }
}

static StrView Slurp(Arena* a, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return StrView{};
  StrBuf b = StrBuf_Make(a, 256);
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) StrBuf_Append(&b, StrView{buf, (u32)n});
  fclose(f);
  return StrBuf_View(&b);
}

static bool RunTool(const Tool& t, Arena* a, StrView argsJson, ToolResult* out,
                    StrBuf* err, const ToolUpdateSink* updates = nullptr,
                    volatile i32* abort = nullptr) {
  JsonSchema schema = {};
  if (!JsonSchema_Compile(a, t.ParamsJsonSchema, &schema, err)) return false;
  JsonValue* root = Json_Parse(a, argsJson);
  if (!root) {
    StrBuf_AppendCStr(err, "invalid JSON");
    return false;
  }
  if (t.PrepareArguments) {
    JsonValue* prepared = root;
    if (!t.PrepareArguments(t.Ctx, a, root, &prepared, err)) return false;
    root = prepared;
  }
  if (!JsonSchema_Validate(a, &schema, root, err)) return false;
  StrBuf canonical = StrBuf_Make(a, argsJson.Length + 32);
  Json_WriteValue(&canonical, root);
  ToolArgs args = {root, StrBuf_View(&canonical)};
  return t.Execute(t.Ctx, a, SV("id"), &args, abort, updates, out, err);
}

static bool CaptureChunk(void* user, StrView chunk) {
  StrBuf_Append((StrBuf*)user, chunk);
  return true;
}

static StrView Args1(Arena* a, const char* k, const char* v) {
  StrBuf b = StrBuf_Make(a, 64);
  StrBuf_AppendCStr(&b, "{\"");
  StrBuf_AppendCStr(&b, k);
  StrBuf_AppendCStr(&b, "\":\"");
  StrBuf_AppendCStr(&b, v);
  StrBuf_AppendCStr(&b, "\"}");
  return StrBuf_View(&b);
}

static bool DescribeToolPermission(Arena* a, const Tool& tool,
                                   StrView argsJson, StrView workingDirectory,
                                   ToolPermissionInfo* out, StrBuf* err,
                                   StrView writableRoot = {}) {
  JsonValue* root = Json_ParseCopy(a, argsJson);
  if (!root) {
    StrBuf_AppendCStr(err, "invalid JSON");
    return false;
  }
  StrBuf canonical = StrBuf_Make(a, argsJson.Length + 32);
  Json_WriteValue(&canonical, root);
  ToolArgs args = {root, StrBuf_View(&canonical)};
  if (!tool.DescribePermission) return false;
  ToolPermissionScope scope = {};
  scope.WorkingDirectory = workingDirectory;
  scope.WritableRoots = writableRoot.Length ? &writableRoot : nullptr;
  scope.WritableRootCount = writableRoot.Length ? 1 : 0;
  return tool.DescribePermission(tool.Ctx, a, &args, &scope, out, err);
}

static FsPermissionTarget ResolveTarget(Arena* a, StrView workingDirectory,
                                        StrView target, StrView writableRoot = {}) {
  ToolPermissionScope scope = {};
  scope.WorkingDirectory = workingDirectory;
  scope.WritableRoots = writableRoot.Length ? &writableRoot : nullptr;
  scope.WritableRootCount = writableRoot.Length ? 1 : 0;
  return Fs_ResolvePermissionTarget(a, &scope, target);
}

static StrView WriteArgs(Arena* a, StrView path) {
  StrBuf json = StrBuf_Make(a, path.Length + 48);
  StrBuf_AppendCStr(&json, "{\"path\":");
  Json_WriteString(&json, path);
  StrBuf_AppendCStr(&json, ",\"content\":\"x\"}");
  return StrBuf_View(&json);
}

// A writable root widens Auto's allowance to one directory outside the working
// directory, and to nothing beside or above it.
static void TestWritableRootPermissions(Arena* a, const Tool& write,
                                        StrView workingDirectory) {
  Fs_MkdirP(a, SV("clanker_tool_permission_memory/pages"));
  StrView memory = Fs_RealPath(a, SV("clanker_tool_permission_memory"));
  StrView inside = Fs_Join(a, memory, SV("pages/note.md"));
  StrView escape = Fs_Join(a, memory, SV("../escape.txt"));
  StrView sibling = Fs_Join(a, Fs_Parent(memory),
                            SV("clanker_tool_permission_memory-evil/x.md"));

  ToolPermissionInfo info = {};
  StrBuf err = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, write, WriteArgs(a, inside), workingDirectory,
                               &info, &err));
  CHECK(!info.AutoAllow);
  info = {};
  CHECK(DescribeToolPermission(a, write, WriteArgs(a, inside), workingDirectory,
                               &info, &err, memory));
  CHECK(info.AutoAllow);
  info = {};
  CHECK(DescribeToolPermission(a, write, WriteArgs(a, escape), workingDirectory,
                               &info, &err, memory));
  CHECK(!info.AutoAllow);
  info = {};
  CHECK(DescribeToolPermission(a, write, WriteArgs(a, sibling), workingDirectory,
                               &info, &err, memory));
  CHECK(!info.AutoAllow);
  info = {};
  StrView missing = Fs_Join(a, memory, SV("not-created"));
  CHECK(DescribeToolPermission(a, write,
                               WriteArgs(a, Fs_Join(a, missing, SV("x.md"))),
                               workingDirectory, &info, &err, missing));
  CHECK(!info.AutoAllow);

  CLANKER_RMDIR("clanker_tool_permission_memory/pages");
  CLANKER_RMDIR("clanker_tool_permission_memory");
}

static void TestToolPermissions(Arena* a) {
  Tool read = ReadTool();
  Tool ls = LsTool();
  Tool find = FindTool();
  Tool grep = GrepTool();
  Tool ask = AskUserTool();
  CHECK_ALL(read.PermissionEffect == ToolPermissionEffect::Inspection,
            ls.PermissionEffect == ToolPermissionEffect::Inspection,
            find.PermissionEffect == ToolPermissionEffect::Inspection,
            grep.PermissionEffect == ToolPermissionEffect::Inspection,
            ask.PermissionEffect == ToolPermissionEffect::UserInteraction);

  const char* root = "clanker_tool_permission_root";
  Fs_MkdirP(a, SV("clanker_tool_permission_root/sub"));
  StrView workingDirectory = Fs_RealPath(a, SV(root));

  Tool write = WriteTool();
  Tool edit = EditTool();
  CHECK_ALL(write.PermissionEffect == ToolPermissionEffect::Sensitive,
            edit.PermissionEffect == ToolPermissionEffect::Sensitive,
            write.DescribePermission != nullptr,
            edit.DescribePermission != nullptr);

  ToolPermissionInfo inside = {};
  StrBuf insideErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, write, SV("{\"path\":\"sub/../new.txt\",\"content\":\"x\"}"),
      workingDirectory, &inside, &insideErr));
  CHECK_ALL(inside.AutoAllow, inside.Action.Length > 0,
            inside.Action.Length <= 2048, inside.GrantKey.Length > 0,
            inside.GrantKey.Length <= 8192);

  ToolPermissionInfo sameTarget = {};
  StrBuf sameTargetErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, write, SV("{\"path\":\"./new.txt\",\"content\":\"different\"}"),
      workingDirectory, &sameTarget, &sameTargetErr));
  CHECK(StrView_Eq(inside.GrantKey, sameTarget.GrantKey));

  ToolPermissionInfo missingTail = {};
  StrBuf missingTailErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, edit, SV("{\"path\":\"new/dir/file.txt\",\"edits\":[]}"),
      workingDirectory, &missingTail, &missingTailErr));
  CHECK(missingTail.AutoAllow);

  ToolPermissionInfo outside = {};
  StrBuf outsideErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, write, SV("{\"path\":\"../outside.txt\",\"content\":\"x\"}"),
      workingDirectory, &outside, &outsideErr));
  CHECK_ALL(!outside.AutoAllow, outside.ReviewIncomplete);
  TestWritableRootPermissions(a, write, workingDirectory);

  // Writes that steer later execution -- git hooks and config, project hooks,
  // skills -- need review even inside the workspace.
  const char* protectedPaths[] = {
      ".git/config", ".GIT/hooks/pre-commit", "sub/.git/config",
      ".clanker/hooks.json", ".Clanker/settings.json",
      ".agents/skills/x/SKILL.md", ".git",
  };
  for (u32 i = 0; i < (u32)(sizeof(protectedPaths) / sizeof(protectedPaths[0])); i++) {
    FsPermissionTarget target =
        ResolveTarget(a, workingDirectory, SV(protectedPaths[i]));
    CHECK_ALL(target.Contained, target.Protected);
    StrBuf json = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&json, "{\"path\":\"");
    StrBuf_AppendCStr(&json, protectedPaths[i]);
    StrBuf_AppendCStr(&json, "\",\"content\":\"x\"}");
    ToolPermissionInfo info = {};
    StrBuf infoErr = StrBuf_Make(a, 64);
    CHECK(DescribeToolPermission(a, write, StrBuf_View(&json), workingDirectory,
                                 &info, &infoErr));
    CHECK(!info.AutoAllow);
  }
#if defined(_WIN32)
  // Win32 opens each of these as `.git`.
  const char* aliases[] = {".git./config", ".git /config",
                           ".git::$INDEX_ALLOCATION/config"};
  for (u32 i = 0; i < (u32)(sizeof(aliases) / sizeof(aliases[0])); i++) {
    FsPermissionTarget target =
        ResolveTarget(a, workingDirectory, SV(aliases[i]));
    CHECK(!target.Contained || target.Protected);
  }
#endif
  const char* ordinaryPaths[] = {".github/workflows/ci.yml", ".gitignore",
                                 "my.git/x", "agents/x", ".clankerrc"};
  for (u32 i = 0; i < (u32)(sizeof(ordinaryPaths) / sizeof(ordinaryPaths[0])); i++) {
    FsPermissionTarget target =
        ResolveTarget(a, workingDirectory, SV(ordinaryPaths[i]));
    CHECK_ALL(target.Contained, !target.Protected);
  }
  // Only components below the workspace root count.
  StrView gitRoot = Fs_Join(a, workingDirectory, SVL("sub/.git"));
  Fs_MkdirP(a, gitRoot);
  FsPermissionTarget insideGitRoot =
      ResolveTarget(a, Fs_RealPath(a, gitRoot), SVL("file.txt"));
  CHECK_ALL(insideGitRoot.Contained, !insideGitRoot.Protected);
  CLANKER_RMDIR("clanker_tool_permission_root/sub/.git");
  // A writable root inside a protected directory is an explicit grant; only
  // protected components below that root still need review.
  Fs_MkdirP(a, SV("clanker_tool_permission_root/.clanker/memory"));
  StrView grantedRoot =
      Fs_RealPath(a, SV("clanker_tool_permission_root/.clanker/memory"));
  FsPermissionTarget granted = ResolveTarget(
      a, workingDirectory, SVL(".clanker/memory/pages/a.md"), grantedRoot);
  CHECK_ALL(granted.Contained, !granted.Protected);
  FsPermissionTarget grantedGit = ResolveTarget(
      a, workingDirectory, SVL(".clanker/memory/.git/config"), grantedRoot);
  CHECK_ALL(grantedGit.Contained, grantedGit.Protected);
  CLANKER_RMDIR("clanker_tool_permission_root/.clanker/memory");
  CLANKER_RMDIR("clanker_tool_permission_root/.clanker");

  CHECK(Fs_PathContained(workingDirectory, workingDirectory));
  CHECK(Fs_PathContained(workingDirectory,
                         Fs_Join(a, workingDirectory, SV("sub"))));
  CHECK(!Fs_PathContained(workingDirectory,
                          Fs_Join(a, Fs_Parent(workingDirectory),
                                  SV("sibling"))));

  Tool shell = ShellTool();
  ToolPermissionInfo command = {};
  StrBuf commandErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell, SV("{\"command\":\"  git   status  \"}"),
                               workingDirectory, &command, &commandErr));
  CHECK_ALL(command.AutoAllow,
            StrView_Eq(command.Action, SVL("Run git status")),
            StrView_Eq(command.GrantKey, SVL("shell\ngit status")));

  ToolPermissionInfo chain = {};
  StrBuf chainErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, shell, SV("{\"command\":\"git status && git diff\"}"),
      workingDirectory, &chain, &chainErr));
  CHECK(chain.AutoAllow);

  ToolPermissionInfo multiline = {};
  StrBuf multilineErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, shell, SVL("{\"command\":\"git status\\ngit diff\"}"),
      workingDirectory, &multiline, &multilineErr));
  CHECK_ALL(!multiline.AutoAllow, !multiline.ReviewIncomplete,
            StrView_Eq(multiline.Action, SVL("Run git status\ngit diff")));

  const char* reviewerCommands[] = {
      "git status && rm -rf x", "git status | head", "git status > out",
      "git status; git diff", "git status &", "git status $(pwd)",
      "git status 'x'", "git diff --output=changed.txt",
      "git show --ext-diff", "unknown-command", "git branch"};
  for (u32 i = 0; i < (u32)(sizeof(reviewerCommands) / sizeof(reviewerCommands[0]));
       i++) {
    StrBuf json = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&json, "{\"command\":\"");
    StrBuf_AppendCStr(&json, reviewerCommands[i]);
    StrBuf_AppendCStr(&json, "\"}");
    ToolPermissionInfo rejected = {};
    StrBuf rejectedErr = StrBuf_Make(a, 64);
    CHECK(DescribeToolPermission(a, shell, StrBuf_View(&json),
                                 workingDirectory, &rejected, &rejectedErr));
    CHECK(!rejected.AutoAllow);
  }

  ToolPermissionInfo help = {};
  StrBuf helpErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell, SV("{\"command\":\"python --version\"}"),
                               workingDirectory, &help, &helpErr));
  CHECK(help.AutoAllow);

  ToolPermissionInfo gitHelp = {};
  StrBuf gitHelpErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell,
                               SV("{\"command\":\"git --version\"}"),
                               workingDirectory, &gitHelp, &gitHelpErr));
  CHECK(gitHelp.AutoAllow);

  ToolPermissionInfo branch = {};
  StrBuf branchErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, shell, SV("{\"command\":\"git branch --show-current\"}"),
      workingDirectory, &branch, &branchErr));
  CHECK(branch.AutoAllow);

  // The tool hands over the whole command; bounding it for display is the
  // permission engine's job, which can then tell a clipped action apart.
  StrBuf longJson = StrBuf_Make(a, 3100);
  StrBuf_AppendCStr(&longJson, "{\"command\":\"echo ");
  for (u32 i = 0; i < 3000; i++) StrBuf_AppendChar(&longJson, 'a');
  StrBuf_AppendCStr(&longJson, "\"}");
  ToolPermissionInfo longCommand = {};
  StrBuf longErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell, StrBuf_View(&longJson), workingDirectory,
                               &longCommand, &longErr));
  CHECK(longCommand.Action.Length == sizeof("Run echo ") - 1 + 3000);

  CLANKER_RMDIR("clanker_tool_permission_root/sub");
  CLANKER_RMDIR(root);
}

// ---- read ----------------------------------------------------------------

static void TestRead(Arena* a) {
  CHECK(Fs_MkdirP(a, SVL("out")));
  const struct {
    StrView Extension;
    StrView Mime;
  } images[] = {
      {SVL(".png"), SVL("image/png")}, {SVL(".PNG"), SVL("image/png")},
      {SVL(".jpg"), SVL("image/jpeg")}, {SVL(".JpEg"), SVL("image/jpeg")},
      {SVL(".GIF"), SVL("image/gif")}, {SVL(".WeBp"), SVL("image/webp")},
      {SVL(".BMP"), SVL("image/bmp")}, {StrView{}, StrView{}},
      {SVL(".png-extra"), StrView{}}, {SVL(".svg"), StrView{}},
  };
  for (u32 i = 0; i < sizeof(images) / sizeof(images[0]); i++)
    CHECK(StrView_Eq(Img_MimeForExt(images[i].Extension), images[i].Mime));

  const char* path = "out/clanker_tool_read_test.txt";
  MakeFile(path, "line1\nline2\n");

  Tool rt = ReadTool();
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(rt, a, Args1(a, "path", path), &out, &err);
  CHECK_ALL(ok, out.Content.Count == 1,
            StrView_EqCStr(out.Content.Data[0].Text, "line1\nline2"));
  remove(path);

  // missing path + nonexistent file => errors
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  CHECK(!RunTool(rt, a, SV("{}"), &o2, &e2));
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  CHECK(!RunTool(rt, a, SV("{\"path\":\"no_such_file_xyz.txt\"}"), &o3, &e3));

  // POSIX can open a directory as a FILE but must not report a failed read as
  // a successful empty file.
  ToolResult directoryResult = {};
  StrBuf directoryError = StrBuf_Make(a, 64);
  CHECK(!RunTool(rt, a, SVL("{\"path\":\".\"}"), &directoryResult, &directoryError));
  CHECK_ALL(directoryResult.Content.Count == 0, directoryError.Length > 0);

  // offset + limit window with a continuation notice
  const char* p2 = "out/clanker_tool_read_ol.txt";
  MakeFile(p2, "l1\nl2\nl3\nl4\nl5\n");
  ToolResult o4 = {};
  StrBuf e4 = StrBuf_Make(a, 32);
  CHECK(RunTool(rt, a, SV("{\"path\":\"out/clanker_tool_read_ol.txt\",\"offset\":2,\"limit\":2}"), &o4, &e4));
  CHECK_ALL(StrView_Contains(o4.Content.Data[0].Text, "l2"),
            StrView_Contains(o4.Content.Data[0].Text, "l3"),
            !StrView_Contains(o4.Content.Data[0].Text, "l5"),
            StrView_Contains(o4.Content.Data[0].Text, "more lines in file"));
  remove(p2);

  // Sniffed image type wins over the extension.
  const char* pimg = "out/clanker_tool_read.gif";
  unsigned char png[12] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13};
  {
    FILE* f = fopen(pimg, "wb");
    if (f) {
      fwrite(png, 1, sizeof(png), f);
      fclose(f);
    }
  }
  ToolResult o5 = {};
  StrBuf e5 = StrBuf_Make(a, 32);
  CHECK(RunTool(rt, a, Args1(a, "path", pimg), &o5, &e5));
  CHECK_ALL(o5.Content.Count == 2,
            StrView_Contains(o5.Content.Data[0].Text, "image/png"),
            o5.Content.Data[1].Kind == ContentKind::Image,
            StrView_EqCStr(o5.Content.Data[1].ImageMimeType, "image/png"),
            StrView_EqCStr(o5.Content.Data[1].ImageData, "iVBORw0KGgoAAAAN"));
  remove(pimg);
}

static void TestReadStreamCancellation();

static void TestReadTruncation(Arena* a) {
  struct ReadCase {
    u32 LineBytes;
    u32 Lines;
    u32 Offset;
    u32 Limit;
    u32 KeptBytes;
    const char* Notice;
  };
  const ReadCase cases[] = {
      {1, 2001, 1, 0, 4000,
       "\n\n[Showing lines 1-2000 of 2001. Use offset=2001 to continue.]"},
      {1023, 60, 2, 0, 51200,
       "\n\n[Showing lines 2-51 of 60 (50.0KB limit). Use offset=52 to continue.]"},
      {61440, 3, 2, 1, 51200,
       "\n\n[Line 2 is 60.0KB, exceeds 50.0KB limit. Use shell: sed -n '2p' "
       "out/clanker_tool_read_truncation.txt | head -c 51200]"},
  };
  const char* path = "out/clanker_tool_read_truncation.txt";
  for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const ReadCase& test = cases[i];
    StrBuf input = StrBuf_Make(a, (test.LineBytes + 1) * test.Lines + 1);
    for (u32 line = 0; line < test.Lines; line++) {
      for (u32 column = 0; column < test.LineBytes; column++)
        StrBuf_AppendChar(&input, 'x');
      StrBuf_AppendChar(&input, '\n');
    }
    MakeFile(path, input.Data);
    StrBuf args = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&args, "{\"path\":\"out/clanker_tool_read_truncation.txt\",\"offset\":");
    StrBuf_AppendU64(&args, test.Offset);
    StrBuf_AppendCStr(&args, ",\"limit\":");
    StrBuf_AppendU64(&args, test.Limit);
    StrBuf_AppendChar(&args, '}');
    ToolResult out = {};
    StrBuf err = StrBuf_Make(a, 64);
    CHECK(RunTool(ReadTool(), a, StrBuf_View(&args), &out, &err));
    CHECK(out.Content.Count == 1);
    if (out.Content.Count == 1) {
      StrView text = out.Content.Data[0].Text;
      CHECK(text.Length == test.KeptBytes + strlen(test.Notice));
      if (text.Length >= test.KeptBytes) {
        CHECK(StrView_Eq(StrView{text.Data, test.KeptBytes},
                         StrView{input.Data + (test.Offset - 1) * (test.LineBytes + 1),
                                 test.KeptBytes}));
        CHECK(StrView_EqCStr(StrView{text.Data + test.KeptBytes,
                                    text.Length - test.KeptBytes}, test.Notice));
      }
    }
    remove(path);
  }

  auto oracle = [](Arena* memory, StrView input, StrView file, i64 offset,
                   i64 limit, StrBuf* expected) {
    u32 total = Trunc_CountLines(input);
    u64 start = offset > 0 ? (u64)(offset - 1) : 0;
    if (total && start >= total) {
      StrBuf_AppendCStr(expected, "Offset ");
      StrBuf_AppendI64(expected, offset);
      StrBuf_AppendCStr(expected, " is beyond end of file (");
      StrBuf_AppendU64(expected, total);
      StrBuf_AppendCStr(expected, " lines total)");
      return false;
    }
    u64 end = limit > 0 ? ClankerMin(start + (u64)limit, (u64)total) : total;
    auto lineStart = [](StrView content, u64 line) {
      if (!line) return 0u;
      for (u32 i = 0; i < content.Length; i++)
        if (content.Data[i] == '\n' && !--line) return i + 1u;
      return content.Length;
    };
    u32 beginByte = lineStart(input, start);
    u32 endByte = lineStart(input, end);
    if (endByte && input.Data[endByte - 1] == '\n') endByte--;
    if (endByte < beginByte) endByte = beginByte;
    StrView selected{input.Data + beginByte, endByte - beginByte};
    TruncResult tr = Trunc_Head(memory, selected, TRUNC_MAX_LINES, TRUNC_MAX_BYTES);
    StrBuf_Append(expected, tr.Content);
    if (tr.Truncated) {
      u32 first = 0;
      if (tr.ByBytes && tr.OutputLines <= 1)
        while (first < selected.Length && selected.Data[first] != '\n') first++;
      if (expected->Length) StrBuf_AppendCStr(expected, "\n\n");
      if (first > TRUNC_MAX_BYTES) {
        StrBuf_AppendCStr(expected, "[Line ");
        StrBuf_AppendU64(expected, start + 1);
        StrBuf_AppendCStr(expected, " is ");
        Trunc_AppendKb(expected, first);
        StrBuf_AppendCStr(expected, ", exceeds 50.0KB limit. Use shell: sed -n '");
        StrBuf_AppendU64(expected, start + 1);
        StrBuf_AppendCStr(expected, "p' ");
        StrBuf_Append(expected, file);
        StrBuf_AppendCStr(expected, " | head -c 51200]");
      } else {
        StrBuf_AppendCStr(expected, "[Showing lines ");
        StrBuf_AppendU64(expected, start + 1);
        StrBuf_AppendChar(expected, '-');
        StrBuf_AppendU64(expected, start + tr.OutputLines);
        StrBuf_AppendCStr(expected, " of ");
        StrBuf_AppendU64(expected, total);
        if (tr.ByBytes) StrBuf_AppendCStr(expected, " (50.0KB limit)");
        StrBuf_AppendCStr(expected, ". Use offset=");
        StrBuf_AppendU64(expected, start + 1 + tr.OutputLines);
        StrBuf_AppendCStr(expected, " to continue.]");
      }
    } else if (limit > 0 && start + (u64)limit < total) {
      if (expected->Length) StrBuf_AppendCStr(expected, "\n\n");
      StrBuf_AppendChar(expected, '[');
      StrBuf_AppendU64(expected, total - start - (u64)limit);
      StrBuf_AppendCStr(expected, " more lines in file. Use offset=");
      StrBuf_AppendU64(expected, start + (u64)limit + 1);
      StrBuf_AppendCStr(expected, " to continue.]");
    }
    return true;
  };

  const char* boundedPath = "out/clanker_tool_read_bounded.txt";
  auto checkText = [&](StrView input, i64 offset, i64 limit) {
    FILE* file = fopen(boundedPath, "wb");
    CHECK(file != nullptr);
    if (!file) return;
    CHECK(fwrite(input.Data, 1, input.Length, file) == input.Length);
    CHECK(fclose(file) == 0);
    Arena reference = ArenaCreate();
    StrBuf expected = StrBuf_Make(&reference, TRUNC_MAX_BYTES + 512);
    bool expectedOk = oracle(&reference, input, SV(boundedPath), offset, limit, &expected);
    Arena actual = ArenaCreate(256u * 1024u);
    StrBuf args = StrBuf_Make(&actual, 256);
    StrBuf_AppendCStr(&args, "{\"path\":\"out/clanker_tool_read_bounded.txt\",\"offset\":");
    StrBuf_AppendI64(&args, offset);
    StrBuf_AppendCStr(&args, ",\"limit\":");
    StrBuf_AppendI64(&args, limit);
    StrBuf_AppendChar(&args, '}');
    ToolResult output = {};
    StrBuf error = StrBuf_Make(&actual, 256);
    bool ok = RunTool(ReadTool(), &actual, StrBuf_View(&args), &output, &error);
    CHECK(ok == expectedOk);
    if (ok && output.Content.Count == 1)
      CHECK(StrView_Eq(output.Content.Data[0].Text, StrBuf_View(&expected)));
    else if (!ok)
      CHECK(StrView_Eq(StrBuf_View(&error), StrBuf_View(&expected)));
    CHECK(output.Content.Count == (expectedOk ? 1u : 0u));
    CHECK_ALL(actual.Used < 64u * 1024u, actual.Committed <= 64u * 1024u);
    ArenaRelease(&actual);
    ArenaRelease(&reference);
  };
  const StrView edges[] = {
      SVL(""), SVL("\n"), SVL("\n\n"), SVL("x"), SVL("x\n"),
      SVL("x\n\n"), SVL("x\r\n\r\nz\r"), SVL("\r\n"), SVL("x\0y\nz"),
      SVL("\xC3\xA9\n\xF0\x9F\x98\x80\r\nlast")};
  const i64 offsets[] = {-1, 0, 1, 2, 3, 4294967297LL};
  const i64 limits[] = {-1, 0, 1, 2, 4294967297LL};
  for (StrView input : edges)
    for (i64 offset : offsets)
      for (i64 limit : limits) checkText(input, offset, limit);

  Arena fixtures = ArenaCreate();
  char* edge = ArenaPushArray(&fixtures, char, 150000);
  const u32 lengths[] = {51199, 51200, 51201, 65551, 65552, 131089};
  for (u32 length : lengths) {
    memset(edge, 'x', length);
    checkText(StrView{edge, length}, 1, 0);
    edge[length - 1] = '\n';
    checkText(StrView{edge, length}, 1, 0);
    // The byte cut and the 64KiB chunk boundary bisect UTF-8 and CRLF.
    memcpy(edge + 51199, "\xF0\x9F\x98\x80", 4);
    if (length > 65552) memcpy(edge + 65551, "\r\n", 2);
    if (length > 51203) checkText(StrView{edge, length}, 1, 0);
  }
  for (u32 i = 0; i < 12000; i++) edge[i] = i % 3u ? '\n' : 'x';
  checkText(StrView{edge, 12000}, 1, 0);
  checkText(StrView{edge, 12000}, 2000, 2000);
  checkText(StrView{edge, 12000}, 2001, 2001);
  u32 random = 0x8d29f651u;
  for (u32 sample = 0; sample < 32; sample++) {
    u32 length = 66000u + sample * 137u;
    for (u32 i = 0; i < length; i++) {
      random = random * 1664525u + 1013904223u;
      edge[i] = random % 97u == 0 ? '\n' : random % 83u == 0 ? '\r' : 'x';
    }
    checkText(StrView{edge, length}, sample % 5u, sample % 3u ? 0 : 2);
  }
  ArenaRelease(&fixtures);
  CHECK(remove(boundedPath) == 0);

  const char* largePath = "out/clanker_tool_read_large.txt";
  char block[65536];
  auto writeLarge = [&](bool newlines) {
    memset(block, 'x', sizeof(block));
    if (newlines)
      for (u32 i = 1; i < sizeof(block); i += 2) block[i] = '\n';
    FILE* file = fopen(largePath, "wb");
    CHECK(file != nullptr);
    if (!file) return false;
    bool ok = true;
    for (u32 i = 0; i < 1024; i++)
      if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) ok = false;
    if (fclose(file)) ok = false;
    CHECK(ok);
    return ok;
  };
  for (u32 mode = 0; mode < 2; mode++) {
    if (!writeLarge(mode != 0)) continue;
    Arena memory = ArenaCreate(256u * 1024u);
    ToolResult output = {};
    StrBuf error = StrBuf_Make(&memory, 128);
    CHECK(RunTool(ReadTool(), &memory, Args1(&memory, "path", largePath), &output, &error));
    CHECK(output.Content.Count == 1);
    if (output.Content.Count == 1) {
      StrView text = output.Content.Data[0].Text;
      CHECK(text.Length <= TRUNC_MAX_BYTES + 256);
      CHECK(StrView_Contains(text, mode
          ? "[Showing lines 1-2000 of 33554432. Use offset=2001 to continue.]"
          : "[Line 1 is 65536.0KB, exceeds 50.0KB limit."));
    }
    CHECK_ALL(memory.Used < 64u * 1024u, memory.Committed <= 64u * 1024u);
    ArenaRelease(&memory);
  }

  // Pre-cancelled regular files must not allocate their contents.
  auto checkCancellation = [](const char* filePath) {
    Arena memory = ArenaCreate(16u * 1024u * 1024u);
    Tool tool = ReadTool();
    ToolArgs args = {Json_Parse(&memory, Args1(&memory, "path", filePath)), {}};
    volatile i32 abort = 1;
    ToolResult output = {};
    StrBuf error = StrBuf_Make(&memory, 128);
    CHECK(!tool.Execute(tool.Ctx, &memory, SVL("id"), &args, &abort, nullptr,
                        &output, &error));
    CHECK(StrView_Contains(StrBuf_View(&error), "aborted"));
    CHECK_ALL(output.Content.Count == 0, memory.Used < 64u * 1024u);
    ArenaRelease(&memory);
  };
  checkCancellation(largePath);
  CHECK(remove(largePath) == 0);

  const char* imagePath = "out/clanker_tool_read_boundary.png";
  const u32 imageSizes[] = {0, 1, 2, 3, 15, 16, 17, 65535, 65536, 65537,
                           20u * 1024u * 1024u - 1u,
                           20u * 1024u * 1024u,
                           20u * 1024u * 1024u + 1u};
  memset(block, 'x', sizeof(block));
  for (u32 size : imageSizes) {
    FILE* file = fopen(imagePath, "wb");
    CHECK(file != nullptr);
    if (!file) continue;
    for (u32 written = 0; written < size;) {
      u32 count = ClankerMin((u32)sizeof(block), size - written);
      CHECK(fwrite(block, 1, count, file) == count);
      written += count;
    }
    CHECK(fclose(file) == 0);
    Arena memory = ArenaCreate(32u * 1024u * 1024u);
    ToolResult output = {};
    StrBuf error = StrBuf_Make(&memory, 256);
    bool ok = RunTool(ReadTool(), &memory, Args1(&memory, "path", imagePath), &output, &error);
    if (size > 20u * 1024u * 1024u) {
      CHECK_ALL(!ok, output.Content.Count == 0,
                StrView_Contains(StrBuf_View(&error), "20 MiB"),
                memory.Used < 64u * 1024u, memory.Committed <= 64u * 1024u);
    } else {
      CHECK_ALL(ok, output.Content.Count == 2);
      if (ok && output.Content.Count == 2) {
        StrView encoded = output.Content.Data[1].ImageData;
        CHECK(encoded.Length == ((size + 2u) / 3u) * 4u);
        bool same = true;
        for (u32 i = 0; i < size / 3u * 4u; i++)
          if (encoded.Data[i] != "eHh4"[i % 4u]) same = false;
        if (size % 3u)
          same = same && StrView_EqCStr(
              StrView{encoded.Data + encoded.Length - 4u, 4},
              size % 3u == 1 ? "eA==" : "eHg=");
        CHECK(same);
        CHECK(memory.Used < encoded.Length + 4096u);
      }
    }
    ArenaRelease(&memory);
    if (size == 20u * 1024u * 1024u) checkCancellation(imagePath);
  }
  CHECK(remove(imagePath) == 0);
  TestReadStreamCancellation();
}

static void TestReadStreamCancellation() {
  for (u32 image = 0; image < 2; image++) {
    Arena memory = ArenaCreate(16u * 1024u * 1024u);
    StrBuf name = StrBuf_Make(&memory, 128);
#if defined(_WIN32)
    StrBuf_AppendCStr(&name, "\\\\.\\pipe\\clanker-read-cancel-");
#else
    StrBuf_AppendCStr(&name, "out/clanker-read-cancel-");
#endif
    StrBuf_AppendU64(&name, MonotonicMicros());
    StrBuf_AppendU64(&name, image);
#if defined(_WIN32)
    HANDLE pipe = CreateNamedPipeA(StrBuf_CStr(&name),
                                  PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  1, 65536, 65536, 0, nullptr);
    CHECK(pipe != INVALID_HANDLE_VALUE);
    if (pipe == INVALID_HANDLE_VALUE) {
      ArenaRelease(&memory);
      continue;
    }
    OVERLAPPED operation = {};
    operation.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CHECK(operation.hEvent != nullptr);
    if (!operation.hEvent) {
      CloseHandle(pipe);
      ArenaRelease(&memory);
      continue;
    }
#else
    CHECK(mkfifo(StrBuf_CStr(&name), 0600) == 0);
    int pipe = open(StrBuf_CStr(&name), O_RDWR | O_NONBLOCK);
    CHECK(pipe >= 0);
    if (pipe < 0) {
      remove(StrBuf_CStr(&name));
      ArenaRelease(&memory);
      continue;
    }
#endif
    StrBuf json = StrBuf_Make(&memory, 256);
    StrBuf_AppendCStr(&json, "{\"path\":");
    Json_WriteString(&json, StrBuf_View(&name));
    StrBuf_AppendChar(&json, '}');
    struct ReadRequest {
      Arena* Memory;
      ToolArgs Args;
      ToolResult Output;
      StrBuf Error;
      volatile i32 Abort;
      bool Ok;
    } request = {};
    request.Memory = &memory;
    request.Args = {Json_Parse(&memory, StrBuf_View(&json)), {}};
    request.Error = StrBuf_Make(&memory, 256);
    Thread reader = {};
    bool started = Thread_Start(&reader, [](void* user) {
      ReadRequest* request = (ReadRequest*)user;
      Tool tool = ReadTool();
      request->Ok = tool.Execute(tool.Ctx, request->Memory, SVL("id"),
                                 &request->Args, &request->Abort, nullptr,
                                 &request->Output, &request->Error);
    }, &request);
    CHECK(started);
    if (started) {
      char bytes[65552];
      memset(bytes, 'x', sizeof(bytes));
      if (image) memcpy(bytes, "\x89PNG\r\n\x1a\n", 8);
      bool drained = false;
#if defined(_WIN32)
      bool connected = ConnectNamedPipe(pipe, &operation) != FALSE;
      if (!connected) {
        DWORD error = GetLastError();
        connected = error == ERROR_PIPE_CONNECTED;
        if (error == ERROR_IO_PENDING) {
          DWORD transferred = 0;
          connected = WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
                      GetOverlappedResult(pipe, &operation, &transferred, FALSE);
        }
      }
      CHECK(connected);
      if (connected) {
        ResetEvent(operation.hEvent);
        DWORD written = 0;
        bool sent = WriteFile(pipe, bytes, sizeof(bytes), &written, &operation) != FALSE;
        if (!sent && GetLastError() == ERROR_IO_PENDING)
          sent = WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
                 GetOverlappedResult(pipe, &operation, &written, FALSE);
        CHECK(sent && written == sizeof(bytes));
        // A server flush waits for the client to consume all supplied bytes,
        // including the sniff and the first full chunk. It is not a sleep.
        drained = sent && written == sizeof(bytes) && FlushFileBuffers(pipe);
      }
#else
      u32 written = 0;
      u64 deadline = MonotonicMillis() + 5000;
      while (written < sizeof(bytes) && MonotonicMillis() < deadline) {
        ssize_t count = write(pipe, bytes + written, sizeof(bytes) - written);
        if (count > 0) written += (u32)count;
        else if (count < 0 && errno != EAGAIN && errno != EINTR) break;
        else Sleep_Ms(1);
      }
      CHECK(written == sizeof(bytes));
      while (written == sizeof(bytes) && MonotonicMillis() < deadline) {
        int unread = 0;
        if (ioctl(pipe, FIONREAD, &unread) != 0) break;
        if (!unread) {
          drained = true;
          break;
        }
        Sleep_Ms(1);
      }
#endif
      CHECK(drained);
      Atomic_StoreI32(&request.Abort, 1);
    }
#if defined(_WIN32)
    CancelIoEx(pipe, nullptr);
    CloseHandle(pipe);
    CloseHandle(operation.hEvent);
#else
    close(pipe);
#endif
    if (started) {
      Thread_Join(&reader);
      CHECK_ALL(!request.Ok, request.Output.Content.Count == 0,
                StrView_Contains(StrBuf_View(&request.Error), "aborted"),
                memory.Used < 64u * 1024u, memory.Committed <= 64u * 1024u);
    }
#if !defined(_WIN32)
    CHECK(remove(StrBuf_CStr(&name)) == 0);
#endif
    ArenaRelease(&memory);
  }
}

// ---- shell ---------------------------------------------------------------

struct CaptureFileTest {
  u64 Bytes;
  u64 Hash = 14695981039346656037ull;
  u32 Opens;
  u32 Writes;
  u32 Closes;
  bool FailOpen;
  u32 FailWrite;
  bool FailClose;
};

static u64 CaptureHash(u64 hash, StrView bytes) {
  for (u32 i = 0; i < bytes.Length; i++)
    hash = (hash ^ (u8)bytes.Data[i]) * 1099511628211ull;
  return hash;
}

static ShellCaptureIo CaptureTestIo(CaptureFileTest* file) {
  ShellCaptureIo io = {};
  io.User = file;
  io.Open = [](void* user, Arena*, StrView, StrView* path) -> void* {
    CaptureFileTest* file = (CaptureFileTest*)user;
    file->Opens++;
    if (file->FailOpen) return nullptr;
    *path = SVL("injected-shell.log");
    return file;
  };
  io.Write = [](void*, void* handle, StrView bytes) {
    CaptureFileTest* file = (CaptureFileTest*)handle;
    file->Writes++;
    if (file->Writes == file->FailWrite) return false;
    file->Bytes += bytes.Length;
    file->Hash = CaptureHash(file->Hash, bytes);
    return true;
  };
  io.Close = [](void*, void* handle) {
    CaptureFileTest* file = (CaptureFileTest*)handle;
    file->Closes++;
    return !file->FailClose;
  };
  return io;
}

static void TestShellCapture() {
  Arena source = ArenaCreate((usize)1 << 20);
  Arena retained = ArenaCreate((usize)256 << 10);
  Arena snapshot = ArenaCreate((usize)256 << 10);
  Arena oracle = ArenaCreate((usize)1 << 20);
  const u32 lengths[] = {0, 1, 1999, 2000, 2001, 51199, 51200, 51201, 150003};
  const u32 chunks[] = {1, 7, 4096, 65536};
  const StrView patterns[] = {
      SVL("x"), SVL("\n"), SVL("abc\r\n"), SVL("\xF0\x9F\x98\x80"),
      SVL("\nabcdefghijklmnopqrstuvwxyz"), SVL("x\n\n\r"),
  };
  for (StrView pattern : patterns) {
    for (u32 length : lengths) {
      ArenaReset(&source);
      char* bytes = ArenaPushArray(&source, char, length + 1);
      for (u32 i = 0; i < length; i++) bytes[i] = pattern.Data[i % pattern.Length];
      bytes[length] = '\0';
      StrView input = {bytes, length};
      ArenaReset(&oracle);
      TruncResult expected = Trunc_Tail(&oracle, input, TRUNC_MAX_LINES, TRUNC_MAX_BYTES);
      for (u32 chunkSize : chunks) {
        ArenaReset(&retained);
        CaptureFileTest file = {};
        ShellCaptureIo io = CaptureTestIo(&file);
        ShellCapture capture = {};
        ShellCapture_Init(&capture, &retained, {}, &io);
        usize initial = retained.Used;
        bool fed = true;
        for (u32 offset = 0; offset < length;) {
          u32 count = ClankerMin(chunkSize, length - offset);
          fed = ShellCapture_Feed(&capture, StrView{bytes + offset, count}) && fed;
          offset += count;
        }
        CHECK(fed);
        ArenaReset(&snapshot);
        bool byBytes = false;
        StrView actual = ShellCapture_Tail(&snapshot, &capture, &byBytes);
        CHECK_ALL(StrView_Eq(actual, expected.Content), byBytes == expected.ByBytes,
                  capture.TotalBytes == length,
                  ShellCapture_LineCount(&capture) == expected.TotalLines,
                  retained.Used == initial,
                  retained.Used + snapshot.Used <= (256u << 10));
        CHECK(ShellCapture_Finish(&capture));
        CHECK(ShellCapture_Finish(&capture));
        CHECK_ALL(file.Opens == (expected.Truncated ? 1u : 0u),
                  file.Closes == file.Opens,
                  file.Bytes == (expected.Truncated ? length : 0u));
        if (expected.Truncated)
          CHECK(file.Hash == CaptureHash(14695981039346656037ull, input));
      }
    }
  }

  // 100x more output must not grow retained storage, including when previews
  // are rebuilt after every chunk rather than throttled by wall-clock time.
  char block[4096];
  memset(block, 'x', sizeof(block));
  block[sizeof(block) - 1] = '\n';
  ArenaReset(&retained);
  CaptureFileTest file = {};
  ShellCaptureIo io = CaptureTestIo(&file);
  ShellCapture capture = {};
  ShellCapture_Init(&capture, &retained, {}, &io);
  usize initial = retained.Used;
  bool fed = true;
  usize peak = 0;
  for (u32 i = 0; i < 6400; i++) {
    fed = ShellCapture_Feed(&capture, StrView{block, sizeof(block)}) && fed;
    ArenaReset(&snapshot);
    ShellCapture_Tail(&snapshot, &capture);
    usize used = retained.Used + snapshot.Used;
    if (used > peak) peak = used;
  }
  CHECK_ALL(fed, capture.TotalBytes == (u64)6400 * sizeof(block),
            retained.Used == initial, peak <= (256u << 10));
  CHECK(ShellCapture_Finish(&capture));
  CHECK(file.Bytes == capture.TotalBytes);

  ArenaReset(&retained);
  file = CaptureFileTest{};
  io = CaptureTestIo(&file);
  ShellCapture_Init(&capture, &retained, {}, &io);
  CHECK(ShellCapture_Feed(&capture, StrView{(const char*)source.Base, 60000}));
  capture.TotalBytes += (u64)1 << 32;
  capture.Newlines += (u64)1 << 32;
  u64 previousLines = ShellCapture_LineCount(&capture);
  CHECK(ShellCapture_Feed(&capture, SVL("\nx")));
  CHECK_ALL(capture.TotalBytes == ((u64)1 << 32) + 60002,
            ShellCapture_LineCount(&capture) == previousLines + 1);
  CHECK(ShellCapture_Finish(&capture));

  for (u32 failure = 0; failure < 4; failure++) {
    ArenaReset(&retained);
    file = CaptureFileTest{};
    file.FailOpen = failure == 0;
    file.FailWrite = failure == 1 ? 1u : failure == 2 ? 2u : 0u;
    file.FailClose = failure == 3;
    io = CaptureTestIo(&file);
    ShellCapture_Init(&capture, &retained, {}, &io);
    CHECK(ShellCapture_Feed(&capture, SVL("prefix\n")));
    StrView large = {(const char*)source.Base, 60000};
    bool accepted = ShellCapture_Feed(&capture, large);
    CHECK(accepted == (failure == 3));
    CHECK(!ShellCapture_Finish(&capture));
    CHECK_ALL(capture.Error.Length > 0, file.Closes == (failure == 0 ? 0u : 1u),
              (capture.Path.Length > 0) == (failure != 0));
    CHECK(!ShellCapture_Finish(&capture));
    CHECK(file.Closes <= 1);
  }
  ArenaRelease(&oracle);
  ArenaRelease(&snapshot);
  ArenaRelease(&retained);
  ArenaRelease(&source);
}

static StrView ShellTestCommand(Arena* arena, const char* subcommand,
                                const char* argument) {
  StrView exe = Fs_RealPath(arena, SV(g_test_executable));
  StrBuf command = StrBuf_Make(arena, exe.Length + 64);
  StrBuf_AppendChar(&command, '"');
  StrBuf_Append(&command, exe);
  StrBuf_AppendCStr(&command, "\" ");
  StrBuf_AppendCStr(&command, subcommand);
  StrBuf_AppendChar(&command, ' ');
  StrBuf_AppendCStr(&command, argument);
  return StrBuf_View(&command);
}

struct ShellProcessTest {
  Arena Mem;
  ShellCapture Capture;
  ExecResult Result;
  StrView Command;
  StrView Directory;
};

static void RunShellProcessTest(void* user) {
  ShellProcessTest* test = (ShellProcessTest*)user;
  test->Mem = ArenaCreate((usize)256 << 10);
  ShellCapture_Init(&test->Capture, &test->Mem, test->Directory);
  test->Result = Exec_RunStreaming(
      &test->Mem, test->Command, 30000, nullptr,
      [](void* user, StrView chunk) {
        return ShellCapture_Feed((ShellCapture*)user, chunk);
      }, &test->Capture);
  ShellCapture_Finish(&test->Capture);
}

static void TestShellStreamingProcess(Arena* arena) {
  StrBuf directory = StrBuf_Make(arena, 80);
  StrBuf_AppendCStr(&directory, "out/clanker-shell-stream-");
  StrBuf_AppendU64(&directory, MonotonicMillis());
  StrView flood = ShellTestCommand(arena, "--flood-stdout", "4000");
  ShellProcessTest processes[2] = {};
  Thread threads[2] = {};
  bool started[2] = {};
  for (u32 i = 0; i < 2; i++) {
    processes[i].Command = flood;
    processes[i].Directory = StrBuf_View(&directory);
    started[i] = Thread_Start(&threads[i], RunShellProcessTest, &processes[i]);
    CHECK(started[i]);
  }
  for (u32 i = 0; i < 2; i++) {
    if (!started[i]) continue;
    Thread_Join(&threads[i]);
    ShellProcessTest* test = &processes[i];
    CHECK_ALL(test->Result.Spawned, test->Result.ExitCode == 0,
              test->Result.Output.Length == 0, !test->Capture.Error.Length,
              ShellCapture_LineCount(&test->Capture) == 4000,
              test->Mem.Used < (256u << 10), test->Capture.Path.Length > 0);
    FILE* file = fopen(StrView_DupCStr(arena, test->Capture.Path), "rb");
    CHECK(file != nullptr);
    if (file) {
      u64 bytes = 0;
      u64 lines = 0;
      bool valid = true;
      char block[4096];
      size_t count;
      const char* pattern =
#if defined(_WIN32)
          "0123456789012345678901234567890123456789012345678901234567890123\r\n";
#else
          "0123456789012345678901234567890123456789012345678901234567890123\n";
#endif
      u32 patternLength = (u32)strlen(pattern);
      while ((count = fread(block, 1, sizeof(block), file)) != 0) {
        for (u32 j = 0; j < count; j++) {
          valid = valid && block[j] == pattern[(bytes + j) % patternLength];
          if (block[j] == '\n') lines++;
        }
        bytes += count;
      }
      CHECK_ALL(valid, bytes == test->Capture.TotalBytes, lines == 4000,
                ferror(file) == 0);
      CHECK(fclose(file) == 0);
    }
  }
  if (started[0] && started[1])
    CHECK(!StrView_Eq(processes[0].Capture.Path, processes[1].Capture.Path));
  for (u32 i = 0; i < 2; i++) {
    if (!started[i]) continue;
    if (processes[i].Capture.Path.Length)
      CHECK(remove(StrView_DupCStr(arena, processes[i].Capture.Path)) == 0);
    ArenaRelease(&processes[i].Mem);
  }
  if (Fs_IsDir(StrBuf_View(&directory)))
    CHECK(CLANKER_RMDIR(StrBuf_CStr(&directory)) == 0);

  StrBuf arguments = StrBuf_Make(arena, flood.Length + 32);
  StrBuf_AppendCStr(&arguments, "{\"command\":");
  Json_WriteString(&arguments, flood);
  StrBuf_AppendChar(&arguments, '}');
  for (u32 failure = 0; failure < 3; failure++) {
    CaptureFileTest file = {};
    file.FailOpen = failure == 0;
    file.FailWrite = failure == 1 ? 1u : 0u;
    file.FailClose = failure == 2;
    ShellCaptureIo io = CaptureTestIo(&file);
    ShellToolContext context = {};
    context.OutputIo = &io;
    ToolResult result = {};
    StrBuf error = StrBuf_Make(arena, 64);
    CHECK(!RunTool(ShellTool(&context), arena, StrBuf_View(&arguments), &result, &error));
    CHECK_ALL(result.Content.Count == 0,
              StrView_Contains(StrBuf_View(&error), "shell output log"),
              !StrView_Contains(StrBuf_View(&error), "Full output: "),
              StrView_Contains(StrBuf_View(&error), "Incomplete output: injected-shell.log") ==
                  (failure != 0),
              file.Closes == (failure == 0 ? 0u : 1u));
  }

  StrView hang = ShellTestCommand(arena, "--hang", "10000");
  u64 start = MonotonicMillis();
  ExecResult timed = Exec_RunStreaming(arena, hang, 100, nullptr, nullptr, nullptr);
  CHECK_ALL(timed.Spawned, timed.TimedOut, MonotonicMillis() - start < 5000);

  volatile i32 abort = 0;
  Thread canceller = {};
  bool cancelStarted = Thread_Start(&canceller, [](void* user) {
    Sleep_Ms(100);
    Atomic_StoreI32((volatile i32*)user, 1);
  }, (void*)&abort);
  CHECK(cancelStarted);
  if (cancelStarted) {
    start = MonotonicMillis();
    ExecResult cancelled =
        Exec_RunStreaming(arena, hang, 3000, &abort, nullptr, nullptr);
    Thread_Join(&canceller);
    CHECK_ALL(cancelled.Spawned, !cancelled.TimedOut,
              MonotonicMillis() - start < 5000);
  }
  u32 calls = 0;
  ExecResult stopped = Exec_RunStreaming(
      arena, ShellTestCommand(arena, "--flood-stdout", "1000000"),
      5000, nullptr, [](void* user, StrView) {
        (*(u32*)user)++;
        return false;
      }, &calls);
  CHECK_ALL(stopped.Spawned, !stopped.TimedOut, calls == 1);

#if !defined(_WIN32)
  start = MonotonicMillis();
  ExecResult closedPipe = Exec_RunStreaming(
      arena, SVL("exec 1>&- 2>&-; sleep 10"), 100, nullptr, nullptr, nullptr);
  CHECK_ALL(closedPipe.Spawned, closedPipe.TimedOut, MonotonicMillis() - start < 5000);
#endif
}

// The path the truncation notice reports, i.e. everything between
// "Full output: " and the closing bracket.
static StrView ReportedOverflowPath(StrView text) {
  const char* marker = "Full output: ";
  u32 markerLength = (u32)strlen(marker);
  if (text.Length < markerLength) return StrView{};
  for (u32 i = 0; i + markerLength <= text.Length; i++) {
    if (memcmp(text.Data + i, marker, markerLength) != 0) continue;
    u32 start = i + markerLength;
    for (u32 end = start; end < text.Length; end++)
      if (text.Data[end] == ']') return StrView{text.Data + start, end - start};
    return StrView{};
  }
  return StrView{};
}

static void RemoveEmptyDir(const char* path) {
  CLANKER_RMDIR(path);
}

// Output past the truncation limit spills to a file whose path is reported back
// to the model. That file belongs to the session directory when there is one.
static void TestShellOverflow(Arena* a) {
  const char* source = "clanker_tool_shell_big.txt";
  {
    FILE* f = fopen(source, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    // Comfortably past both the 2000-line and 50KB limits.
    for (u32 i = 0; i < 3000; i++) fprintf(f, "line %u padding padding\n", i);
    fclose(f);
  }
#if defined(_WIN32)
  StrView command = SVL("{\"command\":\"type clanker_tool_shell_big.txt\"}");
#else
  StrView command = SVL("{\"command\":\"cat clanker_tool_shell_big.txt\"}");
#endif

  const char* sessionDir = "clanker_tool_shell_session";
  ShellToolContext context = {};
  context.Directory = SV(sessionDir);
  Tool scoped = ShellTool(&context);

  ToolResult small = {};
  StrBuf smallErr = StrBuf_Make(a, 64);
  CHECK(RunTool(scoped, a, SVL("{\"command\":\"echo small\"}"), &small, &smallErr));
  CHECK(!Fs_Exists(SV(sessionDir)));

  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 64);
  CHECK(RunTool(scoped, a, command, &out, &err));
  CHECK(out.Content.Count == 1);
  StrView notice = out.Content.Data[0].Text;
  StrView logPath = ReportedOverflowPath(notice);
  // The directory is created on demand, and the spill lands inside it.
  CHECK_ALL(StrView_Contains(notice, "Showing lines"), logPath.Length > 0,
            StrView_Contains(logPath, sessionDir),
            StrView_Contains(logPath, "shell-"), Fs_IsDir(SV(sessionDir)));

  StrBuf logNative = StrBuf_Make(a, logPath.Length + 1);
  StrBuf_Append(&logNative, logPath);
  StrView spilled = Slurp(a, StrBuf_CStr(&logNative));
  // The spill holds the whole output, not the truncated view.
  CHECK_ALL(spilled.Length > TRUNC_MAX_BYTES,
            StrView_Contains(spilled, "line 0 padding"),
            StrView_Contains(spilled, "line 2999 padding"));
  remove(StrBuf_CStr(&logNative));
  RemoveEmptyDir(sessionDir);
  CHECK(!Fs_Exists(SV(sessionDir)));

  ShellToolContext blocked = {};
  blocked.Directory = SV(source);
  ToolResult failed = {};
  StrBuf failedErr = StrBuf_Make(a, 64);
  CHECK(!RunTool(ShellTool(&blocked), a, command, &failed, &failedErr));
  CHECK_ALL(failed.Content.Count == 0,
            StrView_Contains(StrBuf_View(&failedErr), "failed to create shell output log"),
            !StrView_Contains(StrBuf_View(&failedErr), "Full output: "));

  // With no session directory the tool falls back to the OS temp directory.
  Tool unscoped = ShellTool();
  ToolResult fallback = {};
  StrBuf fallbackErr = StrBuf_Make(a, 64);
  CHECK(RunTool(unscoped, a, command, &fallback, &fallbackErr));
  StrView fallbackPath = ReportedOverflowPath(fallback.Content.Data[0].Text);
  CHECK_ALL(fallbackPath.Length > 0,
            !StrView_Contains(fallbackPath, sessionDir),
            StrView_Contains(fallbackPath, "clanker-shell-"));
  StrBuf fallbackNative = StrBuf_Make(a, fallbackPath.Length + 1);
  StrBuf_Append(&fallbackNative, fallbackPath);
  CHECK(Fs_Exists(StrBuf_View(&fallbackNative)));
  remove(StrBuf_CStr(&fallbackNative));

  remove(source);
}

static void TestShell(Arena* a) {
  TestShellCapture();
  TestShellStreamingProcess(a);
  Tool shell = ShellTool();
  CHECK_ALL(StrView_Eq(shell.Name, SVL("shell")), StrView_Eq(shell.Label, SVL("Shell")));

  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(shell, a, SV("{\"command\":\"echo hello123\"}"), &out, &err);
  CHECK_ALL(ok, out.Content.Count == 1,
            StrView_Contains(out.Content.Data[0].Text, "hello123"),
            !StrView_Contains(out.Content.Data[0].Text, "exit code"));

  struct UpdateCapture {
    StrBuf Last;
    u32 Count;
    bool SawEmpty;
  } capture = {StrBuf_Make(a, 64), 0, false};
  bool accepting = true;
  ToolUpdateSink updates = {};
  updates.User = &capture;
  updates.Accepting = &accepting;
  updates.OnUpdate = [](void* user, const ToolResult* partial) {
    UpdateCapture* capture = (UpdateCapture*)user;
    capture->Count++;
    StrBuf_Clear(&capture->Last);
    StrView text = {};
    if (partial->Content.Count) text = partial->Content.Data[0].Text;
    if (text.Length == 0) capture->SawEmpty = true;
    StrBuf_Append(&capture->Last, text);
  };
  ToolResult streamed = {};
  StrBuf streamedErr = StrBuf_Make(a, 32);
  CHECK(RunTool(shell, a, SV("{\"command\":\"echo stream123\"}"),
                &streamed, &streamedErr, &updates));
  CHECK_ALL(capture.Count >= 2, capture.SawEmpty,
            StrView_Contains(StrBuf_View(&capture.Last), "stream123"));

  // non-zero exit => error result
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(shell, a, SV("{\"command\":\"exit 3\"}"), &o2, &e2);
  CHECK_ALL(!ok2, StrView_Contains(StrBuf_View(&e2), "exited with code 3"));

  StrBuf directOutput = StrBuf_Make(a, 64);
  StrView directArgs[] = {SVL("--echo-arg"), SVL("spaces \" & | stay literal")};
  StrView testExecutable = Fs_RealPath(a, SV(g_test_executable));
  ExecArgvSink directSink = {};
  directSink.User = &directOutput;
  directSink.OnStdout = CaptureChunk;
  ExecArgvResult direct =
      Exec_RunArgv(a, testExecutable, directArgs, 2, 10000, nullptr, directSink);
  CHECK_ALL(direct.Spawned, direct.ExitCode == 0,
            StrView_EqCStr(StrBuf_View(&directOutput), "spaces \" & | stay literal"));
}

// ---- write ---------------------------------------------------------------

static void TestWrite(Arena* a) {
  Tool wt = WriteTool();
  const char* path = "clanker_tool_write_test.txt";
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(wt, a, SV("{\"path\":\"clanker_tool_write_test.txt\",\"content\":\"written-data\"}"),
                    &out, &err);
  CHECK_ALL(ok, StrView_Contains(out.Content.Data[0].Text, "Successfully wrote"),
            StrView_EqCStr(Slurp(a, path), "written-data"));
  remove(path);
}

// ---- edit ----------------------------------------------------------------

static void TestEdit(Arena* a) {
  Tool et = EditTool();

  // exact match
  const char* p1 = "clanker_tool_edit_exact.txt";
  MakeFile(p1, "hello world\nsecond line\n");
  ToolResult o1 = {};
  StrBuf e1 = StrBuf_Make(a, 32);
  bool ok1 = RunTool(
      et, a,
      SV("{\"path\":\"clanker_tool_edit_exact.txt\",\"edits\":[{\"oldText\":\"hello world\",\"newText\":"
         "\"HELLO\"}]}"),
      &o1, &e1);
  CHECK_ALL(ok1, StrView_EqCStr(Slurp(a, p1), "HELLO\nsecond line\n"));
  remove(p1);

  ToolResult missingPath = {};
  StrBuf missingPathErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(
      et, a, SV("{\"oldText\":\"hello\",\"newText\":\"goodbye\"}"),
      &missingPath, &missingPathErr));
  CHECK(StrView_Contains(StrBuf_View(&missingPathErr), "root.path"));

  // fuzzy match (different internal whitespace)
  const char* p2 = "clanker_tool_edit_fuzzy.txt";
  MakeFile(p2, "x = foo   bar\n");
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(
      et, a,
      SV("{\"path\":\"clanker_tool_edit_fuzzy.txt\",\"edits\":[{\"oldText\":\"foo bar\",\"newText\":"
         "\"baz\"}]}"),
      &o2, &e2);
  CHECK_ALL(ok2, StrView_Contains(Slurp(a, p2), "baz"));
  remove(p2);

  // not-found => error
  const char* p3 = "clanker_tool_edit_err.txt";
  MakeFile(p3, "alpha\n");
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  bool ok3 = RunTool(
      et, a,
      SV("{\"path\":\"clanker_tool_edit_err.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"q\"}]}"),
      &o3, &e3);
  CHECK(!ok3);
  remove(p3);
}

// ---- ls / find / grep (share a temp tree) --------------------------------

static void TestTreeTools(Arena* a) {
  Fs_MkdirP(a, SV("clanker_tools_it/sub"));
  MakeFile("clanker_tools_it/a.txt", "alpha\nbeta\ngamma\n");
  MakeFile("clanker_tools_it/b.md", "hello\n");
  MakeFile("clanker_tools_it/sub/c.txt", "deep\n");
  {
    FILE* longFile = fopen("clanker_tools_it/long.txt", "wb");
    if (longFile) {
      for (u32 i = 0; i < 600; i++) fputc('x', longFile);
      fputc('\n', longFile);
      fclose(longFile);
    }
  }

  // ls
  Tool lt = LsTool();
  ToolResult lo = {};
  StrBuf le = StrBuf_Make(a, 32);
  CHECK(RunTool(lt, a, Args1(a, "path", "clanker_tools_it"), &lo, &le));
  CHECK_ALL(StrView_Contains(lo.Content.Data[0].Text, "a.txt"),
            StrView_Contains(lo.Content.Data[0].Text, "b.md"),
            StrView_Contains(lo.Content.Data[0].Text, "sub/"));

  // find *.txt (basename match, recursive)
  Tool ft = FindTool();
  ToolResult fo = {};
  StrBuf fe = StrBuf_Make(a, 32);
  CHECK(RunTool(ft, a, SV("{\"pattern\":\"*.txt\",\"path\":\"clanker_tools_it\"}"), &fo, &fe));
  CHECK_ALL(StrView_Contains(fo.Content.Data[0].Text, "a.txt"),
            StrView_Contains(fo.Content.Data[0].Text, "c.txt"),
            !StrView_Contains(fo.Content.Data[0].Text, "b.md"));

  // Search the actual files, including case folding and context formatting.
  Tool gt = GrepTool();
  ToolResult go = {};
  StrBuf ge = StrBuf_Make(a, 32);
  CHECK(RunTool(
      gt, a,
      SV("{\"pattern\":\"BETA\",\"path\":\"clanker_tools_it\",\"glob\":\"a.txt\","
         "\"ignoreCase\":true,\"literal\":true,\"context\":1}"),
      &go, &ge));
  CHECK(StrView_EqCStr(go.Content.Data[0].Text,
                       "a.txt-1- alpha\na.txt:2: beta\na.txt-3- gamma"));

  ToolResult limited = {};
  StrBuf limitedErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a,
                SV("{\"pattern\":\"alpha|beta|gamma\",\"path\":\"clanker_tools_it/a.txt\","
                   "\"limit\":2}"),
                &limited, &limitedErr));
  CHECK_ALL(StrView_Contains(limited.Content.Data[0].Text, "a.txt:1: alpha"),
            StrView_Contains(limited.Content.Data[0].Text, "a.txt:2: beta"),
            !StrView_Contains(limited.Content.Data[0].Text, "gamma"),
            StrView_Contains(limited.Content.Data[0].Text, "2 matches limit reached"));

  ToolResult longLine = {};
  StrBuf longErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"x+\",\"path\":\"clanker_tools_it/long.txt\"}"),
                &longLine, &longErr));
  CHECK_ALL(StrView_Contains(longLine.Content.Data[0].Text, "... [truncated]"),
            StrView_Contains(longLine.Content.Data[0].Text,
                             "Some lines truncated to 500 chars"));

  ToolResult noMatches = {};
  StrBuf noMatchesErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a,
                SV("{\"pattern\":\"definitely-no-match\",\"path\":\"clanker_tools_it/a.txt\"}"),
                &noMatches, &noMatchesErr));
  CHECK(StrView_EqCStr(noMatches.Content.Data[0].Text, "No matches found"));

  ToolResult invalidRegex = {};
  StrBuf invalidRegexErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(gt, a, SV("{\"pattern\":\"[\",\"path\":\"clanker_tools_it/a.txt\"}"),
                 &invalidRegex, &invalidRegexErr));
  CHECK(StrView_Contains(StrBuf_View(&invalidRegexErr), "regex parse error"));

  const char* invalidGlobs[] = {"[", "{a,b", "{a,{b,c}}", "a}", "a\\"};
  for (const char* glob : invalidGlobs) {
    StrBuf json = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&json, "{\"pattern\":\"alpha\",\"path\":\"clanker_tools_it\",\"glob\":");
    Json_WriteString(&json, SV(glob));
    StrBuf_AppendChar(&json, '}');
    ToolResult invalidGlob = {};
    StrBuf invalidGlobErr = StrBuf_Make(a, 64);
    CHECK(!RunTool(gt, a, StrBuf_View(&json), &invalidGlob, &invalidGlobErr));
    CHECK(StrView_Contains(StrBuf_View(&invalidGlobErr), "glob parse error"));
  }

  ToolResult missingPath = {};
  StrBuf missingPathErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(gt, a, SV("{\"pattern\":\"x\",\"path\":\"clanker_tools_missing\"}"),
                 &missingPath, &missingPathErr));
  CHECK(StrView_Contains(StrBuf_View(&missingPathErr), "Path not found"));

  ToolResult emptyPattern = {};
  StrBuf emptyPatternErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"\",\"path\":\"clanker_tools_it/a.txt\"}"),
                &emptyPattern, &emptyPatternErr));
  CHECK(StrView_Contains(emptyPattern.Content.Data[0].Text, "a.txt:1: alpha"));

  remove("clanker_tools_it/a.txt");
  remove("clanker_tools_it/b.md");
  remove("clanker_tools_it/long.txt");
  remove("clanker_tools_it/sub/c.txt");
  CLANKER_RMDIR("clanker_tools_it/sub");
  CLANKER_RMDIR("clanker_tools_it");
}

// ---- native search ------------------------------------------------------

static StrView RunGrep(Arena* arena, const char* json) {
  ToolResult result = {};
  StrBuf err = StrBuf_Make(arena, 128);
  bool ok = RunTool(GrepTool(), arena, SV(json), &result, &err);
  CHECK(ok);
  if (!ok) fprintf(stderr, "grep: %.*s\n", (int)err.Length, err.Data);
  return result.Content.Count ? result.Content.Data[0].Text : StrView{};
}

static void TestUnicodeGrep(Arena* a) {
  StrView root = SVL("clanker_unicode_grep_it");
  StrView directory = Fs_Join(a, root, SVL("\xe4\xb8\xad\xf0\x9f\x98\x80"));
  CHECK(Fs_MkdirP(a, directory));
  StrView marker = Fs_Join(a, root, SVL(".git"));
  StrView topFile = Fs_Join(a, root, SVL("\xe4\xb8\xad\xf0\x9f\x98\x80.txt"));
  StrView file = Fs_Join(a, directory, SVL("\xc3\xa9.txt"));
  StrView ignored = Fs_Join(a, directory, SVL("\xe6\x96\x87.txt"));
  StrView ignore = Fs_Join(a, directory, SVL(".gitignore"));
  char longName[201];
  memset(longName, 'x', 200);
  longName[200] = 0;
  StrView longDirectory = Fs_Join(a, directory, SV(longName));
  StrView longFile = Fs_Join(a, longDirectory, SVL("deep.txt"));
  CHECK(Fs_MkdirP(a, longDirectory));
  CHECK(File_WriteAll(a, marker, StrView{}) == FileIoResult::Success);
  CHECK(File_WriteAll(a, ignore, SVL("\xe6\x96\x87.txt\n")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, topFile, SVL("needle\n")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, file, SVL("needle\n")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, ignored, SVL("needle\n")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, longFile, SVL("needle\n")) == FileIoResult::Success);
  StrView bytes = {};
  CHECK(File_ReadAll(a, file, &bytes) == FileIoResult::Success);
  CHECK(StrView_EqCStr(bytes, "needle\n"));
  CHECK(ResolveTarget(a, Fs_RealPath(a, root), Fs_RealPath(a, file)).Contained);
  CHECK(ResolveTarget(a, Fs_RealPath(a, root), Fs_RealPath(a, longFile)).Contained);

  StrView result = RunGrep(a, "{\"pattern\":\"needle\",\"path\":\"clanker_unicode_grep_it\"}");
  CHECK_ALL(StrView_Contains(result, "\xe4\xb8\xad\xf0\x9f\x98\x80.txt:1: needle"),
            StrView_Contains(result, "\xe4\xb8\xad\xf0\x9f\x98\x80/\xc3\xa9.txt:1: needle"),
            StrView_Contains(result, "deep.txt:1: needle"),
            !StrView_Contains(result, "\xe6\x96\x87.txt"));
  StrView paths[] = {directory, file};
  for (StrView path : paths) {
    StrBuf json = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&json, "{\"pattern\":\"needle\",\"glob\":\"\xc3\xa9.txt\",\"path\":");
    Json_WriteString(&json, path);
    StrBuf_AppendChar(&json, '}');
    CHECK(StrView_EqCStr(RunGrep(a, json.Data), "\xc3\xa9.txt:1: needle"));
  }

  StrView files[] = {marker, topFile, file, ignored, ignore, longFile};
  for (StrView path : files) {
#if defined(_WIN32)
    wchar_t native[4096];
    CHECK(Win32_Path(path, native, 4096));
    CHECK(_wremove(native) == 0);
#else
    CHECK(remove(StrView_DupCStr(a, path)) == 0);
#endif
  }
  StrView directories[] = {longDirectory, directory};
  for (StrView path : directories) {
#if defined(_WIN32)
    wchar_t native[4096];
    CHECK(Win32_Path(path, native, 4096));
    CHECK(_wrmdir(native) == 0);
#else
    CHECK(rmdir(StrView_DupCStr(a, path)) == 0);
#endif
  }
  CHECK(CLANKER_RMDIR(StrView_DupCStr(a, root)) == 0);
}

static void TestNativeGrep(Arena* a) {
  const char* root = "clanker_native_grep_it";
  CHECK(Fs_MkdirP(a, SVL("clanker_native_grep_it/.git")));
  CHECK(Fs_MkdirP(a, SVL("clanker_native_grep_it/sub")));
  CHECK(Fs_MkdirP(a, SVL("clanker_native_grep_it/ignored-dir")));
  const struct { const char* Name; const char* Text; } files[] = {
      {".gitignore", "ignored.txt\n*.skip\n!keep.skip\nignored-dir/\n"},
      {".ignore", "extra.txt\n"},
      {"sample.txt", "zero\r\nalpha 123\r\nBETA beta\r\ncaf\xc3\xa9 \xc3\x89" "COLE\r\nliteral a+b[0]\r\nlast"},
      {".hidden.txt", "needle\n"},
      {"ignored.txt", "needle\n"},
      {"extra.txt", "needle\n"},
      {"drop.skip", "needle\n"},
      {"keep.skip", "needle\n"},
      {"other.md", "needle\n"},
      {"only.txt", "needle\n"},
      {"sub/.gitignore", "/only.txt\n"},
      {"sub/only.txt", "needle\n"},
      {"sub/drop.skip", "needle\n"},
      {"sub/deep.txt", "needle\n"},
      {"ignored-dir/hidden.txt", "needle\n"},
      {".git/config", "needle\n"},
      {"empty.txt", ""},
      {"blanks.txt", "\n\n"},
  };
  for (const auto& file : files) {
    StrView path = Fs_Join(a, SV(root), SV(file.Name));
    CHECK(File_WriteAll(a, path, SV(file.Text)) == FileIoResult::Success);
  }
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/binary.dat"),
                     SVL("needle\n\0needle")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/utf16.txt"),
                     SVL("\xff\xfe" "n\0e\0e\0d\0l\0e\0\n\0")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/utf16be.txt"),
                     SVL("\xfe\xff" "\0n\0e\0e\0d\0l\0e\0\n")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/bom.txt"),
                     SVL("\xef\xbb\xbf" "needle\n")) == FileIoResult::Success);
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/invalid.txt"),
                     SVL("\xffneedle\n")) == FileIoResult::Success);

  StrView result = RunGrep(a, "{\"pattern\":\"^alpha\\\\s+[0-9]{3}$\",\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_EqCStr(result, "sample.txt:2: alpha 123"));
  result = RunGrep(a, "{\"pattern\":\"(?i)\\\\bbeta\\\\b\",\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_EqCStr(result, "sample.txt:3: BETA beta"));
  result = RunGrep(a, "{\"pattern\":\"a+b[0]\",\"literal\":true,\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_EqCStr(result, "sample.txt:5: literal a+b[0]"));
  result = RunGrep(a, "{\"pattern\":\"a+b[0]\",\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_EqCStr(result, "No matches found"));
  result = RunGrep(a, "{\"pattern\":\"\xc3\xa9" "cole\",\"literal\":true,\"ignoreCase\":true,\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_Contains(result, "sample.txt:4: caf\xc3\xa9 \xc3\x89" "COLE"));
  result = RunGrep(a, "{\"pattern\":\"\\\\p{L}+\",\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_Contains(result, "sample.txt:4: caf\xc3\xa9"));
  result = RunGrep(a, "{\"pattern\":\"^last$\",\"context\":1,\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_EqCStr(result, "sample.txt-5- literal a+b[0]\nsample.txt:6: last"));
  result = RunGrep(a, "{\"pattern\":\"alpha|BETA\",\"context\":1,\"path\":\"clanker_native_grep_it/sample.txt\"}");
  CHECK(StrView_EqCStr(result,
      "sample.txt-1- zero\nsample.txt:2: alpha 123\nsample.txt-3- BETA beta\n"
      "sample.txt-2- alpha 123\nsample.txt:3: BETA beta\nsample.txt-4- caf\xc3\xa9 \xc3\x89" "COLE"));

  result = RunGrep(a, "{\"pattern\":\"needle\",\"path\":\"clanker_native_grep_it\"}");
  CHECK_ALL(StrView_Contains(result, ".hidden.txt:1: needle"),
            StrView_Contains(result, "sub/deep.txt:1: needle"),
            StrView_Contains(result, "only.txt:1: needle"),
            StrView_Contains(result, "keep.skip:1: needle"),
            !StrView_Contains(result, "ignored.txt"),
            !StrView_Contains(result, "ignored-dir"),
            !StrView_Contains(result, "drop.skip"),
            !StrView_Contains(result, "sub/only.txt"),
            !StrView_Contains(result, "extra.txt"),
            !StrView_Contains(result, ".git/config"),
            !StrView_Contains(result, "binary.dat"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"path\":\"clanker_native_grep_it/sub\"}");
  CHECK(StrView_EqCStr(result, "deep.txt:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"glob\":\"**/*.txt\",\"path\":\"clanker_native_grep_it\"}");
  CHECK_ALL(StrView_Contains(result, "only.txt:1: needle"),
            StrView_Contains(result, "sub/deep.txt:1: needle"),
            !StrView_Contains(result, "other.md"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"glob\":\"ignored.txt\",\"path\":\"clanker_native_grep_it\"}");
  CHECK(StrView_EqCStr(result, "ignored.txt:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"glob\":\"!*.txt\",\"path\":\"clanker_native_grep_it\"}");
  CHECK_ALL(StrView_Contains(result, "other.md:1: needle"),
            !StrView_Contains(result, "deep.txt"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"glob\":\"!sub\",\"path\":\"clanker_native_grep_it\"}");
  CHECK_ALL(!StrView_Contains(result, "sub/"), StrView_Contains(result, "other.md:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"glob\":\"{**/*.txt,*.md}\",\"path\":\"clanker_native_grep_it\"}");
  CHECK_ALL(StrView_Contains(result, "only.txt:1: needle"),
            StrView_Contains(result, "sub/deep.txt:1: needle"),
            StrView_Contains(result, "other.md:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"glob\":\"{[o,x]ther.md,only.txt}\",\"path\":\"clanker_native_grep_it\"}");
  CHECK_ALL(StrView_Contains(result, "other.md:1: needle"),
            StrView_Contains(result, "only.txt:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"^needle$\",\"path\":\"clanker_native_grep_it/utf16.txt\"}");
  CHECK(StrView_EqCStr(result, "utf16.txt:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"^needle$\",\"path\":\"clanker_native_grep_it/utf16be.txt\"}");
  CHECK(StrView_EqCStr(result, "utf16be.txt:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"^needle$\",\"path\":\"clanker_native_grep_it/bom.txt\"}");
  CHECK(StrView_EqCStr(result, "bom.txt:1: needle"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"path\":\"clanker_native_grep_it/invalid.txt\"}");
  CHECK(StrView_Contains(result, "invalid.txt:1: \xffneedle"));
  result = RunGrep(a, "{\"pattern\":\"needle\",\"path\":\"clanker_native_grep_it/binary.dat\"}");
  CHECK(StrView_EqCStr(result, "No matches found"));
  result = RunGrep(a, "{\"pattern\":\"\",\"path\":\"clanker_native_grep_it/empty.txt\"}");
  CHECK(StrView_EqCStr(result, "No matches found"));
  result = RunGrep(a, "{\"pattern\":\"\",\"path\":\"clanker_native_grep_it/blanks.txt\"}");
  CHECK(StrView_EqCStr(result, "blanks.txt:1: \nblanks.txt:2: "));

  StrBuf content = StrBuf_Make(a, 20000);
  for (u32 i = 0; i < 16383; i++) StrBuf_AppendChar(&content, 'a');
  StrBuf_AppendCStr(&content, "\xc3\xa9\nneedle");
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/chunks.txt"),
                     StrBuf_View(&content)) == FileIoResult::Success);
  result = RunGrep(a, "{\"pattern\":\"^needle$\",\"path\":\"clanker_native_grep_it/chunks.txt\"}");
  CHECK(StrView_EqCStr(result, "chunks.txt:2: needle"));
  StrBuf_Clear(&content);
  for (u32 i = 0; i < 499; i++) StrBuf_AppendChar(&content, 'x');
  StrBuf_AppendCStr(&content, "\xf0\x9f\x98\x80z\n");
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/wide.txt"),
                     StrBuf_View(&content)) == FileIoResult::Success);
  result = RunGrep(a, "{\"pattern\":\"x\",\"path\":\"clanker_native_grep_it/wide.txt\"}");
  CHECK_ALL(StrView_Contains(result, "\xf0\x9f\x98\x80... [truncated]"),
            !StrView_Contains(result, "\xf0\x9f\x98\x80z"));

  StrBuf_Clear(&content);
  for (u32 i = 0; i < 300; i++) {
    for (u32 j = 0; j < 499; j++) StrBuf_AppendChar(&content, 'x');
    StrBuf_AppendChar(&content, '\n');
  }
  CHECK(File_WriteAll(a, SVL("clanker_native_grep_it/output.txt"),
                     StrBuf_View(&content)) == FileIoResult::Success);
  result = RunGrep(a, "{\"pattern\":\"x\",\"limit\":1000,\"path\":\"clanker_native_grep_it/output.txt\"}");
  CHECK_ALL(result.Length <= TRUNC_MAX_BYTES + 512,
            StrView_Contains(result, "50.0KB limit reached"),
            !StrView_Contains(result, "matches limit reached"));
  result = RunGrep(a, "{\"pattern\":\"x\",\"context\":4294967295,\"path\":\"clanker_native_grep_it/output.txt\"}");
  CHECK(StrView_Contains(result, "50.0KB limit reached"));

  volatile i32 abort = 1;
  ToolResult cancelled = {};
  StrBuf cancelErr = StrBuf_Make(a, 128);
  CHECK(!RunTool(GrepTool(), a,
                 SVL("{\"pattern\":\"x\",\"path\":\"clanker_native_grep_it\"}"),
                 &cancelled, &cancelErr, nullptr, &abort));
  CHECK(StrView_Contains(StrBuf_View(&cancelErr), "Operation aborted"));

  FILE* oversized = fopen("clanker_native_grep_it/oversized.txt", "wb");
  CHECK(oversized != nullptr);
  if (oversized) {
    char block[16384];
    memset(block, 'x', sizeof(block));
    for (u32 i = 0; i < 512; i++) CHECK(fwrite(block, 1, sizeof(block), oversized) == sizeof(block));
    CHECK(fputc('x', oversized) != EOF);
    CHECK(fclose(oversized) == 0);
    ToolResult oversizedResult = {};
    StrBuf oversizedErr = StrBuf_Make(a, 128);
    CHECK(!RunTool(GrepTool(), a,
                   SVL("{\"pattern\":\"x\",\"path\":\"clanker_native_grep_it/oversized.txt\"}"),
                   &oversizedResult, &oversizedErr));
    CHECK(StrView_Contains(StrBuf_View(&oversizedErr), "Search line exceeds 8MiB"));
  }

  Arena regexArena = ArenaCreate();
  SearchRegex regex = {};
  StrBuf regexErr = StrBuf_Make(a, 128);
  bool compiled = SearchRegex_Compile(&regexArena, SVL("^a+$"), false, false,
                                       nullptr, &regex, &regexErr);
  CHECK(compiled);
  if (compiled) {
    bool matched = false;
    CHECK(SearchRegex_Match(&regex, SVL("aaaa"), &matched, &regexErr));
    CHECK(matched);
    usize warm = regexArena.Used;
    for (u32 i = 0; i < 1000; i++) {
      CHECK(SearchRegex_Match(&regex, SVL("aaaa"), &matched, &regexErr));
      CHECK(matched);
    }
    CHECK(regexArena.Used == warm);
  }
  ArenaRelease(&regexArena);
  regexArena = ArenaCreate();
  regex = {};
  StrBuf_Clear(&regexErr);
  compiled = SearchRegex_Compile(&regexArena, SVL("^(a+)+$"), false, false,
                                  nullptr, &regex, &regexErr);
  CHECK(compiled);
  if (compiled) {
    bool matched = false;
    CHECK(!SearchRegex_Match(&regex,
        SVL("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!"),
        &matched, &regexErr));
    CHECK(regexErr.Length > 0);
  }
  ArenaRelease(&regexArena);

  for (const auto& file : files) remove(StrView_DupCStr(a, Fs_Join(a, SV(root), SV(file.Name))));
  const char* extra[] = {"binary.dat", "utf16.txt", "utf16be.txt", "bom.txt",
                         "invalid.txt", "chunks.txt", "wide.txt", "output.txt",
                         "oversized.txt"};
  for (const char* file : extra) remove(StrView_DupCStr(a, Fs_Join(a, SV(root), SV(file))));
  CLANKER_RMDIR("clanker_native_grep_it/.git");
  CLANKER_RMDIR("clanker_native_grep_it/sub");
  CLANKER_RMDIR("clanker_native_grep_it/ignored-dir");
  CLANKER_RMDIR(root);
}

// ---- ask_user ------------------------------------------------------------

struct FakeAsk {
  StrView SeenQuestion;
  u32 SeenChoiceCount;
  u32 Calls;
  AgentAskStatus Reply;
  u32 ReplyIndex;
  StrView ReplyText;
  bool Fail;
};

static bool FakeAskFn(void* user, Arena* arena, const AgentAskRequest* request,
                      AgentAskResponse* out) {
  FakeAsk* fake = (FakeAsk*)user;
  fake->Calls++;
  fake->SeenQuestion = StrView_Dup(arena, request->Question);
  fake->SeenChoiceCount = request->ChoiceCount;
  if (fake->Fail) return false;
  out->Status = fake->Reply;
  out->ChoiceIndex = fake->ReplyIndex;
  out->Answer = StrView_Dup(
      arena, request->ChoiceCount && fake->Reply == AgentAskStatus::Answered
                 ? request->Choices[fake->ReplyIndex]
                 : fake->ReplyText);
  return true;
}

static void TestAskUser(Arena* a) {
  // No seam installed: the tool must report that nobody can answer rather than
  // block or fabricate a reply. This is the `-p` and offline-suite path.
  {
    Tool tool = AskUserTool(nullptr);
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"Ship it?\"}"), &result, &err));
    CHECK(StrView_Contains(StrBuf_View(&err), "no user is available"));
  }

  FakeAsk fake = {};
  AskUserToolContext context = {};
  context.Ask.User = &fake;
  context.Ask.Ask = FakeAskFn;
  Tool tool = AskUserTool(&context);

  // Free-text question: the answer comes back verbatim as the tool result.
  {
    fake.Reply = AgentAskStatus::Answered;
    fake.ReplyText = SVL("use postgres");
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(RunTool(tool, a, SVL("{\"question\":\"Which database?\"}"), &result, &err));
    CHECK_ALL(fake.Calls == 1, fake.SeenChoiceCount == 0,
              StrView_Eq(fake.SeenQuestion, SVL("Which database?")));
    CHECK(result.Content.Count == 1);
    CHECK(StrView_Eq(result.Content.Data[0].Text, SVL("use postgres")));
  }

  // Choices are forwarded in order and the selection is echoed back.
  {
    fake = FakeAsk{};
    fake.Reply = AgentAskStatus::Answered;
    fake.ReplyIndex = 1;
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(RunTool(tool, a,
                  SVL("{\"question\":\"Pick one\",\"choices\":[\"a\",\"b\"]}"),
                  &result, &err));
    CHECK(fake.SeenChoiceCount == 2);
    CHECK(result.Content.Count == 1);
    CHECK(StrView_Eq(result.Content.Data[0].Text, SVL("b")));
  }

  // A dismissal is distinct from an unavailable seam, and both are errors the
  // model can read rather than a crash.
  {
    fake = FakeAsk{};
    fake.Reply = AgentAskStatus::Declined;
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"Ship it?\"}"), &result, &err));
    CHECK(StrView_Contains(StrBuf_View(&err), "dismissed"));

    fake = FakeAsk{};
    fake.Fail = true;
    StrBuf failErr = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"Ship it?\"}"), &result, &failErr));
    CHECK(StrView_Contains(StrBuf_View(&failErr), "no user is available"));
  }

  // Malformed arguments are rejected before anyone is disturbed.
  {
    fake = FakeAsk{};
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"\"}"), &result, &err));
    StrBuf err2 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"q\",\"choices\":[\"a\",\"\"]}"),
                   &result, &err2));
    StrBuf err3 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"q\",\"choices\":\"a\"}"), &result,
                   &err3));
    StrBuf err4 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"choices\":[\"a\"]}"), &result, &err4));
    StrBuf err5 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a,
                   SVL("{\"permission_request_id\":\"pending\",\"question\":\"q\"}"),
                   &result, &err5));
    CHECK(fake.Calls == 0);
  }

  // A `oneOf` parameter schema invites models to fill the unused branch with
  // explicit nulls. A null key is not an argument, so neither form may be
  // rejected for carrying one.
  {
    fake = FakeAsk{};
    fake.Reply = AgentAskStatus::Answered;
    fake.ReplyText = SVL("sqlite");
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(RunTool(tool, a,
                  SVL("{\"question\":\"Which database?\",\"choices\":null,"
                      "\"permission_request_id\":null}"),
                  &result, &err));
    CHECK_ALL(fake.Calls == 1, fake.SeenChoiceCount == 0,
              StrView_Eq(fake.SeenQuestion, SVL("Which database?")));
    CHECK(result.Content.Count == 1);
    CHECK(StrView_Eq(result.Content.Data[0].Text, SVL("sqlite")));

    // The permission branch reaches the engine rather than tripping the
    // mutual-exclusion check on its own null siblings.
    fake = FakeAsk{};
    StrBuf idErr = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a,
                   SVL("{\"permission_request_id\":\"abc\",\"question\":null,"
                       "\"choices\":null}"),
                   &result, &idErr));
    CHECK(StrView_Contains(StrBuf_View(&idErr), "no permission engine"));
    CHECK(fake.Calls == 0);
  }

  // A batch must not put two questions on screen at once.
  CHECK(tool.ExecMode == ToolExecMode::Sequential);
}

void TestTools() {
  Arena a = ArenaCreate();
  TestToolPermissions(&a);
  TestRead(&a);
  TestReadTruncation(&a);
  TestShell(&a);
  TestShellOverflow(&a);
  TestWrite(&a);
  TestEdit(&a);
  TestTreeTools(&a);
  TestNativeGrep(&a);
  TestUnicodeGrep(&a);
  TestAskUser(&a);
  ArenaRelease(&a);
}
