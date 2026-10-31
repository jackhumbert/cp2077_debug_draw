"""Debug Draw shader compiler: HLSL pixel shaders -> a mod folder the Debug Draw plugin serves.

For each effect (one .hlsl per effect, named after the file stem unless given as name=file.hlsl):
  1. compile with dxc (ps_6_0, -O3, stripped of reflection and debug info);
  2. validate an unstripped build against the debug-draw interface (signatures, bindings, shader model);
  3. build a material template (.mt) from the game's engine\\materials\\debugdraw.remt named <mod>_<effect>;
  4. package: red4ext\\plugins\\debug_draw\\shaders\\<mod>\\ (blobs + manifest), archive\\pc\\mod\\<mod>_shaders.archive
     (the .mt files) and readme.txt.

Python 3 standard library only. Run with python -I. See README.md beside this file.
"""

import argparse
import copy
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
BUNDLED_INCLUDE = os.path.join(TOOL_DIR, "include")
DEFAULT_GAME = r"D:\Games\SteamLibrary\steamapps\common\Cyberpunk 2077"
DEFAULT_WOLVENKIT = shutil.which("WolvenKit.CLI.exe") or "WolvenKit.CLI.exe"
DONOR = "debugdraw"
DONOR_DEPOT = r"engine\materials\debugdraw.remt"
PLUGIN_SHADERS = os.path.join("red4ext", "plugins", "debug_draw", "shaders")
PROFILE = "ps_6_0"
NAME_RE = re.compile(r"^[a-z0-9_]+$")

# Byte sizes of the engine constant buffers a debug-draw pixel shader may bind (docs/shader_constants.md).
# b4 holds the material parameters: one float4 row per parameter (its size limit depends on the shader).
ALLOWED_CBUFFERS = {0: ("GlobalShaderConsts", 528), 1: ("CameraShaderConsts", 852), 4: ("ShaderSpecificConstants", None),
                    7: ("MaterialModifiersConsts", 512)}
ALLOWED_TEXT = "cb0 (GlobalShaderConsts), cb1 (CameraShaderConsts), cb4 (ShaderSpecificConstants) and cb7 (MaterialModifiersConsts)"
# Templates whose CName differs from their file stem (docs/shader_research.md).
EXTRA_VANILLA_NAMES = ("silverhand_overlay", "water_test")


class ToolError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Hashing


def fnv1a64(text):
    h = 0xCBF29CE484222325
    for byte in text.encode("utf-8"):
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def fold(h):
    return (h & 0xFFFFFFFF) ^ (h >> 32)


def name_fold(name):
    """The 32-bit half of a material lookup key: lo32 ^ hi32 of FNV-1a 64 of the template name."""
    return fold(fnv1a64(name))


# ---------------------------------------------------------------------------------------------------------------
# dxc -dumpbin parsing


def _strip(line):
    line = line.rstrip("\r\n")
    return line[1:] if line.startswith(";") else line


def _table_after(lines, start):
    """Rows of the dash-ruled table that follows lines[start]: a list of dicts keyed by the header words."""
    i = start
    while i < len(lines):
        text = _strip(lines[i])
        if re.match(r"^\s*-{2,}(\s+-{2,})*\s*$", text):
            break
        if i > start and text.strip() and not text.strip().startswith("Name"):
            return []  # e.g. "no parameters"
        i += 1
    else:
        return []
    ruler = _strip(lines[i])
    header = _strip(lines[i - 1])
    spans = [(m.start(), m.end()) for m in re.finditer(r"-+", ruler)]
    keys = []
    for n, (a, b) in enumerate(spans):
        end = spans[n + 1][0] if n + 1 < len(spans) else len(header)
        keys.append(header[a:end].strip())
    rows = []
    for line in lines[i + 1:]:
        text = _strip(line)
        if not text.strip():
            break
        row = {}
        for n, (a, b) in enumerate(spans):
            # Values can overhang their ruler (long names); cut at the next column's start.
            end = spans[n + 1][0] if n + 1 < len(spans) else len(text)
            row[keys[n]] = text[a:end].strip()
        rows.append(row)
    return rows


def parse_signature(dump, which):
    """The '<which> signature' table (which = 'Input' or 'Output') with Mask/Register columns."""
    lines = dump.splitlines()
    for i, line in enumerate(lines):
        if _strip(line).strip() == which + " signature:":
            rows = _table_after(lines, i + 1)
            if not rows or "Register" in rows[0]:
                return [
                    {
                        "name": r["Name"],
                        "index": int(r["Index"]),
                        "mask": r["Mask"].replace(" ", ""),
                        "register": int(r["Register"]) if r["Register"].lstrip("-").isdigit() else -1,
                        "sysvalue": r["SysValue"],
                        "format": r["Format"],
                        "used": r.get("Used", "").replace(" ", ""),
                    }
                    for r in rows
                ]
    raise ToolError("dumpbin has no %s signature table" % which.lower())


def parse_bindings(dump):
    lines = dump.splitlines()
    for i, line in enumerate(lines):
        if _strip(line).strip() == "Resource Bindings:":
            rows = _table_after(lines, i + 1)
            out = []
            for r in rows:
                bind = r.get("HLSL Bind", "")
                m = re.match(r"^(cb|t|s|u)(\d+)(?:,space(\d+))?$", bind)
                out.append(
                    {
                        "name": r["Name"],
                        "type": r["Type"],
                        "dim": r.get("Dim", ""),
                        "bind": bind,
                        "class": m.group(1) if m else None,
                        "register": int(m.group(2)) if m else None,
                        "space": int(m.group(3) or 0) if m else None,
                        "count": r.get("Count", "1"),
                    }
                )
            return out
    return []  # no resources at all


def parse_cbuffer_sizes(dump):
    sizes = {}
    current = None
    for line in dump.splitlines():
        text = _strip(line)
        m = re.match(r"^\s*cbuffer\s+(\S+)\s*$", text)
        if m:
            current = m.group(1)
            continue
        m = re.match(r"^\s*\}\s*\S+;\s*;\s*Offset:\s*\d+\s+Size:\s*(\d+)", text)
        if m and current:
            sizes[current] = int(m.group(1))
            current = None
    return sizes


