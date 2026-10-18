#!/usr/bin/env python3
import pathlib
import struct
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 6:
        raise SystemExit("usage: make_vk_shaders.py glslangValidator out.c ui.vert atlas.frag dedicated.frag")
    compiler, output, *sources = sys.argv[1:]
    names = ('vertex', 'atlas', 'dedicated')
    generated = ['#include "src/vulkan/shaders.h"\n']
    with tempfile.TemporaryDirectory() as directory:
        for name, source in zip(names, sources):
            compiled = pathlib.Path(directory) / (name + '.spv')
            subprocess.run([compiler, '-V', '--target-env', 'vulkan1.0', '-o', str(compiled), source], check=True)
            data = compiled.read_bytes()
            if len(data) % 4:
                raise ValueError(f"{source} compiled to non-word-aligned SPIR-V")
            words = struct.unpack('<' + 'I' * (len(data) // 4), data)
            generated.append(f'const uint32_t rn_vk_{name}_spv[] = {{\n')
            for start in range(0, len(words), 8):
                generated.append('  ' + ', '.join(f'0x{word:08x}u' for word in words[start:start + 8]) + ',\n')
            generated.append('};\n')
            generated.append(f'const size_t rn_vk_{name}_spv_size = sizeof(rn_vk_{name}_spv);\n')
    pathlib.Path(output).write_text(''.join(generated))


if __name__ == '__main__':
    main()

