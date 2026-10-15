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

const fallbackVertex = JSON.parse(compile(`pipeline FallbackVertex {
  attributes { position: float3 }
  pass "Main" {
    use attributes { position }
    vertex {
      output.position = float4(ldexp(attributes.position.x, 1), 0.0, 0.0, 1.0);
    }
    fragment { output.color = float4(1.0); }
  }
}`, '', ''));
assert.equal(fallbackVertex.success, true, JSON.stringify(fallbackVertex));
assert.match(fallbackVertex.shaders.Main.vertex, /in vec3 a_position;/);
assert.doesNotMatch(fallbackVertex.shaders.Main.vertex, /\battr0\b/);
console.log('fallback vertex attribute names: PASS');

for (const name of ['gl_interface_names', 'gl_interface_names_keyword_fallback',
                    'gl_interface_names_nested', 'gl_interface_names_uniform_arrays']) {
  const source = fs.readFileSync(new URL(`resource_p1/${name}.bwsl`, import.meta.url), 'utf8');
  const result = JSON.parse(compile(source, '', ''));
  assert.equal(result.success, true, JSON.stringify(result));
  const shaders = result.shaders.Main.vertex + result.shaders.Main.fragment;
  assert.match(shaders, /bwsl_u_input/);
  assert.doesNotMatch(shaders, /\b(?:vec4|Outer) input\s*;/);
    if (name === 'gl_interface_names_uniform_arrays') {
        for (const member of ['input', 'scales', 'bones', 'entries']) {
            assert.match(result.shaders.Main.fragment, new RegExp(`bwsl_u_${member}\\[3\\]`));
        }
    }
  console.log(`${name} uniform names: PASS`);
}
const reserved = JSON.parse(compile(
  'module Reserved { lerp :: (float x) -> float { return x; } }', '', ''));
assert.equal(reserved.success, false);
assert.match(JSON.stringify(reserved.errors), /reserved for a built-in intrinsic/);
console.log('intrinsic function name diagnostic: PASS');

const unusedNames = JSON.parse(compile(`pipeline Names {
  unused :: () -> float { MissingType value; return missingName; }
  pass "Main" { vertex { output.position = float4(1.0); } }
}`, '', '-source-file /Names.bwsl'));
assert.equal(unusedNames.success, false);
assert.equal(unusedNames.errors.length, 2);
for (const [error, token] of unusedNames.errors.map((error, i) =>
  [error, ['MissingType', 'missingName'][i]])) {
  assert.match(error.message, new RegExp(token));
  assert.equal(error.file, '/Names.bwsl');
  assert.equal(error.line, 2);
  assert.equal(error.endLine, error.line);
  assert.equal(error.endColumn - error.column, token.length);
  assert.equal(error.token, token);
}
console.log('unused names and source spans: PASS');

wasm.FS.writeFile('/NameDependency.bwsl', `module NameDependency {
  unused :: () -> float { return dependencyTypo; }
}`);
const importedNames = JSON.parse(compile(`pipeline Names { import NameDependency
  pass "Main" { vertex { output.position = float4(1.0); } }
}`, '', '-source-file /Names.bwsl -modules /'));
assert.equal(importedNames.success, false);
assert.equal(importedNames.errors.length, 1);
assert.match(importedNames.errors[0].message, /dependencyTypo/);
assert.equal(importedNames.errors[0].file, '/NameDependency.bwsl');
assert.equal(importedNames.errors[0].endColumn - importedNames.errors[0].column, 14);
assert.equal(importedNames.errors[0].token, 'dependencyTypo');
assert.ok(importedNames.errors[0].context.some(line => line.includes('dependencyTypo')));
console.log('imported module name diagnostic: PASS');

const unknownImport = JSON.parse(compile('pipeline Names { import MissingModule }', '',
  '-source-file /Names.bwsl'));
assert.equal(unknownImport.success, false);
assert.equal(unknownImport.errors[0].token, 'MissingModule');
assert.equal(unknownImport.errors[0].endColumn - unknownImport.errors[0].column, 13);
console.log('unknown import source span: PASS');

const aliasTypes = JSON.parse(compile(`module Geometry { struct Point { float x; } }
pipeline Aliases { import Geometry
  using Position = Geometry::Point
  struct Container { Position value; }
  identityPosition :: (Position value) -> Position { return value; }
  field :: (Container value) -> float { return value.value.x; }
  pass "Main" {
    vertex { Position p; p.x = 1.0; output.position = float4(identityPosition(p).x); }
    fragment { output.color = float4(1.0); }
  }
}`, '', ''));
assert.equal(aliasTypes.success, true, JSON.stringify(aliasTypes));
console.log('type aliases in fields and return types: PASS');

// Exercise allocation boundaries through the exported API, including locations
// in stage errors (which do not use the CLI's DiagnosticStream renderer).
const computeLimits = (helper, body) => `pipeline RegisterLimits {
  resources { inbuf: buffer<float> output: buffer<float> }
  ${helper}
  pass "Main" { use resources {inbuf, output} compute "Main" [8,1,1] {
    uint idx=input.global_id.x; float delta=resources.inbuf[idx]; float acc=delta;
    ${body}
    resources.output[idx]=acc;
  }}
}`;
const lowerLimit = computeLimits(
  `burst :: (float x, float delta) -> float { ${'x=x+delta;\n'.repeat(64)} return x; }`,
  'acc=burst(acc,delta);\n'.repeat(260));
