/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Slippi pages (slippi_menu.h). */
#include "slippi_menu.h"

#include "launcher.h"
#include "pc.h"
#include "slippi/slp_user.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <aurora/rmlui.hpp>
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" void lbAudioAx_80024030(int);
extern "C" void pc_net_match_slippi_refresh(void);

namespace {

enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };

// The layout, in px, scaled to fit the frame's inside, as Fight Controls'.
constexpr float kScreenW = 1120.f, kScreenH = 660.f;

const char* const kPrivacyUrl = "https://slippi.gg/privacy";
const char* const kTermsUrl = "https://slippi.gg/tos";

// Slippi's rules, as the Launcher shows them (slippi_online_rules.messages.ts).
const char* const kRulesIntro =
    "These are a set of rules to follow when using Slippi. Breaking these rules may result in a suspension or ban "
    "depending on severity and frequency. This is not an exhaustive list, we reserve the right to suspend or ban "
    "an account for any reason.";
const char* const kRules[] = {
    "Racist, homophobic, transphobic, or otherwise bigoted names and codes are not allowed. Targeted harassment "
    "in names and codes is also not allowed.",
    "Slippi does its best to promote fairness in terms of player matching, result reporting, etc. Attempting to "
    "circumvent these systems is not allowed.",
    "Intentionally manipulating the game performance for your own gain is not allowed.",
    "Macros and bots are not allowed.",
};

enum class Page {
    Login,    // email, password, Sign in, Make an account
    Review,   // Register's name, email, password, Create account
    Verify,   // the email step
    Rules,    // the rules step
    Code,     // the connect code step
    Done,     // signed in
};

// What the keyboard that is up types into.
enum class Field { None, Email, Password, Name, RegEmail, RegPassword, RegConfirm };

enum class Op { SignIn, Register, Check, SendVerify, AcceptRules, ChooseCode, Reset };

// One account call on a worker: the session is the worker's until `done`.
struct Job {
    Op op;
    std::string a, b, c; // the call's text arguments
    bool ok = false;
    PcSlippiSetupStep step = PC_SLIPPI_SETUP_DONE;
    PcSlippiAccount account{};
    char err[192] = {0};
    std::atomic<bool> done{false};
    std::thread thread;
};

// A focusable thing on a page: its element id, its row and column.
struct Item {
    std::string id;
    int row, col;
    bool enabled = true;
};

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

std::string enc(const std::string& s) { return Rml::StringUtilities::EncodeRml(s); }

std::string dots(size_t n) {
    std::string s;
    for (size_t i = 0; i < std::min<size_t>(n, 24); i++)
        s += "\xe2\x80\xa2"; // a bullet
    return s;
}

struct Menu : Rml::EventListener {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    float presence = 0.f;
    float scale = 1.f;
    Rml::Vector2i dims{0, 0}, placedDims{0, 0};
    float placedPresence = -1.f;
    bool pointerWas = false, pointerPressed = false;

    int mode = PC_SLIPPI_MENU_LOGIN;
    Page page = Page::Login;
    bool leave = false;
    std::unique_ptr<PcSlippiSetup> session;
    std::unique_ptr<Job> job;
    PcSlippiAccount account{};

    // Login and Register's fields. The password stays only until it is sent.
    std::string email, password, name, regEmail, regPassword;
    std::string note;      // the page's error or news line
    bool noteGood = false; // news (gold), not an error
    std::string badRows;   // rows marked as wrong: 'e'mail, 'p'assword, 'n'ame
    bool chipLit = false;  // Forgot password glows after a wrong password

    // Setup.
    bool registering = false;   // the track starts with Account
    bool verifySent = false;    // the email step sent its link once
    bool rulesTick[2] = {false, false};
    std::string tag;            // the connect code's start

    std::vector<Item> items;
    int cursor = 0;
    Field keyboardFor = Field::None;
    PcSlippiKeyboard keyboard{};
    bool keyboardWanted = false;
    std::string hintText, titleText;
    std::string focusWant; // the item go() asked for, kept until the page is built

