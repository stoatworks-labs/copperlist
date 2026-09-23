"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds the two GLSL strings that `source/Shaders.cpp` holds.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. The whole claim of the page is that the last step, the chip's
picture onto the output, is the plugin's own shader rather than something
reimplemented to look similar, so the claim needs something enforcing it.

Nothing else can. `cptest` drives the real plugin class in a headless GL
context and has no idea this page exists, and `tools/verify.sh`'s glslc step
compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. The comments carry the reasoning (why the fetch is at
`floor( u ) + 1`, what the ring is), and a comment updated on one side only is
exactly the drift worth catching.

The only transformation allowed is a decode, not a normalisation: a backtick or
a `${` cannot appear raw inside a JavaScript template literal, so `plugin.js`
would escape them as \\` and \\${. Copperlist's GLSL contains neither today, so
the decode is idle -- and *any other backslash on the JS side is rejected*. The
C++ GLSL has no backslash at all (checked below, so that stays true), so a
second escape could only be somebody hiding a difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half, which is almost all of the plugin:
`Chipset`, `Demo`, the font, the sine table, the hash, `ComputeLayout`, the
accumulators and every `Controls.h` conversion in plugin.js are a hand
translation of source/chip/, source/demo/, Render.cpp, Copperlist.cpp and
Controls.h. `crosscheck.mjs`, beside this, compares what that port paints with
the real plugin's output; when you change one of those files, change the port
too and run it.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/Shaders.cpp", "kVertexShader"),
    ("FRAGMENT", "source/Shaders.cpp", "kFragmentShader"),
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the escapes the literal needs, and refuse the rest.
    stray = re.search(r"\\(?!`|\$\{)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick or ${{, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`").replace("\\${", "${"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if "\\" in cpp_text:
            # The decode above assumes the C++ has none. If one arrives (a
            # line continuation in a macro, say), the JS copy needs it as \\
            # and this checker needs to learn that, rather than wave it through.
            print(f"FAIL  {symbol} in {path} now contains a backslash -- teach from_js to decode \\\\")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<10} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
