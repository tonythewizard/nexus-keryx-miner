//! What the walk may use on the Vulkan device llama.cpp created. A borrowed `VkDevice` cannot be
//! asked which features were enabled on it, so this repeats ggml-vulkan's own rules
//! (`ggml_vk_get_device`, `get_device_architecture`, `ggml_vk_khr_cooperative_matrix_support` in
//! the pinned llama.cpp):
//!
//! - int8 cooperative matrices are enabled on AMD's own drivers only for RDNA3/RDNA4, which
//!   advertise the extension on every architecture;
//! - `VK_KHR_shader_integer_dot_product` is enabled only where packed signed 4x8-bit dot products
//!   are hardware-accelerated (not on RDNA1).
//!
//! A walk kernel that relied on a feature the device was created without would be invalid usage.

use ash::vk;
use ash::vk::Handle;
use std::ffi::CStr;

/// RDNA generations as ggml-vulkan classifies AMD devices.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AmdArch {
    Gcn,
    Rdna1,
    Rdna2,
    Rdna3,
    Rdna4,
    Other,
}

pub struct SharedDevice {
    pub name: String,
    pub vendor_id: u32,
    pub arch: Option<AmdArch>,
    /// llama.cpp enabled int8 cooperative matrices, so the matrix-core walk may run.
    pub matrix_cores: bool,
    /// llama.cpp enabled integer dot products, so the v_dot4 walk may run.
    pub integer_dot: bool,
}

const VENDOR_AMD: u32 = 0x1002;
const VENDOR_INTEL: u32 = 0x8086;
const VENDOR_QUALCOMM: u32 = 0x5143;

/// Inspect llama.cpp's device 0 (the inference GPU). The ggml Vulkan backend must be initialised.
pub fn shared_device() -> Result<SharedDevice, String> {
    let (mut instance, mut physical, mut device, mut family) =
        (std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), 0u32);
    // SAFETY: plain query into the initialised backend; the handles stay valid for the process.
    if !unsafe {
        llama_cpp_sys_2::ggml_backend_vk_keryx_raw_handles(0, &mut instance, &mut physical, &mut device, &mut family)
    } {
        return Err("llama.cpp has no Vulkan device with bufferDeviceAddress".into());
    }
    // SAFETY: ggml's live instance and physical device; only queries are issued.
    unsafe {
        let entry = ash::Entry::load().map_err(|e| format!("Vulkan loader not found: {e}"))?;
        let instance = ash::Instance::load(entry.static_fn(), vk::Instance::from_raw(instance as u64));
        let physical = vk::PhysicalDevice::from_raw(physical as u64);
        if let Ok(expected) = std::env::var("NEXUS_VK_EXPECTED_UUID") {
            let mut id = vk::PhysicalDeviceIDProperties::default();
            instance.get_physical_device_properties2(physical, &mut vk::PhysicalDeviceProperties2::default().push_next(&mut id));
            let h: String = id.device_uuid.iter().map(|b| format!("{b:02x}")).collect();
            let actual = format!("GPU-{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..]);
            if actual != expected { return Err("inference GPU differs from the selected physical device".into()); }
        }
        Ok(classify(&instance, physical))
    }
}

