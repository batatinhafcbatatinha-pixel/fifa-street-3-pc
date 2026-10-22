// FIFA Street 3 menu assets: the game's own ITC Avant Garde bitmap fonts, read at run time from
// the user's disc files (never shipped here). fonts-hd.big is a BIG4 archive of .fnt files; an
// .fnt is FONT + a char->glyph table (0x50), then FONG + 20-byte glyph records, then an
// uncompressed 8-bit luminance DDS atlas.
#include "pc_settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

void Require(bool ok) {
  if (!ok) throw std::runtime_error("invalid FIFA Street 3 theme asset");
}
uint32_t BE(View b, size_t p, unsigned n = 4) {
  Require(p <= b.size() && n <= b.size() - p);
  uint32_t v = 0;
  for (unsigned i = 0; i < n; ++i) v = (v << 8) | b[p + i];
  return v;
}
uint32_t LE(View b, size_t p, unsigned n = 4) {
  Require(p <= b.size() && n <= b.size() - p);
  uint32_t v = 0;
  for (unsigned i = 0; i < n; ++i) v |= uint32_t(b[p + i]) << (8 * i);
  return v;
}
size_t Find(View b, const char* tag) {
  size_t n = std::strlen(tag);
  if (n > b.size()) return size_t(-1);
  for (size_t i = 0; i + n <= b.size(); ++i)
    if (!std::memcmp(b.data() + i, tag, n)) return i;
  return size_t(-1);
}

// The .fnt as described at the top. Glyphs whose rectangle leaves the atlas are dropped rather
// than read out of bounds (some of the decorative faces have clipped glyphs in the shipped data).
bool ParseFont(View b, pcs::ThemeFont& out) {
  if (b.size() < 0x60 || std::memcmp(b.data(), "FONT", 4)) return false;
  uint32_t count = BE(b, 0x44);
  uint32_t line = BE(b, 0x2c);
  size_t fong = Find(b, "FONG");
  size_t dds = Find(b, "DDS ");
  if (count == 0 || count > 2000 || fong == size_t(-1) || dds == size_t(-1)) return false;
  if (fong + 8 + size_t(count) * 20 > b.size() || dds + 128 > b.size()) return false;
  uint32_t dh = LE(b, dds + 12), dw = LE(b, dds + 16);
  if (!dw || !dh || dw > 4096 || dh > 4096 || uint64_t(dds) + 128 + uint64_t(dw) * dh > b.size())
    return false;
  out.atlas.width = int(dw);
  out.atlas.height = int(dh);
  out.atlas.rgba.assign(size_t(dw) * dh, 0);
  const uint8_t* pixels = b.data() + dds + 128;
  for (size_t i = 0; i < size_t(dw) * dh; ++i) out.atlas.rgba[i] = uint32_t(pixels[i]) << 24;
  for (uint32_t code = 0; code < count; ++code) {
    uint32_t gid = BE(b, 0x50 + size_t(code) * 4);
    if (gid < 33) continue;
    size_t index = gid - 33;
    if (index >= count) continue;
    const uint8_t* r = b.data() + fong + 8 + index * 20;
    uint16_t y = uint16_t((r[0] << 8) | r[1]);
    // The second field is the glyph's *right* edge; the bitmap starts one glyph width
    // before it (verified against the atlas ink runs of every ASCII glyph).
    uint16_t right = uint16_t((r[2] << 8) | r[3]);
    uint16_t gw = uint16_t((r[6] << 8) | r[7]);
    if (right < gw) continue;
    uint16_t x = uint16_t(right - gw);
    uint16_t gh = uint16_t((r[8] << 8) | r[9]);
    uint16_t adv = uint16_t((r[10] << 8) | r[11]);
    int16_t ox = int16_t((r[12] << 8) | r[13]);
    uint16_t oy = uint16_t((r[14] << 8) | r[15]);
    if (!gw || !gh || x + gw > dw || y + gh > dh) continue;
    pcs::ThemeGlyph g{};
    g.code = uint16_t(code);
    g.x = x;
    g.y = y;
    g.width = uint8_t(std::min<unsigned>(gw, 255));
    g.height = uint8_t(std::min<unsigned>(gh, 255));
    g.offset_x = int8_t(ox);
    g.offset_y = uint8_t(std::min<unsigned>(oy, 255));
    g.advance = adv;
    out.glyphs.push_back(g);
  }
  out.line_height = int(line);
  return !out.glyphs.empty();
}

