// Claude Replay Bot - a simple replay bot (macro recorder / player) for Geode.
// Everything lives in this one file so it's easy to edit from a phone.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Shared state + .crp file handling
// ---------------------------------------------------------------------------
namespace crp {

    enum class Mode { Idle, Recording, Playing };

    struct Input {
        int step;
        int button;
        bool down;
        bool player1;
    };

    struct State {
        Mode mode = Mode::Idle;
        std::vector<Input> macro;
        size_t playIdx = 0;
        int step = 0;
        bool injecting = false;

        std::string macroName = "(none)";
        std::string macroLevel;

        std::vector<std::string> files;
        int fileIdx = 0;

        // settings
        bool autoSave = true;
        bool frameFix = false;
        bool showCounter = true;
        float speed = 1.f;
        float fixedFps = 60.f;
    };

    inline State& S() {
        static State s;
        return s;
    }

    inline const ccColor3B kOrange = {217, 119, 87};
    constexpr float kPanelW = 360.f;
    constexpr float kPanelH = 270.f;

    inline const char* modeText() {
        switch (S().mode) {
            case Mode::Recording: return "REC";
            case Mode::Playing: return "PLAY";
            default: return "Idle";
        }
    }

    inline void loadSettings() {
        auto mod = Mod::get();
        auto& s = S();
        s.autoSave = mod->getSavedValue<bool>("auto-save", true);
        s.frameFix = mod->getSavedValue<bool>("frame-fix", false);
        s.showCounter = mod->getSavedValue<bool>("show-counter", true);
        s.speed = mod->getSavedValue<float>("speed", 1.f);
        s.fixedFps = mod->getSavedValue<float>("fixed-fps", 60.f);
        if (s.speed < 0.05f) s.speed = 1.f;
        if (s.fixedFps < 30.f) s.fixedFps = 60.f;
    }

    inline void saveSettings() {
        auto mod = Mod::get();
        auto& s = S();
        mod->setSavedValue<bool>("auto-save", s.autoSave);
        mod->setSavedValue<bool>("frame-fix", s.frameFix);
        mod->setSavedValue<bool>("show-counter", s.showCounter);
        mod->setSavedValue<float>("speed", s.speed);
        mod->setSavedValue<float>("fixed-fps", s.fixedFps);
    }

    inline std::filesystem::path macroDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    inline void refreshFiles() {
        auto& s = S();
        s.files.clear();
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
            if (e.is_regular_file() && e.path().extension() == ".crp") {
                s.files.push_back(e.path().filename().string());
            }
        }
        std::sort(s.files.begin(), s.files.end());
        if (s.fileIdx >= static_cast<int>(s.files.size())) {
            s.fileIdx = std::max(0, static_cast<int>(s.files.size()) - 1);
        }
    }

    // .crp format (plain text):
    //   CRP1
    //   level=<name>
    //   count=<n>
    //   <step> <button> <down 0/1> <player1 0/1>   (one line per input)
    inline std::string serialize() {
        auto& s = S();
        std::ostringstream o;
        o << "CRP1\n";
        o << "level=" << s.macroLevel << "\n";
        o << "count=" << s.macro.size() << "\n";
        for (auto const& i : s.macro) {
            o << i.step << " " << i.button << " " << (i.down ? 1 : 0) << " " << (i.player1 ? 1 : 0) << "\n";
        }
        return o.str();
    }

    inline bool parse(std::string const& text, std::vector<Input>& out, std::string& level) {
        std::istringstream in(text);
        std::string line;
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != "CRP1") return false;

        out.clear();
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line.rfind("level=", 0) == 0) {
                level = line.substr(6);
                continue;
            }
            if (line.rfind("count=", 0) == 0) continue;

            std::istringstream ls(line);
            int step = 0, button = 0, down = 0, p1 = 1;
            if (ls >> step >> button >> down >> p1) {
                out.push_back(Input{step, button, down != 0, p1 != 0});
            }
        }
        std::stable_sort(out.begin(), out.end(), [](Input const& a, Input const& b) {
            return a.step < b.step;
        });
        return true;
    }

    inline std::string sanitize(std::string s) {
        for (auto& c : s) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
        }
        if (s.empty()) s = "macro";
        return s;
    }

    inline bool saveMacro() {
        auto& s = S();
        if (s.macro.empty()) return false;

        std::time_t t = std::time(nullptr);
        char buf[32] = {0};
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));

        std::string fname = sanitize(s.macroLevel) + "_" + buf + ".crp";
        auto res = utils::file::writeString(macroDir() / fname, serialize());
        if (res.isErr()) return false;

        s.macroName = fname;
        refreshFiles();
        return true;
    }

    inline bool loadMacro(std::string const& fname) {
        auto res = utils::file::readString(macroDir() / fname);
        if (res.isErr()) return false;

        std::vector<Input> in;
        std::string lvl;
        if (!parse(res.unwrap(), in, lvl)) return false;

        auto& s = S();
        s.macro = std::move(in);
        s.macroLevel = lvl;
        s.macroName = fname;
        s.playIdx = 0;
        return true;
    }

    inline void notify(std::string const& msg, bool ok = true) {
        Notification::create(msg, ok ? NotificationIcon::Success : NotificationIcon::Error, 1.5f)->show();
    }

}

