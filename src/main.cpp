#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>

using namespace geode::prelude;
namespace fs = std::filesystem;

// ===========================================================================
// Helpers
// ===========================================================================

// A 1x1 white texture. Scaled up + tinted, it works as a square brush/swatch.
static CCTexture2D* getWhitePixelTexture() {
    static CCTexture2D* tex = nullptr;
    if (!tex) {
        unsigned char pixel[4] = {255, 255, 255, 255};
        tex = new CCTexture2D();
        tex->initWithData(pixel, kCCTexture2DPixelFormat_RGBA8888, 1, 1, CCSize(1, 1));
    }
    return tex;
}

static fs::path getResourcesDir() {
    return dirs::getGameDir() / "Resources";
}

// Texture Loader looks for packs in <GD>/geode/config/geode.texture-loader/packs/
static fs::path getPacksDir() {
    return dirs::getGameDir() / "geode" / "config" / "geode.texture-loader" / "packs";
}

static std::string sanitizePackName(std::string const& raw) {
    std::string out;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == ' ' || c == '-' || c == '_') {
            out += '-';
        }
    }
    return out.empty() ? "my-texture-pack" : out;
}

struct TextureEntry {
    fs::path fullPath;
    std::string relativeName; // relative to Resources, e.g. "GJ_gradientBG-hd.png"
};

