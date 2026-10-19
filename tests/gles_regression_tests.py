"""Validate direct GLES and execute raster readbacks against independent values.

Runtime conversion raises only the ES version to 310 and assigns interface
locations so the GLES fragment can execute on the existing Vulkan test runner.
The expressions, control flow and stores under test are unchanged.
"""
from pathlib import Path
import argparse
import json
import math
import re
import struct
import subprocess


CASES = {
    "uniform_arrays": (
        "int i=input.uv.x<0.0?0:1; "
        "output.color=resources.vectors[i]+resources.entries[i].tint"
        "+resources.matrices[i]*float4(resources.scalars[i])"
        "+float4(resources.entries[i].amount)+resources.vectors[0];",
        [v for _ in range(4) for pixel in ([17,29,41,53], [17,29,41,53],
                                         [67,79,91,103], [67,79,91,103]) for v in pixel]),
    "loop_break_skip": ("float r=0.0; for(int i=0;i<8;i++){ if(i==3){skip;} if(i==6){break;} r+=float(i); } output.color=float4(r);", [12]*4),
    "constant_float_table": ("float[8] a={0.1,0.4,0.2,0.9,0.7,0.3,0.8,0.5};int i=int((input.uv.x+1.0)*4.0);output.color=float4(a[i]);",
                             [v for _ in range(4) for x in (0.4,0.9,0.3,0.5) for v in [x]*4]),
    "constant_vector_table": ("float2[4] a={float2(1.0,2.0),float2(3.0,4.0),float2(5.0,6.0),float2(7.0,8.0)};int i=int((input.uv.x+1.0)*2.0);output.color=float4(a[i],a[i]);",
                              [v for _ in range(4) for x,y in ((1,2),(3,4),(5,6),(7,8)) for v in (x,y,x,y)]),
    "constant_int_table": ("int[4] a={-3,2,-1,4};int i=int((input.uv.x+1.0)*2.0);output.color=float4(float(a[i]));",
                           [v for _ in range(4) for x in (-3,2,-1,4) for v in [x]*4]),
    "constant_table_in_loop": ("float r=0.0;for(int i=0;i<4;i++){float[4] a={1.0,2.0,3.0,4.0};r+=a[i];}output.color=float4(r);", [10]*4),
    "runtime_table": ("float x=input.uv.x;float[4] a={x,x+1.0,x+2.0,x+3.0};output.color=float4(a[0],a[1],a[2],a[3]);",
                      [v for _ in range(4) for x in (-0.75,-0.25,0.25,0.75) for v in (x,x+1,x+2,x+3)]),
    "array_interleaved_updates": ("float[2] a={1.0,2.0};a[0]=a[1]+3.0;a[1]=a[0]+4.0;output.color=float4(a[0],a[1],0.0,1.0);", [5,9,0,1]),
    "array_partial_fill": ("float[4] a; a[0]=input.uv.x;a[1]=2.0;output.color=float4(a[0],a[1],0.0,1.0);",
                          [v for _ in range(4) for x in (-0.75,-0.25,0.25,0.75) for v in (x,2,0,1)]),
    "array_branch_fill": ("float[2] a;if(input.uv.x<0.0){a[0]=1.0;a[1]=2.0;}else{a[0]=3.0;a[1]=4.0;}output.color=float4(a[0],a[1],0.0,1.0);",
                         [v for _ in range(4) for x,y in ((1,2),(1,2),(3,4),(3,4)) for v in (x,y,0,1)]),
    "array_loop": ("float[4] a; for(int i=0;i<4;i++){a[i]=float(i);} float r=0.0; for(int i=0;i<4;i++){r+=a[i];} output.color=float4(r);", [6]*4),
    "nested_while": ("int i=0; float r=0.0; while(i<3){int j=0; while(j<4){r+=float(i+j);j++;}i++;} output.color=float4(r);", [30]*4),
    "loop_phi_swap": ("float a=1.0; float b=2.0; for(int i=0;i<3;i++){float t=a;a=b;b=t;} output.color=float4(a,b,a,b);", [2,1,2,1]),
    "switch_in_loop": ("float r=0.0; for(int i=0;i<4;i++){switch(i){case 0:{r+=1.0;} case 1:{r+=2.0;} default:{r+=4.0;}}} output.color=float4(r);", [11]*4),
    "swizzle_write": ("float4 v=float4(1.0,2.0,3.0,4.0);v.yx=v.zw;output.color=v;", [4,3,3,4]),
    "dynamic_vector_store": ("float4 v=float4(1.0,2.0,3.0,4.0);for(int i=0;i<4;i++){v[i]+=float(i);}output.color=v;", [1,3,5,7]),
    "integer_vector_index": ("int4 v=int4(7,8,9,10);int r=0;for(int i=0;i<4;i++){r+=v[i];}output.color=float4(float(r));", [34]*4),
    "uint_division": ("uint v=2147483648u;output.color=float4(float(v/2u));", [1073741824]*4),
    "signed_remainder": ("int a=(int((input.uv.x+1.0)*2.0)-2)*3+1;output.color=float4(float(a%3),float(a%(-3)),float((-a)%3),float((-a)%(-3)));",
                         [v for _ in range(4) for pixel in ([-2,-2,2,2],[-2,-2,2,2],[1,1,-1,-1],[1,1,-1,-1]) for v in pixel]),
    "vector_remainder": ("int a=(int((input.uv.x+1.0)*2.0)-2)*3+1;int4 r=int4(a,a,-a,-a)%int4(3,-3,3,-3);output.color=float4(r);",
                         [v for _ in range(4) for pixel in ([-2,-2,2,2],[-2,-2,2,2],[1,1,-1,-1],[1,1,-1,-1]) for v in pixel]),
    # count_bits, first_bit_high and first_bit_low (#189, #213): ES 3.00 has
    # no bitCount/findMSB/findLSB, and every backend returns the bit index,
    # -1 for no bit, and the signed form for int.
    "bit_intrinsics_uint": ("int n=int((input.uv.x+1.0)*2.0);uint u=uint(n)*6u;"
                            "output.color=float4(float(count_bits(u)),float(int(first_bit_high(u))),"
                            "float(int(first_bit_low(u))),float(count_bits(~u)));",
                            [v for _ in range(4) for pixel in ([0,-1,-1,32],[2,2,1,30],[2,3,2,30],[2,4,1,30])
                             for v in pixel]),
    "bit_count_int4": ("int n=int((input.uv.x+1.0)*2.0);int4 v=int4(n-2,n*5,-(n+1),n<<30);"
                       "output.color=float4(count_bits(v));",
                       [v for _ in range(4) for pixel in ([31,0,32,0],[32,2,31,1],[0,2,31,1],[1,4,30,2])
                        for v in pixel]),
    "first_bit_high_int4": ("int n=int((input.uv.x+1.0)*2.0);int4 v=int4(n-2,n*5,-(n+1),n<<30);"
                            "output.color=float4(first_bit_high(v));",
                            [v for _ in range(4) for pixel in ([0,-1,-1,-1],[-1,2,0,30],[-1,3,1,30],[0,3,1,29])
                             for v in pixel]),
    "first_bit_low_uint4": ("int n=int((input.uv.x+1.0)*2.0);uint4 w=uint4(int4(n-2,n*5,-(n+1),n<<30));"
                            "output.color=float4(int4(first_bit_low(w)));",
                            [v for _ in range(4) for pixel in ([1,-1,0,-1],[0,0,1,30],[-1,1,0,31],[0,0,2,30])
                             for v in pixel]),
    "bit_index_mixed": ("int n=int((input.uv.x+1.0)*2.0);int4 v=int4(n-2,n*5,-(n+1),n<<30);"
                        "int4 h=int4(first_bit_high(uint4(v)));int4 l=first_bit_low(v);"
                        "output.color=float4(float(h.x),float(h.w),float(l.x),float(l.w));",
                        [v for _ in range(4) for pixel in ([31,-1,1,-1],[31,30,0,30],[-1,31,-1,31],[0,31,0,30])
                         for v in pixel]),
    "matrix_store": ("mat2 m=mat2(1.0);m[1][0]=7.0;mat2 t=transpose(m);output.color=float4(t[0][0],t[0][1],t[1][0],t[1][1]);", [1,7,0,1]),
    "classification_select": ("float4 v=float4(0.0,1.0,-1.0,2.0);bool4 f=isfinite(v);bool4 n=isnormal(v);output.color=float4(all(f)?1.0:0.0,all(n)?1.0:0.0,any(n)?1.0:0.0,isnan(1.0)?1.0:0.0);", [1,0,1,0]),
    "fma": ("float4 r=fma(float4(2.0),float4(3.0),float4(4.0));output.color=r;", [10]*4),
    "float_precision": ("output.color=float4(0.00000013,-0.00000027,123456.789,0.33333334);", [0.00000013,-0.00000027,123456.7890625,0.3333333432674408]),
    "switch_loop_break_skip": ("float r=0.0; for(int i=0;i<8;i++){switch(i){case 1:{skip;} case 5:{break;} default:{r+=float(i);}} r+=10.0;} output.color=float4(r);", [49]*4),
    "divergent_branch_derivative": ("float r=input.uv.x; if(input.uv.x < -0.5){for(int i=0;i<3;i++){r+=1.0;}} output.color=float4(ddx(r),ddy(r),0.0,1.0);", [v for _ in range(4) for dx in (-2.5,-2.5,0.5,0.5) for v in (dx,0.0,0.0,1.0)]),
    "divergent_switch_derivative": ("float r=input.uv.x; switch(int((input.uv.x+1.0)*2.0)){case 0:{for(int i=0;i<3;i++){r+=1.0;}} default:{r+=0.0;}} output.color=float4(ddx(r),ddy(r),0.0,1.0);", [v for _ in range(4) for dx in (-2.5,-2.5,0.5,0.5) for v in (dx,0.0,0.0,1.0)]),
    "divergent_loop_derivative": ("float r=input.uv.x; for(int i=0;i<int((input.uv.x+1.0)*2.0)+1;i++){r+=1.0;} output.color=float4(ddx(r),ddy(r),0.0,1.0);", [1.5,0.0,0.0,1.0]),
    "determinant": ("mat2 m=mat2(2.0);output.color=float4(determinant(m));", [4]*4),
    "pointer_fallback": ("float x=1.0; float^ p = ^x; p^ = 7.0; output.color=float4(x);", [7]*4),
    "boolean_not": ("bool b=input.uv.x<0.0;output.color=float4(!b?1.0:0.0);",
                    [v for _ in range(4) for x in (0,0,1,1) for v in [x]*4]),
    "numeric_not": ("int n=int((input.uv.x+1.0)*2.0);output.color=float4(!n?1.0:0.0,!float(n)?1.0:0.0,!uint(n)?1.0:0.0,1.0);",
                    [v for _ in range(4) for x in (1,0,0,0) for v in (x,x,x,1)]),
    "integer_not": ("int n=int((input.uv.x+1.0)*2.0);output.color=float4(float(~n));",
                    [v for _ in range(4) for x in (-1,-2,-3,-4) for v in [x]*4]),
    "lazy_scalar_expressions": ("int n=0;bool b=input.uv.x<0.0;bool a=b&&++n>0;bool c=b||++n>0;int r=b?++n:++n+10;output.color=float4(float(n),float(r),a?1.0:0.0,c?1.0:0.0);",
                                [v for _ in range(4) for pixel in ([2,2,1,1],[2,2,1,1],[2,12,0,1],[2,12,0,1]) for v in pixel]),
    # Struct locals are accessed in place (#143): element and field writes,
    # copies that stay independent, and a mutating method in a loop.
    "struct_in_place": (
        "State s; for(int i=0;i<4;i++){s.items[i].kind=i%2;s.items[i].center=float2(float(i),1.0);} "
        "s.index=-1; for(int i=1;i<4;i++){if(s.items[i].kind!=s.items[0].kind){s.index=i;s.best=s.items[i].center;}} "
        "output.color=float4(float(s.index),s.best.x,s.best.y,float(s.items[2].kind));",
        [3,3,1,0],
        "struct Item { int kind; float2 center; }; struct State { Item[4] items; float2 best; int index; }; "),
    "struct_copy_isolation": (
        "Pair x; x.a=1.0; Pair y=x; x.a=5.0; y.b=2.0; output.color=float4(x.a,y.a,y.b,x.b);",
        [5,1,2,0],
        "struct Pair { float a; float b; }; "),
    "struct_method_mutation": (
        "Counter c; for(int i=0;i<3;i++){c.bump(float(i));} output.color=float4(float(c.n),c.total,0.0,1.0);",
        [3,3,0,1],
        "struct Counter { int n; float total; "
        "bump :: (float v) -> void { self.n = self.n + 1; total += v; } }; "),
    "large_output": ("float r=input.uv.x;" + "r+=1.0;" * 1800 + "output.color=float4(r);",
                     [value for _ in range(4) for x in (-0.75,-0.25,0.25,0.75) for value in [1800+x]*4]),
}


