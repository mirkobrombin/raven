#!/usr/bin/env python3
"""Generate the Vulkan command marshaling that the guest vulkan-1.dll bridge uses, from the Khronos registry.

Every command the bridge does not implement by hand is described by the registry: its parameters, which of them are handles,
structures, arrays (and how long) or strings, and which structures hold pointers and handles of their own. This script turns that
description into C++ that reads a command's arguments from the guest, copies the structures it points to into host memory with
guest handles replaced by host handles, calls the driver, and writes results, counts and new handles back.

usage: generate-vulkan-marshal.py vk.xml handwritten.def output_directory vulkan_core.h [--report]
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Extensions whose commands and structures are marshaled in addition to core Vulkan 1.0 to 1.4.
EXTENSIONS = """VK_KHR_surface VK_KHR_swapchain VK_KHR_push_descriptor VK_EXT_mutable_descriptor_type VK_VALVE_mutable_descriptor_type VK_EXT_image_sliced_view_of_3d VK_EXT_image_view_min_lod VK_KHR_cooperative_matrix VK_KHR_fragment_shading_rate VK_KHR_calibrated_timestamps VK_KHR_maintenance5 VK_KHR_maintenance6 VK_KHR_maintenance7 VK_KHR_maintenance8 VK_KHR_maintenance9 VK_EXT_extended_dynamic_state3
VK_EXT_extended_dynamic_state2 VK_EXT_extended_dynamic_state VK_EXT_vertex_input_dynamic_state VK_EXT_depth_clip_enable VK_EXT_robustness2
VK_EXT_custom_border_color VK_EXT_memory_priority VK_EXT_non_seamless_cube_map VK_KHR_load_store_op_none VK_EXT_conservative_rasterization
VK_EXT_vertex_attribute_divisor VK_KHR_vertex_attribute_divisor VK_EXT_transform_feedback VK_EXT_line_rasterization VK_KHR_line_rasterization
VK_EXT_graphics_pipeline_library VK_KHR_pipeline_library VK_EXT_debug_utils VK_EXT_conditional_rendering VK_EXT_depth_bias_control
VK_KHR_dynamic_rendering_local_read VK_EXT_dynamic_rendering_unused_attachments VK_EXT_shader_stencil_export VK_KHR_present_id
VK_EXT_multi_draw VK_EXT_pageable_device_local_memory VK_EXT_memory_budget VK_KHR_get_surface_capabilities2 VK_EXT_attachment_feedback_loop_layout
VK_EXT_attachment_feedback_loop_dynamic_state VK_EXT_border_color_swizzle VK_EXT_fragment_shader_interlock VK_EXT_sample_locations
VK_KHR_external_memory VK_KHR_external_semaphore VK_KHR_timeline_semaphore VK_KHR_synchronization2 VK_KHR_dynamic_rendering
VK_KHR_deferred_host_operations VK_KHR_acceleration_structure VK_KHR_ray_tracing_pipeline VK_KHR_ray_query VK_KHR_ray_tracing_maintenance1
VK_KHR_ray_tracing_position_fetch VK_EXT_descriptor_buffer""".split()

# Parameters and members of these types cannot cross the guest/host boundary by copying.
FOREIGN = re.compile(r"^(HANDLE|LPCWSTR|DWORD|SECURITY_ATTRIBUTES|HINSTANCE|HWND|HMONITOR|wl_\w+|xcb_\w+|Display|Window|VisualID|RROutput|"
                     r"ANativeWindow|AHardwareBuffer|zx_handle_t|GgpStreamDescriptor|GgpFrameToken|_screen_\w+|NvSciSync\w*|NvSciBuf\w*|"
                     r"IDirectFB\w*|IDirectFBSurface|CAMetalLayer|MTL\w+|IOSurfaceRef|OH_\w+|OHNativeWindow|OHBufferHandle|"
                     r"StdVideo\w+|struct|int|void)$")
# Commands that belong to platform integration, video and other families the bridge does not offer are matched by whole
# words of their camel-case names, so that for example "Framebuffer" and "ImageView" are not mistaken for "Frame" and "Vi".
SKIP_WORDS = set("""Win32 Xlib Xcb Wayland Android Metal Screen DirectFB Ggp Vi Fuchsia Qnx Ohos Display Displays
FullScreen ImagePipe Sci Cuda Cu Tensor Tensors DataGraph Micromap Micromaps
Opacity Optical Flow Video Decode Encode Mesh Tile Tiles Latency Reflex
Keyed Mutex Zircon Hdr Refresh Cycle Past Fault Faults Proc Addr Binary Binaries Slot Window""".split())
# Words that only matter inside these exact phrases.
SKIP_PHRASES = ("ResourceHeap", "SamplerHeap", "PushData", "DescriptorSize",
                "DeviceMask", "DeviceGroup", "ShaderObject", "ShadersEXT", "ShaderBinary",
                "PipelineBinar", "PipelineExecutable", "PipelineProperties", "PipelineStatistics", "FrameBoundary", "Memory(Win32|Fd|Zircon|Host|Remote)",
                "SemaphoreFd", "FenceFd", "ExecutionSet", "IndirectExecutionSet", "ExecutionGraph", "PrivateData")
# Commands the bridge implements by hand around its own bookkeeping.
SKIP_NAMES = {"vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties", "vkEnumerateDeviceExtensionProperties",
              "vkEnumerateDeviceLayerProperties", "vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkCreateInstance", "vkDestroyInstance",
              "vkCreateDevice", "vkDestroyDevice", "vkDeviceWaitIdle", "vkAllocateMemory", "vkFreeMemory", "vkMapMemory", "vkUnmapMemory",
              "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges", "vkMapMemory2", "vkUnmapMemory2", "vkAllocateCommandBuffers",
              "vkFreeCommandBuffers", "vkDestroyCommandPool", "vkGetPhysicalDeviceSparseImageFormatProperties",
              # Presentation beyond the core surface and swapchain commands of VK_KHR_swapchain: shared and headless surfaces, per-present
              # timing and release, counters, rectangles and the extended capability queries.
              "vkCreateSharedSwapchainsKHR", "vkCreateHeadlessSurfaceEXT", "vkGetPhysicalDeviceSurfaceCapabilities2EXT",
              "vkGetPhysicalDeviceSurfacePresentModes2EXT", "vkGetPhysicalDevicePresentRectanglesKHR", "vkWaitForPresentKHR",
              "vkWaitForPresent2KHR", "vkReleaseSwapchainImagesEXT", "vkReleaseSwapchainImagesKHR", "vkGetSwapchainCounterEXT",
              "vkGetSwapchainStatusKHR", "vkGetDeviceGroupSurfacePresentModes2EXT", "vkSetLocalDimmingAMD",
              # Acceleration structure commands that run on the host take host addresses; the device commands are the ones offered.
              "vkBuildAccelerationStructuresKHR", "vkCopyAccelerationStructureKHR", "vkCopyAccelerationStructureToMemoryKHR",
              "vkCopyMemoryToAccelerationStructureKHR", "vkWriteAccelerationStructuresPropertiesKHR"}


def skipped_command(name):
    if name in SKIP_NAMES:
        return True
    words = re.findall(r"[A-Z][a-z0-9]*|[A-Z]+(?![a-z])", name[2:])
    if any(word in SKIP_WORDS for word in words):
        return True
    return any(re.search(phrase, name) for phrase in SKIP_PHRASES)


class Member:
    def __init__(self, element, structs_text=None):
        self.name = element.find("name").text
        self.type = element.find("type").text
        # A trailing comment element can contain colons (a member described as a copy of Structure::member); it is not part of the declaration.
        text = (element.text or "") + "".join("".join(child.itertext()) * (child.tag != "comment") + (child.tail or "") for child in element)
        before = text.split(self.type, 1)[0]
        after = text.split(self.type, 1)[1].split(self.name, 1)
        middle = after[0]
        self.const = "const" in before or "const" in middle
        self.pointers = middle.count("*")
        self.array = bool(re.search(r"\[", after[1])) if len(after) > 1 else False
        self.bitfield = ":" in (after[1] if len(after) > 1 else "")
        self.len = element.get("altlen") or element.get("len")
        if self.len:
            self.len = [part.strip() for part in self.len.split(",")]
        self.optional = (element.get("optional") or "").split(",")[0] == "true"
        self.values = element.get("values")
        self.element = element


class Registry:
    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.structs, self.unions, self.handles, self.commands, self.aliases_struct, self.aliases_command = {}, {}, {}, {}, {}, {}
        self.basetype_of = {}
        for element in root.iter("type"):
            category = element.get("category")
            name = element.get("name")
            if category in ("struct", "union") and name:
                if element.get("alias"):
                    self.aliases_struct[name] = element.get("alias")
                    continue
                members = [Member(member) for member in element.findall("member") if "vulkan" in (member.get("api") or "vulkan").split(",")]
                target = self.structs if category == "struct" else self.unions
                target[name] = {"members": members, "extends": (element.get("structextends") or "").split(",") if element.get("structextends") else [],
                                "returnedonly": element.get("returnedonly") == "true"}
            elif category == "handle":
                handle_name = element.get("name") or (element.find("name").text if element.find("name") is not None else None)
                if element.get("alias"):
                    self.aliases_struct[handle_name] = element.get("alias")
                elif handle_name:
                    self.handles[handle_name] = {"dispatchable": "VK_DEFINE_HANDLE" in "".join(element.itertext()) and "NON_DISPATCHABLE" not in "".join(element.itertext())}
            elif category == "bitmask":
                inner = element.find("type")
                if inner is not None and element.find("name") is not None:
                    self.basetype_of[element.find("name").text] = inner.text
        for command in root.iter("command"):
            if command.get("alias"):
                self.aliases_command[command.get("name")] = command.get("alias")
                continue
            proto = command.find("proto")
            if proto is None:
                continue
            name = proto.find("name").text
            # A parameter written twice for Vulkan and Vulkan SC counts once: the SC-only spelling is dropped.
            elements = [item for item in command.findall("param") if item.get("api") != "vulkansc"]
            params = [Member(param) for param in elements]
            for param, element in zip(params, elements):
                param.extern = element.get("externsync")
            self.commands[name] = {"return": proto.find("type").text, "params": params, "element": command}
        self.required_commands, self.required_types = set(), set()
        for element in root.iter("feature"):
            if element.get("api") and "vulkan" in element.get("api").split(",") and not "vulkansc" == element.get("api"):
                number = float(element.get("number"))
                if number > 1.4:
                    continue
                self.collect(element)
        for element in root.iter("extension"):
            if element.get("name") in EXTENSIONS and "vulkan" in (element.get("supported") or "").split(","):
                self.collect(element)
        for alias, target in self.aliases_command.items():
            if target in self.commands:
                pass

    def collect(self, element):
        for require in element.findall("require"):
            for command in require.findall("command"):
                self.required_commands.add(command.get("name"))
            for type_ in require.findall("type"):
                self.required_types.add(type_.get("name"))

    def resolve_struct(self, name):
        while name in self.aliases_struct:
            name = self.aliases_struct[name]
        return name

    def resolve_command(self, name):
        while name in self.aliases_command:
            name = self.aliases_command[name]
        return name


SCALAR_64 = {"uint64_t", "int64_t", "VkDeviceSize", "VkDeviceAddress", "size_t", "VkFlags64", "VkRemoteAddressNV"}

CAPTURE_OBJECTS = {"Buffer": "BUFFER", "Image": "IMAGE", "ImageView": "IMAGE_VIEW", "Sampler": "SAMPLER",
                   "AccelerationStructure": "ACCELERATION_STRUCTURE_KHR"}
CAPTURE_COMMANDS = {f"vkGet{name}OpaqueCaptureDescriptorDataEXT": value for name, value in CAPTURE_OBJECTS.items()}


def main():
    registry = Registry(sys.argv[1])
    handwritten = set(Path(sys.argv[2]).read_text().split())
    output = Path(sys.argv[3])
    header_text = Path(sys.argv[4]).read_text()
    # Only names that the installed headers declare can be compiled against.
    declared = set(re.findall(r"\bVk\w+\b", header_text)) | set(re.findall(r"\bPFN_vk\w+\b", header_text))
    report = "--report" in sys.argv
    supported_struct = {}

    def classify_member(member, struct_name):
        """Kind of one structure member, or None when it cannot be copied across."""
        if member.bitfield:
            return None
        base = registry.resolve_struct(member.type)
        if base.startswith("Vk") and base not in declared:
            return None
        if member.name == "pNext" and member.pointers == 1:
            return "pnext"
        if member.name == "sType":
            return "scalar"
        if member.type.startswith("PFN_"):
            return None
        if member.pointers == 0:
            if base in registry.handles:
                return "handle"
            if base in registry.unions:
                if base == "VkDescriptorDataEXT":
                    return "descriptor_data"
                return "union" if union_plain(base) else None
            if base in registry.structs:
                return "struct" if struct_ok(base) else None
            if FOREIGN.match(base) and base not in ("int", "void"):
                return None
            return "scalar"
        if member.pointers == 1:
            if base == "char":
                return "string"
            if base == "void":
                if struct_name == "VkOpaqueCaptureDescriptorDataCreateInfoEXT":
                    return "capture_data"
                if struct_name in CAPTURE_COMMANDS:
                    return "bytes"
                if member.name == "pShaderGroupCaptureReplayHandle":
                    return "null_only"
                return "bytes" if member.len else None
            if base in registry.handles:
                return "handle_array" if member.len else None
            if base in registry.structs:
                return "struct_pointer" if struct_ok(base) else None
            if base in registry.unions:
                return "scalar_pointer" if union_plain(base) else None
            if FOREIGN.match(base):
                return None
            return "scalar_pointer"
        if member.pointers == 2 and base == "char":
            return "string_array" if member.len else None
        if member.pointers == 2 and base in registry.structs and member.len and struct_ok(base):
            return "struct_pointer_array"
        return None

    union_cache = {}

    # A device or host address is passed as the device address: the bridge offers the device commands only, never the host ones.
    ADDRESS_UNIONS = {"VkDeviceOrHostAddressKHR", "VkDeviceOrHostAddressConstKHR", "VkDeviceOrHostAddressConstAliasedKHR"}

    def union_plain(name):
        if name in ADDRESS_UNIONS:
            return True
        if name not in union_cache:
            union_cache[name] = all(m.pointers == 0 and registry.resolve_struct(m.type) not in registry.handles and
                                    (registry.resolve_struct(m.type) not in registry.structs or struct_ok(registry.resolve_struct(m.type)))
                                    for m in registry.unions[name]["members"])
        return union_cache[name]

    state = {}

    def struct_ok(name):
        name = registry.resolve_struct(name)
        if name in state:
            return state[name] is not False
        if name not in registry.structs or name not in declared:
            return False
        state[name] = None  # recursion guard: treat as fine while checking
        info = registry.structs[name]
        result = True
        kinds = {}
        for member in info["members"]:
            kind = classify_member(member, name)
            if kind is None:
                result = False
                break
            kinds[member.name] = kind
        state[name] = kinds if result else False
        return result

    for name in registry.structs:
        struct_ok(name)

    def used_structs():
        return [n for n in registry.structs if state.get(n) not in (False, None) and n in registry.required_types]

    chain_structs = [n for n in used_structs() if registry.structs[n]["members"][0].values]
    if report:
        print(len(registry.structs), "structs;", len(used_structs()), "supported and required;", len(chain_structs), "with sType")
    # Commands
    selected = {}
    reasons = {}
    for name in sorted(registry.required_commands | set(registry.aliases_command)):
        target = registry.resolve_command(name)
        if target not in registry.commands:
            continue
        if name in handwritten or target in handwritten:
            continue
        if skipped_command(target):
            reasons[name] = "special-purpose command"
            continue
        info = registry.commands[target]
        ok = "PFN_" + target in declared and info["return"] in ("void", "VkResult", "uint32_t", "uint64_t", "VkDeviceAddress", "VkBool32", "VkDeviceSize", "int32_t")
        if not ok:
            reasons[name] = "not declared by the installed headers"
        for param in info["params"]:
            if not ok:
                break
            kind = classify_member(param, target) if param.name != "pAllocator" else "allocator"
            if kind in ("pnext",):
                kind = None
            if param.pointers == 1 and registry.resolve_struct(param.type) in registry.handles and not param.const and not param.len:
                kind = "out_handle"
            if kind is None:
                ok = False
                reasons[name] = f"parameter {param.name} ({param.type}{'*' * param.pointers})"
                break
        if ok:
            selected[name] = target
    print(len(selected), "commands generated;", len(reasons), "skipped", file=sys.stderr)
    if report:
        by = {}
        for name, why in reasons.items():
            by.setdefault(why, []).append(name)
        for why, names in sorted(by.items(), key=lambda item: -len(item[1]))[:30]:
            print(len(names), why, names[:3])
        print(sorted(selected)[:40])
    # the generation proper lives in the emitter below
    from_emit = __import__("importlib").import_module  # keep the report path free of emitter imports
    if report:
        return
    emit(registry, selected, state, output, classify_member, declared)



def cxx_len(expression, names):
    """Translate a registry length expression into C++ over the given name map; None when it names something unknown."""
    expression = expression.strip()
    if expression == "null-terminated":
        return None
    def replace(match):
        word = match.group(0)
        if word in names:
            return names[word]
        if re.fullmatch(r"\d+", word):
            return word
        raise KeyError(word)
    try:
        translated = re.sub(r"[A-Za-z_][A-Za-z_0-9]*|\d+", replace, expression.replace("->", "->"))
    except KeyError:
        return None
    return f"static_cast<size_t>({translated})"


def emit(registry, selected, state, output, classify_member, declared):
    output.mkdir(parents=True, exist_ok=True)
    handles = sorted(name for name in registry.handles)
    all_kinds = {name: kinds for name, kinds in state.items() if kinds not in (False, None)}
    # Keep the structures the selected commands use, what those contain, and everything that may extend them in a pNext chain.
    wanted = set()

    def want(type_name):
        type_name = registry.resolve_struct(type_name)
        if type_name in wanted or type_name not in all_kinds:
            return
        wanted.add(type_name)
        for member in registry.structs[type_name]["members"]:
            want(member.type)

    for root in ("VkMemoryAllocateInfo",):
        want(root)
    if "vkGetDescriptorEXT" in selected:
        want("VkDescriptorAddressInfoEXT")
        want("VkDescriptorImageInfo")
    for exported, target in selected.items():
        for param in registry.commands[target]["params"]:
            want(param.type)
    changed = True
    while changed:
        changed = False
        for name, info in registry.structs.items():
            if name in all_kinds and name not in wanted and name in registry.required_types and any(registry.resolve_struct(e) in wanted for e in info["extends"]):
                before = len(wanted)
                want(name)
                changed |= len(wanted) != before
    kinds_of = {name: all_kinds[name] for name in wanted}
    # ---- handle types ----
    with open(output / "vulkan_handle_types.inc", "w") as out:
        out.write("// Generated by scripts/generate-vulkan-marshal.py from the Vulkan registry; do not edit.\n")
        for name in handles:
            out.write(f"{name},\n")
    needs_fix = {}

    def fix_needed(name):
        name = registry.resolve_struct(name)
        if name not in needs_fix:
            needs_fix[name] = False
            for member in registry.structs[name]["members"]:
                kind = kinds_of[name][member.name]
                if kind in ("pnext", "handle", "string", "bytes", "handle_array", "struct_pointer", "struct_pointer_array", "string_array", "scalar_pointer", "null_only", "descriptor_data", "capture_data") or \
                        (kind == "struct" and fix_needed(registry.resolve_struct(member.type))):
                    needs_fix[name] = True
        return needs_fix[name]

    def encodable(name):
        name = registry.resolve_struct(name)
        for member in registry.structs[name]["members"]:
            kind = kinds_of[name][member.name]
            if kind in ("string", "bytes", "handle_array", "struct_pointer", "struct_pointer_array", "string_array", "scalar_pointer", "descriptor_data", "capture_data"):
                return False
            if kind == "struct" and not encodable(member.type):
                return False
        return True

    code = ["// Generated by scripts/generate-vulkan-marshal.py from the Vulkan registry; do not edit.\n"]
    structs = sorted(kinds_of)
    for name in structs:
        code.append(f"static void fix_{name}(Marshal &m, {name} *p);\n")
        code.append(f"static void decode_{name}(Marshal &m, uint64_t guest, {name} *out);\n")
        code.append(f"static {name} *decode_array_{name}(Marshal &m, uint64_t guest, size_t count);\n")
        code.append(f"static const {name} *const *decode_pointer_array_{name}(Marshal &m, uint64_t guest, size_t count);\n")
    code.append("static void *decode_pnext(Marshal &m, uint64_t first);\n")
    code.append("static void encode_pnext(Marshal &m, void *host, uint64_t guest);\n\n")

    def member_len(member, names_expr):
        if not member.len:
            return None
        return cxx_len(member.len[0], names_expr)

    for name in structs:
        info = registry.structs[name]
        kinds = kinds_of[name]
        member_names = {member.name: f"p->{member.name}" for member in info["members"]}
        body = []
        for member in info["members"]:
            kind = kinds[member.name]
            base = registry.resolve_struct(member.type)
            n = member.name
            length = member_len(member, member_names)
            if kind == "pnext":
                body.append(f"p->pNext = decode_pnext(m, reinterpret_cast<uint64_t>(p->pNext));")
            elif kind == "capture_data":
                body.append(f"p->{n} = p->{n} ? m.copy_bytes(reinterpret_cast<uint64_t>(p->{n}), m.capture_descriptor_size(m.capture_object)) : nullptr;")
            elif kind == "descriptor_data":
                body.extend([
                    "switch (p->type) {",
                    "case VK_DESCRIPTOR_TYPE_SAMPLER:",
                    "    p->data.pSampler = m.host_array<VkSampler>(H::VkSampler, reinterpret_cast<uint64_t>(p->data.pSampler), 1); break;",
                    "case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:",
                    "case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:",
                    "    p->data.pSampledImage = p->data.pSampledImage ? m.descriptor_image_infos(reinterpret_cast<uint64_t>(p->data.pSampledImage), 1, p->type) : nullptr; break;",
                    "case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:",
                    "case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:",
                    "    p->data.pUniformBuffer = p->data.pUniformBuffer ? decode_array_VkDescriptorAddressInfoEXT(m, reinterpret_cast<uint64_t>(p->data.pUniformBuffer), 1) : nullptr; break;",
                    "case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: break;",
                    "default: throw std::runtime_error(\"unsupported Vulkan descriptor data type\");",
                    "}"])
            elif kind == "handle":
                body.append(f"p->{n} = reinterpret_cast<{member.type}>(m.host(H::{base}, reinterpret_cast<uint64_t>(p->{n})));")
            elif kind == "struct" and fix_needed(base):
                body.append(f"fix_{base}(m, &p->{n});")
            elif kind == "null_only":
                body.append(f"if (p->{n}) throw std::runtime_error(\"{name}.{n} is not supported\");")
            elif kind == "string":
                body.append(f"p->{n} = p->{n} ? m.string(m.read_string(reinterpret_cast<uint64_t>(p->{n}))) : nullptr;")
            elif kind == "bytes":
                if not length:
                    body.append(f"if (p->{n}) throw std::runtime_error(\"{name}.{n} has no length\");")
                else:
                    body.append(f"p->{n} = p->{n} ? m.copy_bytes(reinterpret_cast<uint64_t>(p->{n}), {length}) : nullptr;")
            elif kind == "handle_array" and name == "VkWriteDescriptorSet" and n == "pTexelBufferView":
                body.append(f"p->{n} = (p->{n} && Marshal::texel_descriptor(p->descriptorType)) ? m.host_array<{member.type}>(H::{base}, reinterpret_cast<uint64_t>(p->{n}), {length}) : nullptr;")
            elif kind == "handle_array":
                body.append(f"p->{n} = p->{n} ? m.host_array<{member.type}>(H::{base}, reinterpret_cast<uint64_t>(p->{n}), {length}) : nullptr;")
            elif kind == "struct_pointer" and name == "VkWriteDescriptorSet" and n == "pBufferInfo":
                body.append(f"p->{n} = (p->{n} && Marshal::buffer_descriptor(p->descriptorType)) ? decode_array_{base}(m, reinterpret_cast<uint64_t>(p->{n}), {length}) : nullptr;")
            elif kind == "struct_pointer" and name == "VkWriteDescriptorSet" and n == "pImageInfo":
                # Which members of an image descriptor are handles depends on the descriptor type; the others are left unset by programs.
                body.append(f"p->{n} = (p->{n} && Marshal::image_descriptor(p->descriptorType)) ? m.descriptor_image_infos(reinterpret_cast<uint64_t>(p->{n}), {length}, p->descriptorType) : nullptr;")
            elif kind == "struct_pointer":
                count = length if length else "1"
                body.append(f"p->{n} = p->{n} ? decode_array_{base}(m, reinterpret_cast<uint64_t>(p->{n}), {count}) : nullptr;")
            elif kind == "struct_pointer_array":
                body.append(f"p->{n} = p->{n} ? decode_pointer_array_{base}(m, reinterpret_cast<uint64_t>(p->{n}), {length}) : nullptr;")
            elif kind == "string_array":
                body.append(f"p->{n} = p->{n} ? m.string_array(reinterpret_cast<uint64_t>(p->{n}), {length}) : nullptr;")
            elif kind == "scalar_pointer":
                count = length if length else "1"
                elem = member.type if member.type not in ("void",) else "uint8_t"
                body.append(f"p->{n} = p->{n} ? static_cast<decltype(p->{n})>(m.copy_bytes(reinterpret_cast<uint64_t>(p->{n}), ({count}) * sizeof({elem}))) : nullptr;")
        if registry.structs[name]["members"][0].values:
            pass
        code.append(f"static void fix_{name}(Marshal &m, {name} *p)\n{{\n")
        for line in body:
            code.append(f"\t{line}\n")
        code.append("\t(void)m; (void)p;\n}\n")
        code.append(f"static void decode_{name}(Marshal &m, uint64_t guest, {name} *out)\n{{\n\tm.read_into(guest, out, sizeof({name}));\n\tfix_{name}(m, out);\n}}\n")
        code.append(f"static {name} *decode_array_{name}(Marshal &m, uint64_t guest, size_t count)\n{{\n"
                    f"\tauto *result = m.make<{name}>(count);\n\tfor (size_t index = 0; index < count; ++index)\n"
                    f"\t\tdecode_{name}(m, guest + index * sizeof({name}), result + index);\n\treturn result;\n}}\n")
        code.append(f"static const {name} *const *decode_pointer_array_{name}(Marshal &m, uint64_t guest, size_t count)\n{{\n"
                    f"\tauto *result = m.make<const {name} *>(count);\n\tfor (size_t index = 0; index < count; ++index)\n"
                    f"\t\tresult[index] = decode_array_{name}(m, m.load<uint64_t>(guest + index * 8), 1);\n\treturn result;\n}}\n")
        if encodable(name):
            enc = []
            for member in info["members"]:
                kind = kinds[member.name]
                n = member.name
                if kind == "pnext":
                    enc.append(f"tmp.pNext = original.pNext;")
                elif kind == "handle":
                    enc.append(f"tmp.{n} = reinterpret_cast<{member.type}>(m.token(H::{registry.resolve_struct(member.type)}, reinterpret_cast<uint64_t>(host.{n})));")
            code.append(f"static void encode_{name}(Marshal &m, const {name} &host, uint64_t guest)\n{{\n"
                        f"\t{name} original{{}};\n\tm.read_into(guest, &original, sizeof({name}));\n\t{name} tmp = host;\n")
            for line in enc:
                code.append(f"\t{line}\n")
            code.append(f"\tm.write(guest, &tmp, sizeof({name}));\n")
            if any(kinds[mm.name] == "pnext" for mm in info["members"]):
                code.append(f"\tif (host.pNext) encode_pnext(m, const_cast<void *>(host.pNext), reinterpret_cast<uint64_t>(original.pNext));\n")
            code.append("}\n")
    # chains: every supported structure that carries an sType may appear
    chain = [n for n in structs if registry.structs[n]["members"][0].values and registry.structs[n]["members"][0].name == "sType"]
    code.append("static void *decode_pnext(Marshal &m, uint64_t first)\n{\n\tvoid *head = nullptr;\n\tVkBaseOutStructure *tail = nullptr;\n"
                "\tfor (size_t depth = 0; first && depth < 64; ++depth) {\n\t\tconst uint32_t type = m.load<uint32_t>(first);\n\t\tconst uint64_t next = m.load<uint64_t>(first + 8);\n\t\tVkBaseOutStructure *element = nullptr;\n\t\tswitch (type) {\n")
    for name in chain:
        code.append(f"\t\tcase {registry.structs[name]['members'][0].values}: {{ auto *item = m.make<{name}>(1); decode_{name}(m, first, item); element = reinterpret_cast<VkBaseOutStructure *>(item); break; }}\n")
    code.append("\t\tdefault: throw std::runtime_error(\"unsupported Vulkan structure in a pNext chain (type \" + std::to_string(type) + \")\");\n\t\t}\n"
                "\t\telement->pNext = nullptr;\n\t\tif (tail) tail->pNext = element; else head = element;\n\t\ttail = element;\n\t\tfirst = next;\n\t}\n"
                "\tif (first) throw std::runtime_error(\"Vulkan pNext chain is too long\");\n\treturn head;\n}\n\n")
    code.append("static void encode_pnext(Marshal &m, void *host, uint64_t guest)\n{\n\tfor (size_t depth = 0; host && guest && depth < 64; ++depth) {\n"
                "\t\tauto *base = static_cast<VkBaseOutStructure *>(host);\n\t\tconst uint64_t next = m.load<uint64_t>(guest + 8);\n\t\tswitch (static_cast<uint32_t>(base->sType)) {\n")
    for name in chain:
        if encodable(name):
            code.append(f"\t\tcase {registry.structs[name]['members'][0].values}: encode_{name}(m, *reinterpret_cast<{name} *>(host), guest); break;\n")
    code.append("\t\tdefault: break;\n\t\t}\n\t\thost = base->pNext;\n\t\tguest = next;\n\t}\n}\n\n")

    # ---- commands ----
    table = []
    counts = {}
    command_code = []
    hooks_pre = {"vkQueueSubmit": "synchronize_to_device", "vkQueueSubmit2": "synchronize_to_device", "vkQueueSubmit2KHR": "synchronize_to_device"}
    hooks_post = {"vkQueueWaitIdle": "synchronize_from_device", "vkWaitForFences": "synchronize_from_device", "vkGetFenceStatus": "synchronize_from_device","vkWaitSemaphores": "synchronize_from_device", "vkWaitSemaphoresKHR": "synchronize_from_device", "vkGetSemaphoreCounterValue": "synchronize_after_counter",
                  "vkGetSemaphoreCounterValueKHR": "synchronize_after_counter"}
    # Commands that can keep the driver busy for long: they run on a host thread while the guest's other threads go on.
    detached = {"vkQueuePresentKHR"}
    # Waits with a timeout: the driver waits in slices so that a process that ends is not held by a wait nothing will satisfy.
    detached_waits = {"vkWaitSemaphores", "vkWaitSemaphoresKHR", "vkWaitForFences"}
    for exported, target in sorted(selected.items()):
        info = registry.commands[target]
        params = info["params"]
        if target in counts:
            table.append((exported, target))
            continue
        counts[target] = True
        lines = []
        names_map = {}
        first = params[0]
        first_base = registry.resolve_struct(first.type)
        level = "device" if first_base in ("VkDevice", "VkQueue", "VkCommandBuffer") else "instance"
        lines.append(f"\tm.context(H::{first_base}, a[0]);")
        for object_name, object_type in CAPTURE_OBJECTS.items():
            if target == f"vkCreate{object_name}" or target == f"vkCreate{object_name}KHR":
                lines.append(f"\tm.capture_object = VK_OBJECT_TYPE_{object_type};")
        # scalars first
        index_of = {param.name: i for i, param in enumerate(params)}
        decl = []
        call_args = [None] * len(params)
        post = []
        for i, param in enumerate(params):
            base = registry.resolve_struct(param.type)
            n = param.name
            v = f"v_{n}"
            names_map[n] = None
            if n == "pAllocator":
                decl.append(f"\tm.require_null(a[{i}]);")
                call_args[i] = "nullptr"
                continue
            if param.pointers == 0 and param.array:
                size = re.search(r"\[([^\]]+)\]", "".join(param.element.itertext()))
                count = size.group(1).strip() if size else "1"
                decl.append(f"\tconst {param.type} *{v} = static_cast<const {param.type} *>(m.copy_bytes(a[{i}], ({count}) * sizeof({param.type})));")
                call_args[i] = v
                continue
            if param.pointers == 0:
                if base in registry.handles:
                    decl.append(f"\tauto {v} = reinterpret_cast<{param.type}>(m.host(H::{base}, a[{i}]));")
                elif param.type == "float":
                    decl.append(f"\tfloat {v} = m.float_argument(x, a, {i});")
                elif param.type == "double":
                    decl.append(f"\tdouble {v} = m.double_argument(x, a, {i});")
                elif param.type in SCALAR_64 or registry.basetype_of.get(param.type) == "VkFlags64":
                    decl.append(f"\t{param.type} {v} = static_cast<{param.type}>(a[{i}]);")
                else:
                    decl.append(f"\t{param.type} {v} = static_cast<{param.type}>(static_cast<uint32_t>(a[{i}]));")
                names_map[n] = v
                call_args[i] = v
        # pointers
        for i, param in enumerate(params):
            if (param.pointers == 0 and not param.array) or param.name == "pAllocator" or (param.pointers == 0 and param.array):
                continue
            base = registry.resolve_struct(param.type)
            n = param.name
            v = f"v_{n}"
            length = None
            if target in CAPTURE_COMMANDS and n == "pData":
                length = f"m.capture_descriptor_size(VK_OBJECT_TYPE_{CAPTURE_COMMANDS[target]})"
            if param.len and param.len[0] != "null-terminated":
                length = cxx_len(param.len[0], {k: val for k, val in names_map.items() if val})
                if length is None and "->" in param.len[0]:
                    left, right = param.len[0].split("->", 1)
                    if names_map.get(left.strip()):
                        length = f"static_cast<size_t>({names_map[left.strip()]}->{right.strip()})"
                if length is None:
                    # a count member of another parameter
                    raise SystemExit(f"cannot evaluate length {param.len} of {target}.{n}")
            if target == "vkCmdBuildAccelerationStructuresKHR" and n == "ppBuildRangeInfos":
                # One array of ranges per build info, as long as that info's geometry count.
                decl.append(f"\tauto **{v}_rows = a[{i}] ? m.make<const VkAccelerationStructureBuildRangeInfoKHR *>({names_map['infoCount']}) : nullptr;\n"
                            f"\tif ({v}_rows) for (size_t k = 0; k < {names_map['infoCount']}; ++k)\n"
                            f"\t\t{v}_rows[k] = decode_array_VkAccelerationStructureBuildRangeInfoKHR(m, m.load<uint64_t>(a[{i}] + k * 8), {names_map['pInfos']}[k].geometryCount);")
                call_args[i] = f"{v}_rows"
                continue
            if param.const or param.pointers == 2:
                if base in registry.structs and param.pointers == 1:
                    count = length or "1"
                    decl.append(f"\tauto *{v} = a[{i}] ? decode_array_{base}(m, a[{i}], {count}) : nullptr;")
                elif base in registry.handles:
                    decl.append(f"\tauto *{v} = a[{i}] ? m.host_array<{param.type}>(H::{base}, a[{i}], {length}) : nullptr;")
                elif param.type == "char" and param.pointers == 1:
                    decl.append(f"\tconst char *{v} = a[{i}] ? m.string(m.read_string(a[{i}])) : nullptr;")
                elif param.type == "char" and param.pointers == 2:
                    decl.append(f"\tconst char *const *{v} = a[{i}] ? m.string_array(a[{i}], {length}) : nullptr;")
                elif param.type == "void":
                    decl.append(f"\tconst void *{v} = a[{i}] ? m.copy_bytes(a[{i}], {length or '0'}) : nullptr;")
                else:
                    count = length or "1"
                    decl.append(f"\tauto *{v} = a[{i}] ? static_cast<const {param.type} *>(m.copy_bytes(a[{i}], ({count}) * sizeof({param.type}))) : nullptr;")
                names_map[n] = v
                call_args[i] = v
                continue
            # non-const single pointers are outputs (or in/out)
            if base in registry.handles:
                if length:
                    decl.append(f"\tauto *{v} = a[{i}] ? m.make<{param.type}>({length}) : nullptr;")
                    # Swapchain images belong to the swapchain: the guest sees one stable token per image and never owns the host image.
                    issue = "swapchain_image_token" if target == "vkGetSwapchainImagesKHR" else "new_token"
                    post.append(f"\tif (a[{i}] && success) for (size_t k = 0; k < {length}; ++k) m.store<uint64_t>(a[{i}] + k * 8, m.{issue}(H::{base}, reinterpret_cast<uint64_t>({v}[k])));")
                else:
                    decl.append(f"\t{param.type} {v} = nullptr;\n\tif (!a[{i}]) throw std::runtime_error(\"{target} needs an output handle\");\n\tm.check_writable(a[{i}], 8);")
                    if target == "vkCreateDescriptorUpdateTemplate":
                        # Updates through a template carry handles at offsets only the template knows, so the entries are kept for them.
                        post.append(f"\tif (success) {{ const auto token = m.new_token(H::{base}, reinterpret_cast<uint64_t>({v})); m.store<uint64_t>(a[{i}], token); b.remember_template(token, v_pCreateInfo); }}")
                    else:
                        post.append(f"\tif (success) m.store<uint64_t>(a[{i}], m.new_token(H::{base}, reinterpret_cast<uint64_t>({v})));")
                call_args[i] = v if length else f"&{v}"
                names_map[n] = v
                continue
            if base in registry.structs:
                if length:
                    decl.append(f"\tauto *{v} = a[{i}] ? decode_array_{base}(m, a[{i}], {length}) : nullptr;")
                    post.append(f"\tif (a[{i}]) for (size_t k = 0; k < {length}; ++k) encode_{base}(m, {v}[k], a[{i}] + k * sizeof({base}));")
                else:
                    decl.append(f"\tauto *{v} = a[{i}] ? decode_array_{base}(m, a[{i}], 1) : nullptr;")
                    post.append(f"\tif (a[{i}]) encode_{base}(m, *{v}, a[{i}]);")
                names_map[n] = v
                call_args[i] = v
                continue
            # scalar outputs
            elem = param.type if param.type != "void" else "uint8_t"
            if length:
                counter = None
                match = re.fullmatch(r"\(\*(v_\w+)\)", length)
                decl.append(f"\tauto *{v} = a[{i}] ? m.make<{elem}>({length} + 0) : nullptr;")
                post.append(f"\tif (a[{i}] && success) m.write(a[{i}], {v}, ({length}) * sizeof({elem}));")
            else:
                decl.append(f"\t{elem} {v}_value{{}};\n\tif (a[{i}]) m.read_into(a[{i}], &{v}_value, sizeof({elem}));\n\t{elem} *{v} = a[{i}] ? &{v}_value : nullptr;")
                post.append(f"\tif (a[{i}]) m.write(a[{i}], &{v}_value, sizeof({elem}));")
                names_map[n] = f"{v}_value"
            call_args[i] = v
        for line in decl:
            lines.append(line)
        pre = hooks_pre.get(exported) or hooks_pre.get(target)
        if pre:
            lines.append(f"\tb.{pre}(m.device_token);")
        ret = info["return"]
        lines.append(f"\tauto function = m.proc<PFN_{target}>(\"{target}\");")
        call = f"function({', '.join(call_args)})"
        if ret == "void":
            lines.append(f"\t{call};\n\tconst bool success = true;\n\t(void)success;\n\tuint64_t result = 0;")
        elif ret == "VkResult" and target in detached_waits:
            sliced = ", ".join("slice" if arg == "v_timeout" else arg for arg in call_args)
            lines.append(f"\tconst VkResult status = b.detached_wait([&](uint64_t slice) {{ return function({sliced}); }}, v_timeout);\n\tconst bool success = status == VK_SUCCESS || status == VK_INCOMPLETE;\n\tuint64_t result = static_cast<uint32_t>(status);")
        elif ret == "VkResult" and target in detached:
            lines.append(f"\tconst VkResult status = b.detached([&] {{ return {call}; }});\n\tconst bool success = status == VK_SUCCESS || status == VK_INCOMPLETE;\n\tuint64_t result = static_cast<uint32_t>(status);")
        elif ret == "VkResult":
            lines.append(f"\tconst VkResult status = {call};\n\tconst bool success = status == VK_SUCCESS || status == VK_INCOMPLETE;\n\tuint64_t result = static_cast<uint32_t>(status);")
        else:
            lines.append(f"\tconst auto value = {call};\n\tconst bool success = true;\n\t(void)success;\n\tuint64_t result = static_cast<uint64_t>(value);")
        for line in post:
            lines.append(line)
        if target.startswith("vkDestroy") and len(params) >= 2 and registry.resolve_struct(params[1].type) in registry.handles and params[1].pointers == 0:
            lines.append("\tif (success) m.forget(a[1]);")
        post_hook = hooks_post.get(exported) or hooks_post.get(target)
        if post_hook:
            # A counter that has not moved since it was last read signals nothing new for the host to see.
            extra = f", a[1], {names_map.get('pValue', 'v_pValue')}" if post_hook == "synchronize_after_counter" else ""
            lines.append(f"\tif (success) b.{post_hook}(m.device_token{extra});")
        lines.append("\treturn result;")
        command_code.append(f"static uint64_t cmd_{target}(VulkanBridge &b, Marshal &m, const std::vector<uint64_t> &a, const std::array<uint64_t, 4> &x)\n{{\n"
                            + "\n".join(lines) + "\n\t(void)b; (void)x;\n}\n")
        table.append((exported, target))
    code.extend(command_code)
    # destroyers for generic handles
    destroy = []
    for name in handles:
        command = "vkDestroy" + name[2:]
        if command in registry.commands:
            params = registry.commands[command]["params"]
            if ("PFN_" + command) in declared and name in declared and len(params) == 3 and params[0].type == "VkDevice" and params[1].type == name and params[2].name == "pAllocator" and name not in (
                    "VkBuffer", "VkImage", "VkImageView", "VkSampler", "VkSemaphore", "VkFence", "VkCommandPool", "VkDescriptorPool", "VkRenderPass",
                    "VkFramebuffer", "VkPipeline", "VkPipelineLayout", "VkDescriptorSetLayout", "VkShaderModule"):
                destroy.append(f"\tcase H::{name}: if (auto fn = m.proc<PFN_{command}>(\"{command}\")) fn(m.host_device, reinterpret_cast<{name}>(host), nullptr); break;")
    code.append("static void destroy_generated_handle(Marshal &m, H type, uint64_t host)\n{\n\tswitch (type) {\n" + "\n".join(destroy) + "\n\tdefault: break;\n\t}\n}\n")
    code.append("static void destroy_generic_objects(VulkanBridge &b, uint64_t device_token);\n")
    code.append("struct GeneratedCommand { const char *name; size_t arguments; uint64_t (*function)(VulkanBridge &, Marshal &, const std::vector<uint64_t> &, const std::array<uint64_t, 4> &); bool device; };\n")
    code.append("static const GeneratedCommand generated_commands[] = {\n")
    for exported, target in sorted(table):
        info = registry.commands[target]
        first = registry.resolve_struct(info["params"][0].type)
        device = first in ("VkDevice", "VkQueue", "VkCommandBuffer")
        code.append(f'\t{{"{exported}", {len(info["params"])}, cmd_{target}, {"true" if device else "false"}}},\n')
    code.append("};\n")
    (output / "vulkan_marshal_generated.inc").write_text("".join(code))
    # exports
    (output / "vulkan_generated_exports.def").write_text("".join(f"{e}\n" for e, t in sorted(table)))
    hand = [line for line in Path(sys.argv[2]).read_text().split() if line.startswith("vk")]
    (output / "vulkan_all_exports.def").write_text("LIBRARY vulkan-1.dll\nEXPORTS\n" + "".join(f"{name}\n" for name in sorted(set(hand) | {e for e, t in table})))
    print(len(table), "exports,", len(structs), "structures,", len(chain), "chain elements", file=sys.stderr)


if __name__ == "__main__":
    main()
