#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/base64.hpp>
#include <miniz.h>

using namespace geode::prelude;

// ---------- gzip/zlib helpers ----------
// GD level strings are URL-safe base64 of a gzip deflate stream. RobTop's
// ZipUtils::(de)compressString is inlined on Windows (no callable address),
// so we do it ourselves with miniz. GD's inflate accepts both zlib and gzip,
// so we WRITE zlib (trivial with miniz) and READ either.

static std::optional<std::string> inflateLevelString(std::string const& b64) {
    auto res = utils::base64::decodeString(b64, utils::base64::Base64Variant::Url);
    if (res.isErr()) return std::nullopt;
    std::string raw = res.unwrap();
    if (raw.empty()) return std::nullopt;

    auto p = reinterpret_cast<unsigned char const*>(raw.data());
    size_t n = raw.size();
    size_t skip = 0;
    int flags = 0;

    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) {
        // gzip: skip the header (usually 10 bytes), then raw deflate
        unsigned char flg = (n >= 4) ? p[3] : 0;
        skip = 10;
        if (flg & 4) { // FEXTRA
            if (skip + 2 > n) return std::nullopt;
            size_t xlen = p[skip] | (p[skip + 1] << 8);
            skip += 2 + xlen;
        }
        if (flg & 8)  { while (skip < n && p[skip]) skip++; skip++; } // FNAME
        if (flg & 16) { while (skip < n && p[skip]) skip++; skip++; } // FCOMMENT
        if (flg & 2)  { skip += 2; }                                  // FHCRC
    } else if (n >= 1 && p[0] == 0x78) {
        flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    }
    if (skip >= n) return std::nullopt;

    size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(p + skip, n - skip, &outLen, flags);
    if (!out) return std::nullopt;
    std::string result(static_cast<char*>(out), outLen);
    mz_free(out);
    return result;
}

static std::optional<std::string> deflateLevelString(std::string const& data) {
    // GD level strings are gzip (not zlib). Produce a real gzip container so
    // RobTop's inflate accepts it: 10-byte header + raw deflate + crc32 + isize.
    int flags = static_cast<int>(
        tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -15, MZ_DEFAULT_STRATEGY)
    );
    size_t deflatedLen = 0;
    void* deflated = tdefl_compress_mem_to_heap(data.data(), data.size(), &deflatedLen, flags);
    if (!deflated) return std::nullopt;

    std::string gz;
    gz.reserve(deflatedLen + 18);
    const unsigned char header[10] = { 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff };
    gz.append(reinterpret_cast<char const*>(header), 10);
    gz.append(static_cast<char const*>(deflated), deflatedLen);
    mz_free(deflated);

    unsigned int crc = static_cast<unsigned int>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<unsigned char const*>(data.data()), data.size())
    );
    unsigned int isize = static_cast<unsigned int>(data.size());
    for (int i = 0; i < 4; i++) gz.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; i++) gz.push_back(static_cast<char>((isize >> (8 * i)) & 0xff));

    return utils::base64::encode(gz, utils::base64::Base64Variant::Url);
}

// takes a (compressed OR already-plain) level string, adds a startpos object,
// returns the new compressed level string
static std::optional<std::string> withStartpos(std::string const& source, std::string const& spObj, bool replaceExisting) {
    std::string objects;
    if (auto inflated = inflateLevelString(source)) {
        objects = *inflated;
    } else if (source.find(';') != std::string::npos && source.find(',') != std::string::npos) {
        objects = source; // already decompressed
    } else {
        return std::nullopt;
    }

    if (replaceExisting) {
        std::string kept;
        size_t pos = 0;
        bool isHeader = true;
        while (pos <= objects.size()) {
            size_t end = objects.find(';', pos);
            if (end == std::string::npos) end = objects.size();
            auto tok = objects.substr(pos, end - pos);
            bool isStartpos = !isHeader && (tok.rfind("1,31,", 0) == 0 || tok == "1,31");
            if (!tok.empty() && !isStartpos) {
                kept += tok;
                kept += ';';
            }
            isHeader = false;
            if (end == objects.size()) break;
            pos = end + 1;
        }
        objects = std::move(kept);
    }

    if (!objects.empty() && objects.back() != ';') objects += ';';
    objects += spObj;
    objects += ';';
    return deflateLevelString(objects);
}

// ---------- link storage (saved values: "link_<key>" -> "<key>") ----------
// key: "o:<id>" for online levels, "l:<name>" for local/editor levels

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

