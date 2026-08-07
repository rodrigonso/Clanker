#pragma once
#include "base/basics.h"

// Decoded key events from the raw terminal byte stream (VT/ANSI on every
// platform; on Windows the input handle is put in virtual-terminal-input mode).
enum class KeyKind : u8 {
  None,  // recognised but ignored (e.g. unknown escape)
  Char,  // a printable codepoint (Codepoint)
  Enter,    // submit
  Newline,  // Shift+Enter / Ctrl+J / Alt+Enter / newline inside a paste
  Backspace,
  Delete,
  Tab,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  ScrollUp,    // mouse wheel up
  ScrollDown,  // mouse wheel down
  MouseMove,
  MouseLeftDown,
  MouseLeftUp,
  WordLeft,
  WordRight,
  CtrlC,
  CtrlD,
  CtrlL,
  CtrlO,
  CtrlU,
  CtrlK,
  CtrlW,
  Escape,
  PasteStart,
  PasteEnd,
};

struct Key {
  KeyKind Kind;
  u32 Codepoint;  // Char
  bool Ctrl;
  bool Alt;
  bool Shift;
  u16 MouseX;
  u16 MouseY;
};

// Stateful byte-stream decoder. Push raw bytes, then pull complete keys.
// Incomplete trailing sequences are retained until more bytes arrive.
struct KeyDecoder {
  char Buf[4096];
  u32 Len;
  bool InPaste;
  bool PendingEscape;
};

void KeyDecoder_Init(KeyDecoder* d);
void KeyDecoder_Push(KeyDecoder* d, const char* data, u32 n);
// Returns true and fills *out if a complete key is available; false if more
// bytes are needed.
bool KeyDecoder_Next(KeyDecoder* d, Key* out);
