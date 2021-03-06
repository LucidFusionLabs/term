/*
 * $Id$
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

#ifndef WIN32
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif
#include "core/app/app.h"
#include "core/app/gl/view.h"
#include "core/app/gl/terminal.h"
#include "core/app/ipc.h"
#include "core/app/shell.h"
#include "core/app/net/resolver.h"
#include "core/web/browser/browser.h"
#include "core/web/browser/document.h"
#ifdef LFL_CRYPTO
#include "core/app/crypto.h"
#include "core/app/net/ssh.h"
#endif
#include "core/app/net/rfb.h"
#include "core/app/db/sqlite.h"
#ifdef LFL_FLATBUFFERS
#include "LTerminal/term_generated.h"
#endif
#if defined(__APPLE__) && !defined(LFL_TERMINAL_MENUS) && !defined(LFL_QT)
#define LFL_TERMINAL_JOIN_READS
#endif

namespace LFL {
#ifdef LFL_CRYPTO
DEFINE_string(ssh,             "",     "SSH to host");
DEFINE_string(login,           "",     "SSH user");
DEFINE_string(keyfile,         "",     "SSH private key");
DEFINE_bool  (compress,        false,  "SSH compression");
DEFINE_bool  (forward_agent,   false,  "SSH agent forwarding");
DEFINE_string(forward_local,   "",     "Forward local_port:remote_host:remote_port");
DEFINE_string(forward_remote,  "",     "Forward remote_port:local_host:local_port");
DEFINE_string(keygen,          "",     "Generate key");
DEFINE_int   (bits,            0,      "Generate key bits");      
#endif
DEFINE_bool  (interpreter,     false,  "Launch interpreter instead of shell");
DEFINE_string(term,            "",     "TERM var");
DEFINE_string(telnet,          "",     "Telnet to host");
DEFINE_string(vnc,             "",     "VNC to host");
DEFINE_string(command,         "",     "Execute initial command");
DEFINE_string(screenshot,      "",     "Screenshot and exit");
DEFINE_string(record,          "",     "Record session to file");
DEFINE_string(playback,        "",     "Playback recorded session file");
DEFINE_bool  (draw_fps,        false,  "Draw FPS");
DEFINE_bool  (resize_grid,     true,   "Resize window in glyph bound increments");
DEFINE_FLAG(dim, point, point(80,25),  "Initial terminal dimensions");
#ifndef LFL_MOBILE
DEFINE_bool  (single_instance, LINUXOS||WINDOWSOS, "Run a single instance of LTerminal");
#endif
extern FlagOfType<bool> FLAGS_enable_network_;

struct MyTerminalMenus;
struct MyTerminalTab;
struct MyTerminalWindow;

struct MyApp : public Application {
  unordered_map<string, Shader> shader_map;
  unique_ptr<Browser> image_browser;
  unique_ptr<TimerInterface> flash_timer;
  unique_ptr<AlertViewInterface> flash_alert, info_alert, confirm_alert, text_alert, passphrase_alert, passphraseconfirm_alert;
  unique_ptr<MenuViewInterface> edit_menu, view_menu, toys_menu;
  unique_ptr<MyTerminalMenus> menus;
  function<unique_ptr<ToolbarViewInterface>(Window*, const string&, MenuItemVec, int)> create_toolbar;
  int new_win_width = FLAGS_dim.x*Fonts::InitFontWidth(), new_win_height = FLAGS_dim.y*Fonts::InitFontHeight();
  int downscale_effects = 1, background_timeout = 180;

  virtual ~MyApp();
  MyApp(int ac, const char* const* av) :
    Application(ac, av), create_toolbar(bind(&ToolkitInterface::CreateToolbar, system_toolkit.get(), _1, _2, _3, _4)) {}

  void OnWindowInit(Window *W);
  void OnWindowStart(Window *W);
  void OnWindowClosed(Window *W) { delete W; }

  Shader *GetShader(const string &shader_name) { 
    auto shader = shader_map.find(shader_name);
    if (shader == shader_map.end()) return nullptr;
    if (!shader->second.ID) Shader::CreateShaderToy(this, shader_name, FileContents(StrCat(shader_name, ".frag")), &shader->second);
    return &shader->second;
  }
} *app;

inline string   LS  (const char *n) { return app->GetLocalizedString(n); }
inline String16 LS16(const char *n) { return app->GetLocalizedString16(n); }

inline MyTerminalWindow *GetActiveWindow() {
  if (auto w = app->focused) return w->GetOwnView<MyTerminalWindow>(1);
  else                       return nullptr;
}

}; // namespace LFL
#include "term.h"
namespace LFL {

struct MyTerminalTab : public TerminalTab {
  TerminalWindowInterface<TerminalTabInterface> *parent;
  Time join_read_interval = Time(100), refresh_interval = Time(33);
  int join_read_pending = 0;
  bool add_reconnect_links = true;
  FrameWakeupTimer timer;
  v2 zoom_val = v2(100, 100);

  virtual ~MyTerminalTab() { root->DelView(terminal); }
  MyTerminalTab(Window *W, TerminalWindowInterface<TerminalTabInterface> *P, int host_id, bool hide_sb) :
    TerminalTab(W, "MyTerminalTab", W->AddView(make_unique<Terminal>(nullptr, W, W->default_font, FLAGS_dim)), host_id, hide_sb), parent(P), timer(W) {
    terminal->new_link_cb      = bind(&MyTerminalTab::NewLinkCB,   this, _1);
    terminal->hover_control_cb = bind(&MyTerminalTab::HoverLinkCB, this, _1);
    if (terminal->bg_color) W->gd->clear_color = terminal->bg_color;
  }

  bool GetFocused() const override { return parent->tabs.top == this; }
  void Draw(const point &p) override {
#ifdef LFL_TERMINAL_JOIN_READS
    timer.ClearWakeupIn();
#endif
    DrawBox(root->gd, root->Box(), true);
  }

  void DrawBox(GraphicsDevice *gd, Box draw_box, bool check_resized) override {
    Box orig_draw_box = draw_box;
    int effects = PrepareEffects(&draw_box, app->downscale_effects, terminal->extra_height);
    if (check_resized) terminal->CheckResized(orig_draw_box);
    gd->DisableBlend();
    terminal->Draw(draw_box, effects ? 0 : Terminal::DrawFlag::DrawCursor, effects ? activeshader : NULL);
    if (effects) { gd->UseShader(0); terminal->DrawCursor((orig_draw_box.Position() + terminal->GetCursorPosition()), activeshader); }
    if (auto shell_controller = dynamic_cast<ShellTerminalController*>(controller.get())) {
      if (shell_controller->NullController()) {
        gd->EnableBlend();
        gd->SetColor(Color::red - Color::Alpha(0.75));
        GraphicsContext::DrawTexturedBox1(gd, draw_box);
        gd->DisableBlend();
      }
    }
    if (terminal->scrolled_lines) DrawScrollBar(draw_box);
  }

  void UpdateTargetFPS() override { parent->UpdateTargetFPS(); }

  void SetFontSize(int n) override {
    bool drew = false;
    root->default_font.desc.size = n;
    CHECK((terminal->style.font = root->default_font.Load(root)));
    int font_width  = terminal->style.font->FixedWidth(), new_width  = font_width  * terminal->term_width;
    int font_height = terminal->style.font->Height(),     new_height = font_height * terminal->term_height;
    if (FLAGS_resize_grid) root->SetResizeIncrements(font_width, font_height);
    if (new_width != root->gl_w || new_height != root->gl_h) drew = root->Reshape(new_width, new_height);
    if (!drew && terminal->line_fb.w && terminal->line_fb.h) terminal->Redraw(true, true);
    INFO("Font: ", app->fonts->DefaultFontEngine()->DebugString(terminal->style.font));
  }

  void UsePlaybackTerminalController(unique_ptr<FlatFile> f) {
    networked = false;
    title = LS("playback");
    ChangeController(make_unique<PlaybackTerminalController>(this, move(f)));
  }

  void UseShellTerminalController(const string &m, bool commands=true, Callback reconnect_cb=Callback()) {
    if (m.size()) connected = Time::zero();
    else {
      networked = false;
      title = LS("interactive_shell");
    }
    auto c = make_unique<ShellTerminalController>
      (this, &root->parent->localfs, m, [=](const string &h){ UseTelnetTerminalController(h, true); },
       [=](const StringVec&) { closed_cb(); }, move(reconnect_cb), commands);
    c->ssh_term = FLAGS_term;
#ifdef LFL_CRYPTO
    c->ssh_cb = [=](SSHClient::Params p){ UseSSHTerminalController(move(p), true, ""); };
#endif
#ifdef LFL_RFB
    c->vnc_cb = [=](RFBClient::Params p){ parent->AddRFBTab(1, !ANDROIDOS, move(p), ""); };
#endif
    ChangeController(move(c));
  }

  void UseReconnectTerminalController(const string &m, bool from_shell, Callback reconnect_cb=Callback()) {
    if (!from_shell && reconnect_toolbar && reconnect_cb) {
      string tb_theme = toolbar ? toolbar->GetTheme() : "Light";
      last_toolbar = ChangeToolbar(app->create_toolbar
                                   (parent->root, tb_theme, MenuItemVec{ { LS("reconnect"), "", move(reconnect_cb) } }, 0));
      reconnect_cb = Callback();
    }
    UseShellTerminalController(m, from_shell, move(reconnect_cb));
  }

#ifdef LFL_CRYPTO
  SSHTerminalController*
  UseSSHTerminalController(SSHClient::Params params, bool from_shell=false, const string &pw="",
                           SSHClient::LoadIdentityCB identity_cb=SSHClient::LoadIdentityCB(),
                           SSHTerminalController::SavehostCB savehost_cb=SSHTerminalController::SavehostCB(),
                           SSHClient::FingerprintCB fingerprint_cb=SSHClient::FingerprintCB()) {
    networked = true;
    title = StrCat(LS("ssh"), " ", params.user, "@", params.hostport);
    bool close_on_disconn = params.close_on_disconnect;
    Callback reconnect_cb = (!add_reconnect_links || close_on_disconn) ? Callback() : [=](){
      if (dynamic_cast<InteractiveTerminalController*>(controller.get()))
        UseSSHTerminalController(params, from_shell, pw, identity_cb, SSHTerminalController::SavehostCB(), fingerprint_cb);
    };
    auto ssh = make_unique<SSHTerminalController>(this, move(params), close_on_disconn ? closed_cb : [=, r = move(reconnect_cb)]() {
      UseReconnectTerminalController(StrCat("\r\n", LS("session_ended"), ".\r\n\r\n\r\n"), from_shell, move(r));
    });
    auto ret = ssh.get();
    ssh->metakey_cb = bind(&TerminalTabInterface::ToggleToolbarButton, this, _1);
    ssh->savehost_cb = move(savehost_cb);
    ssh->fingerprint_cb = move(fingerprint_cb);
    ssh->passphrase_alert = app->passphrase_alert.get();
    ssh->identity_cb = identity_cb;
    if (pw.size()) ssh->password = pw;
    ChangeController(move(ssh));
    return ret;
  }
#endif

  void UseTelnetTerminalController(const string &hostport, bool from_shell=false, bool close_on_disconn=false,
                                   Callback savehost_cb=Callback()) {
    networked = true;
    title = StrCat(LS("telnet"), " ", hostport);
    Callback reconnect_cb = (!add_reconnect_links || close_on_disconn) ? Callback() : [=](){
      if (dynamic_cast<InteractiveTerminalController*>(controller.get()))
        UseTelnetTerminalController(hostport, from_shell, close_on_disconn, Callback());
    };
    auto telnet = make_unique<NetworkTerminalController>(this, hostport, close_on_disconn ? closed_cb : [=, r = move(reconnect_cb)]() {
      UseReconnectTerminalController(StrCat("\r\n", LS("session_ended"), ".\r\n\r\n\r\n"), from_shell, move(r));
    });
    telnet->metakey_cb = bind(&TerminalTabInterface::ToggleToolbarButton, this, _1);
    telnet->success_cb = move(savehost_cb);
    telnet->success_on_connect = true;
    ChangeController(move(telnet));
  }

  void UseDefaultTerminalController() {
#if defined(WIN32) || defined(LFL_TERMINAL_MENUS)
    UseShellTerminalController("");
#else
    if (FLAGS_term.empty()) setenv("TERM", (FLAGS_term = "xterm").c_str(), 1);
    ChangeController(make_unique<PTYTerminalController>(this));
#endif
  }

  void UseInitialTerminalController() {
    if      (FLAGS_playback.size()) return UsePlaybackTerminalController(make_unique<FlatFile>(FLAGS_playback));
    else if (FLAGS_interpreter)     return UseShellTerminalController("");
#ifdef LFL_CRYPTO
    else if (FLAGS_ssh.size()) {
      SSHClient::Params params{FLAGS_ssh, FLAGS_login, FLAGS_term, FLAGS_command, FLAGS_compress,
        FLAGS_forward_agent, 0};
      if (FLAGS_forward_local .size()) SSHClient::ParsePortForward(FLAGS_forward_local,  &params.forward_local);
      if (FLAGS_forward_remote.size()) SSHClient::ParsePortForward(FLAGS_forward_remote, &params.forward_remote);
      SSHClient::LoadIdentityCB identity_cb;
      if (!FLAGS_keyfile.empty()) {
        INFO("Load keyfile ", FLAGS_keyfile);
        auto identity = make_shared<SSHClient::Identity>();
        if (!Crypto::ParsePEM(&LocalFile(FLAGS_keyfile, "r").Contents()[0], &identity->rsa, &identity->dsa, &identity->ec, &identity->ed25519,
                              [&](string v) { return app->passphrase_alert->RunModal(v); })) identity.reset();
        if (identity) identity_cb = [=](shared_ptr<SSHClient::Identity> *out) { *out = identity; return true; };
      }
      return ReturnVoid(UseSSHTerminalController(params, false, string(), identity_cb));
    }
#endif
    else if (FLAGS_telnet.size()) return UseTelnetTerminalController(FLAGS_telnet);
    else                          return UseDefaultTerminalController();
  }

  void OpenedController() override {
    if (FLAGS_command.size()) CHECK_EQ(FLAGS_command.size()+1, controller->Write(StrCat(FLAGS_command, "\n")));
  }

  bool ControllerReadableCB() override {
    int read_size = ReadAndUpdateTerminalFramebuffer();
    if (!parent->root->animating) {
#ifdef LFL_TERMINAL_JOIN_READS
      if (read_size) {
        int *pending = &join_read_pending;
        bool join_read = read_size > 255;
        if (join_read) { if (1            && ++(*pending)) { if (timer.WakeupIn(join_read_interval)) return false; } }
        else           { if ((*pending)<1 && ++(*pending)) { if (timer.WakeupIn(refresh_interval))   return false; } }
        *pending = 0;
      }
#endif
    }
    return GetFocused() && read_size > 0;
  }

  void NewLinkCB(const shared_ptr<TextBox::Control> &link) {
    const char *args = FindChar(link->val.c_str() + 6, isint2<'?', ':'>);
    string image_url(link->val, 0, args ? args - link->val.c_str() : string::npos);
    // if (SuffixMatch(image_url, ".gifv")) return;
    if (!FileSuffix::Image(image_url)) {
      return;
      string prot, host, port, path;
      if (HTTP::ParseURL(image_url.c_str(), &prot, &host, &port, &path) &&
          SuffixMatch(host, "imgur.com") && !FileSuffix::Image(path)) {
        image_url += ".jpg";
      } else return;
    }
    image_url += BlankNull(args);
    if (app->render_process && app->render_process->conn)
      app->RunInNetworkThread([=](){ link->image = app->image_browser->doc.parser->OpenImage(image_url); });
  }

  void HoverLinkCB(TextBox::Control *link) {
    Texture *tex = link ? link->image.get() : 0;
    if (!tex) return;
    tex->Bind();
    root->gd->EnableBlend();
    root->gd->SetColor(Color::white - Color::Alpha(0.25));
    GraphicsContext::DrawTexturedBox1
      (root->gd, Box::DelBorder(root->Box(), root->gl_w*.2, root->gl_h*.2), tex->coord);
    root->gd->ClearDeferred();
  }

  void ChangeFont(const StringVec &arg) {
    if (arg.size() < 2) return app->ShowSystemFontChooser
      (root->default_font.desc, bind(&MyTerminalTab::ChangeFont, this, _1));
    if (arg.size() > 2) FLAGS_font_flag = atoi(arg[2]);
    root->default_font.desc.name = arg[0];
    SetFontSize(atof(arg[1]));
    root->Wakeup();
  }

  void ChangeColors(const string &colors_name, bool redraw=true) {
    if      (colors_name == "Solarized Dark")  terminal->SetColors(Singleton<Terminal::SolarizedDarkColors> ::Set());
    else if (colors_name == "Solarized Light") terminal->SetColors(Singleton<Terminal::SolarizedLightColors>::Set());
    else                                       terminal->SetColors(Singleton<Terminal::StandardVGAColors>   ::Set());
    if (redraw) terminal->Redraw(true, true);
    if (terminal->bg_color) root->gd->clear_color = terminal->bg_color;
    root->Wakeup();
  }

  void ChangeShader(const string &shader_name) override {
    auto shader = app->GetShader(shader_name);
    activeshader = shader ? shader : &app->shaders->shader_default;
    parent->UpdateTargetFPS();
  }
};

#ifdef LFL_RFB
struct MyRFBTab : public TerminalTabInterface {
  TerminalWindowInterface<TerminalTabInterface> *parent;
  FrameBuffer fb;
  RFBTerminalController *rfb;
  Box last_draw_box;

  MyRFBTab(Window *W, TerminalWindowInterface<TerminalTabInterface> *P, int host_id, bool hide_sb,
           RFBClient::Params a, string pw, TerminalTabCB scb) :
    TerminalTabInterface(W, "MyRFBTab", 1.0, 1.0, 0, host_id, hide_sb), parent(P), fb(root) {
    networked = true;
    title = StrCat(LS("vnc"), ": ", a.hostport);
    auto c = make_unique<RFBTerminalController>(this, move(a), [=](){ closed_cb(); }, &fb);
    c->passphrase_alert = app->passphrase_alert.get();
    if (scb) c->savehost_cb = bind(move(scb), this);
    rfb = c.get();
    rfb->password = move(pw);
    (controller = move(c))->Open(nullptr);
  }

  bool                GetFocused() const  override { return parent->tabs.top == this; }
  MouseController    *GetMouseTarget()    override { return rfb; }
  KeyboardController *GetKeyboardTarget() override { return rfb; }
  Box                 GetLastDrawBox()    override { return last_draw_box; }

  void UpdateTargetFPS() override { parent->UpdateTargetFPS(); }
  void SetFontSize(int)  override {}
  void ScrollDown()      override {}
  void ScrollUp()        override {}

  void Draw(const point &p) override { DrawBox(root->gd, root->Box(), true); }
  void DrawBox(GraphicsDevice *gd, Box draw_box, bool check_resized) override {
    float tex[4];
    int effects = PrepareEffects(&draw_box, app->downscale_effects, 0);
    Texture::Coordinates(tex, rfb ? rfb->viewport : Box(), fb.tex.width, fb.tex.height);
    GraphicsContext gc(gd);
    gc.gd->DisableBlend();
    if (effects) {
      float scale = activeshader->scale;
      ShaderToyAsset(activeshader).Draw(gc.gd);
      activeshader->SetUniform1i("iChannelFlip", 1);
      activeshader->SetUniform2f("iChannelScroll", 0, 0);
      activeshader->SetUniform3f("iChannelResolution", XY_or_Y(activeshader->scale, gc.gd->TextureDim(fb.tex.width)),
                                 XY_or_Y(activeshader->scale, gc.gd->TextureDim(fb.tex.height)), 1);
      activeshader->SetUniform2f("iChannelModulus", fb.tex.coord[Texture::maxx_coord_ind],
                                 fb.tex.coord[Texture::maxy_coord_ind]);
      activeshader->SetUniform4f("iTargetBox", draw_box.x, draw_box.y,
                                 XY_or_Y(scale, draw_box.w), XY_or_Y(scale, draw_box.h));
    }
    fb.tex.Bind();
    gc.DrawTexturedBox((last_draw_box = draw_box), tex, 1);
    if (effects) gc.gd->UseShader(0);
  }

  int ReadAndUpdateTerminalFramebuffer() override {
    if (!controller) return 0;
    return controller->Read().len;
  }

  void ChangeShader(const string &shader_name) override {
    auto shader = app->GetShader(shader_name);
    activeshader = shader ? shader : &app->shaders->shader_default;
    parent->UpdateTargetFPS();
  }
};
#endif // LFL_RFB

struct MyTerminalWindow : public TerminalWindowInterface<TerminalTabInterface> {
  MyTerminalWindow(Window *W) : TerminalWindowInterface(W, "MyTerminalWindow") {}
  virtual ~MyTerminalWindow() { for (auto t : tabs.tabs) delete t; }

  MyTerminalTab *AddTerminalTab(int host_id, bool hide_sb=!ANDROIDOS, unique_ptr<ToolbarViewInterface> tb=unique_ptr<ToolbarViewInterface>());
  TerminalTabInterface *AddRFBTab(int host_id, bool hide_sb, RFBClient::Params p, string, TerminalTabCB savehost_cb=TerminalTabCB(),
                                  unique_ptr<ToolbarViewInterface> tb=unique_ptr<ToolbarViewInterface>());
  void InitTab(TerminalTabInterface*);

  void CloseActiveTab() {
    TerminalTabInterface *tab = tabs.top;
    if (!tab) return;
    tab->deleted_cb();
  }

  int Frame(Window *W, unsigned clicks, int flag) {
    if (tabs.top) tabs.top->Draw(point());
    W->DrawDialogs();
    if (FLAGS_draw_fps) W->default_font->Draw(W->gd, StringPrintf("FPS = %.2f", app->focused->fps.FPS()), point(W->gl_w*.85, 0));
    if (FLAGS_screenshot.size()) ONCE(W->shell->screenshot(vector<string>(1, FLAGS_screenshot)); app->run=0;);
    return 0;
  }

  void ConsoleAnimatingCB() { 
    UpdateTargetFPS();
    if (!root->console || !root->console->animating) {
      if ((root->console && root->console->Active()) || tabs.top->controller->frame_on_keyboard_input) app->scheduler.AddMainWaitKeyboard(root);
      else                                                                                             app->scheduler.DelMainWaitKeyboard(root);
    }
  }

  void UpdateTargetFPS() {
    bool animating = tabs.top->Animating() || (root->console && root->console->animating);
    app->scheduler.SetAnimating(root, animating);
    if (app->downscale_effects > 1) app->SetDownScale(tabs.top->Effects());
  }

  void ShowTransparencyControls() {
    SliderDialog::UpdatedCB cb(bind([=](Widget::Slider *s){ root->SetTransparency(s->Percent()); }, _1));
    root->AddDialog(make_unique<SliderDialog>(root, "window transparency", cb, 0, 1.0, .025));
    root->Wakeup();
  }
};

inline TerminalTabInterface *GetActiveTab() { return GetActiveWindow()->tabs.top; }
inline MyTerminalTab *GetActiveTerminalTab() { return dynamic_cast<MyTerminalTab*>(GetActiveTab()); }

#ifdef LFL_TERMINAL_MENUS
}; // namespace LFL
#include "term_menu.h"
#include "term_menu.cpp"
namespace LFL {
#else
struct MyTerminalMenus { int unused; };
#endif

MyTerminalTab *MyTerminalWindow::AddTerminalTab(int host_id, bool hide_statusbar, unique_ptr<ToolbarViewInterface> tb) {
  auto t = new MyTerminalTab(root, this, host_id, hide_statusbar);
  t->toolbar = move(tb);
#ifdef LFL_TERMINAL_MENUS
  t->terminal->line_fb.align_top_or_bot = t->terminal->cmd_fb.align_top_or_bot = true;
  if (atoi(Application::GetSetting("record_session")))
    t->record = make_unique<FlatFile>(StrCat(app->savedir, "session_", logfiletime(Now()), ".data"));
  t->terminal->resize_gui_ind.push_back(t->terminal->mouse.AddZoomBox(Box(), MouseController::ScaleCB([=](int button, v2 p, v2 d, int down) {
    t->zoom_val = t->zoom_val * d;
    int font_size = root->default_font.desc.size, delta=0;
    if      ((t->zoom_val.x > 110 || t->zoom_val.y > 110))                  delta = -1;
    else if ((t->zoom_val.x <  90 || t->zoom_val.y <  90) && font_size > 5) delta =  1;
    if (delta) {
      t->zoom_val = v2(100,100); 
      t->SetFontSize(font_size + delta);
      app->flash_alert->ShowCB(StrCat(LS("font_size"), " ", font_size + delta), "", "", StringCB());
      app->flash_timer->Run(FSeconds(2/3.0), true);
    }
  })));
#endif
  InitTab(t);
  return t;
}

TerminalTabInterface *MyTerminalWindow::AddRFBTab(int host_id, bool hide_statusbar, RFBClient::Params p, string pw,
                                                  TerminalTabCB savehost_cb, unique_ptr<ToolbarViewInterface> tb) {
#ifdef LFL_RFB
  auto t = new MyRFBTab(root, this, host_id, hide_statusbar, move(p), move(pw), move(savehost_cb));
  t->toolbar = move(tb);
  InitTab(t);
  return t;
#else
  return 0;
#endif
}

void MyTerminalWindow::InitTab(TerminalTabInterface *t) {
#ifdef LFL_TERMINAL_MENUS
  t->closed_cb = [=]() {
    t->deleted_cb();
    app->menus->ShowMainMenu(false);
  };
#else
  t->closed_cb = [](){ app->Shutdown(); };
#endif
  t->deleted_cb = [=](){ tabs.DelTab(t); /*XXX*/ app->RunInMainThread([=]{ delete t; }); };
  tabs.AddTab(t);
}

