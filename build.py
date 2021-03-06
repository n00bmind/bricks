#!py3
# -*- coding: utf-8 -*-
# The MIT License

# Copyright (c) 2017 Oscar Peñas Pariente <oscarpp80@gmail.com>

# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.



import os, sys, subprocess, atexit, random, argparse, shutil, fnmatch
from collections import namedtuple


Platform = namedtuple('Platform', ['name', 'compiler', 'toolset', 'common_compiler_flags', 'libs', 'common_linker_flags'])

platform_win = Platform(
        name                  = 'win',
        compiler              = 'cl.exe',
        toolset               = 'CL',
        common_compiler_flags = [
            '-nologo', '-FC', '-Oi', '-GR-', '-EHa-', '-Wall', '-WX',
            '-D_HAS_EXCEPTIONS=0', '-D_CRT_SECURE_NO_WARNINGS',
            '-wd4061',          # Unhandled enum case in switch
            '-wd4062',          # Unhandled enum case in switch
            '-wd4100',          # Unreferenced parameter
            '-wd4101',          # Unused local variable
            '-wd4189',          # Initialized but unreferenced local variable
            '-wd4200',          # Zero-sized array in struct
            '-wd4201',          # Nameless struct/union
            '-wd4312',          # Conversion from int to pointer
            '-wd4426',          # Optimizations changed 
            '-wd4464',          # Relative include path contains '..'
            '-wd4505',          # Unreferenced function
            '-wd4514',          # Unreferenced inline function removed
            '-wd4577',          # Don't whine about exception handling model
            '-wd4582',          # Constructor is not impliclty called
            '-wd4623',          # Default constructor implicitly deleted
            '-wd4625',          # Copy constructor implicitly deleted
            '-wd4626',          # Assignment operator implicitly deleted
            '-wd4668',          # Undefined preprocessor macro
            '-wd4710',          # Function not inlined
            '-wd4711',          # Function inlined
            '-wd4820',          # Padding added
            '-wd5026',          # Move constructor implicitly deleted
            '-wd5027',          # Move assignment implicitly deleted
            '-wd5045',          # Spectre mitigations
            ],
        libs                  = ['dbghelp.lib', 'ws2_32.lib', 'advapi32.lib', 'shlwapi.lib'],
                                #'user32.lib', 'gdi32.lib', 'winmm.lib', 'ole32.lib', 'opengl32.lib', ],
        common_linker_flags   = ['/opt:ref', '/incremental:no']
)

platform_win_clang = platform_win._replace(
        name                  = 'win_clang',
        compiler              = 'clang-cl.exe',
        common_compiler_flags = platform_win.common_compiler_flags + [
            '-fno-exceptions',
            '-fno-rtti',
            '-fdiagnostics-absolute-paths',
            '-Wno-missing-braces',
            '-Wno-unused-variable',
            '-Wno-unused-function',
            '-Wno-missing-field-initializers'
        ],
)



Config = namedtuple('Configuration', ['name', 'platform', 'cmdline_opts', 'compiler_flags', 'linker_flags'])

config_win_debug = Config(
        name           = 'Debug',
        platform       = platform_win,
        cmdline_opts   = ['d', 'dbg', 'debug'],
        compiler_flags = ['-DCONFIG_DEBUG=1', '-Z7', '-MTd', '-Od'],
        linker_flags   = ['/debug:full']                # Required for debugging after hot reloading (and RemedyBG)
)
# This build makes sense as a faster non-release build to use during development when Debug is just too slow,
# for example one that artists could use as their everyday default (not sure how useful all the debug information is in this context).
config_win_develop = Config(
        name           = 'Develop',
        platform       = platform_win,
        cmdline_opts   = ['dev', 'develop'],
        compiler_flags = ['-DCONFIG_DEVELOP=1', '-Z7', '-MT', '-O2', '-GL'],
        linker_flags   = ['/debug:full', '/LTCG']
)
config_win_release = Config(
        name           = 'Release',
        platform       = platform_win,
        cmdline_opts   = ['r', 'rel', 'release'],
        compiler_flags = ['-DCONFIG_RELEASE=1', '-Z7', '-MT', '-O2', '-GL'],
        linker_flags   = ['/debug:full', '/LTCG']
)


