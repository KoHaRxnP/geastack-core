#!/usr/bin/env node

import { existsSync, readFileSync, writeFileSync } from 'node:fs';

const [, , partitionsPath, outputPath] = process.argv;

if (!partitionsPath || !outputPath) {
  console.error('Usage: generate-installed-app-plan.mjs <partitions.csv> <output.h>');
  process.exit(1);
}

let source = '';
try {
  source = readFileSync(partitionsPath, 'utf8');
} catch {
  source = '';
}

const entries = [];
for (const line of source.split(/\r?\n/)) {
  const match = line.match(/^\s*#\s*(ota_\d+)\s*:\s*([A-Za-z0-9_.-]+)\s*$/);
  if (match) entries.push({ slot: match[1], appId: match[2] });
}

const quote = (value) => value.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
const lines = [
  '#pragma once',
  '',
  'typedef struct {',
  '    const char *app_id;',
  '    const char *slot_label;',
  '} gea_embedded_installed_app_plan_entry_t;',
  '',
  'static const gea_embedded_installed_app_plan_entry_t gea_embedded_installed_app_plan[] = {',
];

if (entries.length === 0) {
  lines.push('    { 0, 0 },');
} else {
  for (const entry of entries) {
    lines.push(`    { "${quote(entry.appId)}", "${quote(entry.slot)}" },`);
  }
}

lines.push(
  '};',
  `static const int gea_embedded_installed_app_plan_count = ${entries.length};`,
  '',
);

const newContent = lines.join('\n');
const existing = existsSync(outputPath) ? readFileSync(outputPath, 'utf8') : null;
if (existing !== newContent) {
  writeFileSync(outputPath, newContent);
}
