#!/bin/bash
#
# Regenerates the RickyOS 12pt CJK header WITH IPA/phonetic-symbol coverage.
#
# The stock notosans_cjk_12_rickyos font covers Han + ASCII + Western European
# punctuation, but dictionary phonetic transcriptions use IPA glyphs (ə ɪ ʃ ʒ
# ŋ ˈ ˌ ː and Greek θ/π/etc.) in U+0100-02FF / U+0370-03FF. Without them, the
# dictionary and saved-word cards rendered tofu boxes. This script merges a
# small IPA-only Noto Sans subset into the existing CJK font via fontconvert's
# fontstack: the CJK face keeps priority for every glyph it already has, so all
# 6877 pre-existing glyphs are byte-identical; the second face supplies only
# the missing phonetic glyphs.
#
# Inputs (both committed):
#   source/NotoSansSC/NotoSansSC-Regular.otf  - official Noto Sans CJK SC
#       (SHA-256 2c76254f...; downloaded on demand, gitignored — see
#        build-rickyos-cjk-font.sh / docs/engineering/chinese-build.md)
#   source/IPA/IPA-nokern.ttf                  - IPA-only Noto Sans subset,
#       glyphs U+0100-02FF + U+0370-03FF, hinting/kerning stripped.
#
#   PYTHON=/path/to/venv/bin/python bash build-rickyos-cjk-font-ipa.sh
#
set -euo pipefail

cd "$(dirname "$0")"

PYTHON="${PYTHON:-python3}"
SOURCE_OTF="../builtinFonts/source/NotoSansSC/NotoSansSC-Regular.otf"
IPA_TTF="../builtinFonts/source/IPA/IPA-nokern.ttf"
SOURCE_SHA256="2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
CHARSET_FILE="cn_rickyos_chars.txt"
TMP_DIR="instanced_fonts/NotoSansSC"
SUBSET_OTF="$TMP_DIR/NotoSansSC-Regular.rickyos.otf"
FONT_NAME="notosans_cjk_12_rickyos"
OUTPUT_PATH="../builtinFonts/${FONT_NAME}.h"

for f in "$SOURCE_OTF" "$IPA_TTF"; do
  if [ ! -f "$f" ]; then
    echo "Error: $f not found" >&2
    exit 1
  fi
done

"$PYTHON" - "$SOURCE_OTF" "$SOURCE_SHA256" <<'EOF'
import hashlib, sys
digest = hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest()
if digest != sys.argv[2]:
    sys.exit(f'Error: source SHA-256 {digest} differs from the committed fonts\' {sys.argv[2]}')
EOF

# Rebuild the exact CJK charset the committed font uses (common + GB2312 lv1 + Big5 lv1).
"$PYTHON" - cn_common_chars.txt gb2312_lv1.txt big5_rickyos_chars.txt "$CHARSET_FILE" <<'EOF'
import sys
common = open(sys.argv[1], encoding='utf-8').read().strip()
seen = set(common); text = common
for source in sys.argv[2:4]:
    extra = [c for c in open(source, encoding='utf-8').read()
             if 0x4E00 <= ord(c) <= 0x9FFF and c not in seen and not seen.add(c)]
    text += ''.join(extra)
open(sys.argv[4], 'w', encoding='utf-8').write(text)
EOF

mkdir -p "$TMP_DIR"
"$PYTHON" -m fontTools.subset "$SOURCE_OTF" \
  --output-file="$SUBSET_OTF" \
  --text-file="$CHARSET_FILE" \
  --unicodes="U+0020-007E,U+00A0-00FF,U+2010-2026,U+3000-303F,U+FF00-FF9F,U+FFA1-FFEF,U+FFFD" \
  --layout-features='*' --notdef-outline --recommended-glyphs --no-hinting \
  --drop-tables+=DSIG,GSUB,GPOS

tmp_path="${OUTPUT_PATH}.tmp"
# fontstack: CJK subset first (keeps every existing glyph byte-identical),
# IPA subset second (fills U+0100-02FF / U+0370-03FF phonetic glyphs).
"$PYTHON" fontconvert.py "$FONT_NAME" 12 "$SUBSET_OTF" "$IPA_TTF" \
  --2bit \
  --additional-intervals 0x100,0x2FF \
  --additional-intervals 0x370,0x3FF \
  --additional-intervals 0x4E00,0x9FFF \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0xFF00,0xFFEF \
  > "$tmp_path"
if [ ! -s "$tmp_path" ]; then
  echo "Error: fontconvert.py produced empty $tmp_path" >&2
  rm -f "$tmp_path"
  exit 1
fi
mv "$tmp_path" "$OUTPUT_PATH"
echo "  $(wc -c < "$OUTPUT_PATH") bytes ($(grep -E "Bitmaps\[" "$OUTPUT_PATH" | head -1))"