MyApp::~MyApp() {}

void MyApp::OnWindowInit(Window *W) {
  W->gl_w = app->new_win_width;
  W->gl_h = app->new_win_height;
  W->caption = app->name;
}

void MyApp::OnWindowStart(Window *W) {
  CHECK(W->gd->have_framebuffer);
  CHECK_EQ(0, W->NewView());
  auto tw = W->AddView(make_unique<MyTerminalWindow>(W));
  if (FLAGS_console) W->InitConsole(bind(&MyTerminalWindow::ConsoleAnimatingCB, tw));
  W->frame_cb = bind(&MyTerminalWindow::Frame, tw, _1, _2, _3);
  W->default_controller = [=]() -> MouseController* { if (auto t = GetActiveTab()) return t->GetMouseTarget();    return nullptr; };
  W->default_textbox = [=]() -> KeyboardController* { if (auto t = GetActiveTab()) return t->GetKeyboardTarget(); return nullptr; };
  W->shell = make_unique<Shell>(W);
  if (app->image_browser) W->shell->AddBrowserCommands(app->image_browser.get());
  app->scheduler.AddMainWaitMouse(W);

#ifndef LFL_TERMINAL_MENUS
  TerminalTabInterface *t = nullptr;
  ONCE_ELSE({ if (FLAGS_vnc.size()) t = tw->AddRFBTab(0, !ANDROIDOS, RFBClient::Params{FLAGS_vnc}, "");
              else { auto tt = tw->AddTerminalTab(0); tt->UseInitialTerminalController(); t=tt; }
              },   { auto tt = tw->AddTerminalTab(0); tt->UseDefaultTerminalController(); t=tt; });
  if (FLAGS_resize_grid)
    if (auto tt = dynamic_cast<MyTerminalTab*>(t))
      W->SetResizeIncrements(tt->terminal->style.font->FixedWidth(),
                             tt->terminal->style.font->Height());

  auto binds = W->ReplaceView(0, make_unique<BindMap>(W));
  binds->Add('n',       Key::Modifier::Cmd, Bind::CB(bind(&Application::CreateNewWindow, app)));
  binds->Add('t',       Key::Modifier::Cmd, Bind::CB(bind([=](){ tw->AddTerminalTab(0)->UseDefaultTerminalController(); })));
  binds->Add('w',       Key::Modifier::Cmd, Bind::CB(bind([=](){ tw->CloseActiveTab();      W->Wakeup(); })));
  binds->Add(']',       Key::Modifier::Cmd, Bind::CB(bind([=](){ tw->tabs.SelectNextTab();  W->Wakeup(); })));
  binds->Add('[',       Key::Modifier::Cmd, Bind::CB(bind([=](){ tw->tabs.SelectPrevTab();  W->Wakeup(); })));
  binds->Add(Key::Up,   Key::Modifier::Cmd, Bind::CB(bind([=](){ t->ScrollUp();             W->Wakeup(); })));
  binds->Add(Key::Down, Key::Modifier::Cmd, Bind::CB(bind([=](){ t->ScrollDown();           W->Wakeup(); })));
  binds->Add('=',       Key::Modifier::Cmd, Bind::CB(bind([=](){ t->SetFontSize(W->default_font.desc.size + 1); })));
  binds->Add('-',       Key::Modifier::Cmd, Bind::CB(bind([=](){ t->SetFontSize(W->default_font.desc.size - 1); })));
  binds->Add('6',       Key::Modifier::Cmd, Bind::CB(bind([=](){ W->shell->console(StringVec()); })));
#endif
}

}; // naemspace LFL
using namespace LFL;

