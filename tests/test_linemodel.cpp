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

bool TextEq(StrView actual, StrView expected) { return StrView_Eq(actual, expected); }

StrView ExtractRow(Arena* a, const SpanLine* row, u16 columns, u16 anchor, u16 focus) {
  DisplayRowLayout layout = LineModel_LayoutDisplayRow(row, columns);
  return LineModel_ExtractDisplayRow(a, &layout, anchor, focus);
}

u16 RowWidth(const SpanLine* row) {
  u16 width = 0;
  for (u32 i = 0; i < row->Spans.Count; i++)
    width = (u16)(width + TextWidth(row->Spans.Data[i].Text));
  return width;
}

bool AllRowsFit(const LineModel* model, u16 width) {
  for (u32 i = 0; i < LineModel_Count(model); i++) {
    if (RowWidth(LineModel_At(model, i)) > width) return false;
  }
  return true;
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

bool Finish(LineModel* model, StrView id, StrView result, bool isError = false) {
  return LineModel_FinishTool(model, id, result, result, isError);
}

}  // namespace

void TestLineModel() {
  Arena a = ArenaCreate();
  LineModel_SetColorEnabled(true);

  {
    LineModel model;
    LineModel_Init(&model, &a, 40, 5000);
    LineModel_AddUser(&model, SVL("hello world"), 0);

    CHECK_ALL(model.Entries.Count == 1, model.Entries.Data[0].Id != 0,
              !model.Entries.Data[0].Collapsed,
              model.Entries.Data[0].Text.Timestamp.Length == 5,
              model.Entries.Data[0].Text.Timestamp.Data[2] == ':',
              LineModel_Count(&model) == 1);
    const SpanLine* prompt = LineModel_At(&model, 0);
    CHECK_ALL(prompt->HasRight, prompt->EntryId == model.Entries.Data[0].Id,
              prompt->IsEntryStart, RowHas(&a, prompt, "hello world"),
              !AnyRowHas(&a, &model, "you"));

    LineModel_BeginAssistant(&model);
    LineModel_AppendAssistant(&model, SVL("Hello **world**"));
    LineModel_EndAssistant(&model);
    CHECK_ALL(model.Entries.Data[1].Id != model.Entries.Data[0].Id,
              !model.Entries.Data[1].Collapsed, AnyRowHas(&a, &model, "\xe2\x96\xa0 "),
              AnyRowHas(&a, &model, "Hello world"),
              !AnyRowHas(&a, &model, "assistant"));

    LineModel_SetWidth(&model, 10);
    CHECK(AnyRowHas(&a, &model, "world"));
    const SpanLine* assistantContinuation = FindRow(&a, &model, "world");
    const SpanLine* assistantFirst = FindRow(&a, &model, "Hello");
    CHECK(assistantFirst != nullptr && assistantContinuation != nullptr);
    CHECK_ALL(AllRowsFit(&model, 10), RowStartsWith(&a, assistantContinuation, "   "),
              TextEq(ExtractRow(&a, assistantFirst, 10, 0, 8), SVL("\xe2\x96\xa0  Hello")),
              TextEq(ExtractRow(&a, assistantContinuation, 10, 0, 8), SVL("   world")));
    LineModel_Release(&model);
  }

  {
    LineModel visual;
    LineModel_Init(&visual, &a, 20, 5000);
    LineModel_AddUser(&visual, SVL("hi"), 0);
    const SpanLine* row = LineModel_At(&visual, 0);
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(row, 20);
    CHECK_ALL(layout.RightColumn == 15, layout.ContentEnd == 20);

    StrBuf expected = StrBuf_Make(&a, 32);
    StrBuf_AppendCStr(&expected, "\xe2\x96\xa0  hi          ");
    StrBuf_Append(&expected, row->Right.Text);
    StrBuf partial = StrBuf_Make(&a, 24);
    StrBuf_AppendCStr(&partial, "hi          ");
    StrBuf_Append(&partial, StrView{row->Right.Text.Data, 2});
    CHECK_ALL(
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, layout.ContentEnd),
               StrBuf_View(&expected)),
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 3, 17), StrBuf_View(&partial)),
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 17, 3), StrBuf_View(&partial)));
    LineModel_Release(&visual);
  }

  {
    LineModel hidden;
    LineModel_Init(&hidden, &a, 20, 5000);
    LineModel_AddUser(&hidden, SVL("first line\nsecond line"), 0);
    CHECK(LineModel_ToggleEntry(&hidden, hidden.Entries.Data[0].Id));
    const SpanLine* row = LineModel_At(&hidden, 0);
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(row, 20);
    CHECK_ALL(layout.HiddenColumn == 13, layout.RightColumn == 15);

    StrBuf expected = StrBuf_Make(&a, 32);
    StrBuf_AppendCStr(&expected, "\xe2\x96\xa0  first line\xe2\x80\xa6 ");
    StrBuf_Append(&expected, row->Right.Text);
    CHECK(TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 20), StrBuf_View(&expected)));
    LineModel_Release(&hidden);
  }

  {
    SpanLine blanks = {};
    blanks.Spans = Array_Make<Span>(&a, 2);
    Array_Push(&blanks.Spans, Span{SVL("a b  "), StyleDefault()});
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(&blanks, 8);
    CHECK_ALL(layout.ContentEnd == 5,
              TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 5), SVL("a b  ")),
              TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 8), SVL("a b     ")),
              TextEq(LineModel_ExtractDisplayRow(&a, &layout, 1, 7), SVL(" b    ")));
  }

  {
    SpanLine clipped = {};
    clipped.Spans = Array_Make<Span>(&a, 1);
    Array_Push(&clipped.Spans, Span{SVL("abcdef"), StyleDefault()});
    clipped.Right = Span{SVL("R"), StyleDefault()};
    clipped.HasRight = true;
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(&clipped, 5);
    CHECK_ALL(layout.LeftLimit == 3,
              TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 5), SVL("abc R")));

    clipped.HasHiddenContent = true;
    layout = LineModel_LayoutDisplayRow(&clipped, 5);
    CHECK_ALL(layout.LeftLimit == 2, layout.HiddenColumn == 2,
              TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 5),
                     SVL("ab\xe2\x80\xa6 R")));
  }

  {
    SpanLine wide = {};
    wide.Spans = Array_Make<Span>(&a, 1);
    Array_Push(&wide.Spans, Span{SVL("A\xe4\xbd\xa0" "B"), StyleDefault()});
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(&wide, 6);
    CHECK(layout.ContentEnd == 4);
    DisplayRowRange middle = LineModel_NormalizeDisplayRowRange(&layout, 2, 3);
    DisplayRowRange reverse = LineModel_NormalizeDisplayRowRange(&layout, 4, 2);
    CHECK_ALL(
        middle.Start == 1 && middle.End == 3, reverse.Start == 1 && reverse.End == 4,
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 2, 3), SVL("\xe4\xbd\xa0")),
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 2), SVL("A\xe4\xbd\xa0")),
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 2, 4), SVL("\xe4\xbd\xa0" "B")),
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 4, 2), SVL("\xe4\xbd\xa0" "B")),
        TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 6), SVL("A\xe4\xbd\xa0" "B  ")));
  }

  {
    LineModel combining;
    LineModel_Init(&combining, &a, 6, 5000);
    LineModel_BeginAssistant(&combining);
    LineModel_AppendAssistant(&combining, SVL("a\xcc\x81" "bc"));
    LineModel_EndAssistant(&combining);
    CHECK_ALL(
        LineModel_Count(&combining) == 3,
        TextEq(ExtractRow(&a, LineModel_At(&combining, 0), 6, 0, 3),
               SVL("\xe2\x96\xa0  ")),
        TextEq(ExtractRow(&a, LineModel_At(&combining, 1), 6, 0, 6),
               SVL("   a\xcc\x81" "b")),
        TextEq(ExtractRow(&a, LineModel_At(&combining, 1), 6, 4, 5), SVL("\xcc\x81")),
        TextEq(ExtractRow(&a, LineModel_At(&combining, 2), 6, 0, 4), SVL("   c")));
    LineModel_Release(&combining);
  }

  {
    SpanLine withRight = {};
    withRight.Spans = Array_Make<Span>(&a, 1);
    Array_Push(&withRight.Spans, Span{SVL("abcd"), StyleDefault()});
    withRight.Right = Span{SVL("R\xcc\x81"), StyleDefault()};
    withRight.HasRight = true;

    DisplayRowLayout layout = LineModel_LayoutDisplayRow(&withRight, 6);
    CHECK_ALL(layout.LeftLimit == 3, layout.RightColumn == 4, layout.ContentEnd == 6,
              TextEq(LineModel_ExtractDisplayRow(&a, &layout, 0, 6),
                     SVL("abc R\xcc\x81")));
  }

  {
    LineModel revision;
    LineModel_Init(&revision, &a, 20, 5000);
    u64 rev = LineModel_DisplayRevision(&revision);
    bool stable = rev != 0;
    LineModel_BeginAssistant(&revision);
    stable = stable && LineModel_DisplayRevision(&revision) == rev;
    LineModel_AppendAssistant(&revision, StrView{});
    stable = stable && LineModel_DisplayRevision(&revision) == rev;
    LineModel_AppendAssistant(&revision, SVL("streaming content"));
    stable = stable && LineModel_DisplayRevision(&revision) == rev;
    LineModel_AppendAssistant(&revision, SVL(" more"));
    stable = stable && LineModel_DisplayRevision(&revision) == rev;
    LineModel_EndAssistant(&revision);
    stable = stable && LineModel_DisplayRevision(&revision) == rev;
    CHECK(stable);

    LineModel_SetWidth(&revision, 8);
    CHECK(LineModel_DisplayRevision(&revision) != rev);
    rev = LineModel_DisplayRevision(&revision);
    LineModel_SetWidth(&revision, 8);
    CHECK(LineModel_DisplayRevision(&revision) == rev);
    LineModel_Release(&revision);

    LineModel cropped;
    LineModel_Init(&cropped, &a, 8, 2);
    LineModel_BeginAssistant(&cropped);
    LineModel_AppendAssistant(&cropped, SVL("one"));
    rev = LineModel_DisplayRevision(&cropped);
    LineModel_AppendAssistant(&cropped, SVL("\ntwo\nthree"));
    CHECK(LineModel_DisplayRevision(&cropped) != rev);
    LineModel_Release(&cropped);

    LineModel cappedStable;
    LineModel_Init(&cappedStable, &a, 20, 2);
    LineModel_BeginAssistant(&cappedStable);
    LineModel_AppendAssistant(&cappedStable, SVL("one\ntwo\nthree"));
    rev = LineModel_DisplayRevision(&cappedStable);
    LineModel_AppendAssistant(&cappedStable, SVL("x"));
    CHECK(LineModel_DisplayRevision(&cappedStable) == rev);
    LineModel_Release(&cappedStable);

    LineModel cappedShift;
    LineModel_Init(&cappedShift, &a, 20, 2);
    LineModel_BeginAssistant(&cappedShift);
    LineModel_AppendAssistant(&cappedShift, SVL("x\nx\nx"));
    rev = LineModel_DisplayRevision(&cappedShift);
    LineModel_AppendAssistant(&cappedShift, SVL("\nx"));
    CHECK(LineModel_DisplayRevision(&cappedShift) != rev);
    LineModel_Release(&cappedShift);

    LineModel reflow;
    LineModel_Init(&reflow, &a, 20, 5000);
    LineModel_BeginAssistant(&reflow);
    LineModel_AppendAssistant(&reflow, SVL("abcdefghijklmnopq"));
    rev = LineModel_DisplayRevision(&reflow);
    LineModel_AppendAssistant(&reflow, SVL("r"));
    CHECK(LineModel_DisplayRevision(&reflow) != rev);
    LineModel_Release(&reflow);
  }

  {
    LineModel collapse;
    LineModel_Init(&collapse, &a, 40, 5000);
    LineModel_AddUser(&collapse, SVL("first line\nsecond line"), 0);
    u64 userId = collapse.Entries.Data[0].Id;
    u32 first = 0;
    u32 count = 0;
    CHECK(LineModel_FindEntryRows(&collapse, userId, &first, &count));
    CHECK_ALL(count == 2, LineModel_At(&collapse, first)->IsEntryStart,
              !LineModel_At(&collapse, first + 1)->IsEntryStart);
    CHECK(LineModel_ToggleEntry(&collapse, userId));
    bool found = LineModel_FindEntryRows(&collapse, userId, &first, &count);
    CHECK_ALL(LineModel_IsEntryCollapsed(&collapse, userId), found, count == 1,
              LineModel_At(&collapse, first)->HasHiddenContent,
              LineModel_At(&collapse, first)->HasRight);
    CHECK(LineModel_ToggleEntry(&collapse, userId));
    found = LineModel_FindEntryRows(&collapse, userId, &first, &count);
    CHECK_ALL(!LineModel_IsEntryCollapsed(&collapse, userId), found, count == 2);

    LineModel_BeginAssistant(&collapse);
    u64 assistantId = collapse.Entries.Data[1].Id;
    LineModel_AppendAssistant(&collapse, SVL("streaming assistant content that wraps"));
    CHECK(LineModel_ToggleEntry(&collapse, assistantId));
    found = LineModel_FindEntryRows(&collapse, assistantId, &first, &count);
    CHECK_ALL(found, count == 1, LineModel_At(&collapse, first)->HasHiddenContent);
    LineModel_AppendAssistant(&collapse, SVL(" and keeps growing"));
    found = LineModel_FindEntryRows(&collapse, assistantId, &first, &count);
    CHECK_ALL(found, count == 1);
    CHECK(LineModel_ToggleEntry(&collapse, assistantId));
    found = LineModel_FindEntryRows(&collapse, assistantId, &first, &count);
    CHECK_ALL(found, count > 1);
    LineModel_EndAssistant(&collapse);

    LineModel_AddNotice(&collapse, EntryKind::Info, SVL("notice line one\nnotice line two"));
    u64 noticeId = collapse.Entries.Data[2].Id;
    CHECK(!LineModel_IsEntryCollapsed(&collapse, noticeId));
    CHECK(LineModel_ToggleEntry(&collapse, noticeId));
    found = LineModel_FindEntryRows(&collapse, noticeId, &first, &count);
    CHECK_ALL(found, count == 1, LineModel_At(&collapse, first)->HasHiddenContent);
    LineModel_Release(&collapse);
  }

  {
    LineModel narrow;
    LineModel_Init(&narrow, &a, 7, 5000);
    LineModel_AddUser(&narrow, SVL("long prompt"), 0);
    CHECK_ALL(LineModel_Count(&narrow) > 1, !LineModel_At(&narrow, 0)->HasRight,
              RowStartsWith(&a, LineModel_At(&narrow, 0), "\xe2\x96\xa0  "),
              RowStartsWith(&a, LineModel_At(&narrow, 1), "   "), AllRowsFit(&narrow, 7));
    LineModel_Release(&narrow);
  }

  {
    LineModel wideGlyph;
    LineModel_Init(&wideGlyph, &a, 3, 5000);
    LineModel_BeginAssistant(&wideGlyph);
    LineModel_AppendAssistant(&wideGlyph, SVL("\xe4\xbd\xa0"));
    LineModel_EndAssistant(&wideGlyph);
    CHECK_ALL(AnyRowHas(&a, &wideGlyph, "\xe4\xbd\xa0"), AllRowsFit(&wideGlyph, 3));
    LineModel_Release(&wideGlyph);
  }

  {
    LineModel tools;
    LineModel_Init(&tools, &a, 100, 5000);
    LineModel_AddTool(&tools, SVL("bash-id"), SVL("bash"), SVL("{\"command\":\"echo hi\"}"));
    LineModel_AddTool(&tools, SVL("read-id"), SVL("read"),
                      SVL("{\"path\":\"src/main.cpp\",\"offset\":10,\"limit\":5}"));
    CHECK_ALL(BlankRows(&tools) == 0, tools.Entries.Count == 2,
              StrView_Eq(tools.Entries.Data[0].Tool.View.Marker, SVL("$")),
              tools.Entries.Data[0].Collapsed, tools.Entries.Data[1].Collapsed,
              !tools.Entries.Data[0].Tool.Done, !tools.Entries.Data[1].Tool.Done);

    CHECK(Finish(&tools, SVL("bash-id"), SVL("hi")));
    CHECK_ALL(tools.Entries.Data[0].Tool.Done, !tools.Entries.Data[1].Tool.Done);
    CHECK(Finish(&tools, SVL("read-id"), SVL("alpha-preview\nbeta-preview\ngamma-preview")));
    CHECK_ALL(AnyRowHas(&a, &tools, "\xe2\x96\xa0  echo hi"),
              AnyRowHas(&a, &tools, "Read src/main.cpp L10:14"),
              AnyRowHas(&a, &tools, "3 lines read"),
              !AnyRowHas(&a, &tools, "alpha-preview"));
    u64 bashEntryId = tools.Entries.Data[0].Id;
    u64 readEntryId = tools.Entries.Data[1].Id;
    u32 readFirst = 0;
    u32 readRows = 0;
    bool found = LineModel_FindEntryRows(&tools, readEntryId, &readFirst, &readRows);
    CHECK_ALL(found, readRows == 1, LineModel_At(&tools, readFirst)->HasHiddenContent);

    CHECK(LineModel_ToggleAllTools(&tools));
    CHECK_ALL(!LineModel_IsEntryCollapsed(&tools, bashEntryId),
              !LineModel_IsEntryCollapsed(&tools, readEntryId),
              AnyRowHas(&a, &tools, "alpha-preview"),
              AnyRowHas(&a, &tools, "beta-preview"),
              AnyRowHas(&a, &tools, "gamma-preview"));
    const SpanLine* readPreview = FindRow(&a, &tools, "alpha-preview");
    CHECK(readPreview != nullptr);
    CHECK_ALL(RowStartsWith(&a, readPreview, "   "), readPreview->EntryId == readEntryId);
    CHECK(LineModel_ToggleAllTools(&tools));
    CHECK_ALL(LineModel_IsEntryCollapsed(&tools, bashEntryId),
              LineModel_IsEntryCollapsed(&tools, readEntryId),
              !AnyRowHas(&a, &tools, "alpha-preview"));

    CHECK(LineModel_ToggleEntry(&tools, bashEntryId));
    CHECK_ALL(!LineModel_IsEntryCollapsed(&tools, bashEntryId),
              LineModel_IsEntryCollapsed(&tools, readEntryId));
    CHECK(LineModel_ToggleAllTools(&tools));
    CHECK_ALL(!LineModel_IsEntryCollapsed(&tools, bashEntryId),
              !LineModel_IsEntryCollapsed(&tools, readEntryId));
    CHECK(LineModel_ToggleAllTools(&tools));

    LineModel_AddTool(
        &tools, SVL("control-id"), SVL("bash"),
        SVL("{\"command\":\"first line\\n\\u001b[31msecond line\\u001b[0m\"}"));
    CHECK_ALL(AnyRowHas(&a, &tools, "\xe2\x96\xa0  first line second line"),
              !StrView_Contains(StrBuf_View(&tools.Entries.Data[2].Tool.View.Detail), "\n"),
              !StrView_Contains(StrBuf_View(&tools.Entries.Data[2].Tool.View.Detail), "\x1b"));
    LineModel_Release(&tools);
  }

  {
    LineModel toolWrap;
    LineModel_Init(&toolWrap, &a, 16, 5000);
    LineModel_AddTool(&toolWrap, SVL("bash"), SVL("bash"),
                      SVL("{\"command\":\"echo this command wraps\"}"));
    u64 bashId = toolWrap.Entries.Data[0].Id;
    CHECK_ALL(RowStartsWith(&a, LineModel_At(&toolWrap, 0), "\xe2\x96\xa0  "),
              LineModel_Count(&toolWrap) == 1,
              LineModel_At(&toolWrap, 0)->HasHiddenContent);
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
    CHECK_ALL(RowStartsWith(&a, LineModel_At(&simpleTurns, 0), "\xe2\x96\xa0  "),
              RowStartsWith(&a, LineModel_At(&simpleTurns, 1), "   "));
    LineModel_AddNotice(&simpleTurns, EntryKind::Error, SVL("error message wraps"));
    const SpanLine* error = FindRow(&a, &simpleTurns, "error");
    CHECK(error != nullptr);
    CHECK(RowStartsWith(&a, error, "!  "));
    LineModel_Release(&simpleTurns);
  }

  {
    LineModel summaries;
    LineModel_Init(&summaries, &a, 120, 5000);
    struct SummaryCase {
      StrView Id;
      StrView Name;
      StrView Args;
      StrView Result;
      const char* Expected;
      const char* Detail;
    };
    SummaryCase cases[] = {
        {SVL("grep"), SVL("grep"),
         SVL("{\"pattern\":\"needle\",\"path\":\"src\",\"glob\":\"*.cpp\"}"),
         SVL("a.cpp-1-before\na.cpp:2: one\na.cpp-3-after\nb.cpp:4: two"),
         "Search \"needle\" in src (*.cpp)", "2 lines found"},
        {SVL("find"), SVL("find"), SVL("{\"pattern\":\"*.h\",\"path\":\"src\"}"),
         SVL("a.h\nb.h"), "2 files found", nullptr},
        {SVL("ls"), SVL("ls"), SVL("{\"path\":\"src\"}"), SVL("agent/\nterm/"),
         "2 entries found", nullptr},
        {SVL("write"), SVL("write"), SVL("{\"path\":\"out.txt\",\"content\":\"abc\"}"),
         SVL("Successfully wrote 3 bytes to out.txt"), "3 bytes written", nullptr},
        {SVL("edit"), SVL("edit"),
         SVL("{\"path\":\"a.cpp\",\"edits\":[{\"oldText\":\"a\",\"newText\":\"b\"},{\"oldText\":\"c\",\"newText\":\"d\"}]}"),
         SVL("Successfully replaced 2 blocks"), "2 blocks replaced", nullptr},
        {SVL("single-edit"), SVL("edit"),
         SVL("{\"path\":\"b.cpp\",\"oldText\":\"a\",\"newText\":\"b\"}"),
         SVL("Successfully replaced 1 block"), "1 block replaced", nullptr},
        {SVL("bracket-read"), SVL("read"), SVL("{\"path\":\"notes.txt\"}"),
         SVL("first\n\n[section]\nlast"), "4 lines read", nullptr},
    };
    bool summariesMatch = true;
    for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      SummaryCase c = cases[i];
      LineModel_AddTool(&summaries, c.Id, c.Name, c.Args);
      bool caseMatches = Finish(&summaries, c.Id, c.Result) &&
                         AnyRowHas(&a, &summaries, c.Expected) &&
                         (c.Detail == nullptr || AnyRowHas(&a, &summaries, c.Detail));
      summariesMatch = caseMatches && summariesMatch;
    }
    CHECK(summariesMatch);

    LineModel_AddTool(&summaries, SVL("json-read"), SVL("read"),
                      SVL("{\"path\":\"config.json\"}"));
    CHECK_ALL(
        StrView_Eq(summaries.Entries.Data[summaries.Entries.Count - 1].Tool.View.Marker, SVL("J")),
        AnyRowHas(&a, &summaries, "\xe2\x96\xa0  Read config.json"));

    LineModel_AddTool(&summaries, SVL("unknown"), SVL("custom"), SVL("{\"value\":1}"));
    bool unknownFinished = Finish(&summaries, SVL("unknown"), SVL("done"));
    CHECK_ALL(unknownFinished, AnyRowHas(&a, &summaries, "custom"),
              AnyRowHas(&a, &summaries, "completed"));

    LineModel_AddTool(&summaries, SVL("error"), SVL("bash"), SVL("{\"command\":\"false\"}"));
    CHECK(Finish(&summaries, SVL("error"), SVL("Command exited with code 1"), true));
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
    CHECK(Finish(&noColor, SVL("id"), SVL("ok")));
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
    bool firstFinished =
        Finish(&bounded, SVL("one"), SVL("first-preview-that-fits-on-one-row"));
    LineModel_AddTool(&bounded, SVL("two"), SVL("read"), SVL("{\"path\":\"two.txt\"}"));
    bool secondFinished =
        Finish(&bounded, SVL("two"), SVL("second-preview-that-fits-on-one-row"));
    CHECK_ALL(firstFinished, secondFinished);
    CHECK(LineModel_ToggleAllTools(&bounded));
    usize expandedUsed = bounded.RowsMem.Used;
    CHECK_ALL(LineModel_Count(&bounded) <= bounded.Cap,
              AnyRowHas(&a, &bounded, "first-preview-that-fits-on-one-row"),
              AnyRowHas(&a, &bounded, "second-preview-that-fits-on-one-row"));
    CHECK(LineModel_ToggleAllTools(&bounded));
    CHECK(LineModel_ToggleAllTools(&bounded));
    CHECK_ALL(bounded.RowsMem.Used == expandedUsed,
              LineModel_Count(&bounded) <= bounded.Cap);
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
    CHECK_ALL(a.Used - before < 2048, AnyRowHas(&a, &parseMemory, "Write large.txt"));
    LineModel_Release(&parseMemory);
  }

  ArenaRelease(&a);
}
