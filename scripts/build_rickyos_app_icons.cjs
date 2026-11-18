#!/usr/bin/env node
/** Original RickyOS app-grid SVGs -> native 56 px 1-bpp Flash resources.
 * Same line system as the tab bar (scripts/build_rickyos_navigation_icons.cjs):
 * 56-unit viewBox, 3.6 stroke, round caps and joins, threshold 110.
 * Developer-only dependency: sharp (librsvg).
 * Run: NODE_PATH=/path/to/node_modules node scripts/build_rickyos_app_icons.cjs [--out header.h]
 * The checked-in header makes device/simulator builds independent of Node/sharp.
 */
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');

const root = path.resolve(__dirname, '..');
const sourceDir = path.join(root, 'src/components/icons/sources/rickyos/apps');
const SIZE = 56;
const THRESHOLD = 110;
const names = ['transfer', 'opds', 'weread', 'stats', 'bookmark', 'calculator', 'standby'];

// clang-format lays a braced list out in columns: it keeps as many items per line as
// fit the column limit, then spreads the items evenly over the rows that are left.
// A plain "19 per line" split leaves a single item on the last line, which
// clang-format re-flows, so the generator has to use the same rule.
function itemsPerLine(count, maxPerLine) {
  const rows = Math.ceil(count / maxPerLine);
  return Math.ceil(count / rows);
}

let output = path.join(root, 'src/components/icons/rickyAppIcons.h');
const args = process.argv.slice(2);
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--out' && args[i + 1]) output = path.resolve(args[++i]);
  else throw new Error('Usage: build_rickyos_app_icons.cjs [--out header.h]');
}

async function main() {
  const lines = ['#pragma once', '', '#include <cstdint>', '',
    '// Generated from ORIGINAL RickyOS SVGs by scripts/build_rickyos_app_icons.cjs.',
    '// Edit sources/rickyos/apps/*.svg and regenerate; never edit these bytes manually.',
    `// ${SIZE} x ${SIZE}, MSB-first, 1 = transparent/white, 0 = ink. Upright; no runtime resampling.`, '',
    `inline constexpr int kRickyAppIconSize = ${SIZE};`, ''];
  for (const name of names) {
    const svg = fs.readFileSync(path.join(sourceDir, name + '.svg')).toString()
      .replace('<svg ', `<svg width="${SIZE}" height="${SIZE}" `);
    const { data, info } = await sharp(Buffer.from(svg)).flatten({ background: '#fff' })
      .greyscale().raw().toBuffer({ resolveWithObject: true });
    if (info.channels !== 1 || info.width !== SIZE || info.height !== SIZE) throw new Error(`${name}: bad raster`);
    const rowBytes = Math.ceil(SIZE / 8);
    const bits = Buffer.alloc(rowBytes * SIZE, 255);
    for (let y = 0; y < SIZE; ++y) for (let x = 0; x < SIZE; ++x) {
      if (data[y * SIZE + x] < THRESHOLD) bits[y * rowBytes + (x >> 3)] &= ~(0x80 >> (x & 7));
    }
    lines.push(`// ${name}; ${bits.length} bytes in Flash.`);
    lines.push(`inline constexpr uint8_t ricky_app_${name}_${SIZE}[] = {`);
    const perLine = itemsPerLine(bits.length, 19);  // clang-format packs 19 per line
    for (let offset = 0; offset < bits.length; offset += perLine) {
      lines.push('    ' + [...bits.subarray(offset, offset + perLine)]
        .map(byte => '0x' + byte.toString(16).padStart(2, '0').toUpperCase()).join(', ') + ',');
    }
    lines.push('};', '');
  }
  fs.writeFileSync(output, lines.join('\n'));
  console.log(`Generated ${output}: ${names.length} icons.`);
}

main().catch(error => { console.error(error); process.exit(1); });
