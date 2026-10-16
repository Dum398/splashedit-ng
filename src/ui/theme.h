#pragma once

#include <imgui.h>

// Design tokens. Every colour, size and radius the editor draws with comes
// from here; widgets never hardcode one.
namespace theme {

constexpr ImU32 rgb(unsigned hex, unsigned a = 255) {
    return IM_COL32((hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff, a);
}

namespace color {
// Surfaces, darkest to lightest.
constexpr ImU32 chrome = rgb(0x111318);     // title bar, status bar
constexpr ImU32 base = rgb(0x15171c);       // gaps between panels
constexpr ImU32 panel = rgb(0x1b1e24);      // panel bodies
constexpr ImU32 raised = rgb(0x22262e);     // section headers, cards
constexpr ImU32 field = rgb(0x121419);      // input wells
constexpr ImU32 hover = rgb(0x2a2f39);
constexpr ImU32 active = rgb(0x323846);
constexpr ImU32 border = rgb(0x262a33);
constexpr ImU32 borderStrong = rgb(0x343a46);

// Text.
constexpr ImU32 text = rgb(0xe4e7ee);
constexpr ImU32 textDim = rgb(0xa0a7b6);
constexpr ImU32 textFaint = rgb(0x6b7283);

// Accent and state.
constexpr ImU32 accent = rgb(0x8b7bff);
constexpr ImU32 accentHover = rgb(0x9d8fff);
constexpr ImU32 accentSoft = rgb(0x8b7bff, 46);
constexpr ImU32 good = rgb(0x46c98a);
constexpr ImU32 warn = rgb(0xf0a43a);
constexpr ImU32 warnSoft = rgb(0xf0a43a, 30);
constexpr ImU32 bad = rgb(0xef5a6f);

// Axes, shared by gizmos and vector fields.
constexpr ImU32 axisX = rgb(0xec5b6c);
constexpr ImU32 axisY = rgb(0x7fcb55);
constexpr ImU32 axisZ = rgb(0x4f9bef);
}  // namespace color

// Lua highlighting in the script editor.
namespace code {
constexpr ImU32 keyword = rgb(0xb4a6ff);
constexpr ImU32 constant = rgb(0xf0a46a);
constexpr ImU32 number = rgb(0xf0a46a);
constexpr ImU32 string = rgb(0x9fd77f);
constexpr ImU32 comment = rgb(0x6b7283);
constexpr ImU32 function = rgb(0x7fb6ff);
constexpr ImU32 api = rgb(0x5ccfe0);
constexpr ImU32 apiMember = rgb(0x8fdcea);
constexpr ImU32 op = rgb(0xa0a7b6);
constexpr ImU32 lineNumber = rgb(0x4d5363);
constexpr ImU32 currentLine = rgb(0xffffff, 7);
constexpr ImU32 selection = rgb(0x8b7bff, 70);
constexpr ImU32 match = rgb(0xf0a43a, 50);
constexpr ImU32 scope = rgb(0x5ccfe0, 16);  // the range find-in-selection searches
}  // namespace code

// The block editor: a colour per palette category (blockCategories()
// order), the text on a block and the canvas grid.
namespace blocks {
constexpr ImU32 events = rgb(0xc98a2e);
constexpr ImU32 control = rgb(0xc4673e);
constexpr ImU32 objects = rgb(0x3f7dcc);
constexpr ImU32 sound = rgb(0x9560cc);
constexpr ImU32 variables = rgb(0xc25469);
constexpr ImU32 operators = rgb(0x3d9a66);
constexpr ImU32 input = rgb(0x2c95a8);
constexpr ImU32 debug = rgb(0x687084);
constexpr ImU32 text = rgb(0xffffff);
constexpr ImU32 seam = rgb(0x000000, 70);     // a block's bottom edge
constexpr ImU32 shadow = rgb(0x000000, 60);   // under a stack
constexpr ImU32 grid = rgb(0xffffff, 12);     // the canvas dots
}  // namespace blocks

// 4 px grid.
namespace space {
constexpr float xxs = 2, xs = 4, sm = 8, md = 12, lg = 16, xl = 24;
}

namespace radius {
constexpr float field = 4, button = 5, card = 6, window = 8, pill = 999;
}

namespace size {
constexpr float titleBar = 40;
constexpr float statusBar = 28;
constexpr float row = 26;     // tree rows, inspector rows
constexpr float field = 24;   // input height
constexpr float panelHeader = 34;
constexpr float gutter = 4;   // gap between docked panels
}

// Type scale, in pixels.
namespace type {
constexpr float caption = 12.0f, label = 13.0f, body = 14.0f, title = 15.0f, icon = 15.0f;
constexpr float code = 14.0f;
}

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium = nullptr;
    ImFont* semibold = nullptr;
    ImFont* mono = nullptr;  // the script editor
};

Fonts& fonts();
void load(const char* assetDir);

}  // namespace theme