def run_gles_regression_tests(compiler: Path, output: Path, runner: Path | None = None) -> tuple[int, int]:
    output.mkdir(parents=True, exist_ok=True)
    results = []

    def run(command):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(f"{command}: exit {result.returncode}\n{result.stdout}{result.stderr}")

    for name, (body, expected_pixel, *extra) in CASES.items():
        declarations = extra[0] if extra else ""
        folder = output / name
        folder.mkdir(exist_ok=True)
        source = folder / f"{name}.bwsl"
        resources = ("struct Entry { float4 tint; float amount; } "
                     "resources { vectors: float4[2] scalars: float[2] "
                     "matrices: mat4[2] entries: Entry[2] } ") if name == "uniform_arrays" else ""
        use_resources = "use resources { vectors, scalars, matrices, entries } " if resources else ""
        attributes = "" if resources else "attributes { position: float3 } "
        vertex = ("float2 uv=float2(float((input.vertex_id<<1u)&2u),float(input.vertex_id&2u))*2.0-1.0; "
                  "output.position=float4(uv,0.0,1.0); output.uv=uv; ") if resources else (
                  "output.position=float4(attributes.position,1.0); output.uv=attributes.position.xy; ")
        use_attributes = "" if resources else "use attributes { position } "
        source.write_text('pipeline GLESRegression { ' + declarations + attributes + resources + 'pass "Main" { '
                          + use_resources + use_attributes + 'vertex { ' + vertex
                          + '} fragment { ' + body + ' } } }')
        try:
            run([compiler, source, '-gles-direct', '-spv', '-bindings', '-validation', 'strict', '-o', folder])
            # Arrays filled once with constants are initialized where they're
            # declared; other complete fills become one constructor assignment.
            constructors = {
                'constant_float_table': ('float', 8, True), 'constant_vector_table': ('vec2', 4, False),
                'constant_int_table': ('int', 4, True), 'constant_table_in_loop': ('float', 4, True),
                'runtime_table': ('float', 4, False),
            }
            if name in constructors:
                kind, count, initialized = constructors[name]
                fragment_text = (folder / f'{name}.frag').read_text()
                constructor = kind + r'\[' + str(count) + r'\]\('
                declaration = re.search(kind + r' (r\d+)\[' + str(count) + r'\] = ' + constructor, fragment_text)
                assignment = re.search(r'(r\d+) = ' + constructor, fragment_text)
                if initialized:
                    assert declaration, 'Constant table was not initialized in its declaration'
                    assert not assignment, 'Constant table is also assigned after its declaration'
                    match = declaration
                else:
                    assert assignment, 'Complete array fill did not become a constructor assignment'
                    assert not declaration, 'Non-constant table was initialized in its declaration'
                    match = assignment
                assert not re.search(r'(?m)^\s*' + match[1] + r'\[[^]]+\] = ', fragment_text), 'Per-element table stores remain'
            if name == 'array_interleaved_updates':
                # Written again after its constant fill: must not move into the declaration.
                assert not re.search(r'float r\d+\[2\] = ', (folder / f'{name}.frag').read_text()), \
                    'Array written after its constant fill was initialized in its declaration'
            if name == 'large_output':
                assert (folder / f'{name}.frag').stat().st_size > 65536, 'Output-growth path was not exercised'
            for stage in ('vert', 'frag'):
                run(['glslangValidator', '-S', stage, folder / f'{name}.{stage}'])
            if runner:
                fragment = folder / 'runtime.frag'
                fragment.write_text((folder / f'{name}.frag').read_text().replace('#version 300 es', '#version 310 es'))
                run(['glslangValidator', '-V', '--auto-map-locations', '--auto-map-bindings', '-S', 'frag', fragment, '-o', folder / 'direct.spv'])
                vbo = folder / 'vbo.bin'
                vbo.write_bytes(struct.pack('<9f', -1,-1,0, 3,-1,0, -1,3,0))
                resource_paths = []
                if resources:
                    # std140 gives scalar array elements a 16-byte stride and
                    # rounds Entry (vec4 + float) to a 32-byte array stride.
                    buffers = [list(range(1, 9)), [2,0,0,0,3,0,0,0],
                               [scale if row == col else 0 for scale in (2,3)
                                for col in range(4) for row in range(4)],
                               [10,20,30,40,1,0,0,0,50,60,70,80,2,0,0,0]]
                    for binding, values in enumerate(buffers):
                        path = folder / f"ubo{binding}.bin"
                        path.write_bytes(struct.pack('<' + 'f'*len(values), *values))
                        resource_paths.append(path)
                for backend, frag in [('native', folder / f'{name}.frag.spv'), ('direct', folder / 'direct.spv')]:
                    readback = folder / f'{backend}.bin'
                    resource_args = []
                    if resources:
                        reflected = json.loads((folder / f'{name}.bindings.json').read_text())['resources']
                        bindings = {resource['name']: resource['binding'] for resource in reflected}
                        for binding, (member, path) in enumerate(zip(
                                ('vectors', 'scalars', 'matrices', 'entries'), resource_paths)):
                            slot = bindings[member] if backend == 'native' else binding
                            resource_args += ['--raster-ubo', path, str(slot)]
                    run([runner, '--raster', '--vert-spirv', folder / f'{name}.vert.spv', '--frag-spirv', frag,
                         '--width', '4', '--height', '4', '--output', readback, '--output-size', '256', '--set', '1',
                         '--raster-vbo', vbo, '3', '12', '--raster-vbo-attr', '0', 'R32G32B32_SFLOAT', '0', *resource_args])
                    actual = struct.unpack('<64f', readback.read_bytes())
                    expected = expected_pixel * 16 if len(expected_pixel) == 4 else expected_pixel
                    assert len(actual) == len(expected), (len(actual), len(expected))
                    tolerance = 1e-9 if name == 'float_precision' else 1e-5
                    assert all(math.isfinite(a) and abs(a-e) <= tolerance for a, e in zip(actual, expected)), (backend, actual[:4], expected_pixel)
            results.append({'case': name, 'ok': True, 'cpu_oracle': runner is not None})
            print(f'[PASS] direct GLES/{name}' + (' (native + direct CPU oracle)' if runner else ' (syntax)'))
        except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
            results.append({'case': name, 'ok': False, 'error': str(error)})
            print(f'[FAIL] direct GLES/{name}: {error}')
    # Exercise the reported field-array temporary in both shader stages.
    for stage in ('vertex', 'fragment'):
        name = 'uniform_struct_array_loop_' + stage
        folder = output / name
        folder.mkdir(exist_ok=True)
        source = folder / f'{name}.bwsl'
        loop = ('float4 sum=float4(0.0);for(int i=0;i<4;i++){sum+=resources.scene.colors[i];}'
                + ('output.col' if stage == 'vertex' else 'output.color') + '=sum*resources.scene.tint;')
        source.write_text('pipeline UniformArrayLoop { attributes { position: float3 } '
                          'struct Scene { float4 tint; float4[4] colors; }; resources { scene: Scene } '
                          'pass "Main" { use attributes { position } use resources { scene } '
                          'vertex { output.position=float4(attributes.position,1.0);'
                          + (loop if stage == 'vertex' else '') + ' } fragment {'
                          + ('output.color=input.col;' if stage == 'vertex' else loop) + ' } } }')
        try:
            run([compiler, source, '-gles-direct', '-validation', 'strict', '-o', folder])
            for suffix in ('vert', 'frag'):
                shader = folder / f'{name}.{suffix}'
                run(['glslangValidator', '-S', suffix, shader])
                if runner:
                    runtime = folder / f'runtime.{suffix}'
                    runtime.write_text(shader.read_text().replace('#version 300 es', '#version 310 es'))
                    run(['glslangValidator', '-V', '--auto-map-locations', '--auto-map-bindings',
                         '-S', suffix, runtime, '-o', folder / f'{suffix}.spv'])
            if runner:
                vbo, ubo, readback = folder / 'vbo.bin', folder / 'ubo.bin', folder / 'result.bin'
                vbo.write_bytes(struct.pack('<9f', -1,-1,0, 3,-1,0, -1,3,0))
                ubo.write_bytes(struct.pack('<20f', 0.5,1,2,4, *[v for i in (1,2,3,4) for v in (i,2*i,3*i,4*i)]))
                run([runner, '--raster', '--vert-spirv', folder / 'vert.spv', '--frag-spirv', folder / 'frag.spv',
                     '--width', '4', '--height', '4', '--output', readback, '--output-size', '256', '--set', '0',
                     '--raster-ubo', ubo, '0', '--raster-vbo', vbo, '3', '12',
                     '--raster-vbo-attr', '0', 'R32G32B32_SFLOAT', '0'])
                actual = struct.unpack('<64f', readback.read_bytes())
                assert all(abs(a-e) < 1e-5 for a,e in zip(actual, [5,20,60,160]*16)), actual[:4]
            results.append({'case': name, 'ok': True, 'cpu_oracle': runner is not None})
            print(f'[PASS] direct GLES/{name}' + (' (CPU oracle)' if runner else ' (syntax)'))
        except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
            results.append({'case': name, 'ok': False, 'error': str(error)})
            print(f'[FAIL] direct GLES/{name}: {error}')
    (output / 'results.json').write_text(json.dumps(results, indent=2))
    passed = sum(result['ok'] for result in results)
    return passed, len(results)-passed


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', type=Path, default=Path('build/bwslc'))
    parser.add_argument('--output', type=Path, default=Path('build/gles_regressions'))
    parser.add_argument('--runner', type=Path)
    args = parser.parse_args()
    passed, failed = run_gles_regression_tests(args.compiler.resolve(), args.output.resolve(),
                                             args.runner.resolve() if args.runner else None)
    print(f'Direct GLES: {passed} passed, {failed} failed')
    raise SystemExit(bool(failed))