def parse_shader_model(dump):
    m = re.search(r"^!dx\.shaderModel = !\{!(\d+)\}", dump, re.M)
    if not m:
        raise ToolError("dumpbin has no !dx.shaderModel")
    m = re.search(r'^!%s = !\{!"(\w+)", i32 (\d+), i32 (\d+)\}' % m.group(1), dump, re.M)
    if not m:
        raise ToolError("dumpbin shader model metadata not understood")
    return m.group(1), int(m.group(2)), int(m.group(3))


EXPECTED_INPUTS = [
    # name, index, register, mask (None = any), what it carries
    ("SV_Position", 0, 0, None, "pixel position"),
    ("TEXCOORD", 0, 1, "xyzw", "uv in xy, red and green in zw"),
    ("TEXCOORD", 1, 2, "xy", "blue and alpha"),
]


# The input layouts of debugdraw's two vertex factories (3.0.88): name, index, mask, register.
FACTORY_INPUTS = [("POSITION", 0, "xyz", 0), ("TEXCOORD", 0, "xy", 1), ("COLOR", 0, "xyzw", 2)]
VERTEX_FACTORIES = [
    # label, file suffix, extra dxc defines, donor VS ID, input layout, constant buffers (register: (name, bytes))
    ("DrawBuffer", "drawbuffer", [], "46AB074CDBB5F2E3", FACTORY_INPUTS,
     {0: ("GlobalShaderConsts", 528), 1: ("CameraShaderConsts", 852), 5: ("FrequentVertexConsts", 112)}),
    ("Debug", "debug", ["-D", "DEBUG_FACTORY"], "687287DC8EC613C8",
     FACTORY_INPUTS + [("INSTANCE_TRANSFORM", i, "xyzw", 3 + i) for i in range(3)],
     {0: ("GlobalShaderConsts", 528), 1: ("CameraShaderConsts", 852)}),
]
# Pixel shader inputs the rasterizer makes, which no vertex shader writes.
RASTERIZER_INPUTS = {"SV_ISFRONTFACE", "SV_PRIMITIVEID", "SV_SAMPLEINDEX", "SV_COVERAGE"}


def _element(e):
    return "%s%d (register %d, %s %s)" % (e["name"], e["index"], e["register"], e["mask"], e["format"])


def match_ps_inputs(inputs, variants):
    """Errors for pixel shader inputs that a vertex shader variant does not write at the same register.

    variants: [(label, output signature rows)]. An input matches an output of the same semantic and index at the same
    register and format whose mask covers the input's mask."""
    errors = []
    for e in inputs:
        if e["name"].upper() in RASTERIZER_INPUTS:
            continue
        for label, outputs in variants:
            same = [o for o in outputs if o["name"].upper() == e["name"].upper() and o["index"] == e["index"]]
            if not same:
                errors.append("input %s: the %s vertex shader does not write %s%d"
                              % (_element(e), label, e["name"], e["index"]))
                continue
            o = same[0]
            if o["register"] != e["register"]:
                errors.append("input %s: the %s vertex shader writes it at register %d; declare the pixel shader's "
                              "inputs in the same order as the vertex shader's outputs" % (_element(e), label,
                                                                                            o["register"]))
            elif not set(e["mask"]) <= set(o["mask"]):
                errors.append("input %s: the %s vertex shader writes only %s" % (_element(e), label, o["mask"]))
            elif o["format"] != e["format"]:
                errors.append("input %s: the %s vertex shader writes it as %s" % (_element(e), label, o["format"]))
    return errors


def _signature_key(rows):
    return [(r["name"].upper(), r["index"], r["mask"], r["register"], r["sysvalue"], r["format"]) for r in rows]


def validate_vs_dump(dump, factory):
    """Errors of an unstripped vertex shader variant's dumpbin against its vertex factory."""
    label, _, _, _, layout, cbuffers = factory
    errors = []
    kind, major, minor = parse_shader_model(dump)
    if kind != "vs":
        errors.append("not a vertex shader (%s_%d_%d)" % (kind, major, minor))
    if (major, minor) > (6, 0):
        errors.append("shader model %d.%d: must be vs_6_0 or lower" % (major, minor))
    inputs = parse_signature(dump, "Input")
    got = [(r["name"].upper(), r["index"], r["mask"], r["register"]) for r in inputs]
    if got != [(n, i, m, r) for n, i, m, r in layout] or any(r["format"] != "float" for r in inputs):
        want = ", ".join("%s%d %s (register %d)" % x for x in layout)
        errors.append("inputs are %s; the %s factory's input layout is exactly %s (struct DebugVertex in "
                      "debugdraw_vs.hlsli%s)" % (", ".join(_element(r) for r in inputs) or "none", label, want,
                                                 ", built with -D DEBUG_FACTORY" if label == "Debug" else ""))
    outputs = parse_signature(dump, "Output")
    if not any(o["name"].upper() == "SV_POSITION" and o["index"] == 0 for o in outputs):
        errors.append("outputs have no SV_Position")
    sizes = parse_cbuffer_sizes(dump)
    allowed = ", ".join("cb%d (%s)" % (r, n) for r, (n, _) in sorted(cbuffers.items()))
    for b in parse_bindings(dump):
        where = "%s (%s %s)" % (b["name"], b["type"], b["bind"])
        if b["class"] != "cb" or b["space"] != 0 or b["register"] not in cbuffers:
            errors.append("%s: the %s vertex shader may bind only %s in space 0" % (where, label, allowed))
            continue
        engine_name, limit = cbuffers[b["register"]]
        size = sizes.get(b["name"])
        if size is not None and size > limit:
            errors.append("%s: %d bytes, but the engine's %s is %d bytes" % (where, size, engine_name, limit))
    return errors, outputs


def _fixed_inputs(inputs):
    """Errors for pixel shader inputs that differ from what the game's debug-draw vertex shader writes."""
    errors = []
    want = "SV_Position (register 0), TEXCOORD0 xyzw (register 1), TEXCOORD1 xy (register 2)"
    expected = {(n.upper(), i): (r, m, what) for n, i, r, m, what in EXPECTED_INPUTS}
    seen = set()
    for e in inputs:
        key = (e["name"].upper(), e["index"])
        label = "%s%s" % (e["name"], e["index"] if e["name"].upper() != "SV_POSITION" else "")
        if key not in expected:
            errors.append(
                "input %s (register %d, %s): the debug-draw vertex shader outputs only %s"
                % (label, e["register"], e["mask"], want)
            )
            continue
        seen.add(key)
        register, mask, what = expected[key]
        if e["register"] != register:
            errors.append("input %s is at register %d, must be %d" % (label, e["register"], register))
        if mask and e["mask"] != mask:
            errors.append("input %s has mask %s, must be %s (%s): declare it as float%d"
                          % (label, e["mask"], mask, what, len(mask)))
        if e["format"] != "float":
            errors.append("input %s is %s, must be float" % (label, e["format"]))
    for n, i, r, m, what in EXPECTED_INPUTS:
        if (n.upper(), i) not in seen:
            errors.append("input %s%s missing: the signature must be exactly %s (struct DebugPixel in "
                          "debugdraw.hlsli)" % (n, i if n != "SV_Position" else "", want))

    return errors


