#include "base/arena.h"
#include "check.h"
#include "term/linemodel.h"

namespace {

StrView RowText(Arena* a, const SpanLine* row) {
  StrBuf text = StrBuf_Make(a, 64);
  for (u32 i = 0; i < row->Spans.Count; i++) StrBuf_Append(&text, row->Spans.Data[i].Text);
  return StrBuf_View(&text);
}

bool RowHas(Arena* a, const SpanLine* row, const char* needle) {
  return StrView_Contains(RowText(a, row), needle);
}

bool RowStartsWith(Arena* a, const SpanLine* row, const char* prefix) {
  StrView text = RowText(a, row);
  StrView wanted = SV(prefix);
  return text.Length >= wanted.Length &&
         (wanted.Length == 0 || memcmp(text.Data, wanted.Data, wanted.Length) == 0);
}

u16 RowWidth(const SpanLine* row) {
  u16 width = 0;
  for (u32 i = 0; i < row->Spans.Count; i++)
    width = (u16)(width + TextWidth(row->Spans.Data[i].Text));
  return width;
}

bool AnyRowHas(Arena* a, LineModel* model, const char* needle) {
  for (u32 i = 0; i < LineModel_Count(model); i++) {
    if (RowHas(a, LineModel_At(model, i), needle)) return true;
  }
  return false;
}

u32 BlankRows(const LineModel* model) {
  u32 count = 0;
  for (u32 i = 0; i < LineModel_Count(model); i++) {
    if (LineModel_At(model, i)->Spans.Count == 0) count++;
  }
  return count;
}

const SpanLine* FindRow(Arena* a, LineModel* model, const char* needle) {
  for (u32 i = 0; i < LineModel_Count(model); i++) {
    const SpanLine* row = LineModel_At(model, i);
    if (RowHas(a, row, needle)) return row;
  }
  return nullptr;
}

void Finish(LineModel* model, StrView id, StrView result, bool isError = false) {
  CHECK(LineModel_FinishTool(model, id, result, result, isError));
}

}  // namespace