const variables = Array.from({length: 32}, (_, i) => `v${i}`);
const ssaLimit = computeLimits(`burst :: (float delta, bool flip) -> float {
  ${variables.map(v => `float ${v}=delta;`).join('\n')}
  for(uint j=0u;j<2u;j=j+1u){if(flip){
    ${variables.map((v, i) => `${v}=${variables[(i+1)%32]};`).join('\n')}
  }else{${variables.map(v => `${v}=delta;`).join('\n')}}}
  return ${variables.join('+')};
}`, 'bool flip=(idx&1u)==0u;\n' + 'acc=burst(acc,flip);\n'.repeat(120));
for (const [name, source, diagnostic] of [
  ['lowering register limit', lowerLimit, 'Shader register limit exceeded'],
  ['SSA register limit', ssaLimit, 'SSA register limit exceeded'],
]) {
  const result = JSON.parse(compile(source, '', '-spv'));
  assert.equal(result.success, false, name);
  const errors = JSON.stringify(result.errors);
  assert.ok(errors.includes(diagnostic), errors);
  assert.match(errors, /line [1-9]\d*:[1-9]\d*:/);
  console.log(`${name} located diagnostic: PASS`);
}
for (const unsigned of [false, true]) {
  const kind = unsigned ? 'uint' : 'int';
  for (const count of [600, 8193]) {
    const body = `${kind} value=${kind}(delta);\n` +
      Array.from({length: count}, (_, i) => `value=value+${i}${unsigned ? 'u' : ''};`).join('\n') +
      '\nacc=float(value);';
    const result = JSON.parse(compile(computeLimits('', body), '', '-spv'));
    assert.equal(result.success, count === 600, JSON.stringify(result.errors));
    if (count > 8192) {
      assert.ok(JSON.stringify(result.errors).includes('integer constant limit exceeded'));
      assert.match(JSON.stringify(result.errors), /line [1-9]\d*:[1-9]\d*:/);
    }
    console.log(`${kind} constant pool (${count} values): PASS`);
  }
}
const highGather = `pipeline HighGather { resources {tex: texture2D}
  pass "Main" {use resources {tex} vertex {output.position=float4(1.0);}
    fragment {float delta=input.position.x;float acc=delta;
      ${'acc=acc+delta;\n'.repeat(9000)}
      if(delta>0.0){acc=acc+delta;}else{acc=acc-delta;}
      output.color=gather(resources.tex,float2(acc),0);
    }
  }
}`;
const gatherResult = JSON.parse(compile(highGather, '', '-spv'));
assert.equal(gatherResult.success, true, JSON.stringify(gatherResult.errors));
assert.ok(gatherResult.shaders.Main.fragment.includes('bwsl_gather'));
console.log('high-register gather and direct GLES scratch: PASS');

const shadowSource = `pipeline Shadows {
  helper :: (float value) -> float { { float value = 2.0; } return value; }
  pass "Main" { vertex { output.position = float4(helper(1.0)); } fragment { output.color = float4(1.0); } }
}`;
const shadows = JSON.parse(compile(shadowSource, '', '-source-file /Shadows.bwsl -spv'));
assert.equal(shadows.success, true, JSON.stringify(shadows));
assert.equal(shadows.diagnostics.length, 1);
const shadow = shadows.diagnostics[0];
assert.equal(shadow.severity, 'note');
assert.equal(shadow.file, '/Shadows.bwsl');
assert.equal(shadow.line, 2);
assert.equal(shadow.token, 'value');
assert.equal(shadow.endColumn - shadow.column, 5);
assert.match(shadow.message, /shadows parameter declared at \/Shadows.bwsl:2:/);
assert.ok(shadows.files.some(file => file.name.endsWith('.spv')));
console.log('shadowing notes preserve successful WASM compilation: PASS');

wasm.FS.writeFile('/ShadowDependency.bwsl', `module ShadowDependency {
  helper :: (float value) -> float { { float value = 2.0; } return value; }
}`);
const importedShadows = JSON.parse(compile(`pipeline ImportedShadows { import ShadowDependency
  pass "Main" { vertex { output.position = float4(1.0); } fragment { output.color = float4(1.0); } }
}`, '', '-source-file /Shadows.bwsl -modules / -spv'));
assert.equal(importedShadows.success, true, JSON.stringify(importedShadows));
assert.equal(importedShadows.diagnostics.length, 1);
assert.equal(importedShadows.diagnostics[0].file, '/ShadowDependency.bwsl');
assert.equal(importedShadows.diagnostics[0].token, 'value');
assert.ok(importedShadows.diagnostics[0].context.some(line => line.includes('float value = 2.0')));
console.log('imported shadowing note source/context: PASS');

const shadowError = JSON.parse(compile(shadowSource.replace('return value;', 'return missingName;'),
  '', '-source-file /Shadows.bwsl'));
assert.equal(shadowError.success, false);
assert.equal(shadowError.errors.length, 1);
assert.equal(shadowError.errors[0].severity, 'error');
assert.deepEqual(shadowError.diagnostics.map(item => item.severity), ['note', 'error']);
console.log('shadowing note stays separate from WASM errors: PASS');