def validate_dump(dump, params=0, vs_outputs=None):
    """Errors (list of strings) of an unstripped pixel shader's dumpbin against the debug-draw interface.

    params: the number of @param annotations, which sets the largest cb4 allowed (one float4 each).
    vs_outputs: with our own vertex shader, [(variant label, output signature)]; the inputs must then match those
    instead of the game's debug-draw vertex shader."""
    errors = []

    kind, major, minor = parse_shader_model(dump)
    if kind != "ps":
        errors.append("not a pixel shader (%s_%d_%d)" % (kind, major, minor))
    if (major, minor) > (6, 0):
        errors.append("shader model %d.%d: must be ps_6_0 or lower" % (major, minor))

    inputs = parse_signature(dump, "Input")
    if vs_outputs is not None:
        errors += match_ps_inputs(inputs, vs_outputs)
    else:
        errors += _fixed_inputs(inputs)

    outputs = parse_signature(dump, "Output")
    ok = (len(outputs) == 1 and outputs[0]["name"].upper() == "SV_TARGET" and outputs[0]["index"] == 0
          and outputs[0]["mask"] == "xyzw" and outputs[0]["format"] == "float")
    if not ok:
        got = ", ".join("%s%d %s %s" % (o["name"], o["index"], o["mask"], o["format"]) for o in outputs) or "none"
        errors.append("outputs are %s: the only output must be SV_Target0, a float4 (premultiplied colour)" % got)

    sizes = parse_cbuffer_sizes(dump)
    for b in parse_bindings(dump):
        where = "%s (%s %s)" % (b["name"], b["type"], b["bind"])
        if b["class"] is None:
            errors.append("%s: binding not understood" % where)
            continue
        if b["space"] != 0:
            errors.append("%s: register space %d; everything must be in space 0" % (where, b["space"]))
            continue
        if b["count"] not in ("1", ""):
            errors.append("%s: arrays of resources (count %s) are not available" % (where, b["count"]))
        c, r = b["class"], b["register"]
        if c == "cb":
            if b["name"] == "$Globals":
                errors.append("%s: global variables outside a cbuffer land in $Globals, which nothing fills; "
                              "use static const values or the cbuffers in debugdraw.hlsli" % where)
            elif r not in ALLOWED_CBUFFERS:
                errors.append("%s: cb%d is not bound in the debug-draw pass; only %s are" % (where, r, ALLOWED_TEXT))
            elif r == 4 and not params:
                errors.append("%s: cb4 holds material parameters, but the shader has no // @param lines" % where)
            else:
                engine_name, limit = ALLOWED_CBUFFERS[r]
                size = sizes.get(b["name"])
                if r == 4:
                    limit = 16 * params
                    if size is not None and size > limit:
                        errors.append("%s: %d bytes, more than one float4 for each of the %d @param lines (%d bytes)"
                                      % (where, size, params, limit))
                elif size is not None and size > limit:
                    errors.append("%s: %d bytes, but the engine's %s is %d bytes" % (where, size, engine_name, limit))
        elif c == "t":
            if r != 77:
                errors.append("%s: the debug-draw pass binds one texture, t77 (debugTexture in debugdraw.hlsli); "
                              "t%d is not bound" % (where, r))
            elif b["dim"].lower() != "2d":
                errors.append("%s: t77 is a Texture2D, not %s" % (where, b["dim"]))
        elif c == "s":
            if r != 0:
                errors.append("%s: the debug-draw pass binds one sampler, s0 (debugSampler); s%d is not bound"
                              % (where, r))
        else:
            errors.append("%s: UAVs are not available to a debug-draw pixel shader" % where)
    return errors


# ---------------------------------------------------------------------------------------------------------------
# Material parameters and instances (annotations in the HLSL)

PARAM_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# CPU parameter block: each parameter's size and CMaterialParameterInfo type (as in emissive_basic_transparent.mt).
PARAM_KINDS = {"scalar": (4, 5), "color": (4, 2)}
FLOAT_MAX = 3.40282347e+38


def _number(text):
    try:
        v = float(text)
    except ValueError:
        return None
    return v if abs(v) <= FLOAT_MAX else None


def _color(words):
    """[r, g, b, a] from 3 or 4 integer strings 0..255 (alpha defaults to 255), or None."""
    if len(words) not in (3, 4) or not all(re.match(r"^\d+$", w) for w in words):
        return None
    rgba = [int(w) for w in words] + ([255] if len(words) == 3 else [])
    return rgba if all(0 <= c <= 255 for c in rgba) else None


