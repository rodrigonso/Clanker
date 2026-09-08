#include "app/frontmatter.h"

namespace {

StrView NormalizeNewlines(Arena* arena, StrView content) {
  bool hasCr = false;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\r') {
      hasCr = true;
      break;
    }
  }
  if (!hasCr) return content;
  StrBuf out = StrBuf_Make(arena, content.Length + 1);
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\r' && i + 1 < content.Length &&
        content.Data[i + 1] == '\n')
      continue;
    StrBuf_AppendChar(&out, content.Data[i]);
  }
  return StrBuf_View(&out);
}

void SplitFrontmatter(StrView content, StrView* yaml, StrView* body) {
  if (yaml) *yaml = StrView{content.Data, 0};
  if (body) *body = StrView_Trim(content);
  if (content.Length < 4 || memcmp(content.Data, "---", 3) != 0) return;
  for (u32 i = 3; i + 4 <= content.Length; i++) {
    if (memcmp(content.Data + i, "\n---", 4) != 0) continue;
    if (yaml) *yaml = StrView{content.Data + 4, i > 4 ? i - 4 : 0};
    if (body)
      *body =
          StrView_Trim(StrView{content.Data + i + 4, content.Length - i - 4});
    return;
  }
}

bool NextLine(StrView text, u32* pos, StrView* line) {
  if (*pos > text.Length) return false;
  u32 start = *pos;
  u32 end = start;
  while (end < text.Length && text.Data[end] != '\n') end++;
  *line = StrView{text.Data + start, end - start};
  *pos = end + 1;
  return true;
}

StrView ScalarValue(StrView raw) {
  raw = StrView_Trim(raw);
  if (raw.Length >= 2) {
    char quote = raw.Data[0];
    if ((quote == '"' || quote == '\'') && raw.Data[raw.Length - 1] == quote)
      return StrView{raw.Data + 1, raw.Length - 2};
  }
  for (u32 i = 1; i < raw.Length; i++) {
    if (raw.Data[i] == '#' && (raw.Data[i - 1] == ' ' || raw.Data[i - 1] == '\t'))
      return StrView_Trim(StrView{raw.Data, i});
  }
  return raw;
}

u32 IndentOf(StrView line) {
  u32 n = 0;
  while (n < line.Length && (line.Data[n] == ' ' || line.Data[n] == '\t')) n++;
  return n;
}

StrView BlockScalar(Arena* arena, StrView yaml, u32* pos, bool folded) {
  StrBuf out = StrBuf_Make(arena, 128);
  StrView line = {};
  u32 probe = *pos;
  while (NextLine(yaml, &probe, &line)) {
    StrView trimmed = StrView_Trim(line);
    if (trimmed.Length && IndentOf(line) == 0) break;
    *pos = probe;
    if (out.Length) StrBuf_AppendCStr(&out, folded ? " " : "\n");
    StrBuf_Append(&out, trimmed);
  }
  return StrView_Trim(StrBuf_View(&out));
}

}  // namespace

bool Frontmatter_ParseBool(StrView value) {
  return StrView_EqInsensitiveAscii(value, SVL("true")) ||
         StrView_EqInsensitiveAscii(value, SVL("yes")) ||
         StrView_EqInsensitiveAscii(value, SVL("on"));
}

void Frontmatter_Parse(Arena* arena, StrView content, FrontmatterField* fields,
                       u32 fieldCount, StrView* body) {
  StrView normalized = NormalizeNewlines(arena, content);
  StrView yaml = {};
  SplitFrontmatter(normalized, &yaml, body);
  if (!fields || !fieldCount) return;

  u32 pos = 0;
  StrView line = {};
  while (NextLine(yaml, &pos, &line)) {
    if (!line.Length || IndentOf(line) != 0) continue;
    if (line.Data[0] == '#') continue;
    u32 colon = 0;
    while (colon < line.Length && line.Data[colon] != ':') colon++;
    if (colon >= line.Length) continue;
    StrView key = StrView_Trim(StrView{line.Data, colon});
    StrView raw =
        StrView_Trim(StrView{line.Data + colon + 1, line.Length - colon - 1});

    FrontmatterField* field = nullptr;
    for (u32 i = 0; i < fieldCount; i++) {
      if (StrView_Eq(fields[i].Key, key)) {
        field = &fields[i];
        break;
      }
    }
    // A block scalar's continuation lines must be consumed even when the key
    // is unrecognized, or they would be parsed as top-level keys.
    bool isBlock = raw.Length && (raw.Data[0] == '|' || raw.Data[0] == '>');
    StrView value = isBlock ? BlockScalar(arena, yaml, &pos, raw.Data[0] == '>')
                            : ScalarValue(raw);
    if (field) field->Value = value;
  }
}