static GJGameLevel* findLocalByName(std::string const& name) {
    for (auto lvl : CCArrayExt<GJGameLevel*>(LocalLevelManager::sharedState()->m_localLevels)) {
        if (std::string(lvl->m_levelName) == name) return lvl;
    }
    return nullptr;
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
        auto fallback = CCMenu::create();
        fallback->setPosition({ 25.f, 25.f });
        fallback->addChild(btn);
        layer->addChild(fallback, 10);
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

// ---------- in-game logic ----------

struct $modify(SPPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        this->addEventListener(
            KeybindSettingPressedEventV3(Mod::get(), "switch"),
            [this](Keybind const&, bool down, bool repeat, double) {
                if (down && !repeat) this->switchVersion();
                return ListenerResult::Propagate;
            }
        );
        this->addEventListener(
            KeybindSettingPressedEventV3(Mod::get(), "add-startpos"),
            [this](Keybind const&, bool down, bool repeat, double) {
                if (down && !repeat) this->addStartpos();
                return ListenerResult::Propagate;
            }
        );
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
        // defer to the next frame so the current PlayLayer tears down cleanly
        // (replacing the scene mid-update breaks other mods' save-on-exit hooks)
        Loader::get()->queueInMainThread([other]() {
            auto scene = PlayLayer::scene(other, false, false);
            CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
        });
    }

    void addStartpos() {
        auto p = m_player1;
        if (!p) return;

        int mode = 0;
        if (p->m_isShip) mode = 1;
        else if (p->m_isBall) mode = 2;
        else if (p->m_isBird) mode = 3;
        else if (p->m_isDart) mode = 4;
        else if (p->m_isRobot) mode = 5;
        else if (p->m_isSpider) mode = 6;
        else if (p->m_isSwing) mode = 7;

        int mini = (p->m_vehicleSize < 1.0f) ? 1 : 0;

        int speed;
        float s = p->m_playerSpeed;
        if (s < 0.8f) speed = 1;        // 0.5x
        else if (s < 1.1f) speed = 0;   // 1x
        else if (s < 1.3f) speed = 2;   // 2x
        else if (s < 1.6f) speed = 3;   // 3x
        else speed = 4;                 // 4x

        int dual = (m_player2 && m_player2->isVisible()) ? 1 : 0;

        auto spObj = fmt::format(
            "1,31,2,{:.2f},3,{:.2f},kA2,{},kA3,{},kA4,{},kA8,{}",
            p->getPositionX(), p->getPositionY(), mode, mini, speed, dual
        );

        bool replace = Mod::get()->getSettingValue<bool>("replace-existing");

        // Figure out which LOCAL level to add the StartPos into. We always
        // accumulate StartPoses in a local (editable) level:
        //   - if we're already playing a local level, use it
        //   - else if this level already has a linked local level, use that
        //     one (building on its current contents)
        //   - else create a fresh "<name> SP" copy of this level and link it
        GJGameLevel* target = nullptr;

        if (m_level->m_levelType == GJLevelType::Editor) {
            target = m_level;
        } else {
            if (auto linkedKey = getLink(levelKey(m_level)); !linkedKey.empty()) {
                if (auto linked = resolveKey(linkedKey);
                    linked && linked->m_levelType == GJLevelType::Editor) {
                    target = linked;
                }
            }
            if (!target) {
                auto copyName = std::string(m_level->m_levelName) + " SP";
                if (auto existing = findLocalByName(copyName)) {
                    target = existing;
                    setLink(levelKey(m_level), levelKey(target));
                }
            }
        }

        if (!target) {
            // first time for this online level: create the linked local copy
            auto copyName = std::string(m_level->m_levelName) + " SP";
            auto result = withStartpos(std::string(m_level->m_levelString), spObj, replace);
            if (!result) {
                Notification::create("Couldn't read this level's data", NotificationIcon::Error)->show();
                return;
            }
            auto copy = GJGameLevel::create();
            copy->m_levelName = copyName;
            copy->m_levelType = GJLevelType::Editor;
            copy->m_audioTrack = m_level->m_audioTrack;
            copy->m_songID = m_level->m_songID;
            copy->m_songIDs = m_level->m_songIDs;
            copy->m_sfxIDs = m_level->m_sfxIDs;
            copy->m_levelString = *result;
            LocalLevelManager::sharedState()->m_localLevels->insertObject(copy, 0);
            setLink(levelKey(m_level), levelKey(copy));
            Notification::create(
                fmt::format("Created '{}' - press the switch keybind to play it", copyName),
                NotificationIcon::Success
            )->show();
            return;
        }

        // accumulate into the existing local target (preserving its contents)
        auto result = withStartpos(std::string(target->m_levelString), spObj, replace);
        if (!result) {
            Notification::create("Couldn't read the linked level's data", NotificationIcon::Error)->show();
            return;
        }
        target->m_levelString = *result;
        if (target != m_level) setLink(levelKey(m_level), levelKey(target));
        Notification::create(
            (target == m_level)
                ? "StartPos added - reopen the level to see it"
                : "StartPos added to the linked copy",
            NotificationIcon::Success
        )->show();
    }
};
