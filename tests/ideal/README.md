# Ideal-World Compiler Tests

These tests document behavior we want from the compiler, but they are not part
of the default `tests/run_tests.py` sweep yet. They should be promoted into the
normal regression suite once the compiler supports the feature cleanly.

Run them manually while working on the compiler:

```bash
./build/bwslc tests/ideal/sdf_sumtype_module_import.bwsl -modules tests/ideal -all
./build/bwslc tests/ideal/sdf_sumtype_compute_equiv.bwsl -modules tests/ideal -all
./build/bwslc tests/ideal/sdf_sumtype_qualified_constructor.bwsl -modules tests/ideal -all
```

Current target behavior:

- Sum-type enums declared inside imported modules can call helper functions
  declared in that module from `eval` methods.
- Imported sum-type enum payload dispatch lowers to valid SPIR-V when used from
  vertex, fragment, and compute stages.
- Module-qualified enum types work in user code.
- Module-qualified constructors such as
  `SDFIdeal::SDFShape::Sphere(1.0)` should also work.
