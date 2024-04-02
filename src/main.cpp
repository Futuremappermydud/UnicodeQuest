#include "main.hpp"

#include <GLES3/gl32.h>
#include <android/bitmap.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include "System/IntPtr.hpp"
#include "TMPro/TMP_FontAsset.hpp"
#include "TMPro/TMP_FontAssetUtilities.hpp"
#include "TMPro/TMP_Settings.hpp"
#include "TMPro/TMP_Sprite.hpp"
#include "TMPro/TMP_SpriteAsset.hpp"
#include "TMPro/TMP_SpriteCharacter.hpp"
#include "TMPro/TMP_SpriteGlyph.hpp"
#include "TMPro/TMP_Text.hpp"
#include "TMPro/TMP_TextInfo.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Font.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "jniutils.hpp"

static modloader::ModInfo modInfo = {MOD_ID, VERSION, 0};

using namespace UnityEngine;
using namespace TMPro;

bool added = false;

constexpr int EMOJI_SIZE = 108;
constexpr int SHEET_TILES = 10;
constexpr int SHEET_SIZE = SHEET_TILES * EMOJI_SIZE;

int currentEmojiIndex;
bool textureNeedsApply;
ArrayW<Color> clearPixels;
TMP_SpriteAsset* rootEmojiAsset;
TMP_SpriteAsset* currentEmojiAsset;

struct ThreadGlobals {
    jobject bitmap;
    jobject canvas;
    jobject paint;
};
std::map<std::thread::id, ThreadGlobals> jobjects = {};

std::string utf8ToInt(int unicode) {
    std::stringstream strm;
    strm << unicode;
    return strm.str();
}

std::string utf8ToHex(int unicode) {
    std::stringstream stream;
    stream << std::hex << unicode;
    return stream.str();
}

bool IsCustomAsset(TMP_SpriteAsset* asset) {
    if (!asset)
        return false;
    if (rootEmojiAsset == asset)
        return true;
    for (auto customAsset : ListW<UnityW<TMP_SpriteAsset>>(rootEmojiAsset->fallbackSpriteAssets)) {
        if (customAsset.ptr() == asset)
            return true;
    }
    return false;
}

TMP_SpriteAsset* CreateSpriteAsset() {
    auto texture = Texture2D::New_ctor(SHEET_SIZE, SHEET_SIZE, TextureFormat::RGBA32, false);
    texture->SetPixels(clearPixels);
    texture->Apply(false, false);

    auto spriteAsset = ScriptableObject::CreateInstance<TMP_SpriteAsset*>();
    spriteAsset->fallbackSpriteAssets = ListW<UnityW<TMP_SpriteAsset>>::New();
    spriteAsset->spriteInfoList = ListW<TMP_Sprite*>::New();
    spriteAsset->spriteSheet = texture;

    auto mat = Resources::FindObjectsOfTypeAll<Material*>()->FirstOrDefault([](auto x) { return x->name == std::string_view("UINoGlow"); });
    spriteAsset->material = Object::Instantiate(mat);
    spriteAsset->material->mainTexture = spriteAsset->spriteSheet;

    return spriteAsset;
}