default_config = config_win_debug
# default_config = config_win_develop
default_platform = platform_win

include_dirs = [
    'src',
    '3rdparty/mbedtls/include'
]

lib_dirs = [
    'bench/benchmark',
]

# FIXME Remove this variable and just add the mbedtls dir to the list above plus use a separate pragma lib per config like the benchmark lib above
# TODO Can we use the release libs in Develop?
user_libs = [
    '3rdparty/mbedtls/lib/Debug/mbedTLS.lib',
    # '3rdparty/mbedtls/lib/Release/mbedTLS.lib',
]


class colors:
    GRAY = '\033[1;30m'
    RED = '\033[1;31m'
    GREEN = '\033[1;32m'
    END = '\033[0m'
# print '\033[1;33mYellow like Yolk\033[1;m'
# print '\033[1;34mBlue like Blood\033[1;m'
# print '\033[1;35mMagenta like Mimosa\033[1;m'
# print '\033[1;36mCyan like Caribbean\033[1;m'
# print '\033[1;37mWhite like Whipped Cream\033[1;m'
# print '\033[1;38mCrimson like Chianti\033[1;m'
# print '\033[1;41mHighlighted Red like Radish\033[1;m'
# print '\033[1;42mHighlighted Green like Grass\033[1;m'
# print '\033[1;43mHighlighted Brown like Bear\033[1;m'
# print '\033[1;44mHighlighted Blue like Blood\033[1;m'
# print '\033[1;45mHighlighted Magenta like Mimosa\033[1;m'
# print '\033[1;46mHighlighted Cyan like Caribbean\033[1;m'
# print '\033[1;47mHighlighted Gray like Ghost\033[1;m'
# print '\033[1;48mHighlighted Crimson like Chianti\033[1;m'

def print_color(text, color_string):
    print(color_string, end='')
    print(text, end='')
    print(colors.END)


def begin_time():
    subprocess.call(['ctime', '-begin', 'bricks.time'])

def end_time():
    # TODO Check this picks up failures etc.
    subprocess.call(['ctime', '-end', 'bricks.time'])

    
