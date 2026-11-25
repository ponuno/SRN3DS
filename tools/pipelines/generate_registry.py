#!/usr/bin/env python3
"""Generate the C descriptor table from tools/pipelines/registry.json.

The JSON is the editable source of truth for family requirements, accepted
features, passes, reductions, fallbacks and memory notes. Runtime acceptance
still lives in source/d3d9_pipeline_match.c and checks tokens/CTAB/material contracts.

    .venv\\Scripts\\python.exe tools\\pipelines\\generate_registry.py
    .venv\\Scripts\\python.exe tools\\pipelines\\generate_registry.py --check
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_JSON = Path(__file__).with_name("registry.json")
DEFAULT_C = ROOT / "source" / "d3d9_pipeline_registry.c"

FAMILY_ENUMS = {
    "interface_text": "D9_PIPELINE_INTERFACE_TEXT",
    "sky": "D9_PIPELINE_SKY",
    "landscape": "D9_PIPELINE_LANDSCAPE",
    "static_geometry": "D9_PIPELINE_STATIC_GEOMETRY",
    "characters_animation": "D9_PIPELINE_CHARACTERS_ANIMATION",
    "vegetation": "D9_PIPELINE_VEGETATION",
    "water": "D9_PIPELINE_WATER",
    "particles": "D9_PIPELINE_PARTICLES",
}
SUPPORT_ENUMS = {
    "structural": "D9_PIPELINE_SUPPORT_STRUCTURAL",
    "preprocess": "D9_PIPELINE_SUPPORT_PREPROCESS",
    "planned": "D9_PIPELINE_SUPPORT_PLANNED",
}
SOURCE_ENUMS = {
    "fxp_ps3": "D9_PIPELINE_SOURCE_FXP_PS3",
    "fixed_scaleform": "D9_PIPELINE_SOURCE_FIXED_SCALEFORM",
    "fixed_material": "D9_PIPELINE_SOURCE_FIXED_MATERIAL",
}
MATCHER_ENUMS = {
    "none": "D9_PIPELINE_MATCHER_NONE",
    "blit_exact": "D9_PIPELINE_MATCHER_BLIT_EXACT",
    "simple_tev": "D9_PIPELINE_MATCHER_SIMPLE_TEV",
    "menu_light": "D9_PIPELINE_MATCHER_MENU_LIGHT",
    "scaleform_glyph": "D9_PIPELINE_MATCHER_SCALEFORM_GLYPH",
}
MATERIAL_ENUMS = {
    "none": "D9_PIPELINE_MATERIAL_NONE",
    "scaleform_glyph_atlas": "D9_PIPELINE_MATERIAL_SCALEFORM_GLYPH_ATLAS",
    "static_diffuse_only": "D9_PIPELINE_MATERIAL_STATIC_DIFFUSE_ONLY",
    "vegetation_alpha_tested": "D9_PIPELINE_MATERIAL_VEGETATION_ALPHA_TESTED",
    "effect_billboard": "D9_PIPELINE_MATERIAL_EFFECT_BILLBOARD",
}
POLICY_ENUMS = {
    "none": "D9_PIPELINE_CONSTANTS_NONE",
    "forbid": "D9_PIPELINE_CONSTANTS_FORBID",
    "require_any": "D9_PIPELINE_CONSTANTS_REQUIRE_ANY",
    "require_all": "D9_PIPELINE_CONSTANTS_REQUIRE_ALL",
}
MENU_SIGNATURES = {
    "p5_mad_normal": "D9_MENU_LIGHT_P5_MAD_NORMAL",
    "p1200005_add_normal": "D9_MENU_LIGHT_P1200005_ADD_NORMAL",
}
USAGE_BITS = {
    "position": 1 << 0,
    "blendweight": 1 << 1,
    "blendindices": 1 << 2,
    "normal": 1 << 3,
    "psize": 1 << 4,
    "texcoord": 1 << 5,
    "tangent": 1 << 6,
    "binormal": 1 << 7,
    "tessfactor": 1 << 8,
    "positiont": 1 << 9,
    "color": 1 << 10,
    "fog": 1 << 11,
    "depth": 1 << 12,
    "sample": 1 << 13,
}
USAGE_VALUES = {
    "position": 0,
    "blendweight": 1,
    "blendindices": 2,
    "normal": 3,
    "psize": 4,
    "texcoord": 5,
    "tangent": 6,
    "binormal": 7,
    "tessfactor": 8,
    "positiont": 9,
    "color": 10,
    "fog": 11,
    "depth": 12,
    "sample": 13,
}
PRE_BITS = {
    "normal_prebaked": "D9_PIPELINE_PRE_NORMAL_PREBAKED",
    "tangent_basis_prebaked": "D9_PIPELINE_PRE_TANGENT_BASIS_PREBAKED",
    "ambient_emissive_varying": "D9_PIPELINE_PRE_AMBIENT_EMISSIVE_VARYING",
    "material_alpha_c4": "D9_PIPELINE_PRE_MATERIAL_ALPHA_C4",
    "normals_predecoded": "D9_PIPELINE_PRE_NORMALS_PREDECODED",
    "bone_palette_ready": "D9_PIPELINE_PRE_BONE_PALETTE_READY",
    "wind_parameters_ready": "D9_PIPELINE_PRE_WIND_PARAMETERS_READY",
    "reflection_target_ready": "D9_PIPELINE_PRE_REFLECTION_TARGET_READY",
    "refraction_target_ready": "D9_PIPELINE_PRE_REFRACTION_TARGET_READY",
    "particle_batch_ready": "D9_PIPELINE_PRE_PARTICLE_BATCH_READY",
    "sprite_swizzle_baked": "D9_PIPELINE_PRE_SPRITE_SWIZZLE_BAKED",
}


def cstr(value: str) -> str:
    value.encode("ascii")  # keep generated C independent of source encoding
    return json.dumps(value, ensure_ascii=True)


def enum_value(table: dict[str, str], value: str, what: str) -> str:
    try:
        return table[value]
    except KeyError as exc:
        raise ValueError(f"unknown {what}: {value!r}") from exc


def string_array(name: str, values: list[str]) -> str:
    if not values:
        return f"static const char *const {name}[] = {{NULL}};"
    rows = ",\n    ".join(cstr(value) for value in values)
    return f"static const char *const {name}[] = {{\n    {rows},\n    NULL\n}};"


def validate_data(data: dict[str, Any]) -> None:
    if data.get("schema_version") != 1:
        raise ValueError("unsupported registry schema_version")
    families = data.get("families", [])
    pipelines = data.get("pipelines", [])
    if len(families) != 8 or {row["name"] for row in families} != set(FAMILY_ENUMS):
        raise ValueError("registry must define each of the eight required families")
    if not 1 <= len(pipelines) <= 24:
        raise ValueError("descriptor count outside D9_PIPELINE_MAX_DESCRIPTORS")
    ids: set[str] = set()
    family_names = {row["name"] for row in families}
    for row in pipelines:
        pid = row["id"]
        if pid in ids:
            raise ValueError(f"duplicate pipeline id: {pid}")
        ids.add(pid)
        if row["family"] not in family_names:
            raise ValueError(f"{pid}: unknown family")
        enum_value(SUPPORT_ENUMS, row["support"], "support")
        enum_value(SOURCE_ENUMS, row["source"], "source")
        enum_value(MATCHER_ENUMS, row["matcher"], "matcher")
        enum_value(MATERIAL_ENUMS, row["fixed_material"], "material")
        enum_value(POLICY_ENUMS, row["features"]["constant_policy"], "constant policy")
        if not 1 <= len(row["passes"]) <= 6:
            raise ValueError(f"{pid}: pass count outside 1..6")
        if any(key in row for key in ("sha1", "hash", "bytecode_hash")):
            raise ValueError(f"{pid}: hashes cannot be descriptor acceptance data")
        for usage in row["features"]["allowed_input_usages"]:
            enum_value(USAGE_BITS, usage, "input usage")
        for requirement in row["features"]["required_inputs"]:
            enum_value(USAGE_VALUES, requirement["usage"], "required input usage")
        for name in row["preconditions"]:
            enum_value(PRE_BITS, name, "precondition")
        if row["matcher"] == "menu_light":
            enum_value(MENU_SIGNATURES, row["menu_signature"], "menu signature")
        for opcode in row["features"]["allowed_opcodes"]:
            if not 0 <= opcode < 256:
                raise ValueError(f"{pid}: opcode mask overflow")
        for field in (
            "inventory_signature", "accepted_original", "package_rule",
            "vertex_inputs", "texture_units", "quality_reductions",
            "unsupported", "fallback", "memory",
        ):
            if not isinstance(row[field], str) or not row[field]:
                raise ValueError(f"{pid}: missing {field}")
    for row in pipelines:
        fallback = row.get("fallback_id", "")
        if fallback and fallback not in ids:
            raise ValueError(f"{row['id']}: unresolved fallback {fallback}")


def generate(data: dict[str, Any]) -> str:
    validate_data(data)
    out: list[str] = [
        "/* Generated by tools/pipelines/generate_registry.py; do not edit. */",
        "#include \"d3d9_pipeline_api.h\"",
        "#include \"d3d9_menu_light.h\"",
        "",
    ]
    for index, family in enumerate(data["families"]):
        out.append(f"static const D9PipelineFamilyInfo family_{index} = {{")
        out.append(f"    .name = {cstr(family['name'])},")
        out.append(f"    .title = {cstr(family['title'])},")
        out.append(f"    .default_fallback = {cstr(family['default_fallback'])},")
        out.append(f"    .inventory_note = {cstr(family['inventory_note'])}")
        out.append("};")
    out.extend([
        "",
        "static const D9PipelineFamilyInfo families[] = {",
        "    " + ", ".join(f"family_{index}" for index in range(len(data["families"]))),
        "};",
        "",
    ])
    for index, pipeline in enumerate(data["pipelines"]):
        symbol = f"pipeline_{index}"
        out.append(string_array(f"{symbol}_required_constants",
                                pipeline["features"]["required_constants"]))
        out.append(string_array(f"{symbol}_forbidden_constants",
                                pipeline["features"]["forbidden_constants"]))
        out.append("")

    out.append("static const D9PipelineDescriptor descriptors[] = {")
    family_by_name = {row["name"]: row for row in data["families"]}
    for index, pipeline in enumerate(data["pipelines"]):
        symbol = f"pipeline_{index}"
        feature = pipeline["features"]
        opcode_words = [0, 0, 0, 0]
        for opcode in feature["allowed_opcodes"]:
            opcode_words[opcode // 64] |= 1 << (opcode % 64)
        usage_mask = 0
        for usage in feature["allowed_input_usages"]:
            usage_mask |= USAGE_BITS[usage]
        precondition_mask = 0
        precondition_names = []
        for name in pipeline["preconditions"]:
            precondition_mask |= 1 << list(PRE_BITS).index(name)
            precondition_names.append(name)
        fallback = family_by_name[pipeline["family"]]["default_fallback"]
        if fallback == pipeline["id"]:
            fallback = ""
        menu_signature = "0"
        if pipeline["matcher"] == "menu_light":
            menu_signature = MENU_SIGNATURES[pipeline["menu_signature"]]
        out.append("    {")
        out.append(f"        .id = {cstr(pipeline['id'])},")
        out.append(f"        .family = {FAMILY_ENUMS[pipeline['family']]},")
        out.append(f"        .support = {SUPPORT_ENUMS[pipeline['support']]},")
        out.append(f"        .source = {SOURCE_ENUMS[pipeline['source']]},")
        out.append(f"        .priority = {pipeline['priority']},")
        out.append(f"        .matcher = {MATCHER_ENUMS[pipeline['matcher']]},")
        out.append(f"        .menu_signature = {menu_signature},")
        out.append(f"        .fixed_material = {MATERIAL_ENUMS[pipeline['fixed_material']]},")
        out.append(f"        .precondition_mask = UINT32_C(0x{precondition_mask:08x}),")
        for field in (
            "inventory_signature", "accepted_original", "package_rule",
            "vertex_inputs", "texture_units", "quality_reductions",
            "unsupported", "fallback", "memory",
        ):
            out.append(f"        .{field} = {cstr(pipeline[field])},")
        out.append(f"        .fallback_id = {cstr(fallback)},")
        pre_text = ", ".join(precondition_names) if precondition_names else "none"
        out.append(f"        .preconditions = {cstr(pre_text)},")
        out.append(f"        .required_constants = {symbol}_required_constants,")
        out.append(f"        .forbidden_constants = {symbol}_forbidden_constants,")
        out.append("        .features = {")
        out.append(f"            .version = UINT32_C(0x{feature['version']:08x}),")
        out.append(f"            .min_instructions = {feature['min_instructions']},")
        out.append(f"            .max_instructions = {feature['max_instructions']},")
        for field in (
            "min_textures", "max_textures", "max_samplers", "max_branches",
            "max_loops", "max_tev_stages", "max_tev_textures",
            "max_color_inputs",
        ):
            out.append(f"            .{field} = {feature[field]},")
        for field in (
            "require_single_output", "forbid_mrt", "require_2d", "require_ldr",
        ):
            out.append(f"            .{field} = {int(bool(feature[field]))},")
        out.append(f"            .constant_policy = {POLICY_ENUMS[feature['constant_policy']]},")
        out.append("            .allowed_opcodes = {")
        for word in opcode_words:
            out.append(f"                UINT64_C(0x{word:016x}),")
        out.append("            },")
        out.append(f"            .allowed_input_usages = 0x{usage_mask:04x},")
        out.append(f"            .required_input_count = {len(feature['required_inputs'])},")
        out.append("            .required_input = {")
        for requirement in feature["required_inputs"]:
            out.append(
                "                {"
                f".usage = {USAGE_VALUES[requirement['usage']]}, "
                f".index = {requirement['index']}, "
                f".reg = {requirement['reg']}, "
                f".sampler_type = {requirement['sampler_type']}"
                "},"
            )
        out.append("            },")
        out.append("        },")
        out.append(f"        .pass_count = {len(pipeline['passes'])},")
        out.append("        .passes = {")
        for pass_row in pipeline["passes"]:
            out.append("            {")
            for field in (
                "name", "vertex_inputs", "texture_units", "blend", "depth",
                "alpha_test", "target", "summary",
            ):
                out.append(f"                .{field} = {cstr(pass_row[field])},")
            out.append(f"                .stages = {pass_row['stages']},")
            out.append(f"                .textures = {pass_row['textures']},")
            out.append("            },")
        out.append("        },")
        out.append("    },")
    out.extend([
        "};",
        "",
        "const D9PipelineRegistry d9_pipeline_registry_data = {",
        "    .schema_version = 1,",
        f"    .family_count = {len(data['families'])},",
        f"    .descriptor_count = {len(data['pipelines'])},",
        "    .families = families,",
        "    .descriptors = descriptors,",
        "};",
        "",
    ])
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=Path, default=DEFAULT_JSON)
    parser.add_argument("--output", type=Path, default=DEFAULT_C)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data = json.loads(args.json.read_text(encoding="utf-8"))
    text = generate(data)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
            print(f"registry out of date: {args.output}")
            return 1
        print(f"registry in sync: {args.output}")
        return 0
    args.output.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
