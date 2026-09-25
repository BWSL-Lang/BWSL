#!/usr/bin/env python3
"""AST JSON schema and scope regressions (issue #94)."""
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

    def test_document_roots_in_source_order_exclude_imported_modules(self):
        data = self.parse('''
            pipeline First { import Dependency }
            module Local { helper :: () -> float { return 1.0; } }
            pipeline Second {}
        ''', {'Dependency.bwsl': 'module Dependency { value :: () -> float { return 0.0; } }'})
        names = {node['id']: node['name'] for node in data['modules'] + data['pipelines']}
        self.assertEqual([names[ref] for ref in data['roots']], ['First', 'Local', 'Second'])
        self.assertEqual(data['schema'], 'bwsl.ast.v2')
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
