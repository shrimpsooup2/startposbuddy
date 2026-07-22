#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <geode.custom-keybinds/include/Keybinds.hpp>

using namespace geode::prelude;
using namespace keybinds;

// ---------- link storage (persisted as flat saved values: "link_<key>" -> <key>) ----------

// key format: "o:<id>" for online levels, "l:<name>" for local/editor levels
static std::string levelKey(GJGameLevel* level) {
    if (level->m_levelID.value() > 0) {
        return fmt::format("o:{}", level->m_levelID.value());
    }
    return fmt::format("l:{}", std::string(level->m_levelName));
}

static std::string getLink(std::string const& key) {
    return Mod::get()->getSavedValue<std::string>("link_" + key, "");
}

static void setLink(std::string const& a, std::string const& b) {
    // clear previous links of both sides so pairs stay consistent
    for (auto const& k : { a, b }) {
        auto old = getLink(k);
        if (!old.empty()) Mod::get()->setSavedValue<std::string>("link_" + old, "");
    }
    Mod::get()->setSavedValue<std::string>("link_" + a, b);
    Mod::get()->setSavedValue<std::string>("link_" + b, a);
}

static void clearLink(std::string const& a) {
    auto b = getLink(a);
    if (!b.empty()) Mod::get()->setSavedValue<std::string>("link_" + b, "");
    Mod::get()->setSavedValue<std::string>("link_" + a, "");
}

static GJGameLevel* resolveKey(std::string const& key) {
    if (key.rfind("o:", 0) == 0) {
        return GameLevelManager::sharedState()->getSavedLevel(std::atoi(key.c_str() + 2));
    }
    if (key.rfind("l:", 0) == 0) {
        auto name = key.substr(2);
        for (auto lvl : CCArrayExt<GJGameLevel*>(LocalLevelManager::sharedState()->m_localLevels)) {
            if (std::string(lvl->m_levelName) == name) return lvl;
        }
    }
    return nullptr;
}

static std::string displayName(std::string const& key) {
    if (auto lvl = resolveKey(key)) return std::string(lvl->m_levelName);
    return key;
}

// ---------- level string helpers ----------

static std::string decodedLevelString(GJGameLevel* level) {
    std::string raw = level->m_levelString;
    if (raw.empty()) return raw;
    if (raw.rfind("H4sIA", 0) == 0) {
        return ZipUtils::decompressString(raw, true, 0);
    }
    return raw;
}

// appends (or replaces) a startpos object in a decompressed level string,
// then stores it back compressed
static void applyStartpos(GJGameLevel* level, std::string objects, std::string const& spObj, bool replaceExisting) {
    if (replaceExisting) {
        std::string out;
        out.reserve(objects.size());
        size_t pos = 0;
        bool header = true;
        while (pos < objects.size()) {
            auto end = objects.find(';', pos);
            if (end == std::string::npos) end = objects.size();
            auto tok = objects.substr(pos, end - pos);
            bool isStartpos = !header && (tok.rfind("1,31,", 0) == 0 || tok == "1,31");
            if (!tok.empty() && !isStartpos) {
                out += tok;
                out += ';';
            }
            header = false;
            pos = end + 1;
        }
        objects = std::move(out);
    }
    if (!objects.empty() && objects.back() != ';') objects += ';';
    objects += spObj;
    objects += ';';
    level->m_levelString = ZipUtils::compressString(objects, true, 0);
}

// ---------- linking UI (SP button on level pages) ----------

static std::string s_pending; // level marked for linking this session

static void showLinkPopup(GJGameLevel* level) {
    auto key = levelKey(level);
    auto linked = getLink(key);
    if (!linked.empty()) {
        createQuickPopup(
            "StartPos Link",
            fmt::format("This level is linked with <cy>{}</c>.", displayName(linked)),
            "OK", "Unlink",
            [key](auto, bool btn2) {
                if (btn2) {
                    clearLink(key);
                    Notification::create("Link removed", NotificationIcon::Success)->show();
                }
            }
        );
    } else if (!s_pending.empty() && s_pending != key) {
        auto pending = s_pending;
        createQuickPopup(
            "StartPos Link",
            fmt::format("Link this level with <cy>{}</c>?", displayName(pending)),
            "Cancel", "Link",
            [key, pending](auto, bool btn2) {
                if (btn2) {
                    setLink(key, pending);
                    s_pending.clear();
                    Notification::create("Levels linked! Use the switch keybind in-game.", NotificationIcon::Success)->show();
                }
            }
        );
    } else if (!s_pending.empty() && s_pending == key) {
        createQuickPopup(
            "StartPos Link",
            "This level is <cy>marked</c>.\nOpen the page of the other version and press the <cg>SP</c> button there to link them.",
            "Unmark", "OK",
            [](auto, bool btn2) {
                if (!btn2) s_pending.clear();
            }
        );
    } else {
        createQuickPopup(
            "StartPos Link",
            "No StartPos version linked.\n<cy>Mark</c> this level, then press the <cg>SP</c> button on the other version's page to link the two.",
            "Cancel", "Mark",
            [key](auto, bool btn2) {
                if (btn2) {
                    s_pending = key;
                    Notification::create("Marked! Now open the other level's page.", NotificationIcon::Info)->show();
                }
            }
        );
    }
}

static void addSPButton(CCLayer* layer, CCObject* target, SEL_MenuHandler handler, char const* menuID) {
    auto spr = ButtonSprite::create("SP");
    spr->setScale(0.55f);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, handler);
    btn->setID("sp-link-button"_spr);
    if (auto menu = layer->getChildByID(menuID)) {
        menu->addChild(btn);
        menu->updateLayout();
    } else {
        auto menu = CCMenu::create();
        menu->setPosition({ 25.f, 25.f });
        menu->addChild(btn);
        layer->addChild(menu, 10);
    }
}

