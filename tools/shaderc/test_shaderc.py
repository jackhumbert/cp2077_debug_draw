"""Unit tests for the pure-Python parts of shaderc. Run from tools\\shaderc: python -I -m unittest"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderc  # noqa: E402

# Captured from dxc -dumpbin of tools\shaders\pulse.hlsl (10.0.26100.0, ps_6_0), header and metadata only.
PULSE_DUMP = r"""
;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Position              0   xyzw        0      POS   float    y
; TEXCOORD                 0   xyzw        1     NONE   float   xyzw
; TEXCOORD                 1   xy          2     NONE   float   xy
;
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Target                0   xyzw        0   TARGET   float   xyzw
;
; Input signature:
;
; Name                 Index             InterpMode DynIdx
; -------------------- ----- ---------------------- ------
; SV_Position              0          noperspective
; TEXCOORD                 0                 linear
; TEXCOORD                 1                 linear
;
; Buffer Definitions:
;
; cbuffer GlobalShaderConsts
; {
;
;   struct GlobalShaderConsts
;   {
;
;       float4 globalTime;                            ; Offset:    0
;       float4 globalUnused1;                         ; Offset:   16
;       float4 globalUnused2;                         ; Offset:   32
;       float4 renderTarget;                          ; Offset:   48
;
;   } GlobalShaderConsts;                             ; Offset:    0 Size:    64
;
; }
;
; cbuffer MaterialModifiersConsts
; {
;
;   struct MaterialModifiersConsts
;   {
;
;       float4 materialModifiers[32];                 ; Offset:    0
;
;   } MaterialModifiersConsts;                        ; Offset:    0 Size:   512
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; GlobalShaderConsts                cbuffer      NA          NA     CB0            cb0     1
; MaterialModifiersConsts           cbuffer      NA          NA     CB1            cb7     1
; debugSampler                      sampler      NA          NA      S0             s0     1
; debugTexture                      texture     f32          2d      T0            t77     1
;
;
target triple = "dxil-ms-dx"
!dx.shaderModel = !{!3}
!3 = !{!"ps", i32 6, i32 0}
"""


def edit(text, old, new):
    assert old in text, old
    return text.replace(old, new, 1)


class HashTests(unittest.TestCase):
    def test_fnv1a64(self):
        self.assertEqual(shaderc.fnv1a64(""), 0xCBF29CE484222325)
        self.assertEqual(shaderc.fnv1a64("a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(shaderc.fnv1a64("foobar"), 0x85944171F73967E8)

    def test_fold(self):
        self.assertEqual(shaderc.fold(0x1234567800000000), 0x12345678)
        self.assertEqual(shaderc.fold(0xFFFFFFFF00000001), 0xFFFFFFFE)

    def test_known_template_folds(self):
        # From docs/shader_research.md: renamed templates and their folded names.
        self.assertEqual(shaderc.name_fold("silverhand_overlay"), 0xD218B115)
        self.assertEqual(shaderc.name_fold("water_test"), 0x7CD1600C)


class ParseTests(unittest.TestCase):
    def test_input_signature(self):
        rows = shaderc.parse_signature(PULSE_DUMP, "Input")
        self.assertEqual([(r["name"], r["index"], r["mask"], r["register"]) for r in rows],
                         [("SV_Position", 0, "xyzw", 0), ("TEXCOORD", 0, "xyzw", 1), ("TEXCOORD", 1, "xy", 2)])
        self.assertEqual(rows[0]["used"], "y")
        self.assertEqual(rows[0]["sysvalue"], "POS")

    def test_output_signature(self):
        rows = shaderc.parse_signature(PULSE_DUMP, "Output")
        self.assertEqual(len(rows), 1)
        self.assertEqual((rows[0]["name"], rows[0]["index"], rows[0]["mask"]), ("SV_Target", 0, "xyzw"))

    def test_bindings(self):
        b = shaderc.parse_bindings(PULSE_DUMP)
        self.assertEqual([(x["name"], x["class"], x["register"], x["space"]) for x in b],
                         [("GlobalShaderConsts", "cb", 0, 0), ("MaterialModifiersConsts", "cb", 7, 0),
                          ("debugSampler", "s", 0, 0), ("debugTexture", "t", 77, 0)])

    def test_cbuffer_sizes_and_model(self):
        self.assertEqual(shaderc.parse_cbuffer_sizes(PULSE_DUMP),
                         {"GlobalShaderConsts": 64, "MaterialModifiersConsts": 512})
        self.assertEqual(shaderc.parse_shader_model(PULSE_DUMP), ("ps", 6, 0))


class ValidateTests(unittest.TestCase):
    def test_pulse_is_valid(self):
        self.assertEqual(shaderc.validate_dump(PULSE_DUMP), [])

    def test_extra_input(self):
        dump = edit(PULSE_DUMP, "2     NONE   float   xy",
                    "2     NONE   float   xy\n; TEXCOORD                 2   xyzw        3     NONE   float   xyzw")
        errors = shaderc.validate_dump(dump)
        self.assertEqual(len(errors), 1)
        self.assertIn("TEXCOORD2", errors[0])

    def test_wrong_output(self):
        dump = edit(PULSE_DUMP, "; SV_Target                0   xyzw        0   TARGET   float   xyzw",
                    "; SV_Target                0   xyz         0   TARGET   float   xyz ")
        self.assertIn("SV_Target0", " ".join(shaderc.validate_dump(dump)))

    def test_bindings_outside_the_interface(self):
        dump = edit(PULSE_DUMP, "; debugTexture                      texture     f32          2d      T0            t77     1",
                    "; debugTexture                      texture     f32          2d      T0            t77     1\n"
                    "; other                             texture     f32          2d      T1             t0     1\n"
                    "; Shared                            cbuffer      NA          NA     CB2           cb12     1\n"
                    "; spaced                            sampler      NA          NA      S1      s0,space1     1")
        errors = shaderc.validate_dump(dump)
        self.assertEqual(len(errors), 3)
        self.assertIn("t0 is not bound", errors[0])
        self.assertIn("cb12 is not bound", errors[1])
        self.assertIn("space 1", errors[2])

    def add_cbuffer(self, name, register, size):
        dump = edit(PULSE_DUMP, "; Resource Bindings:",
                    "; cbuffer %s\n; {\n;   } %s;                        ; Offset:    0 Size:   %d\n; }\n;\n"
                    "; Resource Bindings:" % (name, name, size))
        return edit(dump, "; debugSampler ",
                    "; %-30s    cbuffer      NA          NA     CB2            cb%d     1\n; debugSampler "
                    % (name, register))

    def test_camera_cb1(self):
        self.assertEqual(shaderc.validate_dump(self.add_cbuffer("CameraShaderConsts", 1, 592)), [])
        self.assertIn("852 bytes", shaderc.validate_dump(self.add_cbuffer("CameraShaderConsts", 1, 900))[0])

    def test_parameters_cb4(self):
        dump = self.add_cbuffer("ShaderSpecificConstants", 4, 32)
        self.assertEqual(shaderc.validate_dump(dump, params=2), [])
        self.assertIn("more than one float4", shaderc.validate_dump(dump, params=1)[0])
        self.assertIn("no // @param", shaderc.validate_dump(dump, params=0)[0])


DEMO_SOURCE = """// demo
// @param Speed scalar 1
// @param GlowColor color 60 255 60
// @param Edge color 0 0 0 128
// @instance magenta Speed=3 GlowColor=255,40,255
// @instance slow Speed=-0.5
#include "debugdraw.hlsli"
cbuffer ShaderSpecificConstants : register(b4) { float4 speed; float4 glowColor; float4 edge; };
"""


class AnnotationTests(unittest.TestCase):
    def test_params_and_instances(self):
        params, instances, errors = shaderc.parse_annotations(DEMO_SOURCE)
        self.assertEqual(errors, [])
        self.assertEqual(params, [
            {"name": "Speed", "kind": "scalar", "value": 1.0, "register": 0},
            {"name": "GlowColor", "kind": "color", "value": [60, 255, 60, 255], "register": 1},
            {"name": "Edge", "kind": "color", "value": [0, 0, 0, 128], "register": 2},
        ])
        self.assertEqual(instances, [
            {"name": "magenta", "values": [("Speed", 3.0), ("GlowColor", [255, 40, 255, 255])]},
            {"name": "slow", "values": [("Speed", -0.5)]},
        ])
        self.assertTrue(shaderc.declares_b4(DEMO_SOURCE))
        self.assertFalse(shaderc.declares_b4("// register(b4)\ncbuffer X : register(b7) {};"))

    def test_bad_annotations(self):
        text = "\n".join([
            "// @param Tex texture base\\foo.xbm",
            "// @param Speed scalar fast",
            "// @param Glow color 300 0 0",
            "// @param Glow2 color 1 2",
            "// @param Ok scalar 2",
            "// @param Ok scalar 3",
            "// @instance Bad Ok=1",
            "// @instance one Ok=x Nope=1",
            "// @instance two",
            "// @frobnicate",
        ])
        params, instances, errors = shaderc.parse_annotations(text)
        self.assertEqual([p["name"] for p in params], ["Ok"])
        self.assertEqual(instances, [])
        expect = ["'texture' is not supported", "Speed scalar needs", "Glow color needs", "Glow2 color needs",
                  "Ok is declared twice", "unknown annotation @frobnicate", "'Bad' must be [a-z0-9_]",
                  "'x' is not one", "Nope, which is not a @param", "two sets no values"]
        self.assertEqual(len(errors), len(expect), errors)
        for e, x in zip(errors, expect):
            self.assertIn(x, e)

    def test_parameter_block(self):
        params, _, _ = shaderc.parse_annotations(DEMO_SOURCE)
        infos, size = shaderc.parameter_block(params)
        self.assertEqual(infos, [{"name": "Speed", "offset": 0, "type": 5},
                                 {"name": "GlowColor", "offset": 4, "type": 2},
                                 {"name": "Edge", "offset": 8, "type": 2}])
        self.assertEqual(size, 12)

    def test_template_and_instance_json(self):
        donor = {"Header": {"ArchiveFileName": "x"}, "Data": {"RootChunk": {
            "$type": "CMaterialTemplate", "name": shaderc.cname("debugdraw"),
            "parameters": {"Elements": [[], [], []]}, "usedParameters": {"Elements": [[], [], []]},
            "paramBlockSize": {"Elements": [0, 0, 0]}, "parameterInfo": [[], []]}}}
        effect = shaderc.Effect("demo.hlsl", "demo", "mymod")
        effect.params, effect.instances, _ = shaderc.parse_annotations(DEMO_SOURCE)
        mt = shaderc.template_json(donor, effect.template, effect.params)
        self.assertEqual(shaderc.template_problems(mt, effect), [])
        root = mt["Data"]["RootChunk"]
        self.assertEqual(root["parameters"]["Elements"][2][1]["HandleId"], "1")
        self.assertEqual(root["parameters"]["Elements"][2][0]["Data"]["scalar"], 1)
        self.assertEqual(root["paramBlockSize"]["Elements"], [0, 0, 12])
        self.assertEqual(shaderc.template_json(donor, "plain")["Data"]["RootChunk"]["parameters"],
                         donor["Data"]["RootChunk"]["parameters"])
        mi = shaderc.instance_json(donor["Header"], effect, effect.instances[0])
        self.assertEqual(shaderc.instance_problems(mi, effect, effect.instances[0]), [])
        self.assertEqual(mi["Data"]["RootChunk"]["baseMaterial"]["DepotPath"]["$value"],
                         r"debug_draw\shaders\mymod\demo.mt")
        self.assertEqual(mi["Data"]["RootChunk"]["values"][1],
                         {"$type": "Color", "GlowColor": {"$type": "Color", "Alpha": 255, "Blue": 255, "Green": 40,
                                                          "Red": 255}})
        self.assertEqual(effect.instance_depot(effect.instances[0]), r"debug_draw\shaders\mymod\demo_magenta.mi")
        mi["Data"]["RootChunk"]["values"][0]["Speed"] = 2.0
        self.assertIn("values", shaderc.instance_problems(mi, effect, effect.instances[0])[0])

    def test_shader_model_too_new(self):
        dump = edit(PULSE_DUMP, '!3 = !{!"ps", i32 6, i32 0}', '!3 = !{!"ps", i32 6, i32 6}')
        self.assertIn("ps_6_0 or lower", " ".join(shaderc.validate_dump(dump)))


# Captured from dxc -dumpbin of tools\shaders\lit_vs.hlsl, DrawBuffer variant (no define), header and metadata only.
LIT_VS_DUMP = r"""
;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; POSITION                 0   xyz         0     NONE   float   xyz
; TEXCOORD                 0   xy          1     NONE   float   xy
; COLOR                    0   xyzw        2     NONE   float   xyzw
;
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Position              0   xyzw        0      POS   float   xyzw
; TEXCOORD                 0   xyzw        1     NONE   float   xyzw
; TEXCOORD                 1   xy          2     NONE   float   xy
; SV_ClipDistance          0   x           3  CLIPDST   float   x
; TEXCOORD                 2   xyz         4     NONE   float   xyz
;
; Buffer Definitions:
;
; cbuffer CameraShaderConsts
; {
;   } CameraShaderConsts;                             ; Offset:    0 Size:   816
; }
;
; cbuffer DrawConsts
; {
;   } DrawConsts;                                     ; Offset:    0 Size:    48
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; CameraShaderConsts                cbuffer      NA          NA     CB0            cb1     1
; DrawConsts                        cbuffer      NA          NA     CB1            cb5     1
;
;
target triple = "dxil-ms-dx"
!dx.shaderModel = !{!3}
!3 = !{!"vs", i32 6, i32 0}
"""

DRAWBUFFER, DEBUG = shaderc.VERTEX_FACTORIES


def instanced(dump):
    """The Debug variant's dump of the same shader: the instance transform inputs, and no cb5."""
    dump = dump.replace("; COLOR                    0   xyzw        2     NONE   float   xyzw",
                        "; COLOR                    0   xyzw        2     NONE   float   xyzw\n"
                        + "\n".join("; INSTANCE_TRANSFORM       %d   xyzw        %d     NONE   float   xyzw" % (i, 3 + i)
                                    for i in range(3)), 1)
    return dump.replace("; DrawConsts                        cbuffer      NA          NA     CB1            cb5     1\n",
                        "")