TMP_SpriteGlyph* PushSprite(int unicode) {
    if (currentEmojiIndex >= SHEET_TILES * SHEET_TILES) {
        auto newSheet = CreateSpriteAsset();
        rootEmojiAsset->fallbackSpriteAssets->Add(newSheet);
        currentEmojiAsset = newSheet;
        currentEmojiIndex = 0;
    }

    std::string text = utf8ToInt(unicode);

    int column = currentEmojiIndex % SHEET_TILES;
    int row = currentEmojiIndex / SHEET_TILES;

    auto glyph = TMP_SpriteGlyph::New_ctor();
    glyph->index = currentEmojiIndex;
    glyph->metrics = TextCore::GlyphMetrics(EMOJI_SIZE, EMOJI_SIZE, 0.25 * EMOJI_SIZE, 0.75 * EMOJI_SIZE, EMOJI_SIZE);
    glyph->glyphRect = TextCore::GlyphRect(column * EMOJI_SIZE, SHEET_SIZE - ((row + 1) * EMOJI_SIZE), EMOJI_SIZE, EMOJI_SIZE);

    if (!currentEmojiAsset->m_SpriteGlyphTable)
        currentEmojiAsset->m_SpriteGlyphTable = ListW<TMP_SpriteGlyph*>::New();
    currentEmojiAsset->m_SpriteGlyphTable->Add(glyph);

    TMP_SpriteCharacter* character = TMP_SpriteCharacter::New_ctor(unicode, glyph);
    character->name = text;
    character->scale = 1;

    if (!currentEmojiAsset->m_SpriteCharacterTable)
        currentEmojiAsset->m_SpriteCharacterTable = ListW<TMP_SpriteCharacter*>::New();
    currentEmojiAsset->m_SpriteCharacterTable->Add(character);

    currentEmojiAsset->SortGlyphTable();
    currentEmojiAsset->UpdateLookupTables();

    return glyph;
}

// #include "UnityEngine/Graphics.hpp"
// #include "UnityEngine/ImageConversion.hpp"
// #include "UnityEngine/Rect.hpp"
// #include "UnityEngine/RenderTexture.hpp"
// #include "UnityEngine/RenderTextureFormat.hpp"
// #include "UnityEngine/RenderTextureReadWrite.hpp"

