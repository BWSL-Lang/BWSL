#!/usr/bin/env python3
"""AST JSON schema and scope regressions (issues #94, #97 and #99)."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

from run_tests import (
    AST_JSON_POSITION_TESTS, AST_JSON_REFERENCE_TESTS,
    check_ast_json_positions, check_ast_json_references,
)

ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / 'build' / 'bwslc'


def objects(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from objects(child)
    elif isinstance(value, list):
        for child in value:
            yield from objects(child)


class AstJsonTests(unittest.TestCase):
    def parse(self, source, dependencies=None):
        with tempfile.TemporaryDirectory(prefix='bwsl-ast-json-') as directory:
            folder = Path(directory)
            for name, contents in (dependencies or {}).items():
                (folder / name).write_text(contents)
            path = folder / 'main.bwsl'
            path.write_text(textwrap.dedent(source).lstrip('\n'))
            result = subprocess.run(
                [str(COMPILER), str(path), '-ast-json', '-modules', directory],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads(result.stdout)
        ok, message = check_ast_json_references(data, {})
        self.assertTrue(ok, message)
        return data

    def nodes(self, data, kind=None, name=None):
        # The legacy root duplicates a subtree; inspect canonical containers only.
        return [node for node in objects([data['modules'], data['pipelines']])
                if isinstance(node.get('type'), str)
                and (kind is None or node['type'] == kind)
                and (name is None or node.get('name') == name)]

    def edges(self, data, role=None):
        return [edge for edge in data['referenceIndex']['references']
                if role is None or edge['role'] == role]

    def assert_edge(self, data, source, target, role):
        self.assertIn({'from': source, 'to': target, 'role': role}, self.edges(data))

    def assert_type_shape(self, info, element, sizes):
        self.assertEqual(info['elementType'], element)
        self.assertEqual(info['arraySizes'], sizes)
        self.assertEqual(info['arrayDimensions'], len(sizes))
        length = 0 if not sizes else 1
        for size in sizes:
            length *= size
        self.assertEqual(info['arrayLength'], length)

    def test_array_metadata_across_parameters_locals_fields_and_symbols(self):
        data = self.parse('''module Arrays {
            struct Light { float3 color; float[4] weights; }
            sum :: (float[3] values, Light[2] lights, float scale) -> float {
                float[2] local;
                Light[2] localLights;
                float total = values[0] * scale + local[1] + lights[0].color.x
                    + localLights[1].weights[0];
                return total;
            }
        }''')
        function = self.nodes(data, 'FUNCTION', 'sum')[0]
        fields = self.nodes(data, 'STRUCT_DECL', 'Light')[0]['fields']
        declarations = [
            (function['parameters'][0], 'float', [3], 'dataType'),
            (function['parameters'][1], 'Light', [2], 'dataType'),
            (function['parameters'][2], 'float', [], 'dataType'),
            (self.nodes(data, 'VARIABLE_DECL', 'local')[0], 'float', [2], 'declaredType'),
            (self.nodes(data, 'VARIABLE_DECL', 'localLights')[0], 'Light', [2], 'declaredType'),
            (self.nodes(data, 'VARIABLE_DECL', 'total')[0], 'float', [], 'declaredType'),
            (fields[0], 'float3', [], 'dataType'),
            (fields[1], 'float', [4], 'dataType'),
        ]
        symbols = {symbol['id']: symbol for symbol in data['referenceIndex']['symbols']}
        for declaration, element, sizes, type_key in declarations:
            with self.subTest(name=declaration['name']):
                self.assertEqual(declaration[type_key], element)
                self.assert_type_shape(declaration['typeInfo'], element, sizes)
                symbol = symbols[declaration['id']]
                self.assertEqual(symbol['type'], element)
                self.assert_type_shape(symbol['typeInfo'], element, sizes)
        self.assertEqual(fields[0]['typeInfo']['componentCount'], 3)
        self.assertEqual(fields[1]['arraySize'], 4)  # Legacy field retained.
        self.assert_type_shape(function['returnTypeInfo'], 'float', [])
        self.assertIn('sum(float[3],Light[2],float)', symbols[function['id']]['stableId'])

    def test_multidimensional_array_shapes_and_parameter_syntaxes(self):
        data = self.parse('''module Arrays {
            const int ROWS = 2;
            struct Light { float x; }
            inspect :: (float[ROWS][3] values, lights: Arrays::Light[2][4],
                        Arrays::Light[5] qualified, float[6]) -> void {
                float[2][3] local;
                Light[3][4] custom;
            }
        }''')
        function = self.nodes(data, 'FUNCTION', 'inspect')[0]
        for parameter, element, sizes in zip(function['parameters'],
                ['float', 'Arrays::Light', 'Arrays::Light', 'float'],
                [[2, 3], [2, 4], [5], [6]]):
            self.assert_type_shape(parameter['typeInfo'], element, sizes)
        for name, element, sizes in [('local', 'float', [2, 3]), ('custom', 'Light', [3, 4])]:
            node = self.nodes(data, 'VARIABLE_DECL', name)[0]
            self.assert_type_shape(node['typeInfo'], element, sizes)
            self.assertEqual(node['arrayDimensions'], len(sizes))
            self.assertEqual(node['arrayLength'], node['typeInfo']['arrayLength'])

    def test_imported_and_aliased_array_element_types(self):
        data = self.parse('''pipeline P { import Geometry as G
            using Position = G::Point
            struct Container { G::Point[3] points; }
            inspect :: (G::Point[2] points, Position[4] aliases) -> G::Point {
                G::Point[5] local;
                Position[6] aliasLocal;
                return points[0];
            }
        }''', {'Geometry.bwsl': 'module Geometry { struct Point { float x; } }'})
        function = self.nodes(data, 'FUNCTION', 'inspect')[0]
        for parameter, element, sizes in zip(function['parameters'],
                ['Geometry::Point', 'Position'], [[2], [4]]):
            self.assert_type_shape(parameter['typeInfo'], element, sizes)
        local = self.nodes(data, 'VARIABLE_DECL', 'local')[0]
        self.assert_type_shape(local['typeInfo'], 'Geometry::Point', [5])
        alias = self.nodes(data, 'VARIABLE_DECL', 'aliasLocal')[0]
        self.assert_type_shape(alias['typeInfo'], 'Position', [6])
        field = self.nodes(data, 'STRUCT_DECL', 'Container')[0]['fields'][0]
        self.assert_type_shape(field['typeInfo'], 'Geometry::Point', [3])
        point = self.nodes(data, 'STRUCT_DECL', 'Point')[0]
        for declaration in [*function['parameters'], local, alias, field]:
            self.assert_edge(data, declaration['id'], point['id'], 'type')
        self.assert_type_shape(function['returnTypeInfo'], 'Geometry::Point', [])

    def test_vector_matrix_and_return_types_are_not_arrays(self):
        data = self.parse('''module Shapes {
            struct Vectors { float3 direction; mat4 transform; float3[2] points; }
            identity :: (float3 value, mat4 transform) -> float3 {
                float3 direction = value;
                mat4 matrix = transform;
                return direction;
            }
        }''')
        function = self.nodes(data, 'FUNCTION', 'identity')[0]
        for parameter in function['parameters']:
            self.assert_type_shape(parameter['typeInfo'], parameter['dataType'], [])
        for field, sizes in zip(self.nodes(data, 'STRUCT_DECL')[0]['fields'], [[], [], [2]]):
            self.assert_type_shape(field['typeInfo'], field['dataType'], sizes)
        for name in ['direction', 'matrix']:
            node = self.nodes(data, 'VARIABLE_DECL', name)[0]
            self.assert_type_shape(node['typeInfo'], node['declaredType'], [])
        self.assert_type_shape(function['returnTypeInfo'], 'float3', [])

    def test_array_parameter_sizes_use_parsed_constant_values(self):
        data = self.parse('''module Arrays {
            const int COUNT = 4;
            inspect :: (float[COUNT] named, float[0x10] hex, float[0b11] binary) -> void {}
        }''')
        for parameter, sizes in zip(self.nodes(data, 'FUNCTION')[0]['parameters'], [[4], [16], [3]]):
            self.assert_type_shape(parameter['typeInfo'], 'float', sizes)

    def test_array_method_parameters_and_imported_local_metadata(self):
        data = self.parse("pipeline P { import Geometry }", {
            'Geometry.bwsl': """module Geometry {
                struct Point {
                    float x;
                    combine :: (float3[2] vectors, weights: float[3]) -> float {
                        float3[2] scratch;
                        return x;
                    }
                }
            }""",
        })
        method = self.nodes(data, 'FUNCTION', 'combine')[0]
        self.assertTrue(method['sourceFile'].endswith('Geometry.bwsl'))
        symbols = {symbol['id']: symbol for symbol in data['referenceIndex']['symbols']}
        for parameter, element, sizes in zip(method['parameters'], ['float3', 'float'], [[2], [3]]):
            self.assert_type_shape(parameter['typeInfo'], element, sizes)
            self.assert_type_shape(symbols[parameter['id']]['typeInfo'], element, sizes)
        local = self.nodes(data, 'VARIABLE_DECL', 'scratch')[0]
        self.assert_type_shape(local['typeInfo'], 'float3', [2])
        self.assert_type_shape(symbols[local['id']]['typeInfo'], 'float3', [2])

    def test_imported_array_qualifiers_with_identical_source_positions(self):
        data = self.parse('pipeline P { import A, B }', {
            'A.bwsl': 'module A { import First\nstruct Box { First::Point[2] points; }\n}',
            'B.bwsl': 'module B { import Other\nstruct Box { Other::Point[3] points; }\n}',
            'First.bwsl': 'module First { struct Point { float x; } }',
            'Other.bwsl': 'module Other { struct Point { float y; } }',
        })
        boxes = self.nodes(data, 'STRUCT_DECL', 'Box')
        symbols = {symbol['id']: symbol for symbol in data['referenceIndex']['symbols']}
        for box, element, sizes in zip(boxes, ['First::Point', 'Other::Point'], [[2], [3]]):
            field = box['fields'][0]
            self.assertEqual(field['dataType'], element)
            self.assert_type_shape(field['typeInfo'], element, sizes)
            self.assert_type_shape(symbols[field['id']]['typeInfo'], element, sizes)

    def test_array_parameter_rejects_invalid_and_overflowing_sizes(self):
        for suffix in ['[0]', '[262145]', '[512][513]', '[1.5]']:
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'invalid.bwsl'
                path.write_text(f'module M {{ inspect :: (float{suffix} values) -> void {{}} }}')
                result = subprocess.run([str(COMPILER), str(path), '-check', '-errors-json'],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertFalse(json.loads(result.stdout)['success'])

    def test_document_roots_in_source_order_exclude_imported_modules(self):
        data = self.parse('''
            pipeline First { import Dependency }
            module Local { helper :: () -> float { return 1.0; } }
            pipeline Second {}
        ''', {'Dependency.bwsl': 'module Dependency { value :: () -> float { return 0.0; } }'})
        names = {node['id']: node['name'] for node in data['modules'] + data['pipelines']}
        self.assertEqual([names[ref] for ref in data['roots']], ['First', 'Local', 'Second'])
        self.assertEqual(data['schema'], 'bwsl.ast.v3')
        self.assertEqual(data['root']['name'], 'Second')
        self.assertEqual(self.parse('module M {}')['roots'], ['MODULE:0'])

    def test_attribute_metadata_and_use_scopes(self):
        data = self.parse('''
            pipeline A {
                attributes { position: float3 }
                pass "A" { use attributes { position } }
            }
            pipeline B {
                attributes { position: float3 }
                pass "B" { use attributes { position } }
            }
        ''')
        attributes = self.nodes(data, 'ATTRIBUTE_DECL')
        passes = self.nodes(data, 'PASS')
        for attribute, shader_pass in zip(attributes, passes):
            self.assertIsNone(attribute['compression'])
            use = shader_pass['usedAttributes'][0]
            self.assert_edge(data, use['id'], attribute['id'], 'attribute')

    def test_explicit_compression_is_preserved(self):
        data = self.parse("pipeline P { attributes { position: float3 @compressed(10_10_10) } }")
        self.assertEqual(self.nodes(data, 'ATTRIBUTE_DECL')[0]['compression'], '10_10_10')

    def test_reference_checker_rejects_missing_occurrence(self):
        data = self.parse("module M { helper :: () -> float { return 1.0; } }")
        data['referenceIndex']['references'].append(
            {'from': 'IDENTIFIER:999', 'to': 'MODULE:0', 'role': 'qualifier'})
        ok, message = check_ast_json_references(data, {})
        self.assertFalse(ok)
        self.assertIn('source', message)

    def test_parameter_positions_and_ids(self):
        source = '''module M {
    struct Box { float size; }
    f :: (float2 pos, angle: float, Box b, M::Box c, float[2] values, float) -> void {}
}'''
        data = self.parse(source)
        function = self.nodes(data, 'FUNCTION', 'f')[0]
        line = source.splitlines()[2]
        expected = [('pos', 'float2'), ('angle', 'float'), ('b', 'Box'),
                    ('c', 'M::Box'), ('values', 'float[2]'), ('', 'float')]
        starts = [line.index('float2'), line.index('angle:'), line.index('Box b'),
                  line.index('M::Box'), line.index('float[2]'), line.rindex('float')]
        for i, (parameter, (name, spelling), start) in enumerate(zip(function['parameters'], expected, starts)):
            self.assertEqual(parameter['id'], function['id'] + f'/parameter:{i}')
            type_column = line.index(spelling, start) + 1
            self.assertEqual((parameter['typeLine'], parameter['typeColumn']), (3, type_column))
            if name:
                name_column = start + 1 if name == 'angle' else line.index(name, start + len(spelling)) + 1
                self.assertEqual((parameter['nameLine'], parameter['nameColumn']), (3, name_column))
            else:
                self.assertNotIn('nameLine', parameter)
                self.assertNotIn('nameColumn', parameter)

    def test_for_initializer_positions(self):
        data = self.parse('''module M {
    sum :: () -> int {
        int total = 0;
        for (int i = 0; i < 3; i++) { total += i; }
        return total;
    }
}''')
        node = self.nodes(data, 'VARIABLE_DECL', 'i')[0]
        self.assertEqual({key: node[key] for key in ('typeLine', 'typeColumn', 'nameLine', 'nameColumn')},
                         {'typeLine': 4, 'typeColumn': 14, 'nameLine': 4, 'nameColumn': 18})

    def test_struct_field_ids_and_shadowing(self):
        data = self.parse('''
            module M {
                struct Box {
                    float size;
                    area :: () -> float { return size; }
                    parameter :: (float size) -> float { return size; }
                    local :: () -> float { float size = 3.0; return size; }
                }
            }
        ''')
        field = self.nodes(data, 'STRUCT_DECL')[0]['fields'][0]
        self.assertEqual(field['id'], 'STRUCT_DECL:0/field:0')
        identifiers = self.nodes(data, 'IDENTIFIER', 'size')
        self.assert_edge(data, identifiers[0]['id'], field['id'], 'read')
        self.assert_edge(data, identifiers[1]['id'], 'FUNCTION:1/parameter:0', 'read')
        self.assert_edge(data, identifiers[2]['id'], 'VARIABLE_DECL:0', 'read')

    def test_struct_self_field_and_method_references(self):
        data = self.parse("""
            module Self {
                struct Light {
                    float intensity;
                    float3 color;
                    twice :: () -> float { return intensity * 2.0; }
                    scaled :: (float k) -> float {
                        float a = intensity * k;
                        float b = self.intensity * k;
                        float c = self.color.x;
                        float d = self.twice();
                        return a + b + c + d;
                    }
                }
                consume :: (Light l) -> float { return l.intensity + l.twice(); }
            }
        """)
        structure = self.nodes(data, 'STRUCT_DECL', 'Light')[0]
        fields = {field['name']: field['id'] for field in structure['fields']}
        for node in self.nodes(data, 'IDENTIFIER', 'intensity'):
            self.assert_edge(data, node['id'], fields['intensity'], 'read')
        for node in self.nodes(data, 'MEMBER_ACCESS'):
            if node['member'] in fields:
                self.assert_edge(data, node['id'], fields[node['member']], 'member')
            else:  # Swizzles have no declaration target.
                self.assertFalse(any(e['from'] == node['id'] for e in self.edges(data)))
        method = self.nodes(data, 'FUNCTION', 'twice')[0]
        for call in self.nodes(data, 'FUNCTION_CALL', 'twice'):
            self.assert_edge(data, call['id'], method['id'], 'call')
        receivers = self.nodes(data, 'IDENTIFIER', 'self')
        self.assertEqual(len(receivers), 3)
        for receiver in receivers:
            self.assert_edge(data, receiver['id'], structure['id'], 'type')

    def test_struct_self_bypasses_parameter_and_local_shadowing(self):
        data = self.parse("""
            module M {
                struct Box {
                    float size;
                    parameter :: (float size) -> float {
                        self.size = size;
                        return self.size;
                    }
                    local :: () -> float {
                        float size = 3.0;
                        self.size = size;
                        return self.size;
                    }
                }
            }
        """)
        structure = self.nodes(data, 'STRUCT_DECL', 'Box')[0]
        field = structure['fields'][0]
        members = self.nodes(data, 'MEMBER_ACCESS')
        self.assertEqual(len(members), 4)
        for member, role in zip(members, ['write', 'member', 'write', 'member']):
            self.assert_edge(data, member['id'], field['id'], role)
        parameter = self.nodes(data, 'FUNCTION', 'parameter')[0]['parameters'][0]
        local = self.nodes(data, 'VARIABLE_DECL', 'size')[0]
        for node, target in zip(self.nodes(data, 'IDENTIFIER', 'size'), [parameter, local]):
            self.assert_edge(data, node['id'], target['id'], 'read')

    def test_struct_self_uses_enclosing_struct_in_modules_and_pipelines(self):
        data = self.parse("""
            module A {
                struct Box {
                    float size;
                    value :: () -> float { return self.size; }
                    run :: () -> float { return self.value(); }
                }
            }
            module B {
                struct Box {
                    int size;
                    value :: () -> int { return self.size; }
                    run :: () -> int { return self.value(); }
                }
            }
            pipeline P {
                struct Box {
                    float3 size;
                    value :: () -> float3 { return self.size; }
                    run :: () -> float3 { return self.value(); }
                }
            }
        """)
        structures = self.nodes(data, 'STRUCT_DECL', 'Box')
        members = self.nodes(data, 'MEMBER_ACCESS')
        calls = self.nodes(data, 'FUNCTION_CALL', 'value')
        self.assertEqual((len(structures), len(members), len(calls)), (3, 3, 3))
        for structure, member, call in zip(structures, members, calls):
            self.assert_edge(data, member['id'], structure['fields'][0]['id'], 'member')
            method = next(m for m in structure['methods'] if m['name'] == 'value')
            self.assert_edge(data, call['id'], method['id'], 'call')
            for receiver in self.nodes({'modules': [structure], 'pipelines': []}, 'IDENTIFIER', 'self'):
                self.assert_edge(data, receiver['id'], structure['id'], 'type')

    def test_qualified_call_serializes_qualifier(self):
        data = self.parse('''
            module Mod { helper :: () -> float { return 1.0; } }
            module Main { run :: () -> float { return Mod::helper(); } }
        ''')
        call = self.nodes(data, 'FUNCTION_CALL')[0]
        self.assertEqual(call['moduleName'], 'Mod')
        self.assertEqual(call['qualifier']['name'], 'Mod')
        self.assert_edge(data, call['qualifier']['id'], 'MODULE:0', 'qualifier')

    def test_same_name_functions_in_passes(self):
        data = self.parse('''
            pipeline P {
                tonemap :: () -> float { return 0.0; }
                pass "A" { tonemap :: () -> float { return 1.0; } }
                pass "B" {
                    tonemap :: () -> float { return 2.0; }
                    other :: () -> float { return tonemap(); }
                }
            }
        ''')
        self.assertEqual(self.edges(data, 'call'),
                         [{'from': 'FUNCTION_CALL:0', 'to': 'FUNCTION:2', 'role': 'call'}])

    def test_same_name_structs_preserve_identity_through_calls(self):
        data = self.parse('''
            module ModA {
                struct Box { test :: () -> float { return 1.0; } }
                make :: () -> Box { Box value; return value; }
            }
            module ModB {
                struct Box { test :: () -> int { return 2; } }
                run :: (Box parameter) -> void {
                    ModA::Box a;
                    Box b;
                    a.test();
                    b.test();
                    parameter.test();
                    float result = ModA::make().test();
                }
            }
        ''')
        for name, target in [('a', 'STRUCT_DECL:0'), ('b', 'STRUCT_DECL:1')]:
            self.assert_edge(data, self.nodes(data, 'VARIABLE_DECL', name)[0]['id'], target, 'type')
        calls = self.nodes(data, 'FUNCTION_CALL', 'test')
        self.assertEqual([next(e['to'] for e in self.edges(data, 'call') if e['from'] == c['id']) for c in calls],
                         ['FUNCTION:0', 'FUNCTION:2', 'FUNCTION:2', 'FUNCTION:0'])

    def test_locals_do_not_leak_between_functions(self):
        data = self.parse('''
            module M {
                a :: () -> float { float secret = 1.0; return secret; }
                b :: () -> float { return secret; }
            }
        ''')
        identifiers = self.nodes(data, 'IDENTIFIER', 'secret')
        self.assert_edge(data, identifiers[0]['id'], 'VARIABLE_DECL:0', 'read')
        self.assertFalse(any(e['from'] == identifiers[1]['id'] for e in self.edges(data)))

    def test_unqualified_call_does_not_see_another_pass(self):
        data = self.parse("""
            pipeline P {
                pass "A" { helper :: () -> float { return 1.0; } }
                pass "B" { run :: () -> float { return helper(); } }
            }
        """)
        self.assertEqual(self.edges(data, 'call'), [])
        self.assertEqual(self.edges(data, 'construct'), [])

    def test_field_type_uses_declaration_module(self):
        data = self.parse("""
            module A {
                struct Box { float size; method :: () -> float { return 1.0; } }
                struct Container { A::Box box; }
            }
            module B {
                struct Box { method :: () -> int { return 2; } }
                run :: () -> void { A::Container value; value.box.method(); }
            }
        """)
        self.assert_edge(data, 'STRUCT_DECL:1/field:0', 'STRUCT_DECL:0', 'type')
        self.assert_edge(data, 'FUNCTION_CALL:0', 'FUNCTION:0', 'call')

    def test_missing_qualified_function_does_not_fall_back(self):
        data = self.parse('''
            module A { target :: () -> float { return 1.0; } }
            module B { other :: () -> float { return 2.0; } }
            module C { run :: () -> float { return B::target(); } }
        ''')
        self.assertEqual(self.edges(data, 'call'), [])
        self.assertFalse(any(s['id'] == 'builtin:type:target' for s in data['referenceIndex']['symbols']))

    def test_missing_method_does_not_fall_back(self):
        data = self.parse('''
            module M {
                struct Box { float value; }
                test :: () -> float { return 1.0; }
                run :: () -> void { Box b; b.test(); }
            }
        ''')
        self.assertEqual(self.edges(data, 'call'), [])

    def test_using_import_exposes_functions_and_types(self):
        data = self.parse('''
            module A {
                struct Box { float size; method :: () -> float { return 1.0; } }
                helper :: () -> float { return 1.0; }
            }
            module B {
                import A
                using A
                run :: () -> void { Box box; box.method(); helper(); }
            }
        ''')
        self.assert_edge(data, 'VARIABLE_DECL:0', 'STRUCT_DECL:0', 'type')
        self.assert_edge(data, 'FUNCTION_CALL:0', 'FUNCTION:0', 'call')
        self.assert_edge(data, 'FUNCTION_CALL:1', 'FUNCTION:1', 'call')

    def test_overloads_distinguish_same_named_struct_types(self):
        data = self.parse('''
            module A { struct Box { float size; } }
            module B { struct Box { float size; } }
            module C {
                choose :: (A::Box a) -> float { return 1.0; }
                choose :: (B::Box b) -> float { return 2.0; }
                run :: () -> void { A::Box a; B::Box b; choose(a); choose(b); }
            }
        ''')
        self.assert_edge(data, 'FUNCTION_CALL:0', 'FUNCTION:0', 'call')
        self.assert_edge(data, 'FUNCTION_CALL:1', 'FUNCTION:1', 'call')

    def test_nonmatching_pass_overload_allows_pipeline_overload(self):
        data = self.parse('''
            pipeline P {
                helper :: () -> float { return 1.0; }
                pass "Main" {
                    helper :: (float value) -> float { return value; }
                    run :: () -> float { return helper(); }
                }
            }
        ''')
        self.assert_edge(data, 'FUNCTION_CALL:0', 'FUNCTION:0', 'call')

    def test_nonmatching_pipeline_overload_allows_using_overload(self):
        data = self.parse('''
            module Helpers { helper :: () -> float { return 1.0; } }
            pipeline P {
                import Helpers as H
                using H
                helper :: (float value) -> float { return value; }
                pass "Main" { run :: () -> float { return helper(); } }
            }
        ''')
        self.assert_edge(data, 'FUNCTION_CALL:0', 'FUNCTION:0', 'call')

    def test_alias_qualified_calls_keep_owner_scope(self):
        data = self.parse('''
            module First { helper :: () -> float { return 1.0; } }
            module Second { helper :: () -> float { return 2.0; } }
            module A {
                import First as T
                run :: () -> float { return T::helper(); }
            }
            module B {
                import Second as T
                run :: () -> float { return T::helper(); }
            }
        ''')
        for i, call in enumerate(self.nodes(data, 'FUNCTION_CALL', 'helper')):
            self.assertEqual(call['moduleName'], 'T')
            self.assertEqual(call['qualifier']['name'], 'T')
            self.assert_edge(data, call['qualifier']['id'], f'MODULE:{i}', 'qualifier')
            self.assert_edge(data, call['id'], f'FUNCTION:{i}', 'call')

    def test_alias_qualified_member_preserves_written_name(self):
        # Parse-only function references exercise MEMBER_ACCESS separately from calls.
        data = self.parse('''
            module First { helper :: () -> float { return 1.0; } }
            module Second { helper :: () -> float { return 2.0; } }
            module A {
                import First as T
                run :: () -> float { return T::helper; }
            }
            module B {
                import Second as T
                run :: () -> float { return T::helper; }
            }
        ''')
        for i, member in enumerate(self.nodes(data, 'MEMBER_ACCESS')):
            self.assertEqual(member['object']['name'], 'T')
            self.assert_edge(data, member['object']['id'], f'MODULE:{i}', 'qualifier')
            self.assert_edge(data, member['id'], f'FUNCTION:{i}', 'read')

    def test_qualified_overload_mismatch_stays_in_its_module(self):
        data = self.parse('''
            module Helpers { helper :: (float value) -> float { return value; } }
            module Main {
                import Helpers as H
                helper :: () -> float { return 1.0; }
                run :: () -> float { return H::helper(); }
            }
        ''')
        call = self.nodes(data, 'FUNCTION_CALL')[0]
        self.assert_edge(data, call['qualifier']['id'], 'MODULE:0', 'qualifier')
        self.assertEqual(self.edges(data, 'call'), [])

    def position(self, source, line, text, occurrence=0):
        """1-based (line, column) of the n-th `text` on 1-based `line`."""
        row = textwrap.dedent(source).lstrip('\n').splitlines()[line - 1]
        column = -1
        for _ in range(occurrence + 1):
            column = row.index(text, column + 1)
        return line, column + 1

    def assert_position(self, entry, expected, prefix=''):
        line_key, column_key = (prefix + 'Line', prefix + 'Column') if prefix else ('line', 'column')
        self.assertEqual((entry.get(line_key), entry.get(column_key)), expected, entry)

    def test_declarations_record_their_source_file(self):
        data = self.parse('''
            pipeline P { import Dependency import Random }
        ''', {
            'Dependency.bwsl': 'module Dependency { value :: () -> float { return 0.0; } }',
            'DependencyExtra.bwsl': 'submodule DependencyExtra extends Dependency {\n'
                                    '    extra :: () -> float { return 1.0; }\n}',
        })
        modules = {module['name']: module for module in data['modules']}
        self.assertEqual(data['pipelines'][0]['sourceFile'], data['sourceFile'])
        dependency = modules['Dependency']
        self.assertTrue(dependency['sourceFile'].endswith('Dependency.bwsl'))
        self.assertNotIn('sourceUrl', dependency)
        functions = {function['name']: function for function in dependency['functions']}
        self.assertEqual(functions['value']['sourceFile'], dependency['sourceFile'])
        # Merged from the submodule file, with that file's positions.
        self.assertTrue(functions['extra']['sourceFile'].endswith('DependencyExtra.bwsl'))
        self.assertEqual(functions['extra']['line'], 2)
        self.assertEqual(modules['Random']['sourceFile'], 'stdlib://modules/Random.bwsl')
        self.assertRegex(modules['Random']['sourceUrl'],
                         r'^https://github\.com/BWSL-Lang/BWSL/blob/[^/]+/modules/Random\.bwsl$')

    def test_struct_fields_and_fragment_outputs_have_positions(self):
        source = '''
            module M {
                struct Box { float size; float2[2] corners; }
            }
            pipeline P {
                attributes {
                    position: float3
                }
                pass "Main" {
                    use attributes { position }
                    outputs { color: float4, normal: float4 }
                    vertex { output.position = float4(attributes.position, 1.0); }
                    fragment {
                        output.color = float4(1.0, 1.0, 1.0, 1.0);
                        output.normal = float4(0.0, 0.0, 1.0, 0.0);
                    }
                }
            }
        '''
        data = self.parse(source)
        size, corners = self.nodes(data, 'STRUCT_DECL', 'Box')[0]['fields']
        self.assert_position(size, self.position(source, 2, 'size'))
        self.assert_position(size, self.position(source, 2, 'size'), 'name')
        self.assert_position(size, self.position(source, 2, 'float'), 'type')
        self.assert_position(corners, self.position(source, 2, 'corners'), 'name')
        self.assertEqual((size['dataType'], corners['dataType']), ('float', 'float2'))
        self.assertNotIn('type', size)
        shader_pass = self.nodes(data, 'PASS')[0]
        color, normal = shader_pass['fragmentOutputs']
        self.assertEqual(color['id'], shader_pass['id'] + '/fragment-output:0')
        self.assert_position(color, self.position(source, 10, 'color'))
        self.assert_position(normal, self.position(source, 10, 'normal'), 'name')
        self.assert_position(normal, self.position(source, 10, 'float4', 1), 'type')
        symbols = {symbol['id'] for symbol in data['referenceIndex']['symbols']}
        self.assertIn(normal['id'], symbols)
        used = shader_pass['usedAttributes'][0]
        self.assert_position(used, self.position(source, 9, 'position'))

    def test_using_has_id_position_and_edge(self):
        source = '''
            module A { helper :: () -> float { return 1.0; } }
            module B {
                import A as Alias
                using Alias
                run :: () -> float { return helper(); }
            }
        '''
        data = self.parse(source)
        module = self.nodes(data, 'MODULE', 'B')[0]
        using = module['usingImports'][0]
        self.assertEqual(using['id'], module['id'] + '/using:0')
        self.assertEqual((using['name'], using['writtenName']), ('A', 'Alias'))
        self.assert_position(using, self.position(source, 4, 'Alias'))
        self.assert_position(module['imports'][0], self.position(source, 3, 'A'))
        self.assert_edge(data, using['id'], 'MODULE:0', 'using')

    def test_declared_type_qualifiers_have_edges(self):
        source = '''
            module Common { struct Box { float size; } }
            module Main {
                import Common as C
                struct Holder { C::Box box; }
                run :: (Common::Box p) -> float {
                    C::Box b;
                    return b.size + p.size;
                }
            }
        '''
        data = self.parse(source)
        common = self.nodes(data, 'MODULE', 'Common')[0]['id']
        field = self.nodes(data, 'STRUCT_DECL', 'Holder')[0]['fields'][0]
        parameter = self.nodes(data, 'FUNCTION', 'run')[0]['parameters'][0]
        variable = self.nodes(data, 'VARIABLE_DECL', 'b')[0]
        for owner, line, written, column in [(field, 4, 'C', 'C::'), (parameter, 5, 'Common', 'Common::'),
                                             (variable, 6, 'C', 'C::')]:
            qualifier = owner['typeQualifier']
            self.assertEqual(qualifier['id'], owner['id'] + '/type-qualifier')
            self.assertEqual(qualifier['name'], written)
            self.assert_position(qualifier, self.position(source, line, column))
            self.assert_edge(data, qualifier['id'], common, 'qualifier')
        self.assertEqual(field['dataType'], 'Common::Box')
        self.assertEqual(parameter['dataType'], 'Common::Box')
        self.assertNotIn('type', parameter)
        box = self.nodes(data, 'STRUCT_DECL', 'Box')[0]['id']
        self.assert_edge(data, field['id'], box, 'type')

    def test_resource_payload_positions_and_references(self):
        for owner in ('module', 'pipeline'):
            for spelling in ('Lib::Item', 'Lib.Item', 'L::Item', 'L.Item'):
                for form in ('{}', '{}[2]', 'buffer<{}>', 'cbuffer<{}[2]>'):
                    with self.subTest(owner=owner, spelling=spelling, form=form):
                        resource_type = form.format(spelling)
                        source = f'''{owner} Main {{
    import Lib as L
    resources {{
        item: {resource_type};
        scale: float
    }}
}}'''
                        data = self.parse(source, {'Lib.bwsl':
                            'module Lib { struct Item { float4 mask; float w; } }'})
                        resource = self.nodes(data, 'RESOURCE_DECL', 'item')[0]
                        self.assertEqual(resource['typeName'], form.format('Lib::Item'))
                        self.assert_position(resource, self.position(source, 4, resource_type), 'type')
                        qualifier = resource['typeQualifier']
                        self.assertEqual(qualifier['name'], spelling.split('::')[0].split('.')[0])
                        self.assert_position(qualifier, self.position(source, 4, spelling))
                        self.assert_edge(data, qualifier['id'],
                                         self.nodes(data, 'MODULE', 'Lib')[0]['id'], 'qualifier')
                        self.assert_edge(data, resource['id'],
                                         self.nodes(data, 'STRUCT_DECL', 'Item')[0]['id'], 'type')
                        scale = self.nodes(data, 'RESOURCE_DECL', 'scale')[0]
                        self.assert_edge(data, scale['id'], 'builtin:type:float', 'type')

    def test_resource_builtin_and_local_payload_types(self):
        data = self.parse('''pipeline Main {
            struct Item { float value; }
            resources {
                item: Item
                items: buffer<Item>
                constants: cbuffer<float4[2]>
                tex2: texture2D
                tex3: texture3D
                cube: textureCube
                layers: texture2DArray
                samp: sampler
                image: image2D
                wide: double
            }
        }''')
        item = self.nodes(data, 'STRUCT_DECL', 'Item')[0]['id']
        for name in ('item', 'items'):
            self.assert_edge(data, self.nodes(data, 'RESOURCE_DECL', name)[0]['id'], item, 'type')
        self.assertEqual(len(self.nodes(data, 'RESOURCE_DECL')), 10)

    def test_resource_forward_payload_declarations(self):
        for owner in ('pipeline', 'module'):
            with self.subTest(owner=owner):
                data = self.parse(f'''{owner} Main {{
                    resources {{ local: Local external: L.Item buffered: buffer<L::Item> }}
                    struct Local {{ float value; }}
                    import Lib as L
                }}''', {'Lib.bwsl': 'module Lib { struct Item { float value; } }'})
                local = self.nodes(data, 'RESOURCE_DECL', 'local')[0]
                self.assert_edge(data, local['id'],
                                 self.nodes(data, 'STRUCT_DECL', 'Local')[0]['id'], 'type')
                for name, expected in (('external', 'Lib::Item'), ('buffered', 'buffer<Lib::Item>')):
                    resource = self.nodes(data, 'RESOURCE_DECL', name)[0]
                    self.assertEqual(resource['typeName'], expected)
                    self.assert_edge(data, resource['id'],
                                     self.nodes(data, 'STRUCT_DECL', 'Item')[0]['id'], 'type')
                    self.assert_edge(data, resource['typeQualifier']['id'],
                                     self.nodes(data, 'MODULE', 'Lib')[0]['id'], 'qualifier')

    def test_unknown_resource_payloads_are_diagnosed(self):
        with tempfile.TemporaryDirectory(prefix='bwsl-resource-types-') as directory:
            folder = Path(directory)
            (folder / 'Lib.bwsl').write_text('module Lib { struct Item { float w; } }')
            for owner in ('pipeline', 'module'):
                for spelling in ('Lib.Missing', 'Lib::Missing', 'L.Missing', 'L::Missing',
                                 'Nope::Item', 'Nope.Item', 'Missing', 'T'):
                    for form in ('{}', '{}[2]', 'buffer<{}>', 'cbuffer<{}>'):
                        with self.subTest(owner=owner, spelling=spelling, form=form):
                            path = folder / 'main.bwsl'
                            path.write_text(f'''{owner} Main {{
    import Lib as L
    resources {{ item: {form.format(spelling)} }}
}}''')
                            result = subprocess.run(
                                [str(COMPILER), str(path), '-modules', directory,
                                 '-errors-json', '-no-validate', '-check'],
                                capture_output=True, text=True, timeout=30)
                            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                            diagnostics = json.loads(result.stdout)
                            error = next(d for d in diagnostics['diagnostics']
                                         if 'Unknown resource type' in d['message'])
                            self.assertEqual(error['severity'], 'error')
                            self.assertEqual(error['line'], 3)
                            self.assertEqual(error['column'],
                                             path.read_text().splitlines()[2].index(spelling) + 1)

    def test_resource_qualifier_spellings_generate_identical_shaders(self):
        with tempfile.TemporaryDirectory(prefix='bwsl-resource-codegen-') as directory:
            folder = Path(directory)
            (folder / 'Lib.bwsl').write_text(
                'module Lib { struct Item { float4 mask; float w; } }')
            outputs = []
            for spelling in ('Lib.Item', 'Lib::Item', 'L.Item', 'L::Item'):
                path = folder / 'main.bwsl'
                path.write_text(f'''pipeline Main {{
    import Lib as L
    resources {{ item: {spelling} }}
    attributes {{ position: float4 }}
    pass "Main" {{
        use attributes {{ position }}
        use resources {{ item }}
        outputs {{ c: float4 }}
        vertex {{ output.pos = attributes.position; }}
        fragment {{ output.c = float4(resources.item.w) * resources.item.mask; }}
    }}
}}''')
                out = folder / str(len(outputs))
                result = subprocess.run(
                    [str(COMPILER), str(path), '-modules', directory,
                     '-glsl', '-spv', '-validation', 'strict', '-o', str(out)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                shaders = {p.name: p.read_bytes() for p in out.iterdir()
                           if p.suffix in ('.spv', '.vert', '.frag')}
                self.assertEqual(len(shaders), 4)
                outputs.append(shaders)
            for output in outputs[1:]:
                self.assertEqual(output, outputs[0])

    def test_return_types_have_positions_and_qualifier_edges(self):
        source = '''
            module Common { struct Box { float size; } }
            module M {
                import Common as C
                struct Local {
                    float size;
                    wrap :: () -> C::Box { C::Box b; return b; }
                }
                plain :: () -> Local { Local l; return l; }
                qualified :: () -> Common::Box { Common::Box b; return b; }
                scalar :: () -> float { return 1.0; }
            }
        '''
        data = self.parse(source)
        common = self.nodes(data, 'MODULE', 'Common')[0]['id']
        box = self.nodes(data, 'STRUCT_DECL', 'Box')[0]['id']
        local = self.nodes(data, 'STRUCT_DECL', 'Local')[0]['id']

        plain = self.nodes(data, 'FUNCTION', 'plain')[0]
        self.assertEqual(plain['returnType'], 'Local')
        self.assert_position(plain, self.position(source, 8, 'Local'), 'returnType')
        self.assertNotIn('returnTypeQualifier', plain)
        self.assert_edge(data, plain['id'], local, 'return-type')

        scalar = self.nodes(data, 'FUNCTION', 'scalar')[0]
        self.assert_position(scalar, self.position(source, 10, 'float'), 'returnType')

        for name, line, written in [('qualified', 9, 'Common'), ('wrap', 6, 'C')]:
            function = self.nodes(data, 'FUNCTION', name)[0]
            self.assertEqual(function['returnType'], 'Common::Box')
            self.assert_position(function, self.position(source, line, written + '::'),
                                 'returnType')
            qualifier = function['returnTypeQualifier']
            self.assertEqual(qualifier['id'], function['id'] + '/return-type-qualifier')
            self.assertEqual(qualifier['name'], written)
            self.assert_position(qualifier, self.position(source, line, written + '::'))
            self.assert_edge(data, function['id'], box, 'return-type')
            self.assert_edge(data, qualifier['id'], common, 'qualifier')

    def test_intrinsic_results_type_stage_values(self):
        data = self.parse('''
            pipeline StageValueType {
                attributes {
                    position: float4
                    uv: float2
                    n: float3
                    bits: uint
                }
                pass "Main" {
                    use attributes { position, uv, n, bits }
                    outputs { result: float4 }
                    vertex {
                        output.pos = attributes.position;
                        output.unit = normalize(attributes.uv);
                        output.d = dot(attributes.n, attributes.n);
                        output.s = smoothstep(0.0, 1.0, attributes.n);
                        output.m = max(1.0, attributes.uv);
                        output.f = asfloat(attributes.bits);
                    }
                    fragment {
                        output.result = float4(input.unit, input.d, input.f);
                    }
                }
            }
        ''')
        types = {symbol['name']: symbol.get('type')
                 for symbol in data['referenceIndex']['symbols']
                 if symbol['kind'] == 'stage-interface'}
        self.assertEqual(types, {'pos': 'float4', 'unit': 'float2', 'd': 'float',
                                 's': 'float3', 'm': 'float2', 'f': 'float'})

    def test_constants_are_declarations_with_navigable_uses(self):
        source = '''
            module Common {
                const float BASE = 2.0;
                f :: (float x) -> float { return x + BASE; }
            }
            pipeline P {
                import Common as C
                const int COUNT = 3;
                attributes {
                    position: float3
                }
                pass "Main" {
                    const float SCALE = 0.5;
                    use attributes { position }
                    vertex {
                        const float LOCAL = 1.0;
                        float v = C::BASE + SCALE + LOCAL + float(COUNT);
                        output.position = float4(attributes.position, v);
                    }
                }
            }
        '''
        data = self.parse(source)
        base = self.nodes(data, 'MODULE', 'Common')[0]['consts'][0]
        self.assertEqual((base['name'], base['isConst']), ('BASE', True))
        self.assert_position(base, self.position(source, 2, 'BASE'), 'name')
        self.assertEqual(base['sourceFile'], data['sourceFile'])
        count = data['pipelines'][0]['consts'][0]
        self.assertEqual(count['name'], 'COUNT')
        stable = {s['id']: s.get('stableId') for s in data['referenceIndex']['symbols']}
        self.assertEqual(stable[base['id']], 'module:Common/const:BASE')

        folded = {}
        for literal in self.nodes(data, 'LITERAL'):
            if 'foldedFrom' in literal:
                use = literal['foldedFrom']
                self.assertEqual((use['type'], use['id']), ('IDENTIFIER', literal['id'] + '/folded-constant'))
                folded[(use['name'], use['line'])] = use
        # The unqualified use inside the module, then each use in the stage.
        targets = {('BASE', 3): base['id'], ('BASE', 16): base['id'], ('COUNT', 16): count['id'],
                   ('SCALE', 16): self.nodes(data, 'VARIABLE_DECL', 'SCALE')[0]['id'],
                   ('LOCAL', 16): self.nodes(data, 'VARIABLE_DECL', 'LOCAL')[0]['id']}
        for key, target in targets.items():
            self.assertIn(key, folded)
            self.assert_edge(data, folded[key]['id'], target, 'read')
        self.assert_position(folded[('BASE', 3)], self.position(source, 3, 'BASE'))
        qualified = folded[('BASE', 16)]
        self.assert_position(qualified, self.position(source, 16, 'BASE'))
        self.assertEqual(qualified['qualifier']['name'], 'C')
        self.assert_edge(data, qualified['qualifier']['id'], 'MODULE:0', 'qualifier')

    def test_named_nodes_report_name_positions(self):
        source = '''
            module Dep { helper :: () -> float { return 1.0; } }
            module M {
                struct Box { float size; area :: () -> float { return size; } }
            }
            pipeline Names {
                import Dep
                import M
                attributes {
                    position: float3
                }
                pass "Main" {
                    use attributes { position }
                    vertex {
                        M::Box b;
                        float a = b.area() + Dep::helper() + b.size;
                        output.position = float4(attributes.position, a);
                    }
                }
            }
        '''
        data = self.parse(source)
        expected = [
            ('MODULE', 'M', self.position(source, 2, 'M')),
            ('STRUCT_DECL', 'Box', self.position(source, 3, 'Box')),
            ('PIPELINE', 'Names', self.position(source, 5, 'Names')),
            ('PASS', 'Main', self.position(source, 11, 'Main')),
            ('ATTRIBUTE_DECL', 'position', self.position(source, 9, 'position')),
            ('FUNCTION', 'area', self.position(source, 3, 'area')),
            ('FUNCTION_CALL', 'area', self.position(source, 15, 'area')),
            ('FUNCTION_CALL', 'helper', self.position(source, 15, 'helper')),
        ]
        for kind, name, position in expected:
            with self.subTest(kind=kind, name=name):
                self.assert_position(self.nodes(data, kind, name)[0], position, 'name')
        member = next(node for node in self.nodes(data, 'MEMBER_ACCESS') if node['member'] == 'size')
        self.assert_position(member, self.position(source, 15, 'size'), 'name')

    def test_existing_fixtures(self):
        for path in sorted((ROOT / 'tests' / 'ast_json').glob('*.bwsl')):
            with self.subTest(fixture=path.name):
                data = self.parse(path.read_text())
                if path.stem in AST_JSON_POSITION_TESTS:
                    self.assertEqual(check_ast_json_positions(data, AST_JSON_POSITION_TESTS[path.stem]), (True, ''))
                if path.stem in AST_JSON_REFERENCE_TESTS:
                    self.assertEqual(check_ast_json_references(data, AST_JSON_REFERENCE_TESTS[path.stem]), (True, ''))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', type=Path, default=COMPILER)
    args, remaining = parser.parse_known_args()
    COMPILER = args.compiler.resolve()
    unittest.main(argv=[__file__, *remaining])