// ---------------------------------------------------------------------------
// Game hooks
// ---------------------------------------------------------------------------

class $modify(CRPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto& s = crp::S();
        s.mode = crp::Mode::Idle;
        s.step = 0;
        s.playIdx = 0;

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto label = CCLabelBMFont::create("", "chatFont.fnt");
        label->setID("step-label"_spr);
        label->setAnchorPoint({0.f, 1.f});
        label->setScale(0.6f);
        label->setOpacity(170);
        label->setColor(crp::kOrange);
        label->setPosition({8.f, win.height - 6.f});
        this->addChild(label, 1000);
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& s = crp::S();
        s.step = 0;
        s.playIdx = 0;
        if (s.mode == crp::Mode::Recording) s.macro.clear();
    }

    void levelComplete() {
        auto& s = crp::S();
        if (s.mode == crp::Mode::Recording) {
            s.mode = crp::Mode::Idle;
            if (s.autoSave) {
                s.macroLevel = std::string(m_level->m_levelName);
                if (crp::saveMacro()) {
                    crp::notify("Macro auto-saved: " + s.macroName);
                } else {
                    crp::notify("Auto-save failed", false);
                }
            }
        } else if (s.mode == crp::Mode::Playing) {
            s.mode = crp::Mode::Idle;
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        crp::S().mode = crp::Mode::Idle;
        PlayLayer::onQuit();
    }
};