def parse_annotations(text):
    """(params, instances, errors) from the // @param and // @instance lines of a shader's source.

    params: [{"name", "kind", "value", "register"}] in declaration order (register = index).
    instances: [{"name", "values": [(param name, value)]}]."""
    params, instances, errors = [], [], []
    for number, line in enumerate(text.splitlines(), 1):
        m = re.match(r"^\s*//\s*@(\w+)\b(.*)$", line)
        if not m:
            continue
        kind, words = m.group(1), m.group(2).split()
        where = "line %d" % number
        if kind == "param":
            if len(words) < 2:
                errors.append("%s: expected // @param <Name> scalar <default> or // @param <Name> color <r> <g> <b> [<a>]"
                              % where)
                continue
            name, ptype, rest = words[0], words[1].lower(), words[2:]
            if not PARAM_NAME_RE.match(name):
                errors.append("%s: parameter name %r must be [A-Za-z_][A-Za-z0-9_]*" % (where, name))
                continue
            if any(p["name"] == name for p in params):
                errors.append("%s: parameter %s is declared twice" % (where, name))
                continue
            if ptype == "scalar":
                value = _number(rest[0]) if len(rest) == 1 else None
                if value is None:
                    errors.append("%s: %s scalar needs one default value (a float)" % (where, name))
                    continue
            elif ptype == "color":
                value = _color(rest)
                if value is None:
                    errors.append("%s: %s color needs a default <r> <g> <b> [<a>], integers 0..255" % (where, name))
                    continue
            else:
                errors.append("%s: parameter type %r is not supported; only scalar and color (textures and other "
                              "types are not handled yet)" % (where, words[1]))
                continue
            params.append({"name": name, "kind": ptype, "value": value, "register": len(params)})
        elif kind == "instance":
            if not words:
                errors.append("%s: expected // @instance <name> <Param>=<value> ..." % where)
                continue
            instances.append({"name": words[0], "line": number, "assignments": words[1:]})
        elif kind == "vertex":
            pass  # parse_vertex
        else:
            errors.append("%s: unknown annotation @%s (expected @param, @instance or @vertex)" % (where, kind))
    by_name = {p["name"]: p for p in params}
    checked = []
    for inst in instances:
        where = "line %d" % inst["line"]
        if not NAME_RE.match(inst["name"]):
            errors.append("%s: instance name %r must be [a-z0-9_]" % (where, inst["name"]))
            continue
        if any(i["name"] == inst["name"] for i in checked):
            errors.append("%s: instance %s is declared twice" % (where, inst["name"]))
            continue
        if not inst["assignments"]:
            errors.append("%s: instance %s sets no values" % (where, inst["name"]))
            continue
        values, ok = [], True
        for a in inst["assignments"]:
            name, sep, text = a.partition("=")
            param = by_name.get(name)
            if not sep or not text:
                errors.append("%s: %r: expected <Param>=<value>" % (where, a))
                ok = False
            elif param is None:
                errors.append("%s: instance %s sets %s, which is not a @param (%s)"
                              % (where, inst["name"], name, ", ".join(by_name) or "none declared"))
                ok = False
            elif any(n == name for n, _ in values):
                errors.append("%s: instance %s sets %s twice" % (where, inst["name"], name))
                ok = False
            else:
                value = _number(text) if param["kind"] == "scalar" else _color(text.split(","))
                if value is None:
                    errors.append("%s: instance %s: %s is a %s, %r is not one (%s)"
                                  % (where, inst["name"], name, param["kind"], text,
                                     "a float" if param["kind"] == "scalar" else "r,g,b[,a] integers 0..255"))
                    ok = False
                else:
                    values.append((name, value))
        if ok:
            checked.append({"name": inst["name"], "values": values})
    return params, checked, errors


def parse_vertex(text):
    """(path, line, errors) of the // @vertex <file.hlsl> line, or (None, None, errors) without one."""
    found, errors = [], []
    for number, line in enumerate(text.splitlines(), 1):
        m = re.match(r"^\s*//\s*@vertex\b(.*)$", line)
        if m:
            words = m.group(1).split()
            if len(words) != 1:
                errors.append("line %d: expected // @vertex <file.hlsl> (one path, relative to this file)" % number)
            else:
                found.append((words[0], number))
    if len(found) > 1:
        errors.append("line %d: a second @vertex line (the first is line %d)" % (found[1][1], found[0][1]))
    return (found[0] if found else (None, None)) + (errors,)