extern "C" LFApp *MyAppCreate(int argc, const char* const* argv) {
#ifdef LFL_TERMINAL_MENUS
  SystemToolkit::DisableAdvertisingCrashReporting();
  Application::LoadDefaultSettings(StringPairVec{
    StringPair("send_crash_reports", "1"),
    StringPair("write_log_file",     "0"),
    StringPair("record_session",     "0"),
    StringPair("theme",              "Dark"),
  });
  if (atoi(Application::GetSetting("write_log_file"))) {
    FLAGS_logfile = "\x01";
    FLAGS_loglevel = 7;
  }
#endif
#if defined(__APPLE__) && !defined(LFL_IOS_SIM)
  if (atoi(Application::GetSetting("send_crash_reports"))) {
    InitCrashReporting("", Application::GetSetting("crash_report_name"),
                       Application::GetSetting("crash_report_email"));
  }
#endif
  FLAGS_enable_video = FLAGS_enable_input = 1;
  app = make_unique<MyApp>(argc, argv).release();
  app->focused = app->framework->ConstructWindow(app).release();
  app->name = LS("app_name");
  app->window_closed_cb = bind(&MyApp::OnWindowClosed, app, _1);
  app->window_start_cb = bind(&MyApp::OnWindowStart, app, _1);
  app->window_init_cb = bind(&MyApp::OnWindowInit, app, _1);
  app->window_init_cb(app->focused);
#ifdef LFL_TERMINAL_MENUS
  app->downscale_effects = app->SetExtraScale(true);
  app->SetTitleBar(false);
  app->SetKeepScreenOn(false);
  app->SetAutoRotateOrientation(true);
  app->focused->focused_cb = [=](){
    if (auto m = app->menus.get())
      if (m->suspended_timer && !(m->suspended_timer = false)) m->UpdateMainMenuSessionsSectionTimer();
  };
  app->focused->unfocused_cb = [=](){
    if (auto m = app->menus.get()) m->suspended_timer = m->sessions_update_timer->Clear();
  };
#endif
#ifdef LFL_IOS
  app->SetExtendedBackgroundTask([=](){ MSleep(app->background_timeout * 1000); });
#endif
  return app;
}

