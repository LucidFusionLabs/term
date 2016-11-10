#include "gtest/gtest.h"
#include "core/app/app.h"
#include "core/app/gui.h"
#include "core/web/browser.h"
#include "core/app/crypto.h"
#include "core/app/net/resolver.h"
#include "core/app/net/ssh.h"
#include "core/app/net/rfb.h"
#include "core/app/db/sqlite.h"
#include "term/term_generated.h"
#include "term.h"

namespace LFL {
struct MyTerminalTab;
struct MyTerminalMenus;

struct MyAppState {
  unique_ptr<SystemAlertView> info_alert, confirm_alert, passphrase_alert, passphraseconfirm_alert, passphrasefailed_alert;
  unique_ptr<SystemMenuView> toys_menu;
  unique_ptr<MyTerminalMenus> menus;
  virtual ~MyAppState() {}
  Shader *GetShader(const string &shader_name) { return 0; }
} *my_app = nullptr;
  
struct MyTerminalWindow : public TerminalWindowInterface<TerminalTabInterface> {
  MyTerminalWindow(Window *W) : TerminalWindowInterface(W) {}
  MyTerminalTab *AddTerminalTab() { return 0; }
  TerminalTabInterface *AddRFBTab(RFBClient::Params p, string, Callback savehost_cb=Callback()) { return 0; }
  void CloseActiveTab() {}
};

struct MyTerminalTab : public TerminalTab {
  TerminalWindowInterface<TerminalTabInterface> *parent;
  virtual ~MyTerminalTab() { root->DelGUI(terminal); }
  MyTerminalTab(Window *W, TerminalWindowInterface<TerminalTabInterface> *P) :
    TerminalTab(W, W->AddGUI(make_unique<Terminal>(nullptr, W, W->default_font, point(80,25)))), parent(P) {}
  void ChangeColors(const string &colors_name, bool redraw=true) {}
  void UseShellTerminalController(const string &m) {}
  void UseSSHTerminalController(SSHClient::Params params, const string &pw="",
                                shared_ptr<SSHClient::Identity> identity=shared_ptr<SSHClient::Identity>(),
                                StringCB metakey_cb=StringCB(),
                                SSHTerminalController::SavehostCB savehost_cb=SSHTerminalController::SavehostCB(),
                                SSHClient::FingerprintCB fingerprint_cb=SSHClient::FingerprintCB()) {}
  void UseTelnetTerminalController(const string &hostport, Callback savehost_cb=Callback()) {}
};

inline MyTerminalWindow *GetActiveWindow() {
  if (auto w = app->focused) return w->GetOwnGUI<MyTerminalWindow>(0);
  else                       return nullptr;
}

inline TerminalTabInterface *GetActiveTab() { return GetActiveWindow()->tabs.top; }
inline MyTerminalTab *GetActiveTerminalTab() { return dynamic_cast<MyTerminalTab*>(GetActiveTab()); }
}; // namespace LFL

#include "term_menu.h"
#include "term_menu.cpp"

namespace LFL {
void ResetTerminalMenus() {
  LocalFile::unlink(StrCat(app->savedir, "lterm.db"));
  my_app->menus = make_unique<MyTerminalMenus>();
}

}; // namespace LFL
using namespace LFL;

extern "C" void MyAppCreate(int argc, const char* const* argv) {
  app = new Application(argc, argv);
  app->focused = new Window();
  my_app = new MyAppState();
}

extern "C" int MyAppMain() {
  testing::InitGoogleTest(&app->argc, const_cast<char**>(app->argv));
  LFL::FLAGS_font = LFL::FakeFontEngine::Filename();
  CHECK_EQ(0, LFL::app->Create(__FILE__));
  LFL::ResetTerminalMenus();
  exit(RUN_ALL_TESTS());
}

