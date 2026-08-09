#include "base/arena.h"
#include "check.h"
#include "term/screen.h"
#include "ui/widgets.h"

namespace {

void ActivateList(void* user, u32 index) { *(u32*)user = index; }

}  // namespace

void TestUiWidgets() {
  Arena arena = ArenaCreate();
  Arena screenArena = ArenaCreate();
  Screen screen;
  Screen_Init(&screen, &screenArena, nullptr, nullptr);
  UiContext ui;
  Ui_Init(&ui);

  UiListState list = {};
  StrView items[] = {SVL("one"), SVL("two"), SVL("three")};
  u32 activated = 0xffffffffu;
  LineEdit edit;
  LineEdit_Init(&edit, &arena);
  UiTextInputState input{&edit, 0, false};

  Ui_BeginFrame(&ui, &screen);
  UiLayoutStyle root = {};
  root.Width = Ui_Fill();
  root.Height = Ui_Fill();
  root.Gap = 1;
  Ui_BeginColumn(&ui, Ui_Key(1), root);

  UiBorderProps border = {};
  border.Layout.Width = Ui_Fill();
  border.Layout.Height = Ui_Cells(3);
  border.StyleValue = StyleDefault();
  Ui_BeginBorder(&ui, Ui_Key(1), border);
  UiTextProps text = {};
  text.Layout.Width = Ui_Fill();
  text.Layout.Height = Ui_Fill();
  text.StyleValue = StyleDefault();
  text.Wrap = true;
  Ui_Text(&ui, Ui_Key(1), SVL("hello wrapped text"), text);
  Ui_EndContainer(&ui);

  UiListProps listProps = {};
  listProps.Layout.Width = Ui_Fill();
  listProps.Layout.Height = Ui_Cells(3);
  listProps.Items = items;
  listProps.Count = 3;
  listProps.State = &list;
  listProps.NormalStyle = StyleDefault();
  listProps.SelectedStyle =
      StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_REVERSE);
  listProps.OnActivate = ActivateList;
  listProps.User = &activated;
  u32 listNode = Ui_SelectableList(&ui, Ui_Key(2), listProps);

  UiTextInputProps inputProps = {};
  inputProps.Layout.Width = Ui_Fill();
  inputProps.Layout.Height = Ui_Cells(2);
  inputProps.State = &input;
  inputProps.TextStyle = StyleDefault();
  inputProps.SelectionStyle =
      StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_REVERSE);
  u32 inputNode = Ui_TextInput(&ui, Ui_Key(3), inputProps);
  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 10);

  CHECK_ALL(Screen_BackAt(&screen, 0, 0).Ch == 0x256d,
            Screen_BackAt(&screen, 1, 1).Ch == 'h',
            ui.Focused == Ui_GetNode(&ui, listNode)->Id);

  Key down = {};
  down.Kind = KeyKind::Down;
  CHECK(Ui_Dispatch(&ui, Ui_EventFromKey(down)));
  CHECK(list.Selected == 1);
  Key enter = {};
  enter.Kind = KeyKind::Enter;
  CHECK(Ui_Dispatch(&ui, Ui_EventFromKey(enter)));
  CHECK(activated == 1);

  Ui_RequestFocus(&ui, Ui_GetNode(&ui, inputNode)->Id);
  Key key = {};
  key.Kind = KeyKind::Char;
  key.Codepoint = 'x';
  CHECK(Ui_Dispatch(&ui, Ui_EventFromKey(key)));
  CHECK(StrView_Eq(LineEdit_Text(&edit), SVL("x")));

  ui.BorderCorners = UiBorderCorners::Square;
  Ui_BeginFrame(&ui, &screen);
  UiLayoutStyle squareRoot = {};
  squareRoot.Width = Ui_Fill();
  squareRoot.Height = Ui_Fill();
  Ui_BeginColumn(&ui, Ui_Key(1), squareRoot);
  Ui_BeginBorder(&ui, Ui_Key(1), border);
  Ui_EndContainer(&ui);
  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 10);
  CHECK_ALL(Screen_BackAt(&screen, 0, 0).Ch == 0x250c,
            Screen_BackAt(&screen, 19, 0).Ch == 0x2510,
            Screen_BackAt(&screen, 0, 2).Ch == 0x2514,
            Screen_BackAt(&screen, 19, 2).Ch == 0x2518,
            Screen_BackAt(&screen, 1, 0).Ch == 0x2500,
            Screen_BackAt(&screen, 0, 1).Ch == 0x2502);
  ui.BorderCorners = UiBorderCorners::Rounded;

  Ui_BeginFrame(&ui, &screen);
  root = {};
  root.Width = Ui_Fill();
  root.Height = Ui_Fill();
  Ui_BeginColumn(&ui, Ui_Key(1), root);
  inputNode = Ui_TextInput(&ui, Ui_Key(3), inputProps);
  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 4);
  CHECK_ALL(ui.Focused == Ui_GetNode(&ui, inputNode)->Id,
            screen.CursorVisible, Screen_BackAt(&screen, 0, 0).Ch == 'x');

  UiScrollState scroll = {};
  scroll.Offset = 3;
  scroll.MaxOffset = 9;
  Ui_BeginFrame(&ui, &screen);
  UiLayoutStyle barLayout = {};
  barLayout.Width = Ui_Cells(1);
  barLayout.Height = Ui_Fill();
  u32 bar = Ui_Scrollbar(
      &ui, Ui_Key(9), &scroll,
      StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_DIM),
      StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_BOLD), barLayout);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 1, 6);
  CHECK_ALL(Ui_GetNode(&ui, bar)->Rect.Height == 6,
            Screen_BackAt(&screen, 0, 0).Ch == 0x2595);

  Ui_BeginFrame(&ui, &screen);
  UiTextProps fixedText = {};
  fixedText.Layout.Width = Ui_Cells(5);
  fixedText.StyleValue = StyleDefault();
  fixedText.Wrap = true;
  u32 fixedTextNode =
      Ui_Text(&ui, Ui_Key(20), SVL("abcdefghij"), fixedText);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  CHECK_ALL(Ui_GetNode(&ui, fixedTextNode)->Rect.Width == 5,
            Ui_GetNode(&ui, fixedTextNode)->Rect.Height == 2,
            Screen_BackAt(&screen, 0, 1).Ch == 'f');

  Ui_BeginFrame(&ui, &screen);
  UiLayoutStyle fillRowLayout = {};
  fillRowLayout.Width = Ui_Cells(10);
  u32 fillRow = Ui_BeginRow(&ui, Ui_Key(24), fillRowLayout);
  UiTextProps fillText = {};
  fillText.Layout.Width = Ui_Fill();
  fillText.StyleValue = StyleDefault();
  fillText.Wrap = true;
  u32 leftFill =
      Ui_Text(&ui, Ui_Key(1), SVL("abcdefghij"), fillText);
  u32 rightFill =
      Ui_Text(&ui, Ui_Key(2), SVL("klmnopqrst"), fillText);
  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 10, 5);
  CHECK_ALL(Ui_GetNode(&ui, fillRow)->Rect.Height == 2,
            Ui_GetNode(&ui, leftFill)->Rect.Width == 5,
            Ui_GetNode(&ui, leftFill)->Rect.Height == 2,
            Ui_GetNode(&ui, rightFill)->Rect.Width == 5,
            Ui_GetNode(&ui, rightFill)->Rect.Height == 2,
            Screen_BackAt(&screen, 0, 1).Ch == 'f',
            Screen_BackAt(&screen, 5, 1).Ch == 'p');

  Ui_BeginFrame(&ui, &screen);
  fillRow = Ui_BeginRow(&ui, Ui_Key(25), fillRowLayout);
  fillText.Layout.MinWidth = 1;
  leftFill = Ui_Text(&ui, Ui_Key(1), SVL("left"), fillText);
  fillText.Layout.MinWidth = 8;
  rightFill = Ui_Text(&ui, Ui_Key(2), SVL("right"), fillText);
  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 10, 2);
  CHECK_ALL(Ui_GetNode(&ui, leftFill)->Rect.Width >= 1,
            Ui_GetNode(&ui, rightFill)->Rect.Width >= 8,
            Ui_GetNode(&ui, leftFill)->Rect.Width +
                    Ui_GetNode(&ui, rightFill)->Rect.Width ==
                10);

  Ui_BeginFrame(&ui, &screen);
  fixedText.Layout.Width = Ui_Cells(10);
  fixedText.Layout.MaxWidth = 4;
  fixedTextNode = Ui_Text(&ui, Ui_Key(21), SVL("abcdefgh"), fixedText);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  CHECK(Ui_GetNode(&ui, fixedTextNode)->Rect.Width == 4);

  LineEdit_Clear(&edit);
  LineEdit_InsertText(&edit, SVL("abcdef"));
  LineEdit_SetCursor(&edit, 5);
  input.FirstRow = 0;
  inputProps.Layout.Width = Ui_Cells(5);
  inputProps.Layout.Height = Ui_Cells(2);
  inputProps.WrapWidth = 5;
  Ui_BeginFrame(&ui, &screen);
  inputNode = Ui_TextInput(&ui, Ui_Key(22), inputProps);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 5, 2);
  CHECK_ALL(ui.Focused == Ui_GetNode(&ui, inputNode)->Id,
            screen.CursorVisible, screen.CursorX == 0, screen.CursorY == 1);

  LineEdit_Clear(&edit);
  LineEdit_InsertText(&edit, SVL("\xe4\xb8\xadx"));
  inputProps.Layout.Width = Ui_Cells(4);
  inputProps.Layout.Height = Ui_Cells(1);
  inputProps.WrapWidth = 4;
  Ui_BeginFrame(&ui, &screen);
  inputNode = Ui_TextInput(&ui, Ui_Key(23), inputProps);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 4, 1);
  UiEvent inputDown = {};
  inputDown.Kind = UiEventKind::MouseDown;
  inputDown.X = 1;
  inputDown.Y = 0;
  CHECK(Ui_Dispatch(&ui, inputDown));
  CHECK_ALL(LineEdit_Cursor(&edit) == 3, input.Dragging,
            ui.Captured == Ui_GetNode(&ui, inputNode)->Id);

  Ui_BeginFrame(&ui, &screen);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 4, 1);
  CHECK_ALL(!input.Dragging, ui.Captured == 0);

  LineEdit edit2;
  LineEdit_Init(&edit2, &arena);
  UiTextInputState input2{&edit2, 0, false};
  inputProps.Layout.Width = Ui_Cells(4);
  inputProps.Layout.Height = Ui_Cells(1);
  inputProps.WrapWidth = 4;
  Ui_BeginFrame(&ui, &screen);
  fillRowLayout.Width = Ui_Cells(8);
  Ui_BeginRow(&ui, Ui_Key(26), fillRowLayout);
  u32 firstInput = Ui_TextInput(&ui, Ui_Key(1), inputProps);
  inputProps.State = &input2;
  u32 secondInput = Ui_TextInput(&ui, Ui_Key(2), inputProps);
  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 8, 1);
  inputDown.X = 0;
  CHECK(Ui_Dispatch(&ui, inputDown));
  CHECK_ALL(input.Dragging,
            ui.Captured == Ui_GetNode(&ui, firstInput)->Id);
  inputDown.X = 4;
  CHECK(Ui_Dispatch(&ui, inputDown));
  CHECK_ALL(!input.Dragging, input2.Dragging,
            ui.Captured == Ui_GetNode(&ui, secondInput)->Id);
  Ui_CancelPointer(&ui);
  CHECK_ALL(!input2.Dragging, ui.Captured == 0);

  Ui_Release(&ui);
  ArenaRelease(&screenArena);
  ArenaRelease(&arena);
}
