#include "PluginListActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <functional>

#include "CrossPointSettings.h"
#include "LuaActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {

constexpr char kPluginsDir[] = "/plugins";
constexpr char kMainLua[] = "main.lua";
constexpr char kDescriptionPrefix[] = "-- DESCRIPTION:";
// Length of kDescriptionPrefix without the trailing NUL. Using sizeof - 1
// is fine here because kDescriptionPrefix is a literal — strlen is also
// fine but sizeof avoids a runtime call.
constexpr size_t kDescriptionPrefixLen = sizeof(kDescriptionPrefix) - 1;
// Probe this many bytes from the head of main.lua. 256 is the cap
// documented in docs/plugins.md — adding more buys nothing because real
// DESCRIPTION comments are always on line 1.
constexpr size_t kDescProbeBytes = 256;

}  // namespace

PluginListActivity::PluginListActivity(GfxRenderer& renderer, MappedInputManager& input)
    : Activity("PluginList", renderer, input) {}

PluginListActivity::~PluginListActivity() = default;

void PluginListActivity::onEnter() {
  pluginCount_ = scanPlugins();
  selectorIndex_ = 0;
  LOG_INF("PLUGINLIST", "discovered %d plugin(s) under %s", pluginCount_, kPluginsDir);

  // Schedule the first paint. Without this the screen stays stale (or
  // blank) until the next button press moves the selector through one
  // of the requestUpdate() calls in the nav callbacks installed in
  // loop(). Mirrors NetworkModeSelectionActivity, LanguageSelectActivity,
  // OpdsSettingsActivity, etc. — they all top out onEnter() with a
  // single requestUpdate() and install their ButtonNavigator pumps in
  // loop().
  requestUpdate();
}

void PluginListActivity::onExit() {
  // ButtonNavigator has no reset() — its callbacks are torn down when
  // this Activity instance is destroyed by ActivityManager::loop(). No
  // cleanup needed.
}

void PluginListActivity::loop() {
  // Back press pops back to HomeActivity. The base Activity::loop() is
  // empty — there is no auto Back handling — so we have to poll here like
  // every other simple-list activity does (OptionSelectionActivity,
  // NetworkModeSelectionActivity, LanguageSelectActivity, etc.).
  // finishAfterBackPress() suppresses the held release so it doesn't also
  // reach the freshly-goHome'd HomeActivity on the same gesture.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (pluginCount_ > 0 && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    launchSelected();
  }

  // ButtonNavigator's onNext/onPrevious are STATELESS event polls: each
  // call checks the current frame's pressed-edges against
  // MappedInputManager and fires the callback exactly once on a new
  // edge (onNextPress/onPreviousPress) PLUS fires again on each
  // continuous-hold tick (onNextContinuous/onPreviousContinuous, with
  // 500 ms start + 500 ms interval — see ButtonNavigator defaults).
  // Installing the callbacks here (every loop tick) and NOT in onEnter()
  // is what makes up/down navigation actually work — the prior bug had
  // them registered once in onEnter() and never re-entered, so the
  // press edges were observed once on the very first frame and silently
  // dropped thereafter. Using onNext/onPrevious (not onNextPress/
  // onPreviousPress) matches NetworkModeSelectionActivity::loop() and
  // gives held-key auto-scroll through the (up to kMaxPlugins=24) row
  // list, which would otherwise force one tap per row.
  if (pluginCount_ > 0) {
    buttonNavigator.onPrevious([this] {
      selectorIndex_ = ButtonNavigator::previousIndex(selectorIndex_, pluginCount_);
      requestUpdate();
    });
    buttonNavigator.onNext([this] {
      selectorIndex_ = ButtonNavigator::nextIndex(selectorIndex_, pluginCount_);
      requestUpdate();
    });
  }
}

int PluginListActivity::scanPlugins() {
  int count = 0;

  auto dir = Storage.open(kPluginsDir);
  if (!dir || !dir.isDirectory()) {
    // /plugins may legitimately not exist on a freshly formatted SD
    // card — that's not an error, just an empty plugin list.
    if (dir) dir.close();
    return 0;
  }

  // Reusable per-iteration filename + path buffers so we never need to
  // allocate std::string per directory entry. AGENTS.md caps locals at
  // ~256 bytes; each of these scratch buffers is well under that.
  char childName[64]{};
  // /plugins/<name> + NUL: kPluginsDir("/plugins") + '/' + childName
  char childPath[64 + 9]{};
  // /plugins/<name>/main.lua + NUL: childPath + '/' + "main.lua"
  char mainLuaPath[sizeof(childPath) + 7 + 1]{};

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (count >= kMaxPlugins) {
      // Only LOG_INF/LOG_DBG/LOG_ERR are available in lib/Logging;
      // informational caps deserve INFO, not WARN.
      LOG_INF("PLUGINLIST", "kMaxPlugins=%d reached, hiding additional entries", kMaxPlugins);
      file.close();
      continue;
    }
    if (!file.isDirectory()) {
      file.close();
      continue;
    }
    if (!file.getName(childName, sizeof(childName))) {
      file.close();
      continue;
    }  // Skip hidden entries ("."-prefixed) and macOS metadata ("_" -prefixed)
    // so dragging a plugin folder via Finder doesn't ship ._PluginName
    // alongside the real one. Also reject empty names so a stray POSIX
    // subdir with a corrupted getName() doesn't render a blank row.
    // Mirrors the FileBrowserActivity policy.
    if (childName[0] == '\0' || childName[0] == '.' || childName[0] == '_') {
      file.close();
      continue;
    }

    // Build /plugins/<name> and /plugins/<name>/main.lua paths. snprintf
    // avoids std::string concatenation so scanPlugins uses zero heap.
    std::snprintf(childPath, sizeof(childPath), "%s/%s", kPluginsDir, childName);
    std::snprintf(mainLuaPath, sizeof(mainLuaPath), "%s/%s", childPath, kMainLua);

    // Probe whether main.lua actually exists. We stat via a quick open
    // attempt and immediately close: the handle is cheap and the test
    // exercises the same library we'll read DESCRIPTION from.
    {
      HalFile probe = Storage.open(mainLuaPath);
      if (!probe) {
        LOG_DBG("PLUGINLIST", "skipping %s: no main.lua", childPath);
        file.close();
        continue;
      }
      probe.close();
    }

    PluginEntry& entry = plugins_[count];
    // assign() with an explicit size acts like the old strncpy: it caps
    // growth to kNameMax / kPathMax so a maliciously long /plugins/<name>/
    // folder can't blow past the std::array budget on a 380 KB no-PSRAM
    // target. Each std::string is allocated once here (or stays under
    // SSO for short strings) and reused across all subsequent draws
    // without any per-frame heap churn.
    entry.name.assign(childName, PluginEntry::kNameMax);
    entry.path.assign(mainLuaPath, PluginEntry::kPathMax);
    entry.description = readFirstLineDescription(mainLuaPath);
    if (entry.description.empty()) {
      entry.description = tr(STR_NO_PLUGIN_DESCRIPTION);
    }
    // Description is parsed from the head of main.lua, so it isn't
    // capped at assign-time. Trim after to honor the same soft limit.
    if (entry.description.size() > PluginEntry::kDescMax) {
      entry.description.resize(PluginEntry::kDescMax);
    }
    file.close();
    count++;
  }
  dir.close();
  return count;
}