TEST(MenusTest, GenerateKey) {
  { // RSA
    my_app->menus->genkey.view->SetSectionValues(1, StringVec{"RSA"});
    my_app->menus->genkey.view->SetSectionValues(2, StringVec{"2048"});
    int row1_id = my_app->menus->GenerateKey();
    int row2_id = my_app->menus->GenerateKey();
    EXPECT_NE(0, row2_id);
    EXPECT_NE(row1_id, row2_id);
    MyCredentialModel cred1(&my_app->menus->credential_db, row1_id);
    MyCredentialModel cred2(&my_app->menus->credential_db, row2_id);
    EXPECT_EQ(row1_id, cred1.cred_id);
    EXPECT_EQ(row2_id, cred2.cred_id);
    EXPECT_EQ(CredentialType_PEM, cred1.credtype);
    EXPECT_EQ(CredentialType_PEM, cred2.credtype);
    EXPECT_NE(string(), cred1.name);
    EXPECT_NE(string(), cred2.name);
    EXPECT_NE(string(), cred1.gentype);
    EXPECT_NE(string(), cred2.gentype);
    EXPECT_NE(string(), cred1.gendate);
    EXPECT_NE(string(), cred2.gendate);
    auto identity1 = FindOrNull(my_app->menus->identity_loaded, row1_id);
    auto identity2 = FindOrNull(my_app->menus->identity_loaded, row2_id);
    EXPECT_NE(nullptr, identity1);
    EXPECT_NE(nullptr, identity2);
    string pk1 = RSAOpenSSHPublicKey(identity1->rsa, "");
    string pk2 = RSAOpenSSHPublicKey(identity2->rsa, "");
    EXPECT_NE(string(), pk1);
    EXPECT_NE(string(), pk2);
    EXPECT_NE(pk1, pk2);
  }

  { // ECDSA
    my_app->menus->genkey.view->SetSectionValues(1, StringVec{"ECDSA"});
    my_app->menus->genkey.view->SetSectionValues(2, StringVec{"256"});
    int row1_id = my_app->menus->GenerateKey();
    int row2_id = my_app->menus->GenerateKey();
    EXPECT_NE(0, row2_id);
    EXPECT_NE(row1_id, row2_id);
    MyCredentialModel cred1(&my_app->menus->credential_db, row1_id);
    MyCredentialModel cred2(&my_app->menus->credential_db, row2_id);
    EXPECT_EQ(row1_id, cred1.cred_id);
    EXPECT_EQ(row2_id, cred2.cred_id);
    EXPECT_EQ(CredentialType_PEM, cred1.credtype);
    EXPECT_EQ(CredentialType_PEM, cred2.credtype);
    EXPECT_NE(string(), cred1.name);
    EXPECT_NE(string(), cred2.name);
    EXPECT_NE(string(), cred1.gentype);
    EXPECT_NE(string(), cred2.gentype);
    EXPECT_NE(string(), cred1.gendate);
    EXPECT_NE(string(), cred2.gendate);
    auto identity1 = FindOrNull(my_app->menus->identity_loaded, row1_id);
    auto identity2 = FindOrNull(my_app->menus->identity_loaded, row2_id);
    EXPECT_NE(nullptr, identity1);
    EXPECT_NE(nullptr, identity2);
    string pk1 = ECDSAOpenSSHPublicKey(identity1->ec, "");
    string pk2 = ECDSAOpenSSHPublicKey(identity2->ec, "");
    EXPECT_NE(string(), pk1);
    EXPECT_NE(string(), pk2);
    EXPECT_NE(pk1, pk2);
  }

  { // Ed25519
    my_app->menus->genkey.view->SetSectionValues(1, StringVec{"Ed25519"});
    my_app->menus->genkey.view->SetSectionValues(2, StringVec{"256"});
    int row1_id = my_app->menus->GenerateKey();
    int row2_id = my_app->menus->GenerateKey();
    EXPECT_NE(0, row2_id);
    EXPECT_NE(row1_id, row2_id);
    MyCredentialModel cred1(&my_app->menus->credential_db, row1_id);
    MyCredentialModel cred2(&my_app->menus->credential_db, row2_id);
    EXPECT_EQ(row1_id, cred1.cred_id);
    EXPECT_EQ(row2_id, cred2.cred_id);
    EXPECT_EQ(CredentialType_PEM, cred1.credtype);
    EXPECT_EQ(CredentialType_PEM, cred2.credtype);
    EXPECT_NE(string(), cred1.name);
    EXPECT_NE(string(), cred2.name);
    EXPECT_NE(string(), cred1.gentype);
    EXPECT_NE(string(), cred2.gentype);
    EXPECT_NE(string(), cred1.gendate);
    EXPECT_NE(string(), cred2.gendate);
    auto identity1 = FindOrNull(my_app->menus->identity_loaded, row1_id);
    auto identity2 = FindOrNull(my_app->menus->identity_loaded, row2_id);
    EXPECT_NE(nullptr, identity1);
    EXPECT_NE(nullptr, identity2);
    string pk1 = Ed25519OpenSSHPublicKey(identity1->ed25519, "");
    string pk2 = Ed25519OpenSSHPublicKey(identity2->ed25519, "");
    EXPECT_NE(string(), pk1);
    EXPECT_NE(string(), pk2);
    EXPECT_NE(pk1, pk2);
  }
}