struct $modify(SPLevelInfo, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        addSPButton(this, this, menu_selector(SPLevelInfo::onSPLink), "left-side-menu");
        return true;
    }
    void onSPLink(CCObject*) {
        showLinkPopup(m_level);
    }
};

struct $modify(SPEditLevel, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        addSPButton(this, this, menu_selector(SPEditLevel::onSPLink), "level-actions-menu");
        return true;
    }
    void onSPLink(CCObject*) {
        showLinkPopup(m_level);
    }
};

// ---------- keybinds ----------

$execute {
    BindManager::get()->registerBindable({
        "switch"_spr,
        "Switch Level Version",
        "Swap between the current level and its linked StartPos version.",
        { Keybind::create(KEY_Tab, Modifier::None) },
        "StartPos Buddy"
    });
    BindManager::get()->registerBindable({
        "add-startpos"_spr,
        "Add StartPos Here",
        "Adds a StartPos at your current position to a local copy of the level (creates and links the copy automatically). Best used in practice mode.",
        { Keybind::create(KEY_E, Modifier::None) },
        "StartPos Buddy"
    });
}

// ---------- in-game logic ----------

struct $modify(SPPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        this->template addEventListener<InvokeBindFilter>([this](InvokeBindEvent* event) {
            if (event->isDown()) this->switchVersion();
            return ListenerResult::Propagate;
        }, "switch"_spr);

        this->template addEventListener<InvokeBindFilter>([this](InvokeBindEvent* event) {
            if (event->isDown()) this->addStartpos();
            return ListenerResult::Propagate;
        }, "add-startpos"_spr);

        return true;
    }

    void switchVersion() {
        auto linked = getLink(levelKey(m_level));
        if (linked.empty()) {
            Notification::create("No StartPos version linked to this level", NotificationIcon::Warning)->show();
            return;
        }
        auto other = resolveKey(linked);
        if (!other) {
            Notification::create("Linked level not found (deleted or not saved)", NotificationIcon::Error)->show();
            return;
        }
        if (std::string(other->m_levelString).empty()) {
            Notification::create("Linked level has no data - open it once first", NotificationIcon::Error)->show();
            return;
        }
        auto scene = PlayLayer::scene(other, false, false);
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
    }

    void addStartpos() {
        auto p = m_player1;
        auto decomp = decodedLevelString(m_level);
        if (decomp.empty()) {
            Notification::create("Couldn't read this level's data", NotificationIcon::Error)->show();
            return;
        }

        int mode = 0;
        if (p->m_isShip) mode = 1;
        else if (p->m_isBall) mode = 2;
        else if (p->m_isBird) mode = 3;
        else if (p->m_isDart) mode = 4;
        else if (p->m_isRobot) mode = 5;
        else if (p->m_isSpider) mode = 6;
        else if (p->m_isSwing) mode = 7;

        int speed = 0;
        float s = p->m_playerSpeed;
        if (s < 0.8f) speed = 1;        // 0.5x
        else if (s < 1.0f) speed = 0;   // 1x
        else if (s < 1.2f) speed = 2;   // 2x
        else if (s < 1.45f) speed = 3;  // 3x
        else speed = 4;                 // 4x

        // startpos object: id 31, embeds its settings as kA keys
        auto spObj = fmt::format(
            "1,31,2,{:.2f},3,{:.2f},kA2,{},kA3,{},kA4,{},kA8,{},kA11,{}",
            p->getPositionX(), p->getPositionY(),
            mode,
            p->m_vehicleSize < 1.f ? 1 : 0,   // mini
            speed,
            (m_player2 && m_player2->isVisible()) ? 1 : 0, // dual
            p->m_isUpsideDown ? 1 : 0          // flipped gravity
        );

        bool replace = Mod::get()->getSettingValue<bool>("replace-existing");

        if (m_level->m_levelType == GJLevelType::Editor) {
            // already a local level: add the startpos to it directly
            applyStartpos(m_level, decomp, spObj, replace);
            Notification::create("StartPos added - takes effect when the level reloads", NotificationIcon::Success)->show();
        } else {
            // online level: create/update a local "<name> SP" copy and link it
            auto copyName = std::string(m_level->m_levelName) + " SP";
            GJGameLevel* copy = nullptr;
            for (auto lvl : CCArrayExt<GJGameLevel*>(LocalLevelManager::sharedState()->m_localLevels)) {
                if (std::string(lvl->m_levelName) == copyName) { copy = lvl; break; }
            }
            bool created = false;
            std::string base = decomp;
            if (!copy) {
                copy = GJGameLevel::create();
                copy->m_levelName = copyName;
                copy->m_levelType = GJLevelType::Editor;
                copy->m_audioTrack = m_level->m_audioTrack;
                copy->m_songID = m_level->m_songID;
                copy->m_songIDs = m_level->m_songIDs;
                copy->m_sfxIDs = m_level->m_sfxIDs;
                LocalLevelManager::sharedState()->m_localLevels->insertObject(copy, 0);
                created = true;
            } else {
                auto existing = decodedLevelString(copy);
                if (!existing.empty()) base = existing;
            }
            applyStartpos(copy, base, spObj, replace);
            setLink(levelKey(m_level), levelKey(copy));
            Notification::create(
                created
                    ? fmt::format("Created '{}' - press the switch keybind to play it", copyName)
                    : "StartPos copy updated",
                NotificationIcon::Success
            )->show();
        }
    }
};