void DrawTexture(uint unicode, TMP_SpriteGlyph* glyph) {
    auto env = JNIUtils::GetJNIEnv();

    auto id = std::this_thread::get_id();
    if (!jobjects.contains(id))
        jobjects[id] = {};
    auto globals = jobjects[id];

    if (!globals.bitmap) {
        GET_JCLASS(env, configClass, "android/graphics/Bitmap$Config");
        GET_STATIC_JOBJECT_FIELD(env, config, configClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
        GET_JCLASS(env, bitmapClass, "android/graphics/Bitmap");
        CALL_STATIC_JOBJECT_METHOD(env,
            tmpBitmap,
            bitmapClass,
            "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
            EMOJI_SIZE,
            EMOJI_SIZE,
            config);
        globals.bitmap = env->NewGlobalRef(tmpBitmap);
    }
    if (!globals.canvas) {
        GET_JCLASS(env, canvasClass, "android/graphics/Canvas");
        NEW_JOBJECT(env, tmpCanvas, canvasClass, "(Landroid/graphics/Bitmap;)V", globals.bitmap);
        globals.canvas = env->NewGlobalRef(tmpCanvas);
    }
    if (!globals.paint) {
        GET_JCLASS(env, paintClass, "android/graphics/Paint");
        NEW_JOBJECT(env, tmpPaint, paintClass, "()V");
        CALL_VOID_METHOD(env, tmpPaint, "setTextSize", "(F)V", (jfloat) EMOJI_SIZE * 0.75);
        CALL_VOID_METHOD(env, tmpPaint, "setAntiAlias", "(Z)V", true);
        CALL_VOID_METHOD(env, tmpPaint, "setARGB", "(IIII)V", 255, 255, 255, 255);
        globals.paint = env->NewGlobalRef(tmpPaint);
    }

    CALL_VOID_METHOD(env, globals.bitmap, "eraseColor", "(I)V", 0);
    jstring str;
    int l = 1;
    if (unicode < 0xffff)
        str = env->NewString((jchar*) &unicode, 1);
    else if (unicode < 0x10ffff) {
        uint n = unicode - 0x10000;
        jchar tmp[2];
        tmp[0] = 0xd800 | (n >> 10);
        tmp[1] = 0xdc00 | (n & 0x3ff);
        str = env->NewString((jchar*) tmp, 2);
        l = 2;
    } else {
        logger.warn("too big unicode {}", unicode);
        return;
    }
    CALL_VOID_METHOD(env,
        globals.canvas,
        "drawText",
        "(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V",
        str,
        0,
        l,
        (jfloat) 0,
        (jfloat) EMOJI_SIZE * 0.8,
        globals.paint);

    // debug: saves bitmap with drawn text
    // static bool saved = false;

    // if (!saved) {
    //     GET_JCLASS(env, foutClass, "java/io/FileOutputStream");
    //     NEW_JOBJECT(env, fout, foutClass, "(Ljava/lang/String;)V", env->NewStringUTF("/sdcard/bitmap.png"));
    //     GET_JCLASS(env, cmpClass, "android/graphics/Bitmap$CompressFormat");
    //     GET_STATIC_JOBJECT_FIELD(env, cmp, cmpClass, "PNG", "Landroid/graphics/Bitmap$CompressFormat;");
    //     CALL_JBOOLEAN_METHOD(
    //         env, savebool, globals.bitmap, "compress", "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z", cmp, 100, fout);
    //     saved = true;
    // }

    uint32_t* pixels = nullptr;
    AndroidBitmap_lockPixels(env, globals.bitmap, (void**) &pixels);

    // ARGB8888: 4 sets of 8 bits per pixel
    int pixelsSizeU32 = EMOJI_SIZE * EMOJI_SIZE;

    // find boundaries and flip vertically
    uint32_t flippedPixels[pixelsSizeU32];
    int left = EMOJI_SIZE;
    int right = 0;
    int lower = EMOJI_SIZE;
    int upper = 0;

    for (int row = 0; row < EMOJI_SIZE; row++) {
        int rowOff = row * EMOJI_SIZE;
        int flipRowOff = EMOJI_SIZE * (EMOJI_SIZE - row - 1);
        bool foundPix = false;
        for (int col = EMOJI_SIZE - 1; col >= 0; col--) {
            auto pix = pixels[flipRowOff + col];
            if (pix != 0) {
                foundPix = true;
                if (col > right)
                    right = col;
                if (col < left)
                    left = col;
            }
            flippedPixels[rowOff + col] = pix;
        }
        if (foundPix && row < lower)
            lower = row;
        if (foundPix && row > upper)
            upper = row;
    }

    AndroidBitmap_unlockPixels(env, globals.bitmap);

    auto tex = Texture2D::New_ctor(EMOJI_SIZE, EMOJI_SIZE, TextureFormat::RGBA32, false);
    // tex->LoadRawTextureData(flippedPixels, pixelsSizeU32 * 4);  // size in uint8_t*
    static auto method = il2cpp_utils::FindMethodUnsafe(tex, "LoadRawTextureData", 2);
    il2cpp_utils::RunMethod<void, false>(tex, method, System::IntPtr(flippedPixels), pixelsSizeU32 * 4);
    tex->Apply();

    // debug: saves texure after copy
    // static bool saved2 = false;

    // if (!saved2) {
    //     auto bytes = ImageConversion::EncodeToPNG(tex);
    //     writefile("/sdcard/char.png", std::string((char*) bytes.begin(), bytes->get_Length()));
    //     saved2 = true;
    //     logger.info("l {} r {} b {} t {}", left, right, lower, upper);
    // }

    int column = currentEmojiIndex % SHEET_TILES;
    int row = currentEmojiIndex / SHEET_TILES;
    int x = column * EMOJI_SIZE;
    int y = SHEET_SIZE - ((row + 1) * EMOJI_SIZE);

    static auto CopyTexture_Region =
        il2cpp_utils::resolve_icall<void, UnityEngine::Texture*, int, int, int, int, int, int, UnityEngine::Texture*, int, int, int, int>(
            "UnityEngine.Graphics::CopyTexture_Region");
    CopyTexture_Region(tex, 0, 0, 0, 0, tex->width, tex->height, currentEmojiAsset->spriteSheet, 0, 0, x, y);

    // add a little bit of spacing
    right = std::min(right + (EMOJI_SIZE / 10), EMOJI_SIZE);
    int width = right - left;
    if (width < 0)
        width = 0;
    x += left;
    int height = EMOJI_SIZE;
    // int height = upper - lower;
    // if (height < 0)
    //     height = 0;
    // y += lower;
    glyph->metrics = TextCore::GlyphMetrics(width, height, 0.125 * EMOJI_SIZE, 0.875 * EMOJI_SIZE, width);
    glyph->glyphRect = TextCore::GlyphRect(x, y, width, height);

    currentEmojiAsset->SortGlyphTable();
    currentEmojiAsset->UpdateLookupTables();
}

using namespace GlobalNamespace;

MAKE_HOOK_MATCH(MainFlowCoordinator_DidActivate,
    &MainFlowCoordinator::DidActivate,
    void,
    MainFlowCoordinator* self,
    bool firstActivation,
    bool addedToHierarchy,
    bool screenSystemEnabling) {

    if (!added) {
        clearPixels = ArrayW<Color>(SHEET_SIZE * SHEET_SIZE);

        rootEmojiAsset = CreateSpriteAsset();
        currentEmojiAsset = rootEmojiAsset;
        currentEmojiIndex = 0;
        TMP_Settings::get_instance()->m_defaultSpriteAsset = rootEmojiAsset;

        added = true;
    }

    MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
}

MAKE_HOOK_MATCH(TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset,
    &TMP_FontAssetUtilities::GetSpriteCharacterFromSpriteAsset,
    TMP_SpriteCharacter*,
    uint unicode,
    TMP_SpriteAsset* spriteAsset,
    bool includeFallbacks) {

    auto result = TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset(unicode, spriteAsset, includeFallbacks);

    if (!result && spriteAsset == rootEmojiAsset) {
        logger.debug("unicode {}", unicode);

        auto glyph = PushSprite(unicode);

        DrawTexture(unicode, glyph);
        currentEmojiIndex++;

        currentEmojiAsset->spriteCharacterLookupTable->TryGetValue(unicode, byref(result));

        // debug: saves whole sprite sheet
        // auto tex = (Texture2D*) currentEmojiAsset->spriteSheet.ptr();
        // auto tmp = RenderTexture::GetTemporary(tex->width, tex->height, 0, RenderTextureFormat::ARGB32, RenderTextureReadWrite::Linear);
        // Graphics::Blit(tex, tmp);
        // auto previous = RenderTexture::get_active();
        // RenderTexture::set_active(tmp);
        // auto readable = Texture2D::New_ctor(tex->width, tex->height);
        // readable->ReadPixels({0, 0, (float) tex->width, (float) tex->height}, 0, 0, false);
        // readable->Apply();
        // RenderTexture::set_active(previous);
        // RenderTexture::ReleaseTemporary(tmp);
        // auto bytes = ImageConversion::EncodeToPNG(readable);
        // writefile("/sdcard/sprites.png", std::string((char*) bytes.begin(), bytes->get_Length()));
    }
    return result;
}

MAKE_HOOK_MATCH(TMP_Text_SaveSpriteVertexInfo, &TMP_Text::SaveSpriteVertexInfo, void, TMP_Text* self, Color32 vertexColor) {

    // this check may not be necessary if there are no other sprite assets in the game
    std::optional<bool> tintState = std::nullopt;
    if (IsCustomAsset(self->m_textInfo->characterInfo[self->m_characterCount].spriteAsset)) {
        tintState = self->m_tintSprite;
        self->m_tintSprite = true;
    }

    TMP_Text_SaveSpriteVertexInfo(self, vertexColor);

    if (tintState.has_value())
        self->m_tintSprite = *tintState;
}

extern "C" void setup(CModInfo* info) {
    info->id = MOD_ID;
    info->version = VERSION;
    modInfo.assign(*info);

    logger.info("Completed setup!");
}

extern "C" void late_load() {
    il2cpp_functions::Init();

    logger.info("Installing hooks...");
    INSTALL_HOOK(logger, MainFlowCoordinator_DidActivate);
    INSTALL_HOOK(logger, TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset);
    INSTALL_HOOK(logger, TMP_Text_SaveSpriteVertexInfo);
    logger.info("Installed all hooks!");
}