def declares_b4(text):
    """Whether the source (comments removed) binds something to register b4."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.search(r"register\s*\(\s*b4\s*[,)]", text, re.I) is not None


def parameter_block(params):
    """(infos, size): the CPU parameter block, each parameter packed in declaration order."""
    infos, offset = [], 0
    for p in params:
        size, ptype = PARAM_KINDS[p["kind"]]
        infos.append({"name": p["name"], "offset": offset, "type": ptype})
        offset += size
    return infos, offset


def cname(value):
    return {"$type": "CName", "$storage": "string", "$value": value}


def color_json(rgba):
    return {"$type": "Color", "Alpha": rgba[3], "Blue": rgba[2], "Green": rgba[1], "Red": rgba[0]}


def scalar_json(value):
    return int(value) if float(value).is_integer() and abs(value) < 2 ** 31 else value


def check_dxbc(blob):
    """Errors in a shipped blob: DXBC container, signed (non-zero hash)."""
    if len(blob) < 32 or blob[:4] != b"DXBC":
        return ["the compiled blob is not a DXBC container"]
    if blob[4:20] == b"\0" * 16:
        return ["the compiled blob is unsigned (dxil.dll missing beside dxc.exe?); D3D12 will reject it"]
    return []


# ---------------------------------------------------------------------------------------------------------------
# Tools


def _version_key(path):
    m = re.search(r"\\bin\\(\d+(?:\.\d+)*)\\", path)
    return tuple(int(x) for x in m.group(1).split(".")) if m else ()


def find_dxc(override):
    if override:
        if not os.path.isfile(override):
            raise ToolError("dxc not found: %s" % override)
        return override
    roots = {os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), r"C:\Program Files (x86)"}
    found = []
    for root in roots:
        found += glob.glob(os.path.join(root, "Windows Kits", "10", "bin", "*", "x64", "dxc.exe"))
    if not found:
        raise ToolError("dxc.exe not found under Windows Kits\\10\\bin\\*\\x64; install the Windows SDK or pass --dxc")
    return sorted(set(found), key=_version_key, reverse=True)[0]


def run(cmd, timeout=900):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           timeout=timeout)
    except FileNotFoundError:
        raise ToolError("cannot run %s" % cmd[0])
    except subprocess.TimeoutExpired:
        raise ToolError("%s timed out" % os.path.basename(cmd[0]))
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


class Effect:
    def __init__(self, source, name, mod):
        self.source = source
        self.name = name
        self.template = "%s_%s" % (mod, name)
        self.fold = name_fold(self.template)
        self.folder = "debug_draw\\shaders\\%s" % mod
        self.depot = "%s\\%s.mt" % (self.folder, name)
        self.blob = None
        self.params = []
        self.instances = []
        self.vertex = None       # path of the @vertex shader
        self.vertex_line = None
        self.vs_blobs = {}       # factory file suffix -> stripped bytecode
        self.errors = []
        self.warnings = []

    def vs_file(self, suffix):
        return "%s.%s.vs.dxbc" % (self.name, suffix)

    def instance_depot(self, instance):
        return "%s\\%s_%s.mi" % (self.folder, self.name, instance["name"])


def read_annotations(effect):
    with open(effect.source, encoding="utf-8", errors="replace") as f:
        text = f.read()
    effect.params, effect.instances, errors = parse_annotations(text)
    effect.errors += errors
    vertex, effect.vertex_line, vertex_errors = parse_vertex(text)
    effect.errors += vertex_errors
    if vertex:
        effect.vertex = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(effect.source)), vertex))
        if not os.path.isfile(effect.vertex):
            effect.errors.append("line %d: @vertex %s: no such file (%s)" % (effect.vertex_line, vertex, effect.vertex))
            effect.vertex = None
    b4 = declares_b4(text)
    if effect.params and not b4:
        effect.errors.append("the shader has @param lines but declares no cbuffer at register(b4) (one float4 per "
                             "parameter, in @param order)")
    elif b4 and not effect.params and not errors:
        effect.errors.append("the shader declares register(b4) but has no // @param lines to fill it")


def _build(dxc, profile, source, defines, base):
    """(stripped blob, dumpbin of an unstripped build, errors)."""
    includes = ["-I", os.path.dirname(os.path.abspath(source)), "-I", BUNDLED_INCLUDE]
    common = [dxc, "-nologo", "-T", profile, "-E", "main", "-O3"] + defines + includes
    rc, out = run(common + ["-Qstrip_reflect", "-Qstrip_debug", "-Fo", base + ".dxbc", source])
    if rc != 0:
        return None, None, ["dxc failed:\n" + out.strip()]
    with open(base + ".dxbc", "rb") as f:
        blob = f.read()
    errors = check_dxbc(blob)
    rc, out = run(common + ["-Fo", base + ".full.dxil", source])
    if rc != 0:
        return blob, None, errors + ["dxc (unstripped build) failed:\n" + out.strip()]
    rc, dump = run([dxc, "-dumpbin", base + ".full.dxil"])
    if rc != 0:
        return blob, None, errors + ["dxc -dumpbin failed:\n" + dump.strip()]
    with open(base + ".dumpbin.txt", "w", encoding="utf-8") as f:
        f.write(dump)
    return blob, dump, errors


def compile_vertex(dxc, effect, work):
    """Builds and checks both vertex factory variants of the @vertex shader: [(label, outputs)] for the PS check."""
    variants, failed = [], False
    try:
        shown = os.path.relpath(effect.vertex)
    except ValueError:  # another drive
        shown = effect.vertex
    where = "%s (@vertex, line %d)" % (shown, effect.vertex_line)
    for factory in VERTEX_FACTORIES:
        label, suffix, defines = factory[0], factory[1], factory[2]
        blob, dump, errors = _build(dxc, "vs_6_0", effect.vertex, defines,
                                    os.path.join(work, "%s.%s.vs" % (effect.name, suffix)))
        outputs = None
        if dump is not None:
            try:
                more, outputs = validate_vs_dump(dump, factory)
                errors += more
            except ToolError as e:
                errors.append(str(e))
        for err in errors:
            effect.errors.append("%s, %s variant: %s" % (where, label, err))
        if outputs is not None:
            variants.append((label, outputs))  # the PS is checked against what each variant writes
        if errors or outputs is None:
            continue
        effect.vs_blobs[suffix] = blob
    if len(variants) == 2 and _signature_key(variants[0][1]) != _signature_key(variants[1][1]):
        effect.errors.append("%s: the %s and %s variants write different outputs (%s / %s); both factories share one "
                             "pixel shader, so they must be the same"
                             % (where, variants[0][0], variants[1][0], ", ".join(_element(o) for o in variants[0][1]),
                                ", ".join(_element(o) for o in variants[1][1])))
    return variants


def compile_effect(dxc, effect, work):
    read_annotations(effect)
    vs_outputs = compile_vertex(dxc, effect, work) if effect.vertex else None
    base = os.path.join(work, effect.name)
    includes = ["-I", os.path.dirname(os.path.abspath(effect.source)), "-I", BUNDLED_INCLUDE]
    common = [dxc, "-nologo", "-T", PROFILE, "-E", "main", "-O3"] + includes
    rc, out = run(common + ["-Qstrip_reflect", "-Qstrip_debug", "-Fo", base + ".dxbc", effect.source])
    if rc != 0:
        effect.errors.append("dxc failed:\n" + out.strip())
        return
    if out.strip():
        effect.warnings.append(out.strip())
    with open(base + ".dxbc", "rb") as f:
        effect.blob = f.read()
    effect.errors += check_dxbc(effect.blob)
    rc, out = run(common + ["-Fo", base + ".full.dxil", effect.source])
    if rc != 0:
        effect.errors.append("dxc (unstripped build) failed:\n" + out.strip())
        return
    rc, dump = run([dxc, "-dumpbin", base + ".full.dxil"])
    if rc != 0:
        effect.errors.append("dxc -dumpbin failed:\n" + dump.strip())
        return
    with open(base + ".dumpbin.txt", "w", encoding="utf-8") as f:
        f.write(dump)
    try:
        effect.errors += validate_dump(dump, len(effect.params), vs_outputs)
    except ToolError as e:
        effect.errors.append(str(e))


class WolvenKit:
    def __init__(self, exe):
        if not os.path.isfile(exe):
            raise ToolError("WolvenKit CLI not found: %s (pass --wolvenkit)" % exe)
        if not os.path.isfile(os.path.join(os.path.dirname(exe), "oo2ext_7_win64.dll")):
            raise ToolError("oo2ext_7_win64.dll is not beside %s: copy it from <game>\\bin\\x64 (WolvenKit needs it "
                            "to pack archives)" % exe)
        self.exe = exe

    def __call__(self, *args):
        rc, out = run([self.exe] + list(args))
        if rc != 0 or re.search(r"\[\s*\d+:\s*(Error|Critical)", out):
            raise ToolError("WolvenKit %s failed (exit %d):\n%s" % (args[0], rc, out.strip()[-3000:]))
        return out


def depot_lines(out):
    return [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith("[")
            and not re.match(r"^\d+%$", l.strip())]


def game_stamp(dirs):
    h = hashlib.sha1()
    for d in dirs:
        for p in sorted(glob.glob(os.path.join(d, "*.archive"))):
            st = os.stat(p)
            h.update(("%s|%d|%d\n" % (os.path.abspath(p).lower(), st.st_size, int(st.st_mtime))).encode())
    return h.hexdigest()


def cached(work, name, stamp, make):
    """Text file <work>/<name>, rebuilt by make() when the game's archives changed."""
    path = os.path.join(work, name)
    stamp_path = path + ".stamp"
    if os.path.isfile(path) and os.path.isfile(stamp_path):
        with open(stamp_path, encoding="utf-8") as f:
            if f.read() == stamp:
                with open(path, encoding="utf-8") as g:
                    return g.read()
    text = make()
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    with open(stamp_path, "w", encoding="utf-8") as f:
        f.write(stamp)
    return text