def row(name, index, mask, register, fmt="float"):
    return {"name": name, "index": index, "mask": mask, "register": register, "sysvalue": "NONE", "format": fmt,
            "used": mask}


class VertexTests(unittest.TestCase):
    def test_lit_vs_both_variants(self):
        errors, outputs = shaderc.validate_vs_dump(LIT_VS_DUMP, DRAWBUFFER)
        self.assertEqual(errors, [])
        self.assertEqual([(o["name"], o["index"], o["register"]) for o in outputs],
                         [("SV_Position", 0, 0), ("TEXCOORD", 0, 1), ("TEXCOORD", 1, 2), ("SV_ClipDistance", 0, 3),
                          ("TEXCOORD", 2, 4)])
        errors, debug_outputs = shaderc.validate_vs_dump(instanced(LIT_VS_DUMP), DEBUG)
        self.assertEqual(errors, [])
        self.assertEqual(shaderc._signature_key(outputs), shaderc._signature_key(debug_outputs))

    def test_wrong_factory_inputs_and_bindings(self):
        # The DrawBuffer build checked as the Debug variant: inputs short of the instance transform, and cb5.
        errors, _ = shaderc.validate_vs_dump(LIT_VS_DUMP, DEBUG)
        self.assertEqual(len(errors), 2, errors)
        self.assertIn("Debug factory's input layout is exactly", errors[0])
        self.assertIn("DEBUG_FACTORY", errors[0])
        self.assertIn("cb5", errors[1])
        dump = LIT_VS_DUMP.replace("; COLOR                    0   xyzw ", "; COLOR                    0   xyz  ")
        self.assertIn("DrawBuffer factory's input layout", shaderc.validate_vs_dump(dump, DRAWBUFFER)[0][0])

    def test_no_sv_position(self):
        dump = LIT_VS_DUMP.replace("; SV_Position              0   xyzw        0      POS ",
                                   "; TEXCOORD                 5   xyzw        0     NONE ")
        self.assertIn("no SV_Position", shaderc.validate_vs_dump(dump, DRAWBUFFER)[0][0])

    def test_ps_inputs_against_vs_outputs(self):
        outputs = shaderc.parse_signature(LIT_VS_DUMP, "Output")
        variants = [("DrawBuffer", outputs), ("Debug", outputs)]
        ok = [row("SV_Position", 0, "xyzw", 0), row("TEXCOORD", 0, "xyzw", 1), row("TEXCOORD", 1, "xy", 2),
              row("SV_ClipDistance", 0, "x", 3), row("TEXCOORD", 2, "xyz", 4), row("SV_IsFrontFace", 0, "x", 5, "uint")]
        self.assertEqual(shaderc.match_ps_inputs(ok, variants), [])
        self.assertEqual(shaderc.match_ps_inputs([row("TEXCOORD", 2, "xy", 4)], variants), [])  # fewer components
        errors = shaderc.match_ps_inputs([row("TEXCOORD", 3, "xyz", 4)], variants)
        self.assertEqual(len(errors), 2)
        self.assertIn("DrawBuffer vertex shader does not write TEXCOORD3", errors[0])
        self.assertIn("Debug vertex shader does not write TEXCOORD3", errors[1])
        self.assertIn("at register 4", shaderc.match_ps_inputs([row("TEXCOORD", 2, "xyz", 3)], variants[:1])[0])
        self.assertIn("writes only xyz", shaderc.match_ps_inputs([row("TEXCOORD", 2, "xyzw", 4)], variants[:1])[0])
        self.assertIn("as float", shaderc.match_ps_inputs([row("TEXCOORD", 2, "xyz", 4, "int")], variants[:1])[0])

    def test_ps_with_own_vs_skips_fixed_check(self):
        dump = PULSE_DUMP.replace("; TEXCOORD                 1   xy          2     NONE   float   xy",
                                  "; TEXCOORD                 1   xy          2     NONE   float   xy\n"
                                  "; TEXCOORD                 2   xyz         4     NONE   float   xyz", 1)
        self.assertIn("TEXCOORD2", shaderc.validate_dump(dump)[0])
        outputs = shaderc.parse_signature(LIT_VS_DUMP, "Output")
        self.assertEqual(shaderc.validate_dump(dump, vs_outputs=[("DrawBuffer", outputs), ("Debug", outputs)]), [])

    def test_parse_vertex(self):
        self.assertEqual(shaderc.parse_vertex("// x\n// @vertex lit_vs.hlsl\n"), ("lit_vs.hlsl", 2, []))
        self.assertEqual(shaderc.parse_vertex("float4 main();"), (None, None, []))
        self.assertIn("one path", shaderc.parse_vertex("// @vertex\n")[2][0])
        self.assertIn("second @vertex", shaderc.parse_vertex("// @vertex a.hlsl\n// @vertex b.hlsl\n")[2][0])
        self.assertEqual(shaderc.parse_annotations("// @vertex lit_vs.hlsl\n")[2], [])

    def test_bundled_vs_include_matches(self):
        here = os.path.dirname(os.path.abspath(__file__))
        original = os.path.join(here, "..", "shaders", "debugdraw_vs.hlsli")
        if not os.path.isfile(original):
            self.skipTest("tools/shaders/debugdraw_vs.hlsli not present")
        with open(original, "rb") as f, open(os.path.join(here, "include", "debugdraw_vs.hlsli"), "rb") as g:
            self.assertEqual(f.read(), g.read(), "include/debugdraw_vs.hlsli differs from tools/shaders/debugdraw_vs.hlsli")


