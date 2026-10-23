#!/usr/bin/env python3
"""Instrument a separate pinned dispatcher; never edit the inference runtime."""
import hashlib
from pathlib import Path
import sys

PIN = 'daf652bd7c8aad8714578fe8b61c374b1cdd8d11cee152f9c99f5191460f4b8c'


def instrument(source, hooks):
    if hashlib.sha256(source.encode()).hexdigest() != PIN:
        raise ValueError('Unexpected llama.cpp Vulkan dispatcher; review the new source first')
    anchors = [
        ('static void ggml_vk_mul_mat_vec_q_f16(', hooks + '\nstatic void ggml_vk_mul_mat_vec_q_f16('),
        ('    // For batch_n, the A matrix is the same for each batch, and B/D use the row stride as the batch stride',
         '    nexus_vk_q8_probe_capture(ctx, subctx, src0, src1, d_Y, quantize_y);\n\n'
         '    // For batch_n, the A matrix is the same for each batch, and B/D use the row stride as the batch stride'),
        ('    return GGML_STATUS_SUCCESS;\n\n    UNUSED(backend);',
         '    nexus_vk_q8_probe_finish(ctx);\n    return GGML_STATUS_SUCCESS;\n\n    UNUSED(backend);'),
        ('device->integer_dot_product = device->integer_dot_product && shader_integer_dot_product_props.integerDotProduct4x8BitPackedSignedAccelerated;',
         '// Diagnostic mode permits driver-emulated integer dot products, not unsupported features.\n'
         '        device->integer_dot_product = device->integer_dot_product &&\n'
         '            (shader_integer_dot_product_props.integerDotProduct4x8BitPackedSignedAccelerated ||\n'
         '             (device->physical_device.getProperties().vendorID == VK_VENDOR_ID_AMD &&\n'
         '              getenv("NEXUS_VK_Q8_PROBE_SOFTWARE_DOT")));'),
        ('        vkGetPhysicalDeviceFeatures2(device->physical_device, &device_features2);',
         '        vkGetPhysicalDeviceFeatures2(device->physical_device, &device_features2);\n'
         '        if (device->integer_dot_product && !shader_integer_dot_product_features.shaderIntegerDotProduct) {\n'
         '            throw std::runtime_error("Integer dot product feature is not supported");\n'
         '        }'),
    ]
    for old, new in anchors:
        if source.count(old) != 1:
            raise ValueError('Ambiguous diagnostic hook location')
        source = source.replace(old, new, 1)
    return source


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare_q8_probe.py PINNED_DISPATCHER NEW_OUTPUT')
    source, output = map(Path, sys.argv[1:])
    if source.resolve() == output.resolve() or output.exists():
        raise SystemExit('A new, separate output is required')
    hooks = Path(__file__).with_name('q8_probe_backend.inc').read_text()
    result = instrument(source.read_text(), hooks)
    output.write_text(result)