// Standalone textures only: any .png that does NOT have a sibling .plist
// (a .plist next to a .png means it's a spritesheet/atlas, which we skip).
static std::vector<TextureEntry> findStandaloneTextures() {
    std::vector<TextureEntry> result;
    auto resDir = getResourcesDir();
    std::error_code ec;
    if (!fs::exists(resDir, ec)) return result;

    for (fs::recursive_directory_iterator it(resDir, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;

        auto path = it->path();
        if (path.extension() != ".png") continue;

        auto plist = path;
        plist.replace_extension(".plist");
        if (fs::exists(plist, ec2)) continue; // spritesheet -> skip

        TextureEntry entry;
        entry.fullPath = path;
        entry.relativeName = fs::relative(path, resDir, ec2).generic_string();
        result.push_back(std::move(entry));
    }

    std::sort(result.begin(), result.end(), [](TextureEntry const& a, TextureEntry const& b) {
        return a.relativeName < b.relativeName;
    });
    return result;
}

// Writes the edited texture into the Texture Loader pack (creating the pack
// and its pack.json if needed). Returns true on success.
static bool saveToPack(std::string const& relativeName, CCRenderTexture* canvas) {
    auto packName = sanitizePackName(Mod::get()->getSettingValue<std::string>("pack-name"));
    auto author = Mod::get()->getSettingValue<std::string>("pack-author");

    auto packDir = getPacksDir() / packName;
    std::error_code ec;
    fs::create_directories(packDir, ec);

    // Mirror the original resource path inside the pack so Texture Loader
    // replaces the right file.
    auto pngPath = packDir / relativeName;
    fs::create_directories(pngPath.parent_path(), ec);

    bool ok = false;
    if (auto image = canvas->newCCImage()) {
        ok = image->saveToFile(pngPath.string().c_str(), false); // false = keep alpha
        image->release();
    }

    // Only create pack.json if the pack is new (don't clobber an existing one).
    auto jsonPath = packDir / "pack.json";
    if (!fs::exists(jsonPath, ec)) {
        std::ofstream out(jsonPath);
        out << "{\n"
            << "    \"textureldr\": \"1.5.0\",\n"
            << "    \"name\": \"" << packName << "\",\n"
            << "    \"id\": \"" << sanitizePackName(author) << "." << packName << "\",\n"
            << "    \"version\": \"1.0.0\",\n"
            << "    \"author\": \"" << author << "\"\n"
            << "}\n";
    }

    return ok;
}

// ===========================================================================
// Paint editor scene
// ===========================================================================
class PaintEditorLayer : public CCLayer {
protected:
    std::string m_relativeName;
    CCRenderTexture* m_canvas = nullptr;
    CCSize m_texSize = {0, 0};

    std::vector<ccColor3B> m_palette;
    ccColor3B m_color = {255, 255, 255};
    float m_brushSize = 4.f;
    bool m_erasing = false;

    bool m_hasLast = false;
    CCPoint m_lastLocal = {0, 0};

public:
    static PaintEditorLayer* create(std::string const& relativeName, fs::path const& fullPath) {
        auto ret = new PaintEditorLayer();
        if (ret && ret->init(relativeName, fullPath)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    static CCScene* scene(std::string const& relativeName, fs::path const& fullPath) {
        auto scene = CCScene::create();
        scene->addChild(PaintEditorLayer::create(relativeName, fullPath));
        return scene;
    }

    bool init(std::string const& relativeName, fs::path const& fullPath) {
        if (!CCLayer::init()) return false;

        m_relativeName = relativeName;
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Dark background
        auto bg = CCLayerColor::create({30, 30, 40, 255});
        this->addChild(bg, -10);

        // Load by absolute path so we get the exact file the user picked.
        auto texture = CCTextureCache::sharedTextureCache()->addImage(fullPath.string().c_str(), false);

        if (!texture) {
            auto label = CCLabelBMFont::create("Failed to load texture!", "bigFont.fnt");
            label->setPosition(winSize / 2);
            this->addChild(label);
        } else {
            m_texSize = texture->getContentSize();

            // Canvas: pixel size = content size * content scale factor, which
            // matches the original image's pixel dimensions.
            m_canvas = CCRenderTexture::create(
                static_cast<int>(m_texSize.width),
                static_cast<int>(m_texSize.height)
            );

            // Paint the original image onto the canvas as the starting point.
            m_canvas->beginWithClear(0, 0, 0, 0);
            auto base = CCSprite::createWithTexture(texture);
            base->setAnchorPoint({0.f, 0.f});
            base->setPosition({0.f, 0.f});
            base->visit();
            m_canvas->end();

            // Zoom small textures up so they're paintable on screen.
            float zoom = std::min(
                (winSize.width * 0.6f) / m_texSize.width,
                (winSize.height * 0.6f) / m_texSize.height
            );
            zoom = std::clamp(zoom, 0.1f, 16.f);

            m_canvas->setPosition(winSize / 2);
            m_canvas->setScale(zoom);

            // Framed panel behind the canvas so it reads as a contained
            // surface instead of floating on the bare background.
            float framePad = 14.f;
            auto frame = CCScale9Sprite::create("GJ_square01.png");
            frame->setContentSize({
                m_texSize.width * zoom + framePad * 2.f,
                m_texSize.height * zoom + framePad * 2.f
            });
            frame->setPosition(winSize / 2);
            this->addChild(frame, 0);

            this->addChild(m_canvas, 1);

            // Filename title above the canvas.
            auto title = CCLabelBMFont::create(relativeName.c_str(), "chatFont.fnt");
            title->setScale(0.6f);
            title->setPosition({winSize.width / 2, winSize.height - 20.f});
            this->addChild(title, 10);
        }

        this->setupUI(winSize);

        this->setTouchEnabled(true);
        this->setTouchMode(kCCTouchesOneByOne);
        this->setKeypadEnabled(true);

        return true;
    }

    void setupUI(CCSize const& winSize) {
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu, 10);

        // Back (top-left)
        auto back = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
            this, menu_selector(PaintEditorLayer::onBack)
        );
        back->setPosition({25.f, winSize.height - 25.f});
        menu->addChild(back);

        // Save (top-right)
        auto saveSpr = ButtonSprite::create("Save to Pack");
        saveSpr->setScale(0.8f);
        auto save = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(PaintEditorLayer::onSave));
        save->setPosition({winSize.width - 80.f, winSize.height - 25.f});
        menu->addChild(save);

        // Colour palette (row under the top bar), on its own panel.
        m_palette = {
            {255, 255, 255}, {0, 0, 0}, {255, 0, 0}, {0, 255, 0},
            {0, 0, 255}, {255, 255, 0}, {255, 128, 0}, {160, 32, 240},
        };
        float swatchGap = 32.f;
        float paletteY = winSize.height - 68.f;
        float paletteWidth = swatchGap * static_cast<float>(m_palette.size()) + 20.f;

        auto palettePanel = CCScale9Sprite::create("GJ_square01.png");
        palettePanel->setContentSize({paletteWidth, 46.f});
        palettePanel->setOpacity(190);
        palettePanel->setPosition({20.f + paletteWidth / 2.f, paletteY});
        this->addChild(palettePanel, 5);

        for (size_t i = 0; i < m_palette.size(); ++i) {
            auto swatch = CCSprite::createWithTexture(getWhitePixelTexture());
            swatch->setScale(22.f);
            swatch->setColor(m_palette[i]);
            auto btn = CCMenuItemSpriteExtra::create(swatch, this, menu_selector(PaintEditorLayer::onPickColor));
            btn->setTag(static_cast<int>(i));
            btn->setPosition({35.f + swatchGap * static_cast<float>(i), paletteY});
            menu->addChild(btn);
        }

        // Bottom toolbar panel: brush sizes + eraser together on one bar.
        float toolbarY = 32.f;
        auto toolbarPanel = CCScale9Sprite::create("GJ_square01.png");
        toolbarPanel->setContentSize({winSize.width - 40.f, 50.f});
        toolbarPanel->setOpacity(190);
        toolbarPanel->setPosition({winSize.width / 2.f, toolbarY});
        this->addChild(toolbarPanel, 5);

        float sizes[] = {2.f, 5.f, 10.f};
        for (int i = 0; i < 3; ++i) {
            auto spr = ButtonSprite::create(fmt::format("{}px", static_cast<int>(sizes[i])).c_str());
            spr->setScale(0.6f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PaintEditorLayer::onBrushSize));
            btn->setTag(static_cast<int>(sizes[i]));
            btn->setPosition({70.f + 70.f * static_cast<float>(i), toolbarY});
            menu->addChild(btn);
        }

        auto eraserSpr = ButtonSprite::create("Eraser");
        eraserSpr->setScale(0.6f);
        auto eraser = CCMenuItemSpriteExtra::create(eraserSpr, this, menu_selector(PaintEditorLayer::onToggleEraser));
        eraser->setPosition({winSize.width - 70.f, toolbarY});
        menu->addChild(eraser);
    }

    void onBack(CCObject*) {
        CCDirector::sharedDirector()->popScene();
    }

    void keyBackClicked() override {
        this->onBack(nullptr);
    }

    void onPickColor(CCObject* sender) {
        auto idx = static_cast<CCNode*>(sender)->getTag();
        if (idx >= 0 && idx < static_cast<int>(m_palette.size())) {
            m_color = m_palette[idx];
            m_erasing = false;
        }
    }

    void onBrushSize(CCObject* sender) {
        m_brushSize = static_cast<float>(static_cast<CCNode*>(sender)->getTag());
    }

    void onToggleEraser(CCObject*) {
        m_erasing = !m_erasing;
        Notification::create(m_erasing ? "Eraser on" : "Eraser off", NotificationIcon::None, 0.6f)->show();
    }

    void onSave(CCObject*) {
        if (!m_canvas) return;
        if (saveToPack(m_relativeName, m_canvas)) {
            auto packName = sanitizePackName(Mod::get()->getSettingValue<std::string>("pack-name"));
            Notification::create(
                fmt::format("Saved to pack '{}'. Enable it in Texture Loader.", packName),
                NotificationIcon::Success
            )->show();
        } else {
            Notification::create("Failed to save texture!", NotificationIcon::Error)->show();
        }
    }

    // ---- Painting -----------------------------------------------------

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        m_hasLast = false;
        this->paintAt(touch);
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        this->paintAt(touch);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        m_hasLast = false;
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_hasLast = false;
    }

    void paintAt(CCTouch* touch) {
        if (!m_canvas) return;

        // touch->getLocation() is already resolved to GL/world (design) space
        // by cocos2d, so convert it straight into the canvas's local space -
        // no intermediate round-trip through this layer needed.
        auto worldPt = touch->getLocation();
        auto local = m_canvas->convertToNodeSpace(worldPt);

        bool inside = local.x >= 0 && local.x <= m_texSize.width &&
                      local.y >= 0 && local.y <= m_texSize.height;
        if (!inside) {
            m_hasLast = false;
            return;
        }

        // Fill in the gap between the previous and current point so fast
        // strokes are continuous lines rather than dotted.
        CCPoint from = m_hasLast ? m_lastLocal : local;
        float dist = ccpDistance(from, local);
        float step = std::max(1.f, m_brushSize * 0.5f);
        int count = std::max(1, static_cast<int>(std::ceil(dist / step)));

        m_canvas->begin();
        for (int i = 1; i <= count; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(count);
            CCPoint p = ccp(from.x + (local.x - from.x) * t, from.y + (local.y - from.y) * t);

            auto dot = CCSprite::createWithTexture(getWhitePixelTexture());
            dot->setAnchorPoint({0.5f, 0.5f});
            dot->setPosition(p);
            dot->setScale(m_brushSize);

            if (m_erasing) {
                // Blend (0,0) writes transparent black = erases.
                dot->setBlendFunc(ccBlendFunc{GL_ZERO, GL_ZERO});
            } else {
                dot->setColor(m_color);
            }
            dot->visit();
        }
        m_canvas->end();

        m_lastLocal = local;
        m_hasLast = true;
    }
};