    bool load() {
        if (document != nullptr)
            return true;
        auto* context = aurora::rmlui::get_context();
        if (context == nullptr)
            return false;
        const std::string dir = pc_resources_dir();
        for (const char* face : {"menu.ttf", "menu-bold.ttf"})
            Rml::LoadFontFace(dir + face);
        Rml::LoadFontFace(dir + "menu-heavy.ttf", false, static_cast<Rml::Style::FontWeight>(800));
        document = context->LoadDocument(dir + "slippi-setup.rml");
        if (document == nullptr) {
            pc_log_line("slippi pages: could not load slippi-setup.rml");
            return false;
        }
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Mouseover, this);
        return true;
    }

    // ---- the visit -------------------------------------------------------

    void begin(int m) {
        stop();
        mode = m;
        leave = false;
        session = std::make_unique<PcSlippiSetup>();
        password.clear(), name.clear(), regEmail.clear(), regPassword.clear();
        note.clear(), badRows.clear();
        noteGood = chipLit = false;
        registering = m == PC_SLIPPI_MENU_REGISTER;
        verifySent = false;
        rulesTick[0] = rulesTick[1] = false;
        tag.clear();
        cursor = 0;
        if (registering) {
            regEmail = email;
            ask(Field::Name);
        } else {
            go(Page::Login);
        }
    }

    // The visit's calls and tokens dropped; the overlay stays as it is.
    void stop() {
        if (job != nullptr) {
            // A call still running: let it finish on its own, then drop it.
            if (job->thread.joinable())
                job->thread.join();
            job.reset();
        }
        if (session != nullptr) {
            pc_slippi_setup_end(session.get());
            session.reset();
        }
        wipe(password), wipe(regPassword);
    }

    void end() {
        stop();
        hide();
    }

    static void wipe(std::string& s) {
        std::fill(s.begin(), s.end(), '\0');
        s.clear();
    }

    // ---- account calls ---------------------------------------------------

    void run(Op op, std::string a = {}, std::string b = {}, std::string c = {}) {
        if (job != nullptr)
            return;
        job = std::make_unique<Job>();
        job->op = op;
        job->a = std::move(a), job->b = std::move(b), job->c = std::move(c);
        Job* j = job.get();
        PcSlippiSetup* s = session.get();
        j->thread = std::thread([j, s] {
            char* err = j->err;
            const size_t cap = sizeof j->err;
            bool then_check = true;
            switch (j->op) {
            case Op::SignIn:
                j->ok = pc_slippi_setup_sign_in(s, j->a.c_str(), j->b.c_str(), err, cap);
                break;
            case Op::Register:
                j->ok = pc_slippi_setup_register(s, j->a.c_str(), j->b.c_str(), j->c.c_str(), err, cap);
                break;
            case Op::Check:
                j->ok = true;
                break;
            case Op::SendVerify:
                j->ok = pc_slippi_setup_send_verification(s, err, cap);
                then_check = false;
                break;
            case Op::AcceptRules:
                j->ok = pc_slippi_setup_accept_rules(s, err, cap);
                break;
            case Op::ChooseCode:
                j->ok = pc_slippi_setup_choose_code(s, j->a.c_str(), err, cap);
                break;
            case Op::Reset:
                j->ok = pc_slippi_setup_send_password_reset(j->a.c_str(), err, cap);
                then_check = false;
                break;
            }
            std::fill(j->b.begin(), j->b.end(), '\0');
            std::fill(j->c.begin(), j->c.end(), '\0');
            if (j->ok && then_check)
                j->ok = pc_slippi_setup_check(s, &j->step, &j->account, err, cap);
            j->done = true;
        });
    }

    bool busy() const { return job != nullptr; }

    void poll() {
        if (job == nullptr || !job->done)
            return;
        job->thread.join();
        std::unique_ptr<Job> j = std::move(job);
        if (!j->ok) {
            failed(j->op, j->err);
            return;
        }
        switch (j->op) {
        case Op::SendVerify:
            verifySent = true;
            show_note("Sent. Check your email.", true);
            refresh();
            return;
        case Op::Reset:
            show_note("Sent. Open the link in the email to pick a new password.", true);
            chipLit = false;
            refresh();
            return;
        default:
            break;
        }
        if (j->op == Op::SignIn || j->op == Op::Register)
            wipe(password), wipe(regPassword);
        note.clear(), badRows.clear();
        switch (j->step) {
        case PC_SLIPPI_SETUP_DONE:
            account = j->account;
            pc_log_line("slippi pages: signed in as %s", account.connect_code);
            pc_net_match_slippi_refresh();
            if (session != nullptr)
                pc_slippi_setup_end(session.get());
            go(Page::Done);
            break;
        case PC_SLIPPI_SETUP_VERIFY_EMAIL:
            if (j->op == Op::Check && page == Page::Verify)
                show_note("Not verified yet. Check your spam folder.", false);
            go(Page::Verify);
            if (!verifySent && !busy())
                run(Op::SendVerify);
            break;
        case PC_SLIPPI_SETUP_RULES:
            go(Page::Rules);
            break;
        case PC_SLIPPI_SETUP_CONNECT_CODE:
            if (tag.empty()) {
                char t[8];
                pc_slippi_code_start_suggest(session->display_name, t, sizeof t);
                tag = t;
            }
            go(Page::Code);
            break;
        }
    }

    void failed(Op op, const char* err) {
        const std::string e = err != nullptr && *err ? err : "Something went wrong";
        pc_log_line("slippi pages: %s", e.c_str());
        lbAudioAx_80024030(SFX_BACK);
        switch (op) {
        case Op::SignIn:
            wipe(password);
            if (registering) {
                show_note(e, false);
                go(Page::Review);
                return;
            }
            show_note(e, false);
            if (e.find("Wrong email") != std::string::npos) {
                badRows = "ep";
                chipLit = true;
                go(Page::Login, "password");
            } else {
                go(Page::Login, "signin");
            }
            return;
        case Op::Register: {
            show_note(e, false);
            std::string lower = e;
            for (char& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool mail = lower.find("email") != std::string::npos;
            const bool nm = lower.find("name") != std::string::npos;
            const bool pw = lower.find("password") != std::string::npos;
            badRows = std::string(mail ? "e" : "") + (nm ? "n" : "") + (pw ? "p" : "");
            go(Page::Review, mail ? "r-email" : nm ? "r-name" : pw ? "r-password" : "create");
            return;
        }
        case Op::Reset:
            show_note(e, false);
            refresh();
            return;
        default:
            show_note(e, false);
            refresh();
            return;
        }
    }

    void show_note(const std::string& text, bool good) {
        note = text;
        noteGood = good;
    }

    // ---- keyboards --------------------------------------------------------

    void ask(Field f, const char* prompt = nullptr) {
        keyboardFor = f;
        keyboard = PcSlippiKeyboard{};
        const char* text = "";
        const char* def = "";
        switch (f) {
        case Field::Email:
            text = email.c_str(), def = "Enter the email of your Slippi account.";
            keyboard.max_len = 90, keyboard.shiftable = true;
            break;
        case Field::Password:
            def = "Enter your password.";
            keyboard.max_len = 90, keyboard.shiftable = true, keyboard.mask = true;
            break;
        case Field::Name:
            text = name.c_str(), def = "Pick a display name, up to 15 characters.";
            keyboard.max_len = 15, keyboard.shiftable = true;
            break;
        case Field::RegEmail:
            text = regEmail.c_str(), def = "Enter the email for your new account.";
            keyboard.max_len = 90, keyboard.shiftable = true;
            break;
        case Field::RegPassword:
            def = "Choose a password, 6 characters or more.";
            keyboard.max_len = 90, keyboard.shiftable = true, keyboard.mask = true;
            break;
        case Field::RegConfirm:
            def = "Type the password again.";
            keyboard.max_len = 90, keyboard.shiftable = true, keyboard.mask = true;
            break;
        case Field::None:
            return;
        }
        std::snprintf(keyboard.initial, sizeof keyboard.initial, "%s", text);
        std::snprintf(keyboard.prompt, sizeof keyboard.prompt, "%s", prompt != nullptr ? prompt : def);
        keyboardWanted = true;
    }

    // Where the keyboard's text goes, and what opens next.
    void typed(bool confirmed, const char* text_) {
        const std::string text = text_ != nullptr ? text_ : "";
        const Field f = keyboardFor;
        keyboardFor = Field::None;
        const bool chain = registering && page != Page::Review; // Register's first pass
        if (!confirmed) {
            // Back: the field before it in Register's first pass, else the form.
            if (chain) {
                switch (f) {
                case Field::Name:
                    leave = true;
                    return;
                case Field::RegEmail:
                    ask(Field::Name);
                    return;
                case Field::RegPassword:
                    ask(Field::RegEmail);
                    return;
                case Field::RegConfirm:
                    ask(Field::RegPassword);
                    return;
                default:
                    break;
                }
            }
            go(registering ? Page::Review : Page::Login);
            return;
        }
        switch (f) {
        case Field::Email:
            email = text;
            badRows.erase(std::remove(badRows.begin(), badRows.end(), 'e'), badRows.end());
            go(Page::Login, password.empty() ? "password" : "signin");
            return;
        case Field::Password:
            password = text;
            badRows.erase(std::remove(badRows.begin(), badRows.end(), 'p'), badRows.end());
            go(Page::Login, email.empty() ? "email" : "signin");
            return;
        case Field::Name:
            if (!pc_slippi_display_name_valid(text.c_str())) {
                ask(Field::Name, "Names are 1 to 15 characters, without \\ or `.");
                return;
            }
            name = text;
            if (chain) {
                ask(Field::RegEmail);
                return;
            }
            break;
        case Field::RegEmail:
            if (text.find('@') == std::string::npos) {
                ask(Field::RegEmail, "That doesn't look like an email. Try again.");
                return;
            }
            regEmail = text;
            email = text;
            if (chain) {
                ask(Field::RegPassword);
                return;
            }
            break;
        case Field::RegPassword:
            if (text.size() < 6) {
                ask(Field::RegPassword, "At least 6 characters. Choose another.");
                return;
            }
            wipe(regPassword);
            regPassword = text;
            ask(Field::RegConfirm);
            return;
        case Field::RegConfirm:
            if (text != regPassword) {
                wipe(regPassword);
                ask(Field::RegPassword, "The passwords didn't match. Choose one again.");
                return;
            }
            break;
        case Field::None:
            break;
        }
        badRows.clear();
        note.clear();
        go(Page::Review, "create");
    }

    // ---- pages -----------------------------------------------------------

    // A page, with the cursor on `focus` (else the first usable item). The
    // document may not be loaded yet (the keyboards come first), so the focus
    // waits for the build.
    void go(Page p, const char* focus = nullptr) {
        page = p;
        cursor = -1;
        focusWant = focus != nullptr ? focus : "";
        refresh();
    }

    std::string track() const {
        struct Node {
            const char* label;
            bool done, now;
        };
        std::vector<Node> nodes;
        const int at = page == Page::Review ? 0
                       : page == Page::Verify ? 1
                       : page == Page::Rules  ? 2
                       : page == Page::Code   ? 3
                       : page == Page::Done   ? 4
                                              : -1;
        if (registering)
            nodes.push_back({"ACCOUNT", at > 0, at == 0});
        const char* labels[3] = {"EMAIL", "RULES", "CODE"};
        // Steps behind are done (one this account never needed too: it is
        // passed over); the rest wait.
        for (int k = 0; k < 3; k++)
            nodes.push_back({labels[k], at > k + 1, at == k + 1});
        int lastDone = -1;
        for (size_t i = 0; i < nodes.size(); i++)
            if (nodes[i].done)
                lastDone = static_cast<int>(i);
        const float n = static_cast<float>(nodes.size() - 1);
        std::string r = "<div id=\"track\"><div class=\"line\"></div><div class=\"line done\" style=\"width: " +
                        fmt(lastDone < 0 ? 0.f : 100.f * lastDone / n) + "%;\"></div>";
        for (size_t i = 0; i < nodes.size(); i++) {
            const Node& d = nodes[i];
            r += "<div class=\"step" + std::string(d.done ? " done" : "") + (d.now ? " now" : "") +
                 "\" style=\"left: " + fmt(100.f * i / n) + "%;\"><div class=\"dot\"><div class=\"mark\"></div></div><div class=\"lbl\">" +
                 d.label + "</div></div>";
        }
        return r + "</div>";
    }

    std::string field(const char* id, const char* key, const std::string& value, const char* empty, bool bad,
                      bool masked) const {
        std::string v = value.empty() ? std::string("<div class=\"v empty\">") + empty + "</div>"
                                      : "<div class=\"v\">" + (masked ? dots(value.size()) : enc(value)) + "</div>";
        return "<div id=\"" + std::string(id) + "\" class=\"field" + (bad ? " bad" : "") + "\"><div class=\"k\">" +
               key + (bad ? "  \xc2\xb7  CHECK IT" : "") + "</div>" + v + "<div class=\"edit\">" +
               (value.empty() ? "Type" : "Change") + "</div></div>";
    }

    void add(const char* id, int row, int col, bool enabled = true) { items.push_back({id, row, col, enabled}); }

    void build() {
        if (document == nullptr)
            return;
        items.clear();
        const bool b = busy();
        std::string r;
        auto noteHtml = [this]() {
            return note.empty() ? std::string()
                                : "<div id=\"note\" class=\"" + std::string(noteGood ? "good" : "bad") + "\">" +
                                      enc(note) + "</div>";
        };
        switch (page) {
        case Page::Login: {
            const bool ready = !email.empty() && !password.empty();
            r += "<div class=\"form\"><div class=\"h\">Sign in to Slippi</div><div class=\"sub\">The email and "
                 "password of your Slippi account.</div>";
            r += field("email", "EMAIL", email, "Press A to type your email", badRows.find('e') != std::string::npos,
                       false);
            r += field("password", "PASSWORD", password, "Press A to type your password",
                       badRows.find('p') != std::string::npos, true);
            r += "<div class=\"buttons\"><button id=\"signin\" class=\"btn" + std::string(ready && !b ? "" : " off") +
                 "\">" + (b ? "<span class=\"pulse\"></span>Signing in..." : "Sign in") +
                 "</button><button id=\"make\" class=\"btn" + (b ? " off" : "") + "\">Make an account</button></div>";
            r += noteHtml() + "</div>";
            r += "<div id=\"chip\" class=\"chip" + std::string(chipLit ? " lit" : "") + (b ? " off" : "") +
                 "\"><div class=\"bean\">X</div><div class=\"lab\">FORGOT<br/>PASSWORD</div></div>";
            add("email", 0, 0, !b), add("password", 1, 0, !b), add("signin", 2, 0, ready && !b),
                add("make", 2, 1, !b);
            break;
        }
        case Page::Review:
            r += track();
            r += "<div class=\"form review\">";
            r += field("r-name", "DISPLAY NAME", name, "Press A to type a name", badRows.find('n') != std::string::npos,
                       false);
            r += field("r-email", "EMAIL", regEmail, "Press A to type your email",
                       badRows.find('e') != std::string::npos, false);
            r += field("r-password", "PASSWORD", regPassword.empty() ? std::string() : regPassword,
                       "Press A to choose a password", badRows.find('p') != std::string::npos, true);
            r += "<div class=\"buttons\"><button id=\"create\" class=\"btn" +
                 std::string(!b && !name.empty() && !regEmail.empty() && !regPassword.empty() ? "" : " off") + "\">" +
                 (b ? "<span class=\"pulse\"></span>Creating..." : "Create account") +
                 "</button><button id=\"instead\" class=\"btn" + (b ? " off" : "") + "\">Sign in instead</button></div>";
            r += note.empty() ? "<div class=\"fine\">Next: check your email, accept Slippi's rules, choose your "
                                "connect code.</div>"
                              : noteHtml();
            r += "</div>";
            add("r-name", 0, 0, !b), add("r-email", 1, 0, !b), add("r-password", 2, 0, !b),
                add("create", 3, 0, !b && !regPassword.empty()), add("instead", 3, 1, !b);
            break;
        case Page::Verify:
            r += track();
            r += "<img class=\"art\" src=\"slippi-setup/envelope.png\" />";
            r += "<div class=\"side\"><div class=\"sub\">" +
                 std::string(registering ? "Account made. We sent a link to" : "We sent a link to") +
                 "</div><div class=\"big\">" + enc(session != nullptr ? session->email : email) +
                 "</div><div class=\"text\">Open it on your phone or computer, then come back here.</div>";
            r += "<div class=\"buttons\"><button id=\"opened\" class=\"btn" + std::string(b ? " off" : "") + "\">" +
                 (b ? "<span class=\"pulse\"></span>Checking..." : "I opened it") +
                 "</button><button id=\"resend\" class=\"btn" + (b ? " off" : "") + "\">Send again</button></div>";
            r += noteHtml() + "</div>";
            add("opened", 0, 0, !b), add("resend", 0, 1, !b);
            break;
        case Page::Rules: {
            r += track();
            r += "<div id=\"rules\"><div class=\"rh\">Slippi Online Rules</div><div class=\"intro\">" +
                 enc(kRulesIntro) + "</div>";
            for (int i = 0; i < 4; i++)
                r += "<div class=\"rule\"><div class=\"num\">" + std::to_string(i + 1) + "</div><div class=\"rt\">" +
                     enc(kRules[i]) + "</div></div>";
            r += "</div><div id=\"ticks\">";
            r += "<div id=\"tick0\" class=\"chk" + std::string(rulesTick[0] ? " ticked" : "") +
                 "\"><div class=\"box\"><div class=\"mark\"></div></div><div class=\"t\">I accept the Slippi Online Rules</div><div "
                 "class=\"s\">read them on the left</div></div>";
            r += "<div id=\"tick1\" class=\"chk tall" + std::string(rulesTick[1] ? " ticked" : "") +
                 "\"><div class=\"box\"><div class=\"mark\"></div></div><div class=\"t\">I accept the Privacy Policy and Terms of "
                 "Service</div><div class=\"s\"><span class=\"btnkey\">X</span> privacy \xc2\xa0 <span "
                 "class=\"btnkey\">Y</span> terms</div></div>";
            const bool all = rulesTick[0] && rulesTick[1];
            r += "<button id=\"accept\" class=\"btn" + std::string(all && !b ? "" : " off") + "\">" +
                 (b ? "<span class=\"pulse\"></span>Accepting..." : "Accept and continue") + "</button>";
            r += noteHtml() + "</div>";
            add("tick0", 0, 0, !b), add("tick1", 1, 0, !b), add("accept", 2, 0, all && !b);
            break;
        }
        case Page::Code: {
            r += track();
            r += "<div class=\"codehead\">Your code is how friends find you in Direct.</div><div id=\"tiles\">";
            for (int i = 0; i < 4; i++) {
                const bool has = i < static_cast<int>(tag.size());
                r += "<div class=\"cell" + std::string(has ? "" : " empty") +
                     (i == static_cast<int>(tag.size()) ? " cur" : "") + "\">" + (has ? std::string(1, tag[i]) : "") +
                     "</div>";
            }
            r += "<div class=\"hash\">#\?\?\?</div></div><div id=\"keys\">";
            // Nine across: A-I, J-R, S-Y and Delete, then Z and Confirm.
            for (int i = 0; i < 26; i++) {
                const std::string id = std::string("k-") + static_cast<char>('A' + i);
                r += "<div id=\"" + id + "\" class=\"key\">" + std::string(1, static_cast<char>('A' + i)) + "</div>";
                const int row = i < 18 ? i / 9 : i < 25 ? 2 : 3;
                const int col = i < 18 ? i % 9 : i < 25 ? i - 18 : 0;
                items.push_back({id, row, col, !b});
                if (i == 24) {
                    r += "<div id=\"k-del\" class=\"key wide\">Delete</div>";
                    items.push_back({"k-del", 2, 7, !b});
                }
            }
            r += "<div id=\"k-ok\" class=\"key confirm" + std::string(pc_slippi_code_start_valid(tag.c_str()) ? "" : " off") +
                 "\">" + (b ? "Setting it..." : "Confirm code") + "</div>";
            items.push_back({"k-ok", 3, 1, !b && pc_slippi_code_start_valid(tag.c_str())});
            r += "</div><div class=\"fine center\">2 to 4 letters  \xc2\xb7  Slippi adds the numbers "
                 " \xc2\xb7  changing it later costs a one-time payment</div>";
            r += noteHtml();
            break;
        }
        case Page::Done:
            r += track();
            r += "<div id=\"card\"><div class=\"tag\">SIGNED IN</div><div class=\"name\">" + enc(account.display_name) +
                 "</div><div class=\"code\">" + enc(account.connect_code) +
                 "</div><div class=\"sub\">Ready for Unranked and Direct.</div></div>";
            r += "<button id=\"continue\" class=\"btn\">Continue</button>";
            add("continue", 0, 0);
            break;
        }
        document->GetElementById("board")->SetInnerRML(r);
        document->GetElementById("board")->SetClass("register", registering);
    }

    void refresh() {
        build();
        if (items.empty())
            return;
        if (cursor < 0 || !focusWant.empty()) {
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].id == focusWant && items[i].enabled)
                    cursor = static_cast<int>(i);
            if (document != nullptr)
                focusWant.clear();
        }
        cursor = std::clamp(cursor, 0, static_cast<int>(items.size()) - 1);
        if (!items[cursor].enabled) {
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].enabled) {
                    cursor = static_cast<int>(i);
                    break;
                }
        }
        for (size_t i = 0; i < items.size(); i++)
            if (auto* e = document->GetElementById(items[i].id))
                e->SetClass("cursor", static_cast<int>(i) == cursor && items[i].enabled);
    }

    // ---- input -----------------------------------------------------------

    void move(int dir) {
        if (items.empty())
            return;
        const Item& at = items[cursor];
        int best = -1;
        float bestScore = 1e9f;
        for (size_t i = 0; i < items.size(); i++) {
            const Item& it = items[i];
            if (!it.enabled || static_cast<int>(i) == cursor)
                continue;
            const int dr = it.row - at.row, dc = it.col - at.col;
            bool ok = false;
            float score = 0;
            switch (dir) {
            case PC_SLIPPI_IN_UP:
                ok = dr < 0, score = -dr * 10.f + std::abs(dc);
                break;
            case PC_SLIPPI_IN_DOWN:
                ok = dr > 0, score = dr * 10.f + std::abs(dc);
                break;
            case PC_SLIPPI_IN_LEFT:
                ok = dr == 0 && dc < 0, score = -dc;
                break;
            case PC_SLIPPI_IN_RIGHT:
                ok = dr == 0 && dc > 0, score = dc;
                break;
            }
            if (ok && score < bestScore)
                bestScore = score, best = static_cast<int>(i);
        }
        if (best >= 0) {
            cursor = best;
            lbAudioAx_80024030(SFX_MOVE);
            refresh();
        }
    }

    void activate(const std::string& id) {
        if (busy())
            return;
        lbAudioAx_80024030(SFX_CONFIRM);
        if (id == "email") {
            ask(Field::Email);
        } else if (id == "password") {
            ask(Field::Password);
        } else if (id == "signin") {
            note.clear(), badRows.clear(), chipLit = false;
            run(Op::SignIn, email, password);
            refresh();
        } else if (id == "make") {
            const std::string carried = email;
            begin(PC_SLIPPI_MENU_REGISTER);
            regEmail = carried;
        } else if (id == "r-name") {
            ask(Field::Name);
        } else if (id == "r-email") {
            ask(Field::RegEmail);
        } else if (id == "r-password") {
            ask(Field::RegPassword);
        } else if (id == "create") {
            note.clear(), badRows.clear();
            run(Op::Register, name, regEmail, regPassword);
            refresh();
        } else if (id == "instead") {
            const std::string carried = regEmail;
            begin(PC_SLIPPI_MENU_LOGIN);
            email = carried;
            go(Page::Login, carried.empty() ? "email" : "password");
        } else if (id == "opened") {
            note.clear();
            run(Op::Check);
            refresh();
        } else if (id == "resend") {
            note.clear();
            run(Op::SendVerify);
            refresh();
        } else if (id == "tick0" || id == "tick1") {
            rulesTick[id == "tick1"] = !rulesTick[id == "tick1"];
            refresh();
            if (rulesTick[0] && rulesTick[1])
                go(Page::Rules, "accept");
        } else if (id == "accept") {
            note.clear();
            run(Op::AcceptRules);
            refresh();
        } else if (id.size() == 3 && id[0] == 'k' && id[1] == '-' && id[2] >= 'A' && id[2] <= 'Z') {
            if (tag.size() < 4)
                tag += id[2];
            refresh();
        } else if (id == "k-del") {
            if (!tag.empty())
                tag.pop_back();
            refresh();
        } else if (id == "k-ok") {
            confirm_code();
        } else if (id == "continue") {
            leave = true;
        }
    }

    void confirm_code() {
        if (busy() || !pc_slippi_code_start_valid(tag.c_str()))
            return;
        note.clear();
        run(Op::ChooseCode, tag);
        refresh();
    }

    void input(int what) {
        if (!shown || presence < 1.f)
            return;
        switch (what) {
        case PC_SLIPPI_IN_UP:
        case PC_SLIPPI_IN_DOWN:
        case PC_SLIPPI_IN_LEFT:
        case PC_SLIPPI_IN_RIGHT:
            move(what);
            break;
        case PC_SLIPPI_IN_CONFIRM:
            if (!items.empty() && items[cursor].enabled)
                activate(items[cursor].id);
            break;
        case PC_SLIPPI_IN_BACK:
            if (busy())
                break;
            if (page == Page::Code && !tag.empty()) {
                tag.pop_back();
                lbAudioAx_80024030(SFX_BACK);
                refresh();
                break;
            }
            lbAudioAx_80024030(SFX_BACK);
            leave = true;
            break;
        case PC_SLIPPI_IN_X:
            if (page == Page::Login && !busy()) {
                lbAudioAx_80024030(SFX_CONFIRM);
                note.clear();
                run(Op::Reset, email);
                refresh();
            } else if (page == Page::Rules) {
                lbAudioAx_80024030(SFX_CONFIRM);
                SDL_OpenURL(kPrivacyUrl);
            }
            break;
        case PC_SLIPPI_IN_Y:
            if (page == Page::Rules) {
                lbAudioAx_80024030(SFX_CONFIRM);
                SDL_OpenURL(kTermsUrl);
            }
            break;
        case PC_SLIPPI_IN_START:
            if (page == Page::Code)
                confirm_code();
            break;
        }
    }

    // ---- words -----------------------------------------------------------

    const char* title() {
        titleText = page == Page::Login                    ? "Login"
                    : page == Page::Review                 ? "Register"
                                                           : "Setup";
        return titleText.c_str();
    }

    const char* hint() {
        if (!shown)
            return nullptr;
        const std::string id = items.empty() ? std::string() : items[cursor].id;
        switch (page) {
        case Page::Login:
            hintText = busy()                                  ? "Signing in..."
                       : chipLit                               ? "Wrong password? X sends a reset email."
                       : id == "email"                         ? "The email of your Slippi account."
                       : id == "password"                      ? "Your Slippi password."
                       : id == "make"                          ? "Make a new Slippi account."
                                                               : "Sign in with your Slippi account.";
            break;
        case Page::Review:
            hintText = busy() ? "Creating your account..." : id == "r-email" ? "Make sure your email is right."
                       : id == "instead"                                     ? "Already have one? Sign in."
                                                                             : "Check your details, then create it.";
            break;
        case Page::Verify:
            hintText = "Check again after opening the link.";
            break;
        case Page::Rules:
            hintText = "Privacy and Terms open in your browser.";
            break;
        case Page::Code:
            hintText = "START confirms your code.";
            break;
        case Page::Done:
            hintText = "Your account is set up.";
            break;
        }
        return hintText.c_str();
    }

    // ---- showing ---------------------------------------------------------

    void show() {
        if (shown || !load())
            return;
        shown = true;
        presence = 0.f;
        placedPresence = -1.f;
        document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        refresh();
        layout();
    }

    void hide() {
        if (!shown)
            return;
        shown = false;
        document->Hide();
    }

    // The screen scaled into the frame's inside (Name Entry's red frame, the
    // same shape as Erase Data's), with the page's fade and rise.
    void layout() {
        auto* context = aurora::rmlui::get_context();
        auto* screen = document->GetElementById("screen");
        if (context == nullptr || screen == nullptr)
            return;
        const Rml::Vector2i d = context->GetDimensions();
        dims = d;
        if (d == placedDims && presence == placedPresence)
            return;
        placedDims = d;
        placedPresence = presence;
        const float x0 = 0.123f * d.x, y0 = 0.170f * d.y;
        const float w = 0.755f * d.x, h = 0.672f * d.y;
        const float p = std::clamp(presence, 0.f, 1.f);
        const float ease = 1.f - (1.f - p) * (1.f - p);
        scale = std::min(w / kScreenW, h / kScreenH);
        screen->SetProperty("transform", "scale(" + fmt(scale) + ")");
        screen->SetProperty("left", fmt(x0 + (w - kScreenW * scale) / 2) + "px");
        screen->SetProperty("top", fmt(y0 + (h - kScreenH * scale) / 2 + (1.f - ease) * 10.f * scale) + "px");
        screen->SetProperty("opacity", fmt(ease));
    }

    static bool id_of(Rml::Element* e, std::string& out) {
        for (; e != nullptr; e = e->GetParentNode()) {
            const Rml::String& id = e->GetId();
            if (!id.empty() && id != "board" && id != "screen") {
                out = id;
                return true;
            }
        }
        return false;
    }

    int item_index(const std::string& id) const {
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].id == id)
                return static_cast<int>(i);
        return -1;
    }

    void ProcessEvent(Rml::Event& event) override {
        if (!shown || presence < 1.f)
            return;
        std::string id;
        if (!id_of(event.GetTargetElement(), id))
            return;
        const int i = item_index(id);
        switch (event.GetId()) {
        case Rml::EventId::Click:
            if (pointerWas)
                break; // a laser's click arrives as the menus' A too (xr_pointer.c)
            if (id == "chip") {
                input(PC_SLIPPI_IN_X);
            } else if (i >= 0 && items[i].enabled) {
                cursor = i;
                activate(id);
            }
            break;
        case Rml::EventId::Mouseover:
            if (i >= 0 && i != cursor && items[i].enabled) {
                cursor = i;
                lbAudioAx_80024030(SFX_MOVE);
                refresh();
            }
            break;
        default:
            break;
        }
    }

    void update() {
        poll();
        if (!shown)
            return;
        layout();
#ifdef AURORA_ENABLE_OPENXR
        float px = 0.f, py = 0.f;
        bool down = false;
        auto* context = aurora::rmlui::get_context();
        if (context != nullptr && aurora_xr_screen_pointer(&px, &py, &down)) {
            context->ProcessMouseMove(static_cast<int>(px * dims.x), static_cast<int>(py * dims.y), 0);
            if (down && !pointerPressed)
                context->ProcessMouseButtonDown(0, 0);
            else if (!down && pointerPressed)
                context->ProcessMouseButtonUp(0, 0);
            pointerPressed = down;
            pointerWas = true;
        } else if (pointerWas && context != nullptr) {
            if (pointerPressed)
                context->ProcessMouseButtonUp(0, 0);
            context->ProcessMouseLeave();
            pointerWas = pointerPressed = false;
        }
#endif
    }
};