// fonts-hd.big: "BIG4", size (LE), entry count (BE at +8), then entry offset/size (BE, absolute)
// and a NUL-terminated name. Entries are not compressed.
bool LoadFontFile(const std::filesystem::path& root, const char* wanted, pcs::ThemeFont& font) {
  std::ifstream f(root / "data" / "fonts" / "fonts-hd.big", std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  auto end = f.tellg();
  if (end <= 16 || end > 64 * 1024 * 1024) return false;
  f.seekg(0);
  Bytes file(static_cast<size_t>(end));
  if (!f.read(reinterpret_cast<char*>(file.data()), std::streamsize(file.size()))) return false;
  View b(file);
  if (std::memcmp(b.data(), "BIG4", 4) && std::memcmp(b.data(), "BIGF", 4)) return false;
  uint32_t count = BE(b, 8);
  if (count > 100000) return false;
  size_t p = 16;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 8 > b.size()) return false;
    uint32_t offset = BE(b, p), size = BE(b, p + 4);
    p += 8;
    size_t name_at = p;
    while (p < b.size() && b[p]) ++p;
    if (p >= b.size()) return false;
    ++p;
    std::string name(reinterpret_cast<const char*>(file.data() + name_at), p - name_at - 1);
    if (name != wanted) continue;
    if (uint64_t(offset) + size > file.size()) return false;
    return ParseFont(View(file.data() + offset, size), font);
  }
  return false;
}

void LoadTheme(pcs::Theme& theme) {
  try {
    std::filesystem::path root;
    auto cvar = rex::cvar::GetFlagByName("game_data_root");
    if (!cvar.empty()) root = std::filesystem::u8path(cvar);
    if (root.empty()) {
      if (const char* env = std::getenv("PCS_S3_ROOT"); env && *env)
        root = std::filesystem::u8path(env);
      else
        root = "D:/FIFA-Street3-game";
    }
    pcs::ThemeFont body, title;
    Require(LoadFontFile(root, "data/fonts/ITC Avant Garde Pro Demi 22.fnt", body));
    if (LoadFontFile(root, "data/fonts/ITC Avant Garde Pro Demi 33.fnt", title))
      theme.title_font = std::move(title);
    theme.font = std::move(body);

    // The colours and metrics measured on the game's own options screen (1168x687 capture,
    // 1280x720 canvas): rows are 33 px apart with 20 px caps, labels are the dark brown ink,
    // the selection is a brick-red bar with white text, the title is white with a dark outline.
    theme.panel = 0xc2b58e;
    theme.panel_width = 900;
    theme.top = 112;
    theme.row_height = 30;
    theme.row_gap = 2;
    theme.padding = 26;
    theme.text = 0xe6e6e6;
    theme.row_text = 0x2a2117;
    theme.muted = 0x7a6a53;
    theme.accent = 0x900505;
    theme.selected_text = 0xf4f4f4;
    theme.title_text = 0xe6e6e6;
    theme.title_outline = 0x241a10;
    theme.title_left = true;
    theme.title_rule = true;
    theme.flat_highlight = true;
    theme.dots = false;
    // The game draws Demi 22 at about 1.25x its bitmap size (20 px caps, 162 -> 201 px for
    // "GAME SETTINGS"); Demi 33 at 1.25x would be far larger than the screen title, so 0.8.
    theme.text_scale = 1.25f;
    theme.title_scale = 0.8f;
    REXLOG_INFO("S3 PC theme: {} body glyphs, {} title glyphs, body atlas {}x{}, title atlas {}x{} from fonts-hd.big",
                theme.font.glyphs.size(), theme.title_font.glyphs.size(), theme.font.atlas.width,
                theme.font.atlas.height, theme.title_font.atlas.width, theme.title_font.atlas.height);
  } catch (const std::exception& e) {
    REXLOG_WARN("S3 PC theme: {} (the menu keeps its fallback look)", e.what());
  }
}
}  // namespace

const pcs::Theme* PcStreet3Theme() {
  static pcs::Theme theme = [] {
    pcs::Theme t;
    t.load = LoadTheme;
    return t;
  }();
  return &theme;
}