class NameTests(unittest.TestCase):
    def test_vanilla_stem_and_self_clash(self):
        a = shaderc.Effect("a.hlsl", "bias", "debugdraw")
        b = shaderc.Effect("b.hlsl", "glow", "mymod")
        shaderc.name_problems([a, b], [r"engine\materials\debugdraw_bias.mt", r"engine\materials\debugdraw.remt"])
        self.assertIn("taken by vanilla", a.errors[0])
        self.assertEqual(b.errors, [])
        self.assertEqual(b.depot, r"debug_draw\shaders\mymod\glow.mt")

    def test_dxbc_check(self):
        self.assertEqual(shaderc.check_dxbc(b"DXBC" + b"\1" * 28), [])
        self.assertIn("unsigned", shaderc.check_dxbc(b"DXBC" + b"\0" * 28)[0])

    def test_bundled_include_matches(self):
        here = os.path.dirname(os.path.abspath(__file__))
        original = os.path.join(here, "..", "shaders", "debugdraw.hlsli")
        if not os.path.isfile(original):
            self.skipTest("tools/shaders/debugdraw.hlsli not present")
        with open(original, "rb") as f, open(os.path.join(here, "include", "debugdraw.hlsli"), "rb") as g:
            self.assertEqual(f.read(), g.read(), "include/debugdraw.hlsli differs from tools/shaders/debugdraw.hlsli")


if __name__ == "__main__":
    unittest.main()