Menu s_menu;

} // namespace

extern "C" void pc_slippi_menu_begin(int mode) { s_menu.begin(mode); }
extern "C" void pc_slippi_menu_end(void) { s_menu.end(); }

extern "C" void pc_slippi_menu_show(bool show) {
    if (show)
        s_menu.show();
    else
        s_menu.hide();
}

extern "C" void pc_slippi_menu_presence(float presence) { s_menu.presence = presence; }
extern "C" void pc_slippi_menu_input(int what) { s_menu.input(what); }

extern "C" bool pc_slippi_menu_keyboard(PcSlippiKeyboard* out) {
    if (!s_menu.keyboardWanted)
        return false;
    s_menu.keyboardWanted = false;
    *out = s_menu.keyboard;
    return true;
}

extern "C" void pc_slippi_menu_keyboard_done(bool confirmed, const char* text) { s_menu.typed(confirmed, text); }

extern "C" bool pc_slippi_menu_leave(void) {
    const bool l = s_menu.leave;
    s_menu.leave = false;
    return l;
}

extern "C" const char* pc_slippi_menu_title(void) { return s_menu.title(); }
extern "C" const char* pc_slippi_menu_hint(void) { return s_menu.hint(); }
extern "C" void pc_slippi_menu_update(void) { s_menu.update(); }
