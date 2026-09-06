"""Validate direct GLES and execute raster readbacks against independent values.

Runtime conversion raises only the ES version to 310 and assigns interface
locations so the GLES fragment can execute on the existing Vulkan test runner.
The expressions, control flow and stores under test are unchanged.
"""
from pathlib import Path
import argparse
import json
import math
import struct
import subprocess


CASES = {
    "loop_break_skip": ("float r=0.0; for(int i=0;i<8;i++){ if(i==3){skip;} if(i==6){break;} r+=float(i); } output.color=float4(r);", [12]*4),
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

    for name, (body, expected_pixel) in CASES.items():
        folder = output / name
        folder.mkdir(exist_ok=True)
        source = folder / f"{name}.bwsl"
        source.write_text('pipeline GLESRegression { attributes { position: float3 } pass "Main" { '
                          'use attributes { position } vertex { output.position=float4(attributes.position,1.0); '
                          'output.uv=attributes.position.xy; } fragment { ' + body + ' } } }')
        try:
            run([compiler, source, '-gles-direct', '-spv', '-validation', 'strict', '-o', folder])
            if name == 'large_output':
                assert (folder / f'{name}.frag').stat().st_size > 65536, 'Output-growth path was not exercised'
            for stage in ('vert', 'frag'):
                run(['glslangValidator', '-S', stage, folder / f'{name}.{stage}'])
            if runner:
                fragment = folder / 'runtime.frag'
                fragment.write_text((folder / f'{name}.frag').read_text().replace('#version 300 es', '#version 310 es'))
                run(['glslangValidator', '-V', '--auto-map-locations', '-S', 'frag', fragment, '-o', folder / 'direct.spv'])
                vbo = folder / 'vbo.bin'
                vbo.write_bytes(struct.pack('<9f', -1,-1,0, 3,-1,0, -1,3,0))
                for backend, frag in [('native', folder / f'{name}.frag.spv'), ('direct', folder / 'direct.spv')]:
                    readback = folder / f'{backend}.bin'
                    run([runner, '--raster', '--vert-spirv', folder / f'{name}.vert.spv', '--frag-spirv', frag,
                         '--width', '4', '--height', '4', '--output', readback, '--output-size', '256', '--set', '1',
                         '--raster-vbo', vbo, '3', '12', '--raster-vbo-attr', '0', 'R32G32B32_SFLOAT', '0'])
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