extern "C" int MyAppMain(LFApp*) {
  if (app->Create(__FILE__)) return -1;
  SettingsFile::Load(&app->localfs, app);
  Terminal::Colors *colors = Singleton<Terminal::SolarizedDarkColors>::Set();
  app->splash_color = colors->GetColor(colors->background_index);
  bool start_network_thread = !(FLAGS_enable_network_.override && !FLAGS_enable_network);

  if (FLAGS_single_instance) {
    string endpoint = SingleProcess::GetEndpointName(app->name), path = "/api/CreateNewWindow";
    (app->net = make_unique<SocketServices>(app, app))->Init();
    auto httpd = app->net->AddService(make_unique<HTTPServer>(app->net.get(), endpoint, false));
    if (!SingleProcess::RunLocalHTTPServerOrPost(app, app->net.get(), httpd, endpoint, path, "{}")) return -1;
    httpd->AddURL(path, new HTTPServer::FunctionResource([=](Connection*, int, const char*, const char*, const char*, const char*, int){
      app->RunInMainThread(bind(&Application::CreateNewWindow, app));
      return HTTPServer::Response("application/json", "{}");
    }));
  }

#ifdef WIN32
  app->asset_cache["MenuAtlas,0,255,255,255,0.0000.glyphs.matrix"] = app->LoadResource(200);
  app->asset_cache["MenuAtlas,0,255,255,255,0.0000.png"]           = app->LoadResource(201);
  app->asset_cache["default.vert"]                                 = app->LoadResource(202);
  app->asset_cache["default.frag"]                                 = app->LoadResource(203);
  app->asset_cache["alien.frag"]                                   = app->LoadResource(204);
  app->asset_cache["emboss.frag"]                                  = app->LoadResource(205);
  app->asset_cache["fire.frag"]                                    = app->LoadResource(206);
  app->asset_cache["fractal.frag"]                                 = app->LoadResource(207);
  app->asset_cache["darkly.frag"]                                  = app->LoadResource(208);
  app->asset_cache["stormy.frag"]                                  = app->LoadResource(209);
  app->asset_cache["twistery.frag"]                                = app->LoadResource(210);
  app->asset_cache["warper.frag"]                                  = app->LoadResource(211);
  app->asset_cache["water.frag"]                                   = app->LoadResource(212);
  app->asset_cache["waves.frag"]                                   = app->LoadResource(213);
  if (FLAGS_console) {
    app->asset_cache["VeraMoBd.ttf,32,255,255,255,4.0000.glyphs.matrix"] = app->LoadResource(214);
    app->asset_cache["VeraMoBd.ttf,32,255,255,255,4.0000.png"]           = app->LoadResource(215);
  }
#endif

  if (app->Init()) return -1;
#if defined(LFL_WINDOWS) or defined(LFL_LINUX)
  app->paste_bind = Bind(Mouse::Button::_2);
#endif

  app->flash_timer = SystemToolkit::CreateTimer([=](){ app->flash_alert->Hide(); });
  app->flash_alert = app->system_toolkit->CreateAlert(app->focused, AlertItemVec{
    { "style", "" }, { "", "" }, { "", "" }, { "", "" } });
  app->info_alert = app->system_toolkit->CreateAlert(app->focused, AlertItemVec{
    { "style", "" }, { "", "" }, { "", "" }, { LS("continue_"), "" } });
  app->confirm_alert = app->system_toolkit->CreateAlert(app->focused, AlertItemVec{
    { "style", "" }, { "", "" }, { LS("cancel"), "" }, { LS("continue_"), "" } });
  app->text_alert = app->system_toolkit->CreateAlert(app->focused, AlertItemVec{
    { "style", "textinput" }, { "", "" }, { LS("cancel"), "" }, { LS("continue_"), "" } });
  app->passphrase_alert = app->system_toolkit->CreateAlert(app->focused, AlertItemVec{
    { "style", "pwinput" }, { LS("passphrase"), LS("passphrase") }, { LS("cancel"), "" }, { LS("continue_"), "" } });
  app->passphraseconfirm_alert = app->system_toolkit->CreateAlert(app->focused, AlertItemVec{
    { "style", "pwinput" }, { LS("passphrase"), LS("confirm_passphrase") }, { LS("cancel"), "" }, { LS("continue_"), "" } });
#ifndef LFL_TERMINAL_MENUS
  app->edit_menu = app->system_toolkit->CreateEditMenu(app->focused, vector<MenuItem>());
  app->view_menu = app->system_toolkit->CreateMenu(app->focused, "View", MenuItemVec{
#ifdef __APPLE__
    MenuItem{ "=", "Zoom In" },
    MenuItem{ "-", "Zoom Out" },
#endif
    MenuItem{ "", "Fonts",        [=](){ if (auto t = GetActiveTerminalTab()) t->ChangeFont(StringVec()); } },
    MenuItem{ "", "Transparency", [=](){ if (auto w = LFL::GetActiveWindow()) w->ShowTransparencyControls(); } },
    MenuItem{ "", "VGA Colors",             [=](){ if (auto t = GetActiveTerminalTab()) t->ChangeColors("VGA");             } },
    MenuItem{ "", "Solarized Dark Colors",  [=](){ if (auto t = GetActiveTerminalTab()) t->ChangeColors("Solarized Dark");  } },
    MenuItem{ "", "Solarized Light Colors", [=](){ if (auto t = GetActiveTerminalTab()) t->ChangeColors("Solarized Light"); } }
  });
  if (FLAGS_term.empty()) FLAGS_term = BlankNull(getenv("TERM"));
#endif

  app->toys_menu = app->system_toolkit->CreateMenu(app->focused, LS("toys"), vector<MenuItem>{
    MenuItem{ "", LS("none"), [=](){ if (auto t = GetActiveTab()) t->ChangeShader("none");     } },
    MenuItem{ "", "Warper",   [=](){ if (auto t = GetActiveTab()) t->ChangeShader("warper");   } },
    MenuItem{ "", "Water",    [=](){ if (auto t = GetActiveTab()) t->ChangeShader("water");    } },
    MenuItem{ "", "Twistery", [=](){ if (auto t = GetActiveTab()) t->ChangeShader("twistery"); } },
    MenuItem{ "", "Fire",     [=](){ if (auto t = GetActiveTab()) t->ChangeShader("fire");     } },
    MenuItem{ "", "Waves",    [=](){ if (auto t = GetActiveTab()) t->ChangeShader("waves");    } },
    MenuItem{ "", "Emboss",   [=](){ if (auto t = GetActiveTab()) t->ChangeShader("emboss");   } },
    MenuItem{ "", "Stormy",   [=](){ if (auto t = GetActiveTab()) t->ChangeShader("stormy");   } },
    MenuItem{ "", "Alien",    [=](){ if (auto t = GetActiveTab()) t->ChangeShader("alien");    } },
    MenuItem{ "", "Fractal",  [=](){ if (auto t = GetActiveTab()) t->ChangeShader("fractal");  } },
    MenuItem{ "", "Darkly",   [=](){ if (auto t = GetActiveTab()) t->ChangeShader("darkly");   } },
#ifndef LFL_MOBILE
    MenuItem{ "", "<separator>" },
    MenuItem{ "", "Controls", [=](){ if (auto t = GetActiveTab()) t->ShowEffectsControls(); } },
#endif
  });

  app->shader_map.insert(make_pair("warper",   Shader(app)));
  app->shader_map.insert(make_pair("water",    Shader(app)));
  app->shader_map.insert(make_pair("twistery", Shader(app)));
  app->shader_map.insert(make_pair("fire",     Shader(app)));
  app->shader_map.insert(make_pair("waves",    Shader(app)));
  app->shader_map.insert(make_pair("emboss",   Shader(app)));
  app->shader_map.insert(make_pair("stormy",   Shader(app)));
  app->shader_map.insert(make_pair("alien",    Shader(app)));
  app->shader_map.insert(make_pair("fractal",  Shader(app)));
  app->shader_map.insert(make_pair("darkly",   Shader(app)));

#ifdef LFL_CRYPTO
  INFO("Using ", Crypto::LibraryName(), " cryptography");
  Crypto::PublicKeyInit();
  if (FLAGS_keygen.size()) {
    string pw = app->passphrase_alert->RunModal(""), fn="identity", pubkey, privkey;
    if (!Crypto::GenerateKey(FLAGS_keygen, FLAGS_bits, pw, "", &pubkey, &privkey))
      return ERRORv(-1, "keygen ", FLAGS_keygen, " bits=", FLAGS_bits, ": failed");
    LocalFile(fn, "w").WriteString(privkey);
    LocalFile(StrCat(fn, ".pub"), "w").WriteString(pubkey);
    INFO("Wrote ", fn, " and ", fn, ".pub");
    return 1;
  }
#endif

  if (start_network_thread) {
    if (!app->net) app->net = make_unique<SocketServices>(app, app);
#if !defined(LFL_MOBILE)
    app->log_pid = true;
    app->render_process = make_unique<ProcessAPIClient>(app, app, app->net.get(), app, app->fonts.get());
    app->render_process->StartServerProcess(StrCat(app->bindir, "LTerminal-render-sandbox", app->localfs.executable_suffix));
#endif
    CHECK(app->CreateNetworkThread(FLAGS_single_instance, true));
  }

  app->image_browser = make_unique<Browser>(app, app->focused, app, app->fonts.get(),
                                            app->net.get(), app->render_process.get(), app);
  app->StartNewWindow(app->focused);
  app->SetPinchRecognizer(true);
#ifdef LFL_TERMINAL_MENUS
  app->SetPanRecognizer(true);
  app->menus = make_unique<MyTerminalMenus>(app, app->toolkit);
  app->menus->hosts_nav->PushTableView(app->menus->hosts.view.get());
  app->menus->hosts_nav->Show(true);
  app->create_toolbar = bind(&MyTerminalMenus::CreateToolbar, app->menus.get(), _1, _2, _3);
#else
  app->SetVerticalSwipeRecognizer(2);
  app->SetHorizontalSwipeRecognizer(2);
  auto tw = LFL::GetActiveWindow();
  if (auto t = dynamic_cast<MyTerminalTab*>(tw->tabs.top)) {
    app->new_win_width  = t->terminal->style.font->FixedWidth() * t->terminal->term_width;
    app->new_win_height = t->terminal->style.font->Height()     * t->terminal->term_height;
    if (FLAGS_record.size()) t->record = make_unique<FlatFile>(FLAGS_record);
    t->terminal->Draw(app->focused->Box());
    INFO("Starting ", app->name, " ", app->focused->default_font.desc.name, " (w=", t->terminal->style.font->FixedWidth(),
         ", h=", t->terminal->style.font->Height(), ", scale=", app->downscale_effects, ")");
  }
#endif

  return app->Main();
}