class $modify(CRBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        auto pl = PlayLayer::get();
        auto& s = crp::S();

        // only touch the real PlayLayer (not the editor's playtest)
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        // playback: inject the recorded inputs for this physics step
        if (s.mode == crp::Mode::Playing) {
            while (s.playIdx < s.macro.size() && s.macro[s.playIdx].step <= s.step) {
                auto const in = s.macro[s.playIdx];
                s.injecting = true;
                this->handleButton(in.down, in.button, in.player1);
                s.injecting = false;
                s.playIdx++;
            }
        }

        GJBaseGameLayer::processCommands(dt);
        s.step++;

        if (s.step % 8 == 0) {
            if (auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("step-label"_spr))) {
                if (s.showCounter) {
                    lbl->setVisible(true);
                    lbl->setString(fmt::format("{}  step {}", crp::modeText(), s.step).c_str());
                } else {
                    lbl->setVisible(false);
                }
            }
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        auto& s = crp::S();

        if (pl && static_cast<GJBaseGameLayer*>(pl) == this && !s.injecting) {
            // while a macro plays, ignore real taps
            if (s.mode == crp::Mode::Playing) return;
            if (s.mode == crp::Mode::Recording) {
                s.macro.push_back(crp::Input{s.step, button, down, isPlayer1});
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// Speedhack + Frame Fix live here (only active inside a level)
class $modify(CRScheduler, CCScheduler) {
    void update(float dt) {
        auto& s = crp::S();
        if (PlayLayer::get()) {
            if (s.frameFix) dt = 1.f / s.fixedFps;
            dt *= s.speed;
        }
        CCScheduler::update(dt);
    }
};

// ---------------------------------------------------------------------------
// Floating draggable icon + transparent menu
// ---------------------------------------------------------------------------

class CRPOverlay : public CCLayer {
protected:
    CCSprite* m_icon = nullptr;
    CCNode* m_panel = nullptr;
    CCLabelBMFont* m_statusLbl = nullptr;
    CCLabelBMFont* m_macroLbl = nullptr;
    CCLabelBMFont* m_speedLbl = nullptr;
    CCLabelBMFont* m_fpsLbl = nullptr;

    bool m_dragging = false;
    bool m_moved = false;
    CCPoint m_touchStart = {0.f, 0.f};
    CCPoint m_iconStart = {0.f, 0.f};

public:
    static CRPOverlay* create() {
        auto ret = new CRPOverlay();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        auto win = CCDirector::sharedDirector()->getWinSize();

        m_icon = CCSprite::create("logo.png"_spr);
        if (!m_icon) m_icon = CCSprite::createWithSpriteFrameName("GJ_button_01.png");
        float w = m_icon->getContentSize().width;
        m_icon->setScale(w > 0.f ? 38.f / w : 1.f);
        m_icon->setPosition({
            Mod::get()->getSavedValue<float>("icon-x", 36.f),
            Mod::get()->getSavedValue<float>("icon-y", win.height / 2.f)
        });
        this->addChild(m_icon, 5);

        this->setTouchEnabled(true);
        this->schedule(schedule_selector(CRPOverlay::tick), 0.1f);
        return true;
    }

    void registerWithTouchDispatcher() override {
        // after menus/sliders (-128) but before the game (0)
        CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -100, true);
    }

    // ---- touch: drag the icon, tap to open/close the menu ----
    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        auto loc = t->getLocation();

        if (m_panel) {
            auto lp = m_panel->convertToNodeSpace(loc);
            CCRect panelRect(-crp::kPanelW / 2.f, -crp::kPanelH / 2.f, crp::kPanelW, crp::kPanelH);
            if (panelRect.containsPoint(lp)) return true; // swallow so the game doesn't get the tap
        }

        auto p = this->convertToNodeSpace(loc);
        auto bb = m_icon->boundingBox();
        CCRect hit(bb.origin.x - 10.f, bb.origin.y - 10.f, bb.size.width + 20.f, bb.size.height + 20.f);
        if (!hit.containsPoint(p)) return false;

        m_dragging = true;
        m_moved = false;
        m_touchStart = p;
        m_iconStart = m_icon->getPosition();
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (!m_dragging) return;
        auto p = this->convertToNodeSpace(t->getLocation());
        float dx = p.x - m_touchStart.x;
        float dy = p.y - m_touchStart.y;
        if (std::hypot(dx, dy) > 8.f) m_moved = true;
        if (!m_moved) return;

        auto win = CCDirector::sharedDirector()->getWinSize();
        float x = std::clamp(m_iconStart.x + dx, 20.f, win.width - 20.f);
        float y = std::clamp(m_iconStart.y + dy, 20.f, win.height - 20.f);
        m_icon->setPosition({x, y});
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (!m_dragging) return;
        m_dragging = false;
        if (m_moved) {
            Mod::get()->setSavedValue<float>("icon-x", m_icon->getPositionX());
            Mod::get()->setSavedValue<float>("icon-y", m_icon->getPositionY());
        } else {
            this->togglePanel();
        }
    }

    void ccTouchCancelled(CCTouch* t, CCEvent* e) override {
        this->ccTouchEnded(t, e);
    }

    // ---- status refresh ----
    void tick(float) {
        this->updateStatus();
    }

    void updateStatus() {
        auto& s = crp::S();
        if (m_statusLbl) {
            m_statusLbl->setString(
                fmt::format("{} | {} | {} inputs", crp::modeText(), s.macroName, s.macro.size()).c_str()
            );
            m_statusLbl->limitLabelWidth(330.f, 0.55f, 0.2f);
        }
        if (m_macroLbl) {
            std::string txt = s.files.empty()
                ? std::string("no .crp files")
                : fmt::format("({}/{}) {}", s.fileIdx + 1, s.files.size(), s.files[s.fileIdx]);
            m_macroLbl->setString(txt.c_str());
            m_macroLbl->limitLabelWidth(170.f, 0.5f, 0.15f);
        }
    }

    // ---- panel ----
    void togglePanel() {
        if (m_panel) this->closePanel();
        else this->openPanel();
    }

    void closePanel() {
        if (!m_panel) return;
        m_panel->removeFromParentAndCleanup(true);
        m_panel = nullptr;
        m_statusLbl = nullptr;
        m_macroLbl = nullptr;
        m_speedLbl = nullptr;
        m_fpsLbl = nullptr;
    }

    CCMenuItemSpriteExtra* addBtn(CCMenu* menu, char const* text, int width, CCPoint pos, SEL_MenuHandler cb) {
        auto spr = ButtonSprite::create(text, width, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, nullptr, this, cb);
        btn->setPosition(pos);
        menu->addChild(btn);
        return btn;
    }

    void addToggle(CCMenu* menu, char const* text, CCPoint pos, bool on, SEL_MenuHandler cb) {
        auto t = CCMenuItemToggler::createWithStandardSprites(this, cb, 0.55f);
        t->toggle(on);
        t->setPosition(pos);
        menu->addChild(t);

        auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
        lbl->setScale(0.4f);
        lbl->setAnchorPoint({0.f, 0.5f});
        lbl->setPosition({pos.x + 16.f, pos.y});
        m_panel->addChild(lbl, 2);
    }

    void openPanel() {
        crp::refreshFiles();
        auto& s = crp::S();
        auto win = CCDirector::sharedDirector()->getWinSize();

        m_panel = CCNode::create();
        m_panel->setPosition({win.width / 2.f, win.height / 2.f});
        this->addChild(m_panel, 10);

        // transparent orange rounded background
        auto bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize({crp::kPanelW, crp::kPanelH});
        bg->setColor(crp::kOrange);
        bg->setOpacity(185);
        m_panel->addChild(bg, 0);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setTouchPriority(-600);
        m_panel->addChild(menu, 5);

        auto title = CCLabelBMFont::create("Claude Replay", "bigFont.fnt");
        title->setScale(0.6f);
        title->setPosition({0.f, 112.f});
        m_panel->addChild(title, 2);

        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        closeSpr->setScale(0.6f);
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, nullptr, this, menu_selector(CRPOverlay::onClosePanel));
        closeBtn->setPosition({163.f, 115.f});
        menu->addChild(closeBtn);

        m_statusLbl = CCLabelBMFont::create("", "chatFont.fnt");
        m_statusLbl->setPosition({0.f, 88.f});
        m_panel->addChild(m_statusLbl, 2);

        // row 1: record / play / stop
        this->addBtn(menu, "Record", 90, {-105.f, 52.f}, menu_selector(CRPOverlay::onRecord));
        this->addBtn(menu, "Play", 90, {0.f, 52.f}, menu_selector(CRPOverlay::onPlay));
        this->addBtn(menu, "Stop", 90, {105.f, 52.f}, menu_selector(CRPOverlay::onStop));

        // row 2: macro picker
        auto la = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        la->setScale(0.5f);
        auto lb = CCMenuItemSpriteExtra::create(la, nullptr, this, menu_selector(CRPOverlay::onPrev));
        lb->setPosition({-160.f, 16.f});
        menu->addChild(lb);

        auto ra = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        ra->setScale(0.5f);
        ra->setFlipX(true);
        auto rb = CCMenuItemSpriteExtra::create(ra, nullptr, this, menu_selector(CRPOverlay::onNext));
        rb->setPosition({40.f, 16.f});
        menu->addChild(rb);

        m_macroLbl = CCLabelBMFont::create("", "chatFont.fnt");
        m_macroLbl->setPosition({-60.f, 16.f});
        m_panel->addChild(m_macroLbl, 2);

        this->addBtn(menu, "Load", 70, {125.f, 16.f}, menu_selector(CRPOverlay::onLoad));

        // row 3: toggles
        this->addToggle(menu, "Auto-save", {-165.f, -20.f}, s.autoSave, menu_selector(CRPOverlay::onAutoSave));
        this->addToggle(menu, "Frame Fix", {-50.f, -20.f}, s.frameFix, menu_selector(CRPOverlay::onFrameFix));
        this->addToggle(menu, "Counter", {70.f, -20.f}, s.showCounter, menu_selector(CRPOverlay::onCounter));

        // row 4: speed slider
        m_speedLbl = CCLabelBMFont::create("", "bigFont.fnt");
        m_speedLbl->setScale(0.4f);
        m_speedLbl->setAnchorPoint({0.f, 0.5f});
        m_speedLbl->setPosition({-165.f, -52.f});
        m_panel->addChild(m_speedLbl, 2);

        auto speedSlider = Slider::create(this, menu_selector(CRPOverlay::onSpeed), 0.55f);
        speedSlider->setPosition({95.f, -52.f});
        speedSlider->setValue((s.speed - 0.1f) / 2.9f);
        m_panel->addChild(speedSlider, 3);

        // row 5: frame-fix fps slider
        m_fpsLbl = CCLabelBMFont::create("", "bigFont.fnt");
        m_fpsLbl->setScale(0.4f);
        m_fpsLbl->setAnchorPoint({0.f, 0.5f});
        m_fpsLbl->setPosition({-165.f, -82.f});
        m_panel->addChild(m_fpsLbl, 2);

        auto fpsSlider = Slider::create(this, menu_selector(CRPOverlay::onFps), 0.55f);
        fpsSlider->setPosition({95.f, -82.f});
        fpsSlider->setValue((s.fixedFps - 30.f) / 210.f);
        m_panel->addChild(fpsSlider, 3);

        this->updateSliderLabels();

        // row 6: save / refresh / folder
        this->addBtn(menu, "Save", 70, {-110.f, -115.f}, menu_selector(CRPOverlay::onSave));
        this->addBtn(menu, "Refresh", 80, {-20.f, -115.f}, menu_selector(CRPOverlay::onRefresh));
        this->addBtn(menu, "Folder", 80, {80.f, -115.f}, menu_selector(CRPOverlay::onFolder));

        this->updateStatus();
    }

    void updateSliderLabels() {
        auto& s = crp::S();
        if (m_speedLbl) m_speedLbl->setString(fmt::format("Speed: {:.2f}x", s.speed).c_str());
        if (m_fpsLbl) m_fpsLbl->setString(fmt::format("Frame Fix FPS: {}", static_cast<int>(s.fixedFps)).c_str());
    }

    // ---- callbacks ----
    void onClosePanel(CCObject*) {
        this->closePanel();
    }

    void onRecord(CCObject*) {
        auto pl = PlayLayer::get();
        auto& s = crp::S();
        if (!pl) return crp::notify("Open a level first", false);
        if (pl->m_isPracticeMode) return crp::notify("Turn off practice mode to record", false);

        s.macro.clear();
        s.playIdx = 0;
        s.macroName = "(recording)";
        s.macroLevel = std::string(pl->m_level->m_levelName);
        s.mode = crp::Mode::Recording;
        this->closePanel();
        pl->resetLevel();
        crp::notify("Recording started");
    }

    void onPlay(CCObject*) {
        auto pl = PlayLayer::get();
        auto& s = crp::S();
        if (!pl) return crp::notify("Open a level first", false);
        if (s.macro.empty()) return crp::notify("Record or load a macro first", false);
        if (pl->m_isPracticeMode) return crp::notify("Turn off practice mode to play a macro", false);

        s.playIdx = 0;
        s.mode = crp::Mode::Playing;
        this->closePanel();
        pl->resetLevel();
        crp::notify("Playing macro");
    }

    void onStop(CCObject*) {
        auto& s = crp::S();
        if (s.mode == crp::Mode::Idle) return crp::notify("Nothing to stop", false);
        bool wasRecording = s.mode == crp::Mode::Recording;
        s.mode = crp::Mode::Idle;
        if (wasRecording) {
            if (s.autoSave && !s.macro.empty()) {
                if (crp::saveMacro()) crp::notify("Stopped and saved: " + s.macroName);
                else crp::notify("Stopped, but saving failed", false);
            } else {
                crp::notify("Recording stopped (tap Save to keep it)");
            }
        } else {
            crp::notify("Playback stopped");
        }
        this->updateStatus();
    }

    void onSave(CCObject*) {
        auto& s = crp::S();
        if (s.macro.empty()) return crp::notify("No macro to save", false);
        if (s.macroLevel.empty()) s.macroLevel = "macro";
        if (crp::saveMacro()) crp::notify("Saved: " + s.macroName);
        else crp::notify("Save failed", false);
        this->updateStatus();
    }

    void onPrev(CCObject*) {
        auto& s = crp::S();
        if (s.files.empty()) return;
        s.fileIdx = (s.fileIdx - 1 + static_cast<int>(s.files.size())) % static_cast<int>(s.files.size());
        this->updateStatus();
    }

    void onNext(CCObject*) {
        auto& s = crp::S();
        if (s.files.empty()) return;
        s.fileIdx = (s.fileIdx + 1) % static_cast<int>(s.files.size());
        this->updateStatus();
    }

    void onLoad(CCObject*) {
        auto& s = crp::S();
        if (s.files.empty()) return crp::notify("No .crp files found. Tap Folder for the path.", false);
        if (!crp::loadMacro(s.files[s.fileIdx])) return crp::notify("Couldn't read that .crp file", false);

        crp::notify("Loaded " + s.macroName);
        if (auto pl = PlayLayer::get()) {
            if (!s.macroLevel.empty() && s.macroLevel != std::string(pl->m_level->m_levelName)) {
                crp::notify("Heads up: macro was recorded on \"" + s.macroLevel + "\"", false);
            }
        }
        this->updateStatus();
    }

    void onRefresh(CCObject*) {
        crp::refreshFiles();
        this->updateStatus();
        crp::notify("Found " + std::to_string(crp::S().files.size()) + " macro(s)");
    }

    void onFolder(CCObject*) {
        auto path = crp::macroDir().string();
        FLAlertLayer::create(
            "Macros folder",
            "Put your .crp files here, then tap Refresh:\n" + path,
            "OK"
        )->show();
    }

    void onAutoSave(CCObject* sender) {
        crp::S().autoSave = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        crp::saveSettings();
    }

    void onFrameFix(CCObject* sender) {
        crp::S().frameFix = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        crp::saveSettings();
    }

    void onCounter(CCObject* sender) {
        crp::S().showCounter = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        crp::saveSettings();
    }

    void onSpeed(CCObject* sender) {
        float v = static_cast<SliderThumb*>(sender)->getValue();
        float sp = 0.1f + v * 2.9f;
        crp::S().speed = std::round(sp * 100.f) / 100.f;
        crp::saveSettings();
        this->updateSliderLabels();
    }

    void onFps(CCObject* sender) {
        float v = static_cast<SliderThumb*>(sender)->getValue();
        crp::S().fixedFps = std::round(30.f + v * 210.f);
        crp::saveSettings();
        this->updateSliderLabels();
    }
};

// Create the overlay once and keep it alive across every scene
class $modify(CRPMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        static bool created = false;
        if (!created) {
            created = true;
            auto overlay = CRPOverlay::create();
            if (overlay) {
                overlay->setID("overlay"_spr);
                overlay->setZOrder(10000);
                SceneManager::get()->keepAcrossScenes(overlay);
            }
        }
        return true;
    }
};

$on_mod(Loaded) {
    crp::loadSettings();
    crp::refreshFiles();
}