def vanilla_templates(wk, archive_dirs, work, stamp):
    def make():
        out = wk("archive", *archive_dirs, "-l", "-r", r"\.(mt|remt)$")
        paths = sorted({l for l in depot_lines(out) if re.search(r"\.(mt|remt)$", l, re.I)})
        if len(paths) < 100:
            raise ToolError("the game's archives list only %d material templates; is the game dir right?" % len(paths))
        return "\n".join(paths) + "\n"
    return cached(work, "vanilla_templates.txt", stamp, make).split()


def donor_json(wk, content, work, stamp):
    def make():
        tmp = os.path.join(work, "donor")
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(os.path.join(tmp, "json"))
        wk("unbundle", content, "-o", tmp, "-r", "^" + re.escape(DONOR_DEPOT) + "$")
        remt = os.path.join(tmp, DONOR_DEPOT)
        if not os.path.isfile(remt):
            raise ToolError("could not extract %s from %s" % (DONOR_DEPOT, content))
        wk("cr2w", "-s", remt, "-o", os.path.join(tmp, "json"))
        js = os.path.join(tmp, "json", os.path.basename(DONOR_DEPOT) + ".json")
        if not os.path.isfile(js):
            raise ToolError("WolvenKit did not serialize %s" % DONOR_DEPOT)
        with open(js, encoding="utf-8") as f:
            return f.read()
    return json.loads(cached(work, "debugdraw.remt.json", stamp, make))


def template_json(donor, name, params=()):
    """The donor's JSON with a new name and, if any, the parameters (each a float4 row of cb4 at its register)."""
    doc = copy.deepcopy(donor)
    doc["Header"]["ArchiveFileName"] = ""
    root = doc["Data"]["RootChunk"]
    if root.get("$type") != "CMaterialTemplate":
        raise ToolError("the donor is a %s, not a CMaterialTemplate" % root.get("$type"))
    root["name"] = cname(name)
    if params:
        handles = []
        for n, p in enumerate(params):
            if p["kind"] == "scalar":
                data = {"$type": "CMaterialParameterScalar", "max": FLOAT_MAX, "min": -FLOAT_MAX,
                        "parameterName": cname(p["name"]), "register": p["register"], "scalar": scalar_json(p["value"])}
            else:
                data = {"$type": "CMaterialParameterColor", "color": color_json(p["value"]),
                        "parameterName": cname(p["name"]), "register": p["register"]}
            handles.append({"HandleId": str(n), "Data": data})
        infos, size = parameter_block(params)
        root["parameters"]["Elements"][2] = handles
        root["usedParameters"]["Elements"][2] = [
            {"$type": "MaterialUsedParameter", "name": cname(p["name"]), "register": p["register"]} for p in params]
        root["paramBlockSize"]["Elements"][2] = size
        root["parameterInfo"][1] = [
            {"$type": "CMaterialParameterInfo", "name": cname(i["name"]), "offset": i["offset"], "type": i["type"]}
            for i in infos]
    return doc


def instance_json(header, effect, instance):
    """A CMaterialInstance on the effect's .mt that sets only the instance's values."""
    kinds = {p["name"]: p["kind"] for p in effect.params}
    values = []
    for name, value in instance["values"]:
        if kinds[name] == "scalar":
            values.append({"$type": "Float", name: float(value)})
        else:
            values.append({"$type": "Color", name: color_json(value)})
    head = copy.deepcopy(header)
    head["ArchiveFileName"] = ""
    return {"Header": head, "Data": {"Version": 195, "BuildVersion": 0, "RootChunk": {
        "$type": "CMaterialInstance",
        "audioTag": cname("None"),
        "baseMaterial": {"DepotPath": {"$type": "ResourcePath", "$storage": "string", "$value": effect.depot},
                         "Flags": "Default"},
        "cookingPlatform": "PLATFORM_PC",
        "enableMask": 0,
        "metadata": None,
        "resourceVersion": 4,
        "values": values}, "EmbeddedFiles": []}}


def _same(a, b):
    return abs(float(a) - float(b)) <= 1e-6 * max(1.0, abs(float(b)))


def template_problems(doc, effect):
    """Differences between a round-tripped .mt JSON and what the effect asked for."""
    root = doc["Data"]["RootChunk"]
    problems = []
    if root["name"]["$value"] != effect.template:
        problems.append("name %r, expected %r" % (root["name"]["$value"], effect.template))
    if not effect.params:
        return problems
    got = [h["Data"] for h in root["parameters"]["Elements"][2]]
    if len(got) != len(effect.params):
        return problems + ["%d parameters, expected %d" % (len(got), len(effect.params))]
    for g, p in zip(got, effect.params):
        if g["parameterName"]["$value"] != p["name"] or g["register"] != p["register"]:
            problems.append("parameter %s/%s, expected %s/%s" % (g["parameterName"]["$value"], g["register"],
                                                                  p["name"], p["register"]))
        elif p["kind"] == "scalar" and not (g["$type"] == "CMaterialParameterScalar" and _same(g["scalar"], p["value"])):
            problems.append("parameter %s: %s %s, expected scalar %s" % (p["name"], g["$type"], g.get("scalar"),
                                                                         p["value"]))
        elif p["kind"] == "color" and not (g["$type"] == "CMaterialParameterColor"
                                           and g["color"] == color_json(p["value"])):
            problems.append("parameter %s: %s %s, expected colour %s" % (p["name"], g["$type"], g.get("color"),
                                                                         p["value"]))
    used = [(u["name"]["$value"], u["register"]) for u in root["usedParameters"]["Elements"][2]]
    if used != [(p["name"], p["register"]) for p in effect.params]:
        problems.append("usedParameters %s" % used)
    infos, size = parameter_block(effect.params)
    if root["paramBlockSize"]["Elements"][2] != size:
        problems.append("paramBlockSize %s, expected %d" % (root["paramBlockSize"]["Elements"][2], size))
    got_infos = [{"name": i["name"]["$value"], "offset": i["offset"], "type": i["type"]} for i in root["parameterInfo"][1]]
    if got_infos != infos:
        problems.append("parameterInfo %s, expected %s" % (got_infos, infos))
    return problems


