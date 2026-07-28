#include "base/arena.h"
#include "check.h"
#include "term/screen.h"
#include "ui/widgets.h"

void TestUiDemo() {
  Arena arena = ArenaCreate();
  Screen screen;
  Screen_Init(&screen, &arena, nullptr, nullptr);
  UiContext ui;
  Ui_Init(&ui);

  Ui_BeginFrame(&ui, &screen);
  UiLayoutStyle page = {};
  page.Width = Ui_Fill();
  page.Height = Ui_Fill();
  page.Padding = UiInsets{1, 1, 1, 1};
  page.Gap = 1;
  Ui_BeginColumn(&ui, Ui_Key(SVL("settings-screen")), page);

  UiTextProps title = {};
  title.Layout.Width = Ui_Fill();
  title.Layout.Height = Ui_Cells(1);
  title.StyleValue = StyleMake(75, COL_DEFAULT, ATTR_BOLD);
  Ui_Text(&ui, Ui_Key(SVL("title")), SVL("Settings"), title);

  UiBorderProps panel = {};
  panel.Layout.Width = Ui_Fill();
  panel.Layout.Height = Ui_Fill();
  panel.StyleValue = StyleDefault();
  Ui_BeginPanel(&ui, Ui_Key(SVL("panel")), panel);
  UiTextProps body = {};
  body.Layout.Width = Ui_Fill();
  body.Layout.Height = Ui_Fill();
  body.StyleValue = StyleDefault();
  body.Wrap = true;
  Ui_Text(&ui, Ui_Key(SVL("body")),
          SVL("Model: fixture\nProvider: local\nTheme: terminal"), body);
  Ui_EndContainer(&ui);

  Ui_EndContainer(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 32, 10);

  UiMetrics metrics = Ui_GetMetrics(&ui);
  CHECK_ALL(Screen_BackAt(&screen, 1, 1).Ch == 'S',
            Screen_BackAt(&screen, 1, 3).Ch == 0x256d,
            Screen_BackAt(&screen, 2, 4).Ch == 'M',
            metrics.NodeCount == 5, metrics.HitRegionCount == 0);

  Ui_Release(&ui);
  ArenaRelease(&arena);
}