void TestLineModel() {
  Arena a = ArenaCreate();
  LineModel_SetColorEnabled(true);

  {
    LineModel model;
    LineModel_Init(&model, &a, 40, 5000);
    LineModel_AddUser(&model, SVL("hello world"), 0);

    CHECK(model.Entries.Count == 1);
    CHECK(model.Entries.Data[0].Id != 0);
    CHECK(!model.Entries.Data[0].Collapsed);
    CHECK(model.Entries.Data[0].Text.Timestamp.Length == 5);
    CHECK(model.Entries.Data[0].Text.Timestamp.Data[2] == ':');
    CHECK(LineModel_Count(&model) == 1);
    const SpanLine* prompt = LineModel_At(&model, 0);
    CHECK(prompt->HasRight);
    CHECK(prompt->EntryId == model.Entries.Data[0].Id);
    CHECK(prompt->IsEntryStart);
    CHECK(RowHas(&a, prompt, "hello world"));
    CHECK(!AnyRowHas(&a, &model, "you"));

    LineModel_BeginAssistant(&model);
    LineModel_AppendAssistant(&model, SVL("Hello **world**"));
    LineModel_EndAssistant(&model);
    CHECK(model.Entries.Data[1].Id != model.Entries.Data[0].Id);
    CHECK(!model.Entries.Data[1].Collapsed);
    CHECK(AnyRowHas(&a, &model, "\xe2\x96\xa0 "));
    CHECK(AnyRowHas(&a, &model, "Hello world"));
    CHECK(!AnyRowHas(&a, &model, "assistant"));

    LineModel_SetWidth(&model, 10);
    for (u32 i = 0; i < LineModel_Count(&model); i++)
      CHECK(RowWidth(LineModel_At(&model, i)) <= 10);
    CHECK(AnyRowHas(&a, &model, "world"));
    const SpanLine* assistantContinuation = FindRow(&a, &model, "world");
    CHECK(assistantContinuation != nullptr);
    CHECK(RowStartsWith(&a, assistantContinuation, "   "));
    LineModel_Release(&model);
  }

  {
    LineModel collapse;
    LineModel_Init(&collapse, &a, 40, 5000);
    LineModel_AddUser(&collapse, SVL("first line\nsecond line"), 0);
    u64 userId = collapse.Entries.Data[0].Id;
    u32 first = 0;
    u32 count = 0;
    CHECK(LineModel_FindEntryRows(&collapse, userId, &first, &count));
    CHECK(count == 2);
    CHECK(LineModel_At(&collapse, first)->IsEntryStart);
    CHECK(!LineModel_At(&collapse, first + 1)->IsEntryStart);
    CHECK(LineModel_ToggleEntry(&collapse, userId));
    CHECK(LineModel_IsEntryCollapsed(&collapse, userId));
    CHECK(LineModel_FindEntryRows(&collapse, userId, &first, &count));
    CHECK(count == 1);
    CHECK(LineModel_At(&collapse, first)->HasHiddenContent);
    CHECK(LineModel_At(&collapse, first)->HasRight);
    CHECK(LineModel_ToggleEntry(&collapse, userId));
    CHECK(!LineModel_IsEntryCollapsed(&collapse, userId));
    CHECK(LineModel_FindEntryRows(&collapse, userId, &first, &count));
    CHECK(count == 2);

    LineModel_BeginAssistant(&collapse);
    u64 assistantId = collapse.Entries.Data[1].Id;
    LineModel_AppendAssistant(&collapse, SVL("streaming assistant content that wraps"));
    CHECK(LineModel_ToggleEntry(&collapse, assistantId));
    CHECK(LineModel_FindEntryRows(&collapse, assistantId, &first, &count));
    CHECK(count == 1);
    CHECK(LineModel_At(&collapse, first)->HasHiddenContent);
    LineModel_AppendAssistant(&collapse, SVL(" and keeps growing"));
    CHECK(LineModel_FindEntryRows(&collapse, assistantId, &first, &count));
    CHECK(count == 1);
    CHECK(LineModel_ToggleEntry(&collapse, assistantId));
    CHECK(LineModel_FindEntryRows(&collapse, assistantId, &first, &count));
    CHECK(count > 1);
    LineModel_EndAssistant(&collapse);

    LineModel_AddNotice(&collapse, EntryKind::Info, SVL("notice line one\nnotice line two"));
    u64 noticeId = collapse.Entries.Data[2].Id;
    CHECK(!LineModel_IsEntryCollapsed(&collapse, noticeId));
    CHECK(LineModel_ToggleEntry(&collapse, noticeId));
    CHECK(LineModel_FindEntryRows(&collapse, noticeId, &first, &count));
    CHECK(count == 1);
    CHECK(LineModel_At(&collapse, first)->HasHiddenContent);
    LineModel_Release(&collapse);
  }

  {
    LineModel narrow;
    LineModel_Init(&narrow, &a, 7, 5000);
    LineModel_AddUser(&narrow, SVL("long prompt"), 0);
    CHECK(LineModel_Count(&narrow) > 1);
    CHECK(!LineModel_At(&narrow, 0)->HasRight);
    CHECK(RowStartsWith(&a, LineModel_At(&narrow, 0), "\xe2\x96\xa0  "));
    CHECK(RowStartsWith(&a, LineModel_At(&narrow, 1), "   "));
    for (u32 i = 0; i < LineModel_Count(&narrow); i++) {
      CHECK(RowWidth(LineModel_At(&narrow, i)) <= 7);
    }
    LineModel_Release(&narrow);
  }

  {
    LineModel wideGlyph;
    LineModel_Init(&wideGlyph, &a, 3, 5000);
    LineModel_BeginAssistant(&wideGlyph);
    LineModel_AppendAssistant(&wideGlyph, SVL("\xe4\xbd\xa0"));
    LineModel_EndAssistant(&wideGlyph);
    CHECK(AnyRowHas(&a, &wideGlyph, "\xe4\xbd\xa0"));
    for (u32 i = 0; i < LineModel_Count(&wideGlyph); i++)
      CHECK(RowWidth(LineModel_At(&wideGlyph, i)) <= 3);
    LineModel_Release(&wideGlyph);
  }

  {
    LineModel tools;
    LineModel_Init(&tools, &a, 100, 5000);
    LineModel_AddTool(&tools, SVL("bash-id"), SVL("bash"), SVL("{\"command\":\"echo hi\"}"));
    LineModel_AddTool(&tools, SVL("read-id"), SVL("read"),
                      SVL("{\"path\":\"src/main.cpp\",\"offset\":10,\"limit\":5}"));
    CHECK(BlankRows(&tools) == 0);
    CHECK(tools.Entries.Count == 2);
    CHECK(StrView_Eq(tools.Entries.Data[0].Tool.View.Marker, SVL("$")));
    CHECK(tools.Entries.Data[0].Collapsed);
    CHECK(tools.Entries.Data[1].Collapsed);
    CHECK(!tools.Entries.Data[0].Tool.Done);
    CHECK(!tools.Entries.Data[1].Tool.Done);

    Finish(&tools, SVL("bash-id"), SVL("hi"));
    CHECK(tools.Entries.Data[0].Tool.Done);
    CHECK(!tools.Entries.Data[1].Tool.Done);
    Finish(&tools, SVL("read-id"), SVL("alpha-preview\nbeta-preview\ngamma-preview"));
    CHECK(AnyRowHas(&a, &tools, "\xe2\x96\xa0  echo hi"));
    CHECK(AnyRowHas(&a, &tools, "Read src/main.cpp L10:14"));
    CHECK(AnyRowHas(&a, &tools, "3 lines read"));
    CHECK(!AnyRowHas(&a, &tools, "alpha-preview"));
    u64 bashEntryId = tools.Entries.Data[0].Id;
    u64 readEntryId = tools.Entries.Data[1].Id;
    u32 readFirst = 0;
    u32 readRows = 0;
    CHECK(LineModel_FindEntryRows(&tools, readEntryId, &readFirst, &readRows));
    CHECK(readRows == 1);
    CHECK(LineModel_At(&tools, readFirst)->HasHiddenContent);

    CHECK(LineModel_ToggleAllTools(&tools));
    CHECK(!LineModel_IsEntryCollapsed(&tools, bashEntryId));
    CHECK(!LineModel_IsEntryCollapsed(&tools, readEntryId));
    CHECK(AnyRowHas(&a, &tools, "alpha-preview"));
    CHECK(AnyRowHas(&a, &tools, "beta-preview"));
    CHECK(AnyRowHas(&a, &tools, "gamma-preview"));
    const SpanLine* readPreview = FindRow(&a, &tools, "alpha-preview");
    CHECK(readPreview != nullptr);
    CHECK(RowStartsWith(&a, readPreview, "   "));
    CHECK(readPreview->EntryId == readEntryId);
    CHECK(LineModel_ToggleAllTools(&tools));
    CHECK(LineModel_IsEntryCollapsed(&tools, bashEntryId));
    CHECK(LineModel_IsEntryCollapsed(&tools, readEntryId));
    CHECK(!AnyRowHas(&a, &tools, "alpha-preview"));

    CHECK(LineModel_ToggleEntry(&tools, bashEntryId));
    CHECK(!LineModel_IsEntryCollapsed(&tools, bashEntryId));
    CHECK(LineModel_IsEntryCollapsed(&tools, readEntryId));
    CHECK(LineModel_ToggleAllTools(&tools));
    CHECK(!LineModel_IsEntryCollapsed(&tools, bashEntryId));
    CHECK(!LineModel_IsEntryCollapsed(&tools, readEntryId));
    CHECK(LineModel_ToggleAllTools(&tools));

    LineModel_AddTool(
        &tools, SVL("control-id"), SVL("bash"),
        SVL("{\"command\":\"first line\\n\\u001b[31msecond line\\u001b[0m\"}"));
    CHECK(AnyRowHas(&a, &tools, "\xe2\x96\xa0  first line second line"));
    CHECK(!StrView_Contains(StrBuf_View(&tools.Entries.Data[2].Tool.View.Detail), "\n"));
    CHECK(!StrView_Contains(StrBuf_View(&tools.Entries.Data[2].Tool.View.Detail), "\x1b"));
    LineModel_Release(&tools);
  }

  {
    LineModel toolWrap;
    LineModel_Init(&toolWrap, &a, 16, 5000);
    LineModel_AddTool(&toolWrap, SVL("bash"), SVL("bash"),
                      SVL("{\"command\":\"echo this command wraps\"}"));
    u64 bashId = toolWrap.Entries.Data[0].Id;
    CHECK(RowStartsWith(&a, LineModel_At(&toolWrap, 0), "\xe2\x96\xa0  "));
    CHECK(LineModel_Count(&toolWrap) == 1);
    CHECK(LineModel_At(&toolWrap, 0)->HasHiddenContent);
    CHECK(LineModel_ToggleEntry(&toolWrap, bashId));
    CHECK(RowStartsWith(&a, LineModel_At(&toolWrap, 1), "   "));

    LineModel_AddTool(&toolWrap, SVL("read"), SVL("read"),
                      SVL("{\"path\":\"src/very_long_file.cpp\"}"));
    u64 readId = toolWrap.Entries.Data[1].Id;
    CHECK(LineModel_ToggleEntry(&toolWrap, readId));
    const SpanLine* readPath = FindRow(&a, &toolWrap, "src/");
    CHECK(readPath != nullptr);
    CHECK(RowStartsWith(&a, readPath, "   "));
    LineModel_Release(&toolWrap);
  }

  {
    LineModel simpleTurns;
    LineModel_Init(&simpleTurns, &a, 12, 5000);
    LineModel_AddNotice(&simpleTurns, EntryKind::Info, SVL("info message wraps more"));
    CHECK(RowStartsWith(&a, LineModel_At(&simpleTurns, 0), "\xe2\x97\x8f  "));
    CHECK(RowStartsWith(&a, LineModel_At(&simpleTurns, 1), "   "));
    LineModel_AddNotice(&simpleTurns, EntryKind::Error, SVL("error message wraps"));
    const SpanLine* error = FindRow(&a, &simpleTurns, "error");
    CHECK(error != nullptr);
    CHECK(RowStartsWith(&a, error, "!  "));
    LineModel_Release(&simpleTurns);
  }

  {
    LineModel summaries;
    LineModel_Init(&summaries, &a, 120, 5000);
    LineModel_AddTool(&summaries, SVL("grep"), SVL("grep"),
                      SVL("{\"pattern\":\"needle\",\"path\":\"src\",\"glob\":\"*.cpp\"}"));
    Finish(&summaries, SVL("grep"),
           SVL("a.cpp-1-before\na.cpp:2: one\na.cpp-3-after\nb.cpp:4: two"));
    CHECK(AnyRowHas(&a, &summaries, "Search \"needle\" in src (*.cpp)"));
    CHECK(AnyRowHas(&a, &summaries, "2 lines found"));

    LineModel_AddTool(&summaries, SVL("find"), SVL("find"),
                      SVL("{\"pattern\":\"*.h\",\"path\":\"src\"}"));
    Finish(&summaries, SVL("find"), SVL("a.h\nb.h"));
    CHECK(AnyRowHas(&a, &summaries, "2 files found"));

    LineModel_AddTool(&summaries, SVL("ls"), SVL("ls"), SVL("{\"path\":\"src\"}"));
    Finish(&summaries, SVL("ls"), SVL("agent/\nterm/"));
    CHECK(AnyRowHas(&a, &summaries, "2 entries found"));

    LineModel_AddTool(&summaries, SVL("write"), SVL("write"),
                      SVL("{\"path\":\"out.txt\",\"content\":\"abc\"}"));
    Finish(&summaries, SVL("write"), SVL("Successfully wrote 3 bytes to out.txt"));
    CHECK(AnyRowHas(&a, &summaries, "3 bytes written"));

    LineModel_AddTool(
        &summaries, SVL("edit"), SVL("edit"),
        SVL("{\"path\":\"a.cpp\",\"edits\":[{\"oldText\":\"a\",\"newText\":\"b\"},{\"oldText\":\"c\",\"newText\":\"d\"}]}"));
    Finish(&summaries, SVL("edit"), SVL("Successfully replaced 2 blocks"));
    CHECK(AnyRowHas(&a, &summaries, "2 blocks replaced"));

    LineModel_AddTool(&summaries, SVL("single-edit"), SVL("edit"),
                      SVL("{\"path\":\"b.cpp\",\"oldText\":\"a\",\"newText\":\"b\"}"));
    Finish(&summaries, SVL("single-edit"), SVL("Successfully replaced 1 block"));
    CHECK(AnyRowHas(&a, &summaries, "1 block replaced"));

    LineModel_AddTool(&summaries, SVL("bracket-read"), SVL("read"),
                      SVL("{\"path\":\"notes.txt\"}"));
    Finish(&summaries, SVL("bracket-read"), SVL("first\n\n[section]\nlast"));
    CHECK(AnyRowHas(&a, &summaries, "4 lines read"));

    LineModel_AddTool(&summaries, SVL("json-read"), SVL("read"),
                      SVL("{\"path\":\"config.json\"}"));
    CHECK(
        StrView_Eq(summaries.Entries.Data[summaries.Entries.Count - 1].Tool.View.Marker, SVL("J")));
    CHECK(AnyRowHas(&a, &summaries, "\xe2\x96\xa0  Read config.json"));

    LineModel_AddTool(&summaries, SVL("unknown"), SVL("custom"), SVL("{\"value\":1}"));
    Finish(&summaries, SVL("unknown"), SVL("done"));
    CHECK(AnyRowHas(&a, &summaries, "custom"));
    CHECK(AnyRowHas(&a, &summaries, "completed"));

    LineModel_AddTool(&summaries, SVL("error"), SVL("bash"), SVL("{\"command\":\"false\"}"));
    Finish(&summaries, SVL("error"), SVL("Command exited with code 1"), true);
    CHECK(AnyRowHas(&a, &summaries, "failed"));
    const SpanLine* failed = FindRow(&a, &summaries, "failed");
    CHECK(failed != nullptr);
    CHECK(failed->Spans.Data[0].St.Fg == 196);
    LineModel_Release(&summaries);
  }

  {
    LineModel noColor;
    LineModel_SetColorEnabled(false);
    LineModel_Init(&noColor, &a, 40, 5000);
    LineModel_AddUser(&noColor, SVL("plain"), 0);
    CHECK(LineModel_At(&noColor, 0)->Spans.Data[0].St.Fg == COL_DEFAULT);
    LineModel_AddTool(&noColor, SVL("id"), SVL("bash"), SVL("{\"command\":\"echo\"}"));
    Finish(&noColor, SVL("id"), SVL("ok"));
    const SpanLine* row = FindRow(&a, &noColor, "\xe2\x96\xa0  echo");
    CHECK(row != nullptr);
    CHECK(row->Spans.Data[0].St.Fg == COL_DEFAULT);
    LineModel_Release(&noColor);
    LineModel_SetColorEnabled(true);
  }

  {
    LineModel bounded;
    LineModel_Init(&bounded, &a, 100, 50);
    LineModel_AddTool(&bounded, SVL("one"), SVL("read"), SVL("{\"path\":\"one.txt\"}"));
    Finish(&bounded, SVL("one"), SVL("first-preview-that-fits-on-one-row"));
    LineModel_AddTool(&bounded, SVL("two"), SVL("read"), SVL("{\"path\":\"two.txt\"}"));
    Finish(&bounded, SVL("two"), SVL("second-preview-that-fits-on-one-row"));
    CHECK(LineModel_ToggleAllTools(&bounded));
    usize expandedUsed = bounded.RowsMem.Used;
    CHECK(LineModel_Count(&bounded) <= bounded.Cap);
    CHECK(AnyRowHas(&a, &bounded, "first-preview-that-fits-on-one-row"));
    CHECK(AnyRowHas(&a, &bounded, "second-preview-that-fits-on-one-row"));
    CHECK(LineModel_ToggleAllTools(&bounded));
    CHECK(LineModel_ToggleAllTools(&bounded));
    CHECK(bounded.RowsMem.Used == expandedUsed);
    CHECK(LineModel_Count(&bounded) <= bounded.Cap);
    LineModel_Release(&bounded);
  }

  {
    LineModel parseMemory;
    LineModel_Init(&parseMemory, &a, 80, 5000);
    StrBuf args = StrBuf_Make(&a, 5000);
    StrBuf_AppendCStr(&args, "{\"path\":\"large.txt\",\"content\":\"");
    for (u32 i = 0; i < 4096; i++) StrBuf_AppendChar(&args, 'x');
    StrBuf_AppendCStr(&args, "\"}");
    usize before = a.Used;
    LineModel_AddTool(&parseMemory, SVL("id"), SVL("write"), StrBuf_View(&args));
    CHECK(a.Used - before < 2048);
    CHECK(AnyRowHas(&a, &parseMemory, "Write large.txt"));
    LineModel_Release(&parseMemory);
  }

  ArenaRelease(&a);
}