def instance_problems(doc, effect, instance):
    root = doc["Data"]["RootChunk"]
    problems = []
    if root["baseMaterial"]["DepotPath"]["$value"] != effect.depot:
        problems.append("baseMaterial %r, expected %r" % (root["baseMaterial"]["DepotPath"]["$value"], effect.depot))
    kinds = {p["name"]: p["kind"] for p in effect.params}
    got = []
    for v in root["values"]:
        names = [k for k in v if k != "$type"]
        got.append((v["$type"], names[0] if len(names) == 1 else names, v[names[0]] if len(names) == 1 else None))
    want = [("Float" if kinds[n] == "scalar" else "Color", n, value) for n, value in instance["values"]]
    ok = len(got) == len(want) and all(
        gt == wt and gn == wn and (_same(gv, wv) if wt == "Float" else gv == color_json(wv))
        for (gt, gn, gv), (wt, wn, wv) in zip(got, want))
    if not ok:
        problems.append("values %s, expected %s" % (got, want))
    return problems


def name_problems(effects, vanilla_paths):
    """Errors per effect for template names that clash with vanilla templates or with each other."""
    stems = {}
    for p in vanilla_paths:
        stem = re.sub(r"\.(mt|remt)$", "", p.replace("/", "\\").split("\\")[-1], flags=re.I).lower()
        stems.setdefault(stem, p)
    folds = {}
    for stem in list(stems) + list(EXTRA_VANILLA_NAMES):
        folds.setdefault(name_fold(stem), stem)
    for e in effects:
        if e.template in stems:
            e.errors.append("template name %s is taken by vanilla %s" % (e.template, stems[e.template]))
        elif e.fold in folds:
            e.errors.append("template name %s folds to %08X like vanilla template %s; rename the effect"
                            % (e.template, e.fold, folds[e.fold]))
    mine = {}
    for e in effects:
        other = mine.setdefault(e.fold, e)
        if other is not e:
            e.errors.append("template name %s folds to %08X like %s; rename one" % (e.template, e.fold, other.template))
    files = {}
    for e in effects:
        for i in e.instances:
            path = e.instance_depot(i)
            other = files.setdefault(path, e)
            if other is not e:
                e.errors.append("instance file %s is also made by effect %s; rename one" % (path, other.name))


# ---------------------------------------------------------------------------------------------------------------
# Packaging