if __name__ == '__main__':
    srcdir = 'src'
    bindir = 'bin'

    parser = argparse.ArgumentParser()
    config_group = parser.add_mutually_exclusive_group()
    config_group.add_argument('-d', '--debug', help='Create Debug build', action='store_true')
    config_group.add_argument('--dev', help='Create Develop build', action='store_true')
    config_group.add_argument('-r', '--release', help='Create Release build', action='store_true')
    parser.add_argument('-c', '--clean', help='Delete contents of the bin folder before building', action='store_true')
    parser.add_argument('-v', '--verbose', help='Increase verbosity', action='store_true')
    parser.add_argument('-t', '--runtests', help='Run all tests found in subfolders', action='store_true')
    in_args = parser.parse_args()

    atexit.register(end_time)
    begin_time()

    rootpath = os.path.dirname(os.path.abspath(__file__))
    srcpath = os.path.join(rootpath, srcdir)
    binpath = os.path.join(rootpath, bindir)

    if not os.path.exists(binpath):
        os.mkdir(binpath)

    if in_args.clean:
        print(f'Removing contents of \'{binpath}\'..')
        for f in os.listdir(binpath):
            path = os.path.join(binpath, f)
            try:
                if os.path.isfile(path) or os.path.islink(path):
                    os.unlink(path)
                elif os.path.isdir(path):
                    shutil.rmtree(path)
            except Exception as e:
                print(f'Couldn\'t delete \'{path}\' ({e})')

    # TODO Determine platform/config
    platform = default_platform
    config = default_config
    if in_args.debug:
        config = config_win_debug
    elif in_args.dev:
        config = config_win_develop
    elif in_args.release:
        config = config_win_release

    with open(os.path.join(binpath, 'config'), 'w') as cfg_file:
        print(f'-> Config \'{config.name}\'')
        cfg_file.write(f'Config: {config.name}\n')
        cfg_file.write(f'Platform: {platform.name}\n\n')

        if platform.toolset == 'CL':
            #
            # Build main executable
            # out_args = [platform.compiler]
            # out_args.extend(platform.common_compiler_flags)
            # out_args.extend(config.compiler_flags)
            # for inc in include_dirs:
                # out_args.append(f'/I{os.path.join(rootpath, inc)}')
            # out_args.append(os.path.join(rootpath, 'main.cpp'))
            # out_args.append('-Femain.exe')
            # out_args.append('/link')
            # out_args.extend(platform.common_linker_flags)
            # out_args.extend(config.linker_flags)
            # out_args.append('-subsystem:console,5.2')
            # out_args.extend(platform.libs)
            # for lib in user_libs:
                # out_args.append(f'{os.path.join(rootpath, lib)}')

            # if in_args.verbose:
                # print('\nBuilding platform executable...')
                # print_color(out_args, colors.GRAY)
            # cfg_file.write(f'Platform exe args:\n{out_args}\n\n')

            # ret = subprocess.call(out_args, cwd=binpath)
            ret = True

            #
            # Build test suite
            out_args = [platform.compiler]
            out_args.extend(platform.common_compiler_flags)
            out_args.extend(config.compiler_flags)
            for inc in include_dirs:
                out_args.append(f'/I{os.path.join(rootpath, inc)}')
            out_args.append(os.path.join(rootpath, 'test\\test.cpp'))
            out_args.append('/link')
            out_args.extend(platform.common_linker_flags)
            out_args.extend(config.linker_flags)
            out_args.append('-subsystem:console,5.2')
            out_args.extend(platform.libs)
            for lib in user_libs:
                out_args.append(f'{os.path.join(rootpath, lib)}')

            if in_args.verbose:
                print('\nBuilding test suite...')
                print_color(out_args, colors.GRAY)
            cfg_file.write(f'Test suite args:\n{out_args}\n\n')

            ret |= subprocess.call(out_args, cwd=binpath)

            #
            # Build benchmark suite
            out_args = [platform.compiler]
            out_args.extend(platform.common_compiler_flags)
            out_args.extend(config.compiler_flags)
            for inc in include_dirs:
                out_args.append(f'/I{os.path.join(rootpath, inc)}')
            out_args.append(os.path.join(rootpath, 'bench\\bench.cpp'))
            out_args.append('/link')
            out_args.extend(platform.common_linker_flags)
            out_args.extend(config.linker_flags)
            out_args.append('-subsystem:console,5.2')
            for lib in lib_dirs:
                out_args.append(f'/libpath:{os.path.join(rootpath, lib)}')
            out_args.extend(platform.libs)
            for lib in user_libs:
                out_args.append(f'{os.path.join(rootpath, lib)}')

            if in_args.verbose:
                print('\nBuilding benchmarks...')
                print_color(out_args, colors.GRAY)
            cfg_file.write(f'Benchmark suite args:\n{out_args}\n\n')

            ret |= subprocess.call(out_args, cwd=binpath)

        else:
            sys.exit('Unsupported toolset')

    # if ret == 0 and in_args.runtests:
        # exepath = os.path.join(binpath, 'do.exe')

        # test_folders = ['tests', 'examples']
        # for folder in test_folders:
            # folder = os.path.join(rootpath, folder)
            # test_files = fnmatch.filter(os.listdir(folder), '*.do')

            # for file in test_files:
                # print(f'Testing {file:<30}... ', end='')

                # phase = 'Compilation'
                # # Compile it
                # out_args = [exepath]
                # out_args.append(file)
                # # Capture all output
                # proc = subprocess.run(out_args, cwd=folder, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

                # if proc.returncode == 0:
                    # file = os.path.join(folder, file)
                    # file = os.path.splitext(file)[0] + '.exe'

                    # phase = 'Execution'
                    # # Run it
                    # out_args = [file]
                    # proc = subprocess.run(out_args, cwd=rootpath, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

                # ok = proc.returncode == 0
                # print_color('[ OK ]' if ok else '[FAIL]', colors.GREEN if ok else colors.RED)

                # if not ok:
                    # print(f'{phase} returned {proc.returncode}')
                    # print(proc.stdout.decode())
                    # print()

    sys.exit(ret)
