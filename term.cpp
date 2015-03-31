/*
 * $Id: term.cpp 1336 2014-12-08 09:29:59Z justin $
 * Copyright (C) 2009 Lucid Fusion Labs

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lfapp/lfapp.h"
#include "lfapp/dom.h"
#include "lfapp/css.h"
#include "lfapp/flow.h"
#include "lfapp/gui.h"
#include "crawler/html.h"
#include "crawler/document.h"

namespace LFL {
DEFINE_int(peak_fps,  50,    "Peak FPS");
DEFINE_bool(draw_fps, false, "Draw FPS");
extern FlagOfType<string> FLAGS_default_font_;

Scene scene;
BindMap *binds;
Shader warpershader;
AnyBoolSet effects_mode;
Browser *image_browser;

void MyNewLinkCB(TextArea::Link *link) {
    string image_url = link->link;
    if (!FileSuffix::Image(image_url)) {
        string prot, host, port, path;
        if (HTTP::ParseURL(image_url.c_str(), &prot, &host, &port, &path) &&
            SuffixMatch(host, "imgur.com") && !FileSuffix::Image(path)) {
            image_url += ".jpg";
        } else { 
            return;
        }
    }
    link->image_src.SetNameValue("src", image_url);
    link->image.setAttributeNode(&link->image_src);
    image_browser->doc.parser->Open(image_url, &link->image);
}

void MyHoverLinkCB(TextArea::Link *link) {
    Asset *a = link ? link->image.asset : 0;
    if (!a) return;
    a->tex.Bind();
    screen->gd->SetColor(Color::white - Color::Alpha(0.2));
    Box::DelBorder(screen->Box(), screen->width*.2, screen->height*.2).Draw();
}

struct ReadBuffer {
    string data; int size;
    ReadBuffer(int S=0) : size(S), data(S, 0) {}
    void Reset() { data.resize(size); }
};

struct MyTerminalWindow {
    Process process;
    ReadBuffer read_buf;
    Terminal *terminal=0;
    Shader *activeshader;
    int font_size;
    AnyBoolElement effects_mode;

    MyTerminalWindow() : read_buf(65536), activeshader(&app->video.shader_default), font_size(FLAGS_default_font_size), effects_mode(&LFL::effects_mode) {}
    ~MyTerminalWindow() { if (process.in) app->scheduler.DelWaitForeverSocket(fileno(process.in)); }

    void Open() {
        setenv("TERM", "screen", 1);
        string shell = BlankNull(getenv("SHELL"));
        CHECK(!shell.empty());
        const char *av[] = { shell.c_str(), 0 };
        CHECK_EQ(process.OpenPTY(av), 0);
        app->scheduler.AddWaitForeverSocket(fileno(process.in), SocketSet::READABLE, 0);

        terminal = new Terminal(fileno(process.out), screen, Fonts::Get(FLAGS_default_font, "", font_size));
        terminal->new_link_cb = MyNewLinkCB;
        terminal->hover_link_cb = MyHoverLinkCB;
        terminal->active = true;
        terminal->term_width = 80;
        terminal->term_height = 25;
    }
    void UpdateTargetFPS() {
        effects_mode.Set(CustomShader() || screen->console->animating);
        int target_fps = effects_mode.Get() ? FLAGS_peak_fps : 0;
        if (target_fps != FLAGS_target_fps) app->scheduler.UpdateTargetFPS(target_fps);
    }
    bool CustomShader() const { return activeshader != &app->video.shader_default; }
};

int Frame(Window *W, unsigned clicks, unsigned mic_samples, bool cam_sample, int flag) {
    MyTerminalWindow *tw = (MyTerminalWindow*)W->user1;
    tw->read_buf.Reset();
    if (NBRead(fileno(tw->process.in), &tw->read_buf.data)) tw->terminal->Write(tw->read_buf.data);

    W->gd->DrawMode(DrawMode::_2D);
    tw->terminal->DrawWithShader(W->Box(), true, tw->activeshader);
    W->DrawDialogs();
    if (FLAGS_draw_fps) Fonts::Default()->Draw(StringPrintf("FPS = %.2f", FPS()), point(W->width*.85, 0));
    return 0;
}

void SetFontSize(int n) {
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    tw->font_size = n;
    tw->terminal->font = Fonts::Get(FLAGS_default_font, "", tw->font_size);
    screen->Reshape(tw->terminal->font->FixedWidth() * tw->terminal->term_width,
                    tw->terminal->font->Height()     * tw->terminal->term_height);
}
void MyConsoleAnimating(Window *W) { 
    ((MyTerminalWindow*)W->user1)->UpdateTargetFPS();
    if (!screen->console->animating) {
        if (screen->console->active) app->scheduler.AddWaitForeverKeyboard();
        else                         app->scheduler.DelWaitForeverKeyboard();
    }
}
void MyIncreaseFontCmd(const vector<string>&) { SetFontSize(((MyTerminalWindow*)screen->user1)->font_size + 1); }
void MyDecreaseFontCmd(const vector<string>&) { SetFontSize(((MyTerminalWindow*)screen->user1)->font_size - 1); }
void MyColorsCmd(const vector<string> &arg) {
    string colors_name = arg.size() ? arg[0] : "";
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    if      (colors_name ==       "vga") tw->terminal->SetColors(Singleton<Terminal::StandardVGAColors>::Get());
    else if (colors_name == "solarized") tw->terminal->SetColors(Singleton<Terminal::SolarizedColors>  ::Get());
    tw->terminal->Redraw();
}
void MyShaderCmd(const vector<string> &arg) {
    string shader_name = arg.size() ? arg[0] : "";
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    if (shader_name == "warper") tw->activeshader = &warpershader;
    else                         tw->activeshader = &app->video.shader_default;
    tw->UpdateTargetFPS();
}

void MyInitFonts() {
    Video::InitFonts();
    string console_font = "VeraMoBd.ttf";
    Singleton<AtlasFontEngine>::Get()->Init(FontDesc(console_font, "", 32));
    FLAGS_console_font = StrCat("atlas://", console_font);
}
void MyWindowOpen() {
    ((MyTerminalWindow*)screen->user1)->Open();
    screen->console->animating_cb = bind(&MyConsoleAnimating, screen);
}
void MyWindowInitCB(Window *W) {
    W->width = 80*10;
    W->height = 25*17;
    W->caption = "Terminal";
    W->binds = binds;
    W->user1 = new MyTerminalWindow();
    if (app->initialized) {
        screen->InitConsole();
        MyWindowOpen();
    }
}
void MyWindowClosedCB() {
    delete (MyTerminalWindow*)screen->user1;
}

}; // naemspace LFL
using namespace LFL;

extern "C" int main(int argc, const char *argv[]) {

    app->logfilename = StrCat(LFAppDownloadDir(), "term.txt");
    app->frame_cb = Frame;
    binds = new BindMap();
    MyWindowInitCB(screen);
    FLAGS_target_fps = 0;
    FLAGS_lfapp_video = FLAGS_lfapp_input = 1;
    FLAGS_font_engine = "coretext";

    app->scheduler.AddWaitForeverService(Singleton<HTTPClient>::Get());
    app->scheduler.AddWaitForeverService(Singleton <UDPClient>::Get());
    if (app->Create(argc, argv, __FILE__)) { app->Free(); return -1; }

    if (FLAGS_font_engine == "coretext") FLAGS_default_font_flag = FontDesc::Mono;
    if (FLAGS_font_engine != "atlas") app->video.init_fonts_cb = &MyInitFonts;
    if (FLAGS_default_font_.override) {
    } else if (FLAGS_font_engine == "coretext") {
        FLAGS_default_font = "Monaco";
    } else if (FLAGS_font_engine == "freetype") { 
        FLAGS_default_font = "VeraMoBd.ttf"; // "DejaVuSansMono-Bold.ttf";
        FLAGS_default_missing_glyph = 42;
    } else if (FLAGS_font_engine == "atlas") {
        FLAGS_default_font = "VeraMoBd.ttf";
        FLAGS_default_missing_glyph = 42;
    }
    FLAGS_default_font_size = 32;
    FLAGS_atlas_font_sizes = "32";

    if (app->Init()) { app->Free(); return -1; }
    app->scheduler.AddWaitForeverMouse();
    app->window_init_cb = MyWindowInitCB;
    app->window_closed_cb = MyWindowClosedCB;
    app->shell.command.push_back(Shell::Command("colors", bind(&MyColorsCmd, _1)));
    app->shell.command.push_back(Shell::Command("shader", bind(&MyShaderCmd, _1)));

    binds->Add(Bind('=', Key::Modifier::Cmd, Bind::CB(bind(&MyIncreaseFontCmd, vector<string>()))));
    binds->Add(Bind('-', Key::Modifier::Cmd, Bind::CB(bind(&MyDecreaseFontCmd, vector<string>()))));
    binds->Add(Bind('n', Key::Modifier::Cmd, Bind::CB(bind(&Application::CreateNewWindow, app))));
    binds->Add(Bind('6', Key::Modifier::Cmd, Bind::CB(bind([&](){ screen->console->Toggle(); }))));

    string lfapp_vertex_shader = LocalFile::FileContents(StrCat(ASSETS_DIR, "lfapp_vertex.glsl"));
    string warper_shader = LocalFile::FileContents(StrCat(ASSETS_DIR, "warper.glsl"));
    Shader::Create("warpershader", lfapp_vertex_shader.c_str(), warper_shader.c_str(),
                   "#define TEX2D\n#define VERTEXCOLOR\n", &warpershader);

    image_browser = new Browser();
    MyWindowOpen();
    MyTerminalWindow *tw = (MyTerminalWindow*)screen->user1;
    SetFontSize(tw->font_size);
    tw->terminal->Draw(screen->Box(), false);
    INFO("Starting Terminal ", FLAGS_default_font, " (w=", tw->terminal->font->fixed_width,
                                                   ", h=", tw->terminal->font->Height(), ")");

    app->scheduler.Start();
    return app->Main();
}