def package(wk, effects, mod, out, work, donor):
    shader_dir = os.path.join(out, PLUGIN_SHADERS, mod)
    archive_dir = os.path.join(out, "archive", "pc", "mod")
    archive = os.path.join(archive_dir, mod + "_shaders.archive")
    staging = os.path.join(work, mod + "_shaders")
    mt_dir = os.path.join(staging, "debug_draw", "shaders", mod)
    json_dir = os.path.join(work, "mt_json")
    check_dir = os.path.join(work, "mt_check")
    for d in (shader_dir, staging, json_dir, check_dir):
        shutil.rmtree(d, ignore_errors=True)
    for d in (shader_dir, archive_dir, mt_dir, json_dir, check_dir):
        os.makedirs(d, exist_ok=True)
    if os.path.exists(archive):
        os.remove(archive)

    # Blobs and the manifest.
    for e in effects:
        with open(os.path.join(shader_dir, e.name + ".dxbc"), "wb") as f:
            f.write(e.blob)
        for suffix, blob in e.vs_blobs.items():
            with open(os.path.join(shader_dir, e.vs_file(suffix)), "wb") as f:
                f.write(blob)
    with open(os.path.join(shader_dir, mod + ".txt"), "w", encoding="ascii", newline="\n") as f:
        f.write("# %s shaders for Debug Draw, made by tools/shaderc. material <template name> <donor> <pixel shader>\n"
                % mod)
        for e in effects:
            f.write("material %s %s %s.dxbc\n" % (e.template, DONOR, e.name))
            if e.vertex:
                for factory in VERTEX_FACTORIES:
                    f.write("vertex %s %s %s\n" % (e.template, factory[3], e.vs_file(factory[1])))

    # Material templates, checked by a round trip.
    files = []  # (file name in the depot folder, JSON, check)
    for e in effects:
        files.append((e.name + ".mt", template_json(donor, e.template, e.params),
                      lambda doc, e=e: template_problems(doc, e)))
        for i in e.instances:
            files.append((os.path.basename(e.instance_depot(i)), instance_json(donor["Header"], e, i),
                          lambda doc, e=e, i=i: instance_problems(doc, e, i)))
    for name, doc, _ in files:
        with open(os.path.join(json_dir, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2)
    wk("cr2w", "-d", json_dir, "-o", mt_dir)
    for name, _, _ in files:
        if not os.path.isfile(os.path.join(mt_dir, name)):
            raise ToolError("WolvenKit did not write %s" % name)
    wk("cr2w", "-s", mt_dir, "-o", check_dir)
    for name, _, check in files:
        path = os.path.join(check_dir, name + ".json")
        if not os.path.isfile(path):
            raise ToolError("round trip of %s produced no JSON" % name)
        with open(path, encoding="utf-8") as f:
            problems = check(json.load(f))
        if problems:
            raise ToolError("%s round trip: %s" % (name, "; ".join(problems)))

    # Archive.
    wk("pack", staging, "-o", archive_dir)
    if not os.path.isfile(archive):
        made = [p for p in glob.glob(os.path.join(archive_dir, "*.archive"))]
        raise ToolError("WolvenKit pack did not write %s (found: %s)" % (archive, ", ".join(made) or "nothing"))
    listed = {l.lower() for l in depot_lines(wk("archive", archive, "-l"))}
    missing = [d for e in effects for d in [e.depot] + [e.instance_depot(i) for i in e.instances]
               if d.lower() not in listed]
    if missing:
        raise ToolError("%s lacks %s" % (archive, ", ".join(missing)))

    with open(os.path.join(out, "readme.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("%s shaders for Debug Draw (made by Debug Draw's tools/shaderc)\n\n" % mod)
        f.write("Copy red4ext\\ and archive\\ into the Cyberpunk 2077 folder. Debug Draw's RED4ext plugin must be\n"
                "installed: it reads red4ext\\plugins\\debug_draw\\shaders\\%s\\%s.txt and serves the pixel shaders.\n\n"
                % (mod, mod))
        f.write("Use an effect by giving a channel its material, then drawing in that channel:\n\n")
        for e in effects:
            f.write('  %-20s DebugDraw.SetMaterial(n"channel", "%s")\n' % (e.name, e.depot.replace("\\", "\\\\")))
            for i in e.instances:
                f.write('  %-20s DebugDraw.SetMaterial(n"channel", "%s")\n'
                        % ("%s %s" % (e.name, i["name"]), e.instance_depot(i).replace("\\", "\\\\")))
        if any(e.vertex for e in effects):
            f.write("\nEffects with their own vertex shader (one build per debug-draw vertex factory, DrawBuffer and\n"
                    "Debug; the plugin swaps in each where the donor uses that factory's vertex shader):\n\n")
            for e in effects:
                if e.vertex:
                    f.write("  %-20s %s, %s\n" % (e.name, e.vs_file("drawbuffer"), e.vs_file("debug")))
        if any(e.params for e in effects):
            f.write("\nParameters (defaults in the .mt; an instance .mi overrides some of them):\n\n")
            for e in effects:
                for p in e.params:
                    f.write("  %-20s %s %s\n" % (e.name, p["name"], p["kind"]))
    return archive, sorted(listed)


# ---------------------------------------------------------------------------------------------------------------
# CLI


def parse_effects(args, mod):
    effects = []
    for arg in args:
        m = re.match(r"^([A-Za-z0-9_]+)=(.+)$", arg)
        name, path = (m.group(1), m.group(2)) if m else (None, arg)
        if not os.path.isfile(path):
            raise ToolError("no such shader: %s" % path)
        name = (name or os.path.splitext(os.path.basename(path))[0]).lower()
        if not NAME_RE.match(name):
            raise ToolError("effect name %r (from %s) must be [a-z0-9_]; rename the file or pass name=%s"
                            % (name, path, path))
        if any(e.name == name for e in effects):
            raise ToolError("two effects are called %s" % name)
        effects.append(Effect(path, name, mod))
    return effects


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="shaderc.py",
        description="Compile Debug Draw pixel shaders (HLSL) into a mod folder: blobs, manifest, .mt archive.")
    ap.add_argument("--mod", required=True, help="mod name, [a-z0-9_]; template names are <mod>_<effect>")
    ap.add_argument("--game", help="Cyberpunk 2077 folder (default: CYBERPUNK_2077_GAME_DIR or the Steam path)")
    ap.add_argument("--wolvenkit", help="WolvenKit 10 CLI exe (default: WOLVENKIT_CLI or %s)" % DEFAULT_WOLVENKIT)
    ap.add_argument("--dxc", help="dxc.exe (default: newest Windows Kits\\10\\bin\\*\\x64\\dxc.exe)")
    ap.add_argument("--out", help="output folder, laid out like the game folder (default: tools\\shaderc\\build\\<mod>);"
                                  " work files go to <out>.work")
    ap.add_argument("--check", action="store_true", help="compile and validate only (no WolvenKit, no output)")
    ap.add_argument("shaders", nargs="+", metavar="effect.hlsl", help="one effect per file; name=file.hlsl renames it")
    a = ap.parse_args(argv)

    try:
        mod = a.mod.lower()
        if not NAME_RE.match(mod):
            raise ToolError("--mod %r must be [a-z0-9_]" % a.mod)
        effects = parse_effects(a.shaders, mod)
        dxc = find_dxc(a.dxc)
        print("dxc: %s" % dxc)

        out = os.path.abspath(a.out or os.path.join(TOOL_DIR, "build", mod))
        temp = None
        if a.check:
            temp = tempfile.TemporaryDirectory(prefix="shaderc_")
            work = temp.name
        else:
            work = out.rstrip("\\/") + ".work"
        os.makedirs(os.path.join(work, "blobs"), exist_ok=True)

        for e in effects:
            compile_effect(dxc, e, os.path.join(work, "blobs"))

        if a.check:
            name_problems(effects, [])  # only against each other: the vanilla names need the game
        else:
            game = a.game or os.environ.get("CYBERPUNK_2077_GAME_DIR") or DEFAULT_GAME
            content = os.path.join(game, "archive", "pc", "content")
            if not os.path.isdir(content):
                raise ToolError("no archive\\pc\\content in the game folder %s (pass --game)" % game)
            archive_dirs = [content] + [d for d in [os.path.join(game, "archive", "pc", "ep1")] if os.path.isdir(d)]
            wk = WolvenKit(a.wolvenkit or os.environ.get("WOLVENKIT_CLI") or DEFAULT_WOLVENKIT)
            print("WolvenKit: %s\ngame: %s" % (wk.exe, game))
            stamp = game_stamp(archive_dirs)
            name_problems(effects, vanilla_templates(wk, archive_dirs, work, stamp))

        failed = [e for e in effects if e.errors]
        for e in effects:
            for w in e.warnings:
                print("%s: warning: %s" % (e.source, w))
            for err in e.errors:
                print("%s: error: %s" % (e.source, err))

        archive = listed = None
        if not failed and not a.check:
            donor = donor_json(wk, content, work, stamp)
            archive, listed = package(wk, effects, mod, out, work, donor)

        print("\nSummary (%s, mod %s):" % ("check" if a.check else "build", mod))
        for e in effects:
            size = "%d B" % len(e.blob) if e.blob else "-"
            vs = ("  VS %s" % "/".join("%d B" % len(e.vs_blobs[f[1]]) for f in VERTEX_FACTORIES if f[1] in e.vs_blobs)
                  if e.vertex else "")
            print("  %-4s %-14s %-28s %08X  %8s  %d params, %d instances%s  %s"
                  % ("ok" if not e.errors else "FAIL", e.name, e.template, e.fold, size, len(e.params),
                     len(e.instances), vs, e.source))
        if failed:
            print("%d of %d effects failed%s" % (len(failed), len(effects), "" if a.check else "; nothing packaged"))
            return 1
        if archive:
            print("Output: %s\n  %s (%d files)" % (out, archive, len(listed)))
            for path in listed:
                print("    " + path)
        if temp:
            temp.cleanup()
        return 0
    except ToolError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
