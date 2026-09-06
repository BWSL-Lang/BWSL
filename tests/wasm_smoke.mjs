// Run after `make wasm`: node tests/wasm_smoke.mjs [build/wasm]
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import assert from 'node:assert/strict';

const directory = path.resolve(process.argv[2] ?? 'build/wasm');
const {default: factory} = await import(pathToFileURL(path.join(directory, 'bwsl.js')));
const wasm = await factory({
  wasmBinary: fs.readFileSync(path.join(directory, 'bwsl.wasm')),
  print() {}, printErr() {},
});
const compile = wasm.cwrap('compile', 'string', ['string', 'string', 'string']);
const raster = `pipeline Smoke {
  attributes { position: float3 }
  pass "Main" {
    use attributes { position }
    vertex { output.position = float4(attributes.position, 1.0); }
    fragment {
      float r = 0.0;
      for (int i = 0; i < 8; i++) {
        if (i == 3) { skip; }
        if (i == 6) { break; }
        r += float(i);
      }
      output.color = float4(r);
    }
  }
}`;
const compute = fs.readFileSync(new URL('equivalence/test_regression_binary_literals.bwsl', import.meta.url), 'utf8');
for (const [name, source, count] of [['raster', raster, 6], ['compute', compute, 3]]) {
  const result = JSON.parse(compile(source, '', '-spv'));
  assert.equal(result.success, true, JSON.stringify(result));
  assert.equal(result.files.length, count);
  assert.ok(result.files.some(file => file.name.endsWith('.spv')));
  console.log(`${name} compilation and SPIR-V sidecars: PASS`);
}
const invalid = JSON.parse(compile('pipeline Broken {', '', ''));
assert.equal(invalid.success, false);
assert.ok(invalid.errors.length > 0);
console.log('invalid source diagnostic: PASS');

for (const name of ['test_regression_eval_pipeline_functions', 'test_regression_pattern_plain',
                    'test_semantics_branch_table_growth', 'test_regression_num_workgroups_single']) {
  const file = new URL(`equivalence/${name}.bwsl`, import.meta.url);
  const result = JSON.parse(compile(fs.readFileSync(file, 'utf8'), '', '-spv'));
  assert.equal(result.success, true, `${name}: ${JSON.stringify(result)}`);
  assert.ok(result.files.some(file => file.name.endsWith('.spv')));
  console.log(`${name}: PASS`);
}
for (const [name, diagnostic] of [
  ['remaining_array_fraction', 'Array size must be an integer'],
  ['remaining_const_swizzle', 'Cannot assign to const'],
  ['function_missing_return', 'Non-void function must return a value'],
  ['remaining_float_bitwise', 'Bitwise operators require integer operands'],
  ['remaining_array_folded_read', 'out of bounds'],
]) {
  const source = fs.readFileSync(new URL(`error_cases/${name}.bwsl`, import.meta.url), 'utf8');
  const result = JSON.parse(compile(source, '', '-spv'));
  assert.equal(result.success, false, name);
  assert.ok(JSON.stringify(result.errors).toLowerCase().includes(diagnostic.toLowerCase()), JSON.stringify(result));
  console.log(`${name} diagnostic: PASS`);
}