std::string PluginListActivity::readFirstLineDescription(const char* absPath) {
  if (absPath == nullptr || absPath[0] == '\0') return {};

  // Static so we don't put 256 bytes on the stack — AGENTS.md says
  // anything meaningfully larger than 256 bytes on the stack needs
  // justification. Reads are serialized within the activity so the
  // static buffer is safe here.
  static char buf[kDescProbeBytes + 1]{};
  buf[0] = '\0';

  HalFile f;
  if (!Storage.openFileForRead("PLUGIN", absPath, f)) return {};

  // Read up to kDescProbeBytes. SdFat::read returns bytes actually
  // transferred; capping protects us from any huge main.lua.
  const int readBytes = f.read(buf, kDescProbeBytes);
  f.close();
  if (readBytes <= 0) return {};
  buf[readBytes] = '\0';

  // Search for the magic comment. Docs put the prefix on its own line,
  // but plugin authors occasionally put it after a shebang or imports —
  // strstr handles that without forcing a line-by-line state machine.
  const char* hit = std::strstr(buf, kDescriptionPrefix);
  if (!hit) return {};
  hit += kDescriptionPrefixLen;
  while (*hit == ' ' || *hit == '\t') ++hit;
  if (*hit == '\0' || *hit == '\n' || *hit == '\r') return {};

  std::string out;
  // Most DESCRIPTION comments are well under 64 bytes — pre-reserving
  // makes push_back zero-alloc for the common case and avoids any
  // geometric grow-and-realloc.
  out.reserve(64);
  while (*hit != '\0' && *hit != '\n' && *hit != '\r') {
    out.push_back(*hit);
    ++hit;
  }
  return out;
}

void PluginListActivity::renderHeader() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PLUGINS));
}

void PluginListActivity::renderPluginList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // GUI.drawList handles pagination + per-row highlighting internally,
  // including partial visibility and scrolling, so we just feed it the
  // bounds, count, lambda getters for the two text rows, and the
  // current selectorIndex_.
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, pluginCount_, selectorIndex_,
      // Returning a std::string member does an implicit move +
      // RVO into the std::function value, so there's no per-frame
      // allocation on the hot path. Short names stay zero-alloc
      // via SSO.
      [this](int index) -> std::string { return plugins_[index].name; },
      [this](int index) -> std::string { return plugins_[index].description; });
}

void PluginListActivity::renderEmptyState() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageHeight = renderer.getScreenHeight();
  const int centerY = (pageHeight / 2) - 10;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_NO_PLUGINS));
  // Hint line under the empty message so users with no /plugins folder
  // can copy an example plugin and try again.
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 24, tr(STR_PLUGINS_DIR_HINT));
}

void PluginListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderHeader();
  if (pluginCount_ == 0) {
    renderEmptyState();
  } else {
    renderPluginList();
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PluginListActivity::launchSelected() {
  if (selectorIndex_ < 0 || selectorIndex_ >= pluginCount_) return;
  // Bind by const-ref so we don't allocate a temp std::string here.
  // LuaActivity's constructor takes const std::string& and copies into
  // its own scriptPath_ member once at make_unique time. plugins_[].path
  // was populated in scanPlugins() and outlives this callsite.
  const std::string& path = plugins_[selectorIndex_].path;
  LOG_INF("PLUGINLIST", "launching %s", path.c_str());
  // pushActivity: keep PluginListActivity on the stack so pressing Back
  // from LuaActivity returns here (PluginList maps to HomeMenuItem::PLUGINS
  // in ActivityManager::goHome(), not directly to home). replaceActivity
  // was a deliberate alternative but breaks the rounded Back navigation
  // and would also drop PluginListActivity's `name` from the stack, so
  // goHome could only ever route to HomeActivity's default row.
  activityManager.pushActivity(std::make_unique<LuaActivity>(renderer, mappedInput, path));
}