// ===========================================================================
// Texture browser scene (paged list, no scroll widgets needed)
// ===========================================================================
class TextureBrowserLayer : public CCLayer {
protected:
    static constexpr int PER_PAGE = 9;

    std::vector<TextureEntry> m_textures;
    int m_page = 0;
    CCMenu* m_listMenu = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;

public:
    static TextureBrowserLayer* create() {
        auto ret = new TextureBrowserLayer();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    static CCScene* scene() {
        auto scene = CCScene::create();
        scene->addChild(TextureBrowserLayer::create());
        return scene;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        m_textures = findStandaloneTextures();

        this->addChild(CCLayerColor::create({30, 30, 40, 255}), -10);

        auto title = CCLabelBMFont::create("Pick a texture to edit", "goldFont.fnt");
        title->setScale(0.9f);
        title->setPosition({winSize.width / 2, winSize.height - 25.f});
        this->addChild(title, 10);

        auto listPanel = CCScale9Sprite::create("GJ_square01.png");
        listPanel->setContentSize({winSize.width - 80.f, winSize.height - 130.f});
        listPanel->setOpacity(160);
        listPanel->setPosition({winSize.width / 2.f, winSize.height / 2.f - 10.f});
        this->addChild(listPanel, 1);

        m_listMenu = CCMenu::create();
        m_listMenu->setPosition({0.f, 0.f});
        this->addChild(m_listMenu, 5);

        auto nav = CCMenu::create();
        nav->setPosition({0.f, 0.f});
        this->addChild(nav, 5);

        auto back = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
            this, menu_selector(TextureBrowserLayer::onBack)
        );
        back->setPosition({25.f, winSize.height - 25.f});
        nav->addChild(back);

        auto prevSpr = ButtonSprite::create("Prev");
        prevSpr->setScale(0.6f);
        auto prev = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(TextureBrowserLayer::onPrev));
        prev->setPosition({winSize.width / 2 - 90.f, 25.f});
        nav->addChild(prev);

