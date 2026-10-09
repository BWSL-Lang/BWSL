"""Name diagnostics: unresolved names fail; variable shadowing emits notes."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "build/bwslc"


class NameValidationTests(unittest.TestCase):
    def compile(self, source, dependencies=None, flags=("-check",), validate_tokens=True):
        with tempfile.TemporaryDirectory(prefix="bwsl-names-") as directory:
            folder = Path(directory)
            sources = {"main.bwsl": source, **(dependencies or {})}
            for name, contents in sources.items():
                sources[name] = textwrap.dedent(contents).lstrip("\n")
                (folder / name).write_text(sources[name])
            result = subprocess.run(
                [str(COMPILER), str(folder / "main.bwsl"), *flags,
                 "-errors-json", "-modules", directory, "-o", str(folder / "out")],
                capture_output=True, text=True, timeout=30)
            self.assertIn(result.returncode, (0, 1), result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(result.returncode == 0, data["success"], data)
            if not data["success"]:
                self.assertFalse(list((folder / "out").glob("*.spv")))
            # Every diagnostic must identify a real token in its own source.
            for error in data["diagnostics"]:
                name = Path(error["file"]).name
                self.assertIn(name, sources, error)
                line = sources[name].splitlines()[error["line"] - 1]
                self.assertEqual(error["endLine"], error["line"], error)
                token = line[error["column"] - 1:error["endColumn"] - 1]
                self.assertTrue(token, error)
                if validate_tokens:
                    self.assertIn(token, error["message"], error)
                error["sourceToken"] = token
            return data

    def assert_errors(self, data, tokens, file="main.bwsl"):
        self.assertFalse(data["success"], data)
        errors = data["diagnostics"]
        self.assertEqual(len(errors), len(tokens), errors)
        for error, token in zip(errors, tokens):
            self.assertIn(token, error["message"])
            self.assertEqual(error["sourceToken"], token, error)
            self.assertEqual(Path(error["file"]).name, file)
            self.assertEqual(error["endColumn"] - error["column"], len(token), error)

    def assert_notes(self, data, names):
        self.assertTrue(data['success'], data)
        self.assertEqual(data['errorCount'], 0)
        self.assertEqual(data['warningCount'], 0)
        notes = data['diagnostics']
        self.assertEqual([note['severity'] for note in notes], ['note'] * len(names), notes)
        self.assertEqual([note['sourceToken'] for note in notes], names, notes)
        for note in notes:
            self.assertIn(' shadows ', note['message'])
            self.assertIn(' declared at ', note['message'])
        return notes

    def assert_keyword_error(self, data, keyword, role):
        self.assert_errors(data, [keyword])
        self.assertEqual(data['diagnostics'][0]['message'],
                         f"'{keyword}' is a keyword and cannot be used as a {role} name")

    def test_keyword_parameter_names_recover_in_pass(self):
        for keyword in ('inputs', 'outputs', 'resources', 'attributes', 'variants', 'pass'):
            for parameter in (f'FragmentInputs {keyword}', f'{keyword}: FragmentInputs'):
                with self.subTest(keyword=keyword, parameter=parameter):
                    data = self.compile('''pipeline Pass {
                        attributes { position: float4 }
                        struct FragmentInputs { float4 color; }
                        pass "Main" {
                            use attributes { position }
                            outputs { color: float4 }
                            shade :: (PARAMETER, float k) -> float4 {
                                return KEYWORD.color * k;
                            }
                            vertex { output.pos = attributes.position; output.color = attributes.position; }
                            fragment {
                                FragmentInputs fragmentInputs;
                                fragmentInputs.color = input.color;
                                output.color = shade(fragmentInputs, 2.0);
                            }
                        }
                    }'''.replace('PARAMETER', parameter).replace('KEYWORD', keyword))
                    self.assert_keyword_error(data, keyword, 'parameter')
                    error = data['diagnostics'][0]
                    self.assertEqual(error['line'], 7)
                    self.assertEqual(error['column'], 29 + len('shade :: (') + parameter.index(keyword))

    def test_keyword_local_names_recover(self):
        for declaration, value in (('float inputs = k;', 'inputs'),
                                   ('const float inputs = 2.0;', 'inputs'),
                                   ('float[2] inputs = {k, k};', 'inputs[0]'),
                                   ('Payload inputs;', 'inputs.color'),
                                   ('Payload[2] inputs;', 'inputs[0].color'),
                                   ('Payload^ inputs;', 'k')):
            with self.subTest(declaration=declaration):
                data = self.compile('''module M {
                    struct Payload { float color; }
                    shade :: (float k) -> float {
                        DECLARATION
                        return VALUE;
                    }
                    later :: () -> float { return 1.0; }
                }'''.replace('DECLARATION', declaration).replace('VALUE', value))
                self.assert_keyword_error(data, 'inputs', 'variable')

    def test_keyword_field_names_recover_through_member_access(self):
        for keyword in ('inputs', 'outputs', 'resources', 'attributes', 'variants', 'pass'):
            with self.subTest(keyword=keyword):
                data = self.compile('''module M {
                    struct Payload { float KEYWORD; float valid; }
                    shade :: (Payload value) -> float { return value.KEYWORD + value.valid; }
                }'''.replace('KEYWORD', keyword))
                self.assert_keyword_error(data, keyword, 'field')

    def test_keyword_for_loop_variable_name_recovers(self):
        data = self.compile('''module M {
            shade :: () -> float {
                for (int inputs = 0; inputs < 2; inputs++) {}
                return 1.0;
            }
        }''')
        self.assert_keyword_error(data, 'inputs', 'variable')

    def test_keyword_names_report_each_declaration(self):
        data = self.compile('''module M {
            struct Payload { float inputs; float outputs; }
            shade :: (float inputs, float outputs) -> float {
                float pass = inputs + outputs;
                return pass;
            }
        }''')
        self.assert_errors(data, ['inputs', 'outputs', 'inputs', 'outputs', 'pass'])
        self.assertEqual([d['message'].split(' as a ')[1] for d in data['diagnostics']],
                         ['field name', 'field name', 'parameter name', 'parameter name', 'variable name'])

    def test_type_and_control_keywords_are_identified_as_names(self):
        for keyword in ('float', 'enum', 'as', 'double', 'using', 'return'):
            for parameter in (f'float {keyword}', f'{keyword}: float'):
                with self.subTest(parameter=parameter):
                    data = self.compile(f'module M {{ f :: ({parameter}) -> float {{ return 1.0; }} }}')
                    self.assert_keyword_error(data, keyword, 'parameter')

    def test_keyword_recovery_keeps_other_errors_visible(self):
        data = self.compile('''module M {
            f :: (float inputs) -> float { return inputs; }
            broken :: () -> float { return 1.0 }
        }''', validate_tokens=False)
        self.assertFalse(data['success'], data)
        self.assertEqual(data['errorCount'], 2, data)
        self.assertIn("'inputs' is a keyword", data['diagnostics'][0]['message'])
        self.assertIn("Expected ';'", data['diagnostics'][1]['message'])

    def test_ordinary_and_unnamed_parameters_remain_valid(self):
        data = self.compile('''module M {
            struct Payload { float inputsValue; float range; }
            f :: (Payload inputsValue, float) -> float { return inputsValue.inputsValue; }
            g :: (inputsValue: Payload) -> float { return inputsValue.range; }
            h :: (float range, float it) -> float { return range + it; }
        }''')
        self.assertTrue(data['success'], data)

    def test_nested_shadowing_reports_nearest_declaration_once(self):
        data = self.compile('''module M {
            unused :: (float value) -> float {
                { float value = 2.0; { float value = 3.0; } }
                return value;
            }
        }''')
        notes = self.assert_notes(data, ['value', 'value'])
        self.assertIn('shadows parameter', notes[0]['message'])
        self.assertIn('shadows variable', notes[1]['message'])
        self.assertTrue(notes[1]['message'].endswith(f":{notes[0]['line']}:{notes[0]['column']}"))

    def test_parameter_and_local_shadow_fields_and_constants(self):
        data = self.compile('''module M {
            const float scale = 2.0;
            struct Box {
                float size;
                read :: (float size, float scale) -> float {
                    { float size = 1.0; }
                    return size * scale;
                }
            }
        }''')
        notes = self.assert_notes(data, ['size', 'scale', 'size'])
        self.assertIn('Parameter', notes[0]['message'])
        self.assertIn('shadows field', notes[0]['message'])
        self.assertIn('shadows constant', notes[1]['message'])
        self.assertIn('shadows parameter', notes[2]['message'])

    def test_loop_declarations_report_shadowing(self):
        data = self.compile('''module M {
            unused :: () -> void {
                int i = 9;
                float[2] values;
                for (int i = 0; i < 2; i++) {}
                for (i in 0..2) {}
                foreach (i in 0..2) {}
                for (i in values) {}
            }
        }''')
        self.assert_notes(data, ['i'] * 4)

    def test_comptime_unrolling_keeps_single_written_note(self):
        data = self.compile('''pipeline P {
            pass "Main" { vertex {
                float value = 1.0;
                int i = 9;
                eval foreach (i in 0..3) { float value = float(i); }
                output.position = float4(value);
            } }
        }''')
        self.assert_notes(data, ['i', 'value'])

    def test_sibling_scopes_functions_modules_and_stages_do_not_shadow(self):
        data = self.compile('''module A {
            const float value = 1.0;
            first :: (float arg) -> float { return arg; }
            second :: (float arg) -> float { return arg; }
        }
        module B { const float value = 2.0; }
        pipeline P {
            pass "Main" {
                vertex {
                    { float value = 1.0; }
                    { float value = 2.0; }
                    for (int i = 0; i < 2; i++) {}
                    for (int i = 0; i < 2; i++) {}
                    output.position = float4(1.0);
                }
                fragment { float value = 3.0; output.color = float4(value); }
            }
        }''')
        self.assert_notes(data, [])

    def test_imported_shadowing_uses_original_file_and_token(self):
        data = self.compile('''pipeline P { import Geometry
            pass "Main" { vertex { output.position = float4(1.0); } }
        }''', {'Geometry.bwsl': '''module Geometry {
            unused :: (float value) -> float { { float value = 2.0; } return value; }
        }'''})
        notes = self.assert_notes(data, ['value'])
        self.assertEqual(Path(notes[0]['file']).name, 'Geometry.bwsl')
        self.assertIn('Geometry.bwsl:2:', notes[0]['message'])

    def test_repeated_calls_do_not_duplicate_function_shadow_note(self):
        data = self.compile('''pipeline P {
            helper :: (float value) -> float { { float value = 2.0; } return value; }
            pass "Main" { vertex {
                output.position = float4(helper(1.0) + helper(2.0));
            } }
        }''')
        self.assert_notes(data, ['value'])

    def test_shadow_note_does_not_hide_unresolved_name_error(self):
        data = self.compile('''module M {
            unused :: (float value) -> float {
                { float value = 2.0; }
                return missingName;
            }
        }''')
        self.assertFalse(data['success'])
        self.assertEqual(data['errorCount'], 1)
        self.assertEqual([d['severity'] for d in data['diagnostics']], ['note', 'error'])
        self.assertEqual([d['sourceToken'] for d in data['diagnostics']], ['value', 'missingName'])

    def test_implicit_collection_iterator_has_no_invented_name_span(self):
        data = self.compile('''module M {
            unused :: () -> void {
                int it = 0;
                float[2] values;
                for (values) {}
            }
        }''')
        self.assert_notes(data, [])

    def test_shadowing_is_visible_in_module_text_and_still_generates_shaders(self):
        with tempfile.TemporaryDirectory() as directory:
            module = Path(directory) / 'M.bwsl'
            module.write_text('module M { f :: (float x) -> float { { float x=2.0; } return x; } }')
            result = subprocess.run([str(COMPILER), str(module), '-check'], capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Note', result.stderr)
            self.assertIn("Variable 'x' shadows parameter", result.stderr)
            pipeline = Path(directory) / 'P.bwsl'
            pipeline.write_text('''pipeline P { pass "Main" { vertex {
                float x=1.0; { float x=2.0; } output.position=float4(x);
            } } }''')
            output = Path(directory) / 'out'
            result = subprocess.run([str(COMPILER), str(pipeline), '-spv', '-errors-json', '-o', str(output)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads(result.stdout)
            self.assertTrue(data['success'], data)
            self.assertEqual([d['severity'] for d in data['diagnostics']], ['note'])
            self.assertTrue(list(output.glob('*.spv')))

    def test_many_shadow_notes_do_not_hide_text_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'M.bwsl'
            path.write_text('module M { f :: (float value) -> float {' +
                            '{ float value=2.0; }\n' * 12 + 'return missingName; } }')
            result = subprocess.run([str(COMPILER), str(path), '-check'], capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("Unknown identifier 'missingName'", result.stderr)
            self.assertLess(result.stderr.index("Unknown identifier 'missingName'"),
                            result.stderr.index("Variable 'value' shadows parameter"))

    def test_standalone_module_all_name_kinds(self):
        source = """
            module Geometry {
                struct Point { float x; float y; }
                measure :: (Point p) -> float {
                    float a = undefinedName;
                    float b = missingFunction(a);
                    float c = p.nofield;
                    Nope d;
                    float e = Other::thing(a);
                    return a + b + c + e;
                }
            }
        """
        for flags in (("-check",), ("-check", "-no-validate"), ("-all", "-no-validate")):
            with self.subTest(flags=flags):
                self.assert_errors(self.compile(source, flags=flags),
                                   ["undefinedName", "missingFunction", "nofield", "Nope", "Other"])

    def test_unused_pipeline_function(self):
        self.assert_errors(self.compile("""
            pipeline P {
                unused :: () -> float { return undefinedName; }
                pass "Main" { vertex { output.position = float4(1.0); } }
            }
        """), ["undefinedName"])

    def test_live_pipeline_names_and_cloned_token_spans(self):
        self.assert_errors(self.compile("""
            pipeline P {
                struct Point { float x; }
                pass "Main" { vertex {
                    Point p;
                    float a = undefinedName;
                    float b = missingFunction(a);
                    float c = p.nofield;
                    Nope d;
                    float e = Other::thing(a);
                    output.position = float4(a + b + c + e);
                } }
            }
        """), ["undefinedName", "missingFunction", "nofield", "Nope", "Other"])

    def test_unknown_import_has_a_token_span(self):
        self.assert_errors(self.compile("pipeline P { import MissingModule }"), ["MissingModule"])

    def test_imported_module_uses_original_source(self):
        dependency = """
            module Geometry {
                struct Point { float x; }
                measure :: (Point p) -> float {
                    float a = undefinedName;
                    float b = missingFunction(a);
                    return p.nofield + a + b;
                }
            }
        """
        self.assert_errors(self.compile("""
            pipeline P { import Geometry
                pass "Main" { vertex { output.position = float4(1.0); } }
            }
        """, {"Geometry.bwsl": dependency}),
                           ["undefinedName", "missingFunction", "nofield"], "Geometry.bwsl")

    def test_known_module_unknown_function(self):
        data = self.compile("""
            module Known { valid :: () -> float { return 1.0; } }
            pipeline P { import Known
                unused :: () -> float { return Known::missing(); }
            }
        """)
        self.assert_errors(data, ["missing"])
        self.assertIn("Known::missing", data["diagnostics"][0]["message"])

    def test_unknown_signature_types(self):
        self.assert_errors(self.compile("""
            module M {
                unused :: (MissingParameter p) -> MissingReturn { return 1.0; }
            }
        """), ["MissingReturn", "MissingParameter"])

    def test_qualified_unknown_type_and_module(self):
        self.assert_errors(self.compile("""
            module Known { struct Point { float x; } }
            pipeline P { import Known
                unused :: () -> float {
                    Known::Missing p;
                    Absent::Missing q;
                    return 1.0;
                }
            }
        """), ["Missing", "Absent"])

    def test_nested_struct_field_and_method(self):
        self.assert_errors(self.compile("""
            module M {
                struct Inner { float x; }
                struct Outer { Inner inner; }
                unused :: (Outer value) -> float {
                    float a = value.inner.missingField;
                    return value.inner.missingMethod() + a;
                }
            }
        """), ["missingField", "missingMethod"])

    def test_unknown_array_element_type(self):
        self.assert_errors(self.compile("""
            module M { unused :: () -> float { Missing[2] values; return 1.0; } }
        """), ["Missing"])

    def test_scope_does_not_leak_loop_variables(self):
        self.assert_errors(self.compile("""
            module M { unused :: () -> float {
                for (int i = 0; i < 2; i++) { float x = float(i); }
                return float(i);
            } }
        """), ["i"])

    def test_imported_method_reports_its_file(self):
        self.assert_errors(self.compile("""
            pipeline P { import Geometry
                pass "Main" { vertex { output.position = float4(1.0); } }
            }
        """, {"Geometry.bwsl": """
            module Geometry {
                struct Point {
                    float x;
                    value :: () -> float { return missing; }
                }
            }
        """}), ["missing"], "Geometry.bwsl")

    def test_submodule_preserves_original_source(self):
        self.assert_errors(self.compile("""
            pipeline P { import Geometry
                pass "Main" { vertex { output.position = float4(1.0); } }
            }
        """, {
            "Geometry.bwsl": "module Geometry { valid :: () -> float { return 1.0; } }",
            "Extension.bwsl": """
                submodule Extension extends Geometry {
                    unused :: () -> float { return submoduleTypo; }
                }
            """,
        }), ["submoduleTypo"], "Extension.bwsl")

    def test_unused_pass_block_function(self):
        self.assert_errors(self.compile("""
            module M {
                unused :: () -> pass_block {
                    pass { vertex { output.position = float4(missing); } }
                }
            }
        """), ["missing"])

    def test_unused_alias_target_is_checked(self):
        self.assert_errors(self.compile("""
            module M { using Alias = Missing; }
        """), ["Missing"])

    def test_valid_type_aliases(self):
        data = self.compile("""
            module M { struct Point { float x; } }
            pipeline P { import M as Geometry
                using Position = Geometry::Point
                struct Container { Position value; }
                first :: (Position value) -> float { return value.x; }
                identityPosition :: (Position value) -> Position { return value; }
                field :: (Container value) -> float { return value.value.x; }
                array :: () -> float { Position[2] values; return values[0].x; }
                pass "Main" { vertex { Position p; p.x = 1.0; output.position = float4(first(identityPosition(p))); } }
            }
        """)
        self.assertTrue(data["success"], data)

    def test_qualifier_positions_are_file_local(self):
        # Both parameter qualifiers occupy exactly the same line and column.
        data = self.compile("""
            pipeline P { import A, B
                pass "Main" { vertex { output.position = float4(1.0); } }
            }
        """, {
            "A.bwsl": "module A { import First\nf :: (First::Point p) -> float { return p.x; }\n}",
            "B.bwsl": "module B { import Other\nf :: (Other::Point p) -> float { return p.y; }\n}",
            "First.bwsl": "module First { struct Point { float x; } }",
            "Other.bwsl": "module Other { struct Point { float y; } }",
        })
        self.assertTrue(data["success"], data)

    def test_valid_forward_calls_generics_and_module_alias(self):
        data = self.compile("""
            module M {
                struct Point { float x; }
                first :: (Point p) -> float { return later(p); }
                later :: (Point p) -> float { return p.x; }
                identity :: (T value) -> T { return value; }
            }
            pipeline P { import M as Geometry
                unused :: (Geometry::Point p) -> float { return Geometry::first(p); }
                pass "Main" { vertex { output.position = float4(1.0); } }
            }
        """)
        self.assertTrue(data["success"], data)


def run_name_validation_suite(compiler):
    global COMPILER
    COMPILER = Path(compiler).resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NameValidationTests)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    args = parser.parse_args()
    passed, failed = run_name_validation_suite(args.compiler)
    print(f"Name validation: {passed} passed, {failed} failed")
    raise SystemExit(bool(failed))