unsafe fn classify(instance: &ash::Instance, pd: vk::PhysicalDevice) -> SharedDevice {
    let exts: Vec<String> = instance
        .enumerate_device_extension_properties(pd)
        .unwrap_or_default()
        .iter()
        .filter_map(|e| e.extension_name_as_c_str().ok().map(|n| n.to_string_lossy().into_owned()))
        .collect();
    let has = |name: &CStr| exts.iter().any(|e| e.as_bytes() == name.to_bytes());
    let props = instance.get_physical_device_properties(pd);
    let name = CStr::from_ptr(props.device_name.as_ptr()).to_string_lossy().into_owned();

    let mut driver = vk::PhysicalDeviceDriverProperties::default();
    let mut dot = vk::PhysicalDeviceShaderIntegerDotProductProperties::default();
    let mut subgroup = vk::PhysicalDeviceSubgroupSizeControlProperties::default();
    let mut core = vk::PhysicalDeviceShaderCorePropertiesAMD::default();
    let amd_core = has(c"VK_AMD_shader_core_properties");
    {
        let mut p2 = vk::PhysicalDeviceProperties2::default()
            .push_next(&mut driver)
            .push_next(&mut dot)
            .push_next(&mut subgroup);
        if amd_core {
            p2 = p2.push_next(&mut core);
        }
        instance.get_physical_device_properties2(pd, &mut p2);
    }

    let arch = (props.vendor_id == VENDOR_AMD).then(|| {
        if !amd_core || !has(c"VK_KHR_shader_integer_dot_product") || !has(c"VK_EXT_subgroup_size_control") {
            AmdArch::Other
        } else if subgroup.min_subgroup_size == 64 && subgroup.max_subgroup_size == 64 {
            AmdArch::Gcn
        } else if subgroup.min_subgroup_size == 32 && subgroup.max_subgroup_size == 64 {
            if core.wavefronts_per_simd == 20 {
                AmdArch::Rdna1
            } else if has(c"VK_EXT_shader_float8") {
                AmdArch::Rdna4
            } else if dot.integer_dot_product4x8_bit_packed_mixed_signedness_accelerated == vk::TRUE {
                AmdArch::Rdna3
            } else {
                AmdArch::Rdna2
            }
        } else {
            AmdArch::Other
        }
    });

    let amd_driver = matches!(driver.driver_id, vk::DriverId::AMD_PROPRIETARY | vk::DriverId::AMD_OPEN_SOURCE);
    let coopmat_architecture = match props.vendor_id {
        VENDOR_AMD if amd_driver => matches!(arch, Some(AmdArch::Rdna3 | AmdArch::Rdna4)),
        // ggml limits Intel to Xe2 and Qualcomm to Adreno matrix-core parts; neither is admitted here.
        VENDOR_INTEL | VENDOR_QUALCOMM => false,
        _ => true,
    };
    let matrix_cores = coopmat_architecture
        && has(c"VK_KHR_cooperative_matrix")
        && std::env::var_os("GGML_VK_DISABLE_COOPMAT").is_none();
    let integer_dot = has(c"VK_KHR_shader_integer_dot_product")
        && std::env::var_os("GGML_VK_DISABLE_INTEGER_DOT_PRODUCT").is_none()
        && dot.integer_dot_product4x8_bit_packed_signed_accelerated == vk::TRUE;

    SharedDevice { name, vendor_id: props.vendor_id, arch, matrix_cores, integer_dot }
}

/// Resolve an explicit UUID/PCI identity before the inference engine opens a
/// logical device. No selection by ordinal or model name is accepted.
pub fn select(identity: &str) -> Result<String, String> {
    let wanted = identity.to_ascii_lowercase();
    if !wanted.starts_with("gpu-") && !wanted.starts_with("pci:") {
        return Err("an explicit GPU UUID or pci:dddd:bb:ss.f identity is required".into());
    }
    unsafe {
        let entry = ash::Entry::load().map_err(|e| e.to_string())?;
        let app = vk::ApplicationInfo::default().api_version(vk::make_api_version(0, 1, 3, 0));
        let instance = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None)
            .map_err(|e| e.to_string())?;
        let result = (|| {
            let mut found = None;
            for (index, physical) in instance.enumerate_physical_devices().map_err(|e| e.to_string())?.into_iter().enumerate() {
                let mut id = vk::PhysicalDeviceIDProperties::default();
                let extensions = instance.enumerate_device_extension_properties(physical).map_err(|e| e.to_string())?;
                let has_pci = extensions.iter().any(|e| e.extension_name_as_c_str().ok() == Some(c"VK_EXT_pci_bus_info"));
                let mut pci = vk::PhysicalDevicePCIBusInfoPropertiesEXT::default();
                let mut props = vk::PhysicalDeviceProperties2::default().push_next(&mut id);
                if has_pci { props = props.push_next(&mut pci); }
                instance.get_physical_device_properties2(physical, &mut props);
                let vendor = props.properties.vendor_id;
                let h: String = id.device_uuid.iter().map(|b| format!("{b:02x}")).collect();
                let uuid = format!("GPU-{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..]);
                let pci_id = format!("pci:{:04x}:{:02x}:{:02x}.{}", pci.pci_domain, pci.pci_bus, pci.pci_device, pci.pci_function);
                if wanted == uuid.to_ascii_lowercase() || (has_pci && wanted == pci_id) {
                    // This runtime is admitted for the AMD worker only. A mixed
                    // Windows rig must never silently borrow its NVIDIA GPU.
                    if vendor != VENDOR_AMD { return Err("selected Vulkan device is not AMD".into()); }
                    if id.device_uuid.iter().all(|&b| b == 0) || found.is_some() {
                        return Err("ambiguous or empty Vulkan device UUID".into());
                    }
                    found = Some((index, uuid));
                }
            }
            let (index, uuid) = found.ok_or("selected AMD Vulkan device was not found")?;
            std::env::set_var("NEXUS_VK_EXPECTED_UUID", &uuid);
            std::env::set_var("KERYX_INFER_GPU", index.to_string());
            std::env::set_var("GGML_VK_VISIBLE_DEVICES", index.to_string());
            Ok(uuid)
        })();
        instance.destroy_instance(None);
        result
    }
}