        auto nextSpr = ButtonSprite::create("Next");
        nextSpr->setScale(0.6f);
        auto next = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(TextureBrowserLayer::onNext));
        next->setPosition({winSize.width / 2 + 90.f, 25.f});
        nav->addChild(next);

        m_pageLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_pageLabel->setScale(0.5f);
        m_pageLabel->setPosition({winSize.width / 2, 25.f});
        this->addChild(m_pageLabel);

        if (m_textures.empty()) {
            auto none = CCLabelBMFont::create("No standalone textures found in Resources.", "bigFont.fnt");
            none->setScale(0.5f);
            none->setPosition(winSize / 2);
            this->addChild(none);
        }

        this->setKeypadEnabled(true);
        this->rebuildList();
        return true;
    }

    int pageCount() const {
        return std::max(1, static_cast<int>((m_textures.size() + PER_PAGE - 1) / PER_PAGE));
    }

    void rebuildList() {
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        m_listMenu->removeAllChildren();

        int start = m_page * PER_PAGE;
        int end = std::min(static_cast<int>(m_textures.size()), start + PER_PAGE);

        float y = winSize.height - 65.f;
        float rowHeight = 26.f;
        int rowIndex = 0;
        for (int i = start; i < end; ++i) {
            // Faint alternating row tint for scannability.
            if (rowIndex % 2 == 0) {
                auto rowBg = CCSprite::createWithTexture(getWhitePixelTexture());
                rowBg->setPosition({winSize.width / 2, y});
                rowBg->setScaleX(winSize.width - 90.f);
                rowBg->setScaleY(rowHeight - 2.f);
                rowBg->setColor({255, 255, 255});
                rowBg->setOpacity(18);
                m_listMenu->addChild(rowBg);
            }

            auto label = CCLabelBMFont::create(m_textures[i].relativeName.c_str(), "chatFont.fnt");
            label->limitLabelWidth(winSize.width - 100.f, 0.9f, 0.3f);
            auto btn = CCMenuItemSpriteExtra::create(label, this, menu_selector(TextureBrowserLayer::onSelect));
            btn->setTag(i);
            btn->setPosition({winSize.width / 2, y});
            m_listMenu->addChild(btn);
            y -= rowHeight;
            rowIndex++;
        }

        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, this->pageCount()).c_str());
    }

    void onPrev(CCObject*) {
        m_page = (m_page - 1 + this->pageCount()) % this->pageCount();
        this->rebuildList();
    }

    void onNext(CCObject*) {
        m_page = (m_page + 1) % this->pageCount();
        this->rebuildList();
    }

    void onSelect(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= static_cast<int>(m_textures.size())) return;

        auto const& entry = m_textures[idx];
        CCDirector::sharedDirector()->pushScene(
            CCTransitionFade::create(0.3f, PaintEditorLayer::scene(entry.relativeName, entry.fullPath))
        );
    }

    void onBack(CCObject*) {
        CCDirector::sharedDirector()->popScene();
    }

    void keyBackClicked() override {
        this->onBack(nullptr);
    }
};

// ===========================================================================
// Main menu button
// ===========================================================================
class $modify(TEMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        if (auto menu = this->getChildByID("bottom-menu")) {
            auto spr = ButtonSprite::create("Paint");
            spr->setScale(0.7f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TEMenuLayer::onOpenTextureEditor));
            btn->setID("texture-editor-button"_spr);
            menu->addChild(btn);
            menu->updateLayout();
        }

        return true;
    }

    void onOpenTextureEditor(CCObject*) {
        CCDirector::sharedDirector()->pushScene(
            CCTransitionFade::create(0.3f, TextureBrowserLayer::scene())
        );
    }
};
