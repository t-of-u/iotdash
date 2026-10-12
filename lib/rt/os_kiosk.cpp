// Kiosk mode: one installed app is the board's only screen. It opens when the system starts, and
// every way out of it (the system bar's Home, Back on its first page, os.home() from its pages)
// asks for the password set with it; the right password turns kiosk mode off and shows the home
// screen. Started from the Apps app (os.kiosk.start), system pages only.
//
// Registry: secure.kiosk {app, salt, hash}: hash = SHA-256 of salt + password (system pages only
// see secure.* keys). A factory reset clears it with the rest of the registry.
//
// Also here: the system dialog's password form (askPassword), which the kiosk uses.

#include "js.h"
#include "os.h"
#include "sha256.h"

namespace rt {

static Json jstr(const std::string &s) {
  Json j;
  j.type = Json::Str;
  j.str = s;
  return j;
}

std::string Os::kioskDigest(const std::string &salt, const std::string &password) {
  Sha256 h;
  h.update(salt.data(), salt.size());
  h.update(password.data(), password.size());
  uint8_t out[32];
  h.final(out);
  return Sha256::hex(out);
}

void Os::kioskLoad() {
  kioskApp_.clear();
  kioskSalt_.clear();
  kioskHash_.clear();
  Json k = regJson("secure.kiosk");
  if (k.type != Json::Obj) return;
  kioskApp_ = k.strOr("app", "");
  kioskSalt_ = k.strOr("salt", "");
  kioskHash_ = k.strOr("hash", "");
  if (kioskHash_.empty()) kioskApp_.clear();  // no password, no kiosk
}

bool Os::kioskStart(const std::string &id, const std::string &password, std::string &err) {
  if (!privileged()) {
    err = "only the system can start kiosk mode";
    return false;
  }
  if (password.size() < 4) {
    err = "the password needs 4 characters or more";
    return false;
  }
  bool installed = false;
  for (auto &a : apps(false)) installed = installed || a.id == id;
  if (!installed) {
    err = "no installed app " + id;
    return false;
  }
  // a salt per password: equal passwords on two boards do not look equal
  static unsigned counter = 0;
  std::string seed = std::to_string(plat_.micros()) + "/" + std::to_string(++counter) + "/" + id;
  std::string salt = kioskDigest(seed, std::to_string((uintptr_t)this)).substr(0, 16);
  Json v;
  v.type = Json::Obj;
  v.obj = {{"app", jstr(id)}, {"salt", jstr(salt)}, {"hash", jstr(kioskDigest(salt, password))}};
  regPut("secure.kiosk", v);
  kioskLoad();
  plat_.log(("kiosk: on, " + id).c_str());
  kioskResume();
  return true;
}

void Os::kioskOff() {
  if (reg_.erase("secure.kiosk")) {
    plat_.registrySave(regDumpAll());
    js_.busEmit("registry", "{\"key\":\"secure.kiosk\",\"value\":null}");
  }
  if (!kioskApp_.empty()) plat_.log(("kiosk: off (" + kioskApp_ + ")").c_str());
  kioskLoad();
}

void Os::kioskReset() {
  if (modalPass_) {  // the password question goes too
    passClose();
    modalOpen_ = false;
    setShellClass();
  }
  kioskOff();
  if (inApp()) home();
}

void Os::kioskResume() {
  if (kioskApp_.empty() || (inApp() && app_.id == kioskApp_)) return;
  std::string err;
  if (launch(kioskApp_, err)) return;
  plat_.log(("kiosk: cannot open " + kioskApp_ + ": " + err).c_str());
  if (err.rfind("no app", 0) == 0) kioskOff();  // the app is gone: a kiosk without it would lock nothing
}

void Os::kioskUnlock() {
  askPassword("Kiosk mode", "Enter the password to leave " + app_.title + ".", "Leave",
              [this](const std::string &password) {
                if (kioskApp_.empty() || kioskDigest(kioskSalt_, password) != kioskHash_) return false;
                kioskOff();
                home();
                return true;
              });
}

void Os::userHome(bool ask) {
  if (!kioskApp_.empty() && inApp()) return kioskUnlock();
  if (ask) requestHome();
  else home();
}

// ---------------------------------------------------------------- the password form of the dialog

void Os::askPassword(const std::string &title, const std::string &message, const std::string &ok,
                     std::function<bool(const std::string &)> check) {
  confirm(title, message, ok, "Cancel", nullptr);
  modalPass_ = true;
  passCheck_ = std::move(check);
  passErrorText_.clear();
  passShow();
}

// the form as the state says (also after a page change rebuilt the system UI)
void Os::passShow() {
  if (passError_) {
    passError_->removeAllChildren();
    if (!passErrorText_.empty()) passError_->appendChild(new Text(passErrorText_));
  }
  if (passInput_) {
    eng_.setValue(passInput_, "", false);
    eng_.focus(passInput_);  // opens the on-screen keyboard
  }
  setShellClass();
  eng_.requestFrame();
}

void Os::passClose() {
  modalPass_ = false;
  passCheck_ = nullptr;
  passErrorText_.clear();
  if (passInput_) {
    eng_.setValue(passInput_, "", false);
    if (eng_.focused() == passInput_) eng_.focus(nullptr);
  }
}

// OK / Enter: the right password closes the dialog, a wrong one says so and clears the field
void Os::passSubmit() {
  std::string password = passInput_ ? eng_.currentValue(passInput_) : "";
  auto check = passCheck_;
  if (check && !check(password)) {
    passErrorText_ = "Wrong password";
    passShow();
    return;
  }
  passClose();
  modalOpen_ = false;
  setShellClass();
}

bool Os::internalKey(Element *field, const std::string &key) {
  if (field != passInput_ || !modalPass_) return false;
  if (key == "Enter") passSubmit();
  return key == "Enter" || key == "Escape" || key == "Tab";  // the field keeps the focus
}

}  // namespace rt
