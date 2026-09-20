import assert from 'node:assert/strict'
import test from 'node:test'

import { applyTypedArrayStorage } from '../dist/cpp-store-typed-arrays.js'

const ir = {
  stores: [
    {
      className: 'NotesStore',
      fields: [
        {
          name: 'notes',
          typedStorage: { itemType: 'NotesStore_notes_item', readerName: 'read_NotesStore_notes' },
        },
      ],
    },
  ],
}

const boxedSeed =
  'std::vector<gea_cpp_value>{([&]() { gea_cpp_value __gea_record_value_1; ' +
  '__gea_record_value_1.record_set_literal("id", std::string("n1")); ' +
  'return __gea_record_value_1; })()}'

// The module-split (typed-snapshot) emitter puts the class definition in the
// layout header and the constructor OUT-OF-LINE in the module .cpp. The field
// declaration is retyped when the header is processed, so a .cpp whose ctor
// seed stays `std::vector<gea_cpp_value>{...}` fails to compile ("no viable
// overloaded '='"). This pins the out-of-line rewrite (notes-jsx regression).
test('rewrites boxed ctor seeds in out-of-line store constructors', () => {
  const source = [
    'NotesStore::NotesStore() : Store() {',
    '  this->selectedFolderId = std::string("all");',
    `  this->notes = ${boxedSeed};`,
    '}',
  ].join('\n')
  const next = applyTypedArrayStorage(source, ir)
  assert.ok(!next.includes('this->notes = std::vector<gea_cpp_value>'), next)
  assert.ok(next.includes('std::vector<gea_ir::NotesStore_notes_item>'), next)
})

// The pre-split emitter kept the constructor inline in the class body; that
// path must keep rewriting too.
test('still rewrites boxed ctor seeds inside the class body', () => {
  const source = [
    'class NotesStore : public Store {',
    ' public:',
    '  mutable std::vector<gea_cpp_value> notes{};',
    '  NotesStore() : Store() {',
    `    this->notes = ${boxedSeed};`,
    '  }',
    '};',
  ].join('\n')
  const next = applyTypedArrayStorage(source, ir)
  assert.ok(next.includes('mutable std::vector<gea_ir::NotesStore_notes_item> notes{};'), next)
  assert.ok(!next.includes('this->notes = std::vector<gea_cpp_value>'), next)
})

// Assignments in out-of-line methods that are not array-literal seeds must be
// left alone — the rewrite targets only the seed shapes the emitter produces.
test('leaves non-seed assignments in out-of-line methods untouched', () => {
  const source = [
    'void NotesStore::replaceNotes(gea_cpp_value value) {',
    '  this->notes = coerceNotes(value);',
    '}',
  ].join('\n')
  const next = applyTypedArrayStorage(source, ir)
  assert.equal(next, source)
})
