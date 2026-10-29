//! The validated GoW3 Neural Rendering launch profile. Raw captures remain opt-in.
use std::path::Path;

use indexmap::IndexMap;

use crate::core::graphics_options::Options;

pub fn configure(
    env: &mut IndexMap<String, String>,
    graphics: &Options,
    probe: &Path,
    windows: bool,
) -> Result<(), String> {
    if env.get("SPARTAN_DLSS5_NR").map(String::as_str) != Some("1") {
        return Ok(());
    }
    if !windows {
        return Err("DLSS 5 NR currently requires Windows and an NVIDIA NR-compatible GPU.".into());
    }
    if graphics.render_resolution != "native" || graphics.hdr != "0" {
        return Err("DLSS 5 NR requires native 1920x1080 and SDR. Select the DLSS 5 profile in Graphics again, or disable it.".into());
    }
    for name in ["spartanport_dlssnr.dll", "nvngx_dlssnr.dll"] {
        let path = probe.with_file_name(name);
        if !path.is_file() {
            return Err(format!("DLSS 5 NR runtime missing: {}. Install the NR bridge and NVIDIA runtime beside spartanport-probe.exe, or disable DLSS 5 in Graphics.", path.display()));
        }
    }
    for (key, value) in [
        ("SPARTAN_UPSCALER", "none"),
        ("SPARTAN_EXPERIMENTAL_UPSCALER", "0"),
        ("SPARTAN_DLSS", "0"),
        ("SPARTAN_JITTER", "0"),
        ("SPARTAN_OBJECT_MOTION", "0"),
        ("SPARTAN_DEBUG_MOTION", "0"),
        ("SPARTAN_FSR1", "0"),
        ("SPARTAN_RCAS", "0"),
        ("SPARTAN_DRAW_PIPE", "1"),
        ("SPARTAN_NO_MSAA", "0"),
        ("SPARTAN_GOW3_INPUT_DIAGNOSTICS", "1"),
        ("SPARTAN_GOW3_NR_PREVIEW", "1"),
        ("SPARTAN_GOW3_NR_SCENE", "1"),
    ] {
        env.insert(key.into(), value.into());
    }
    // A render-size compatibility override must not bypass the native profile.
    env.shift_remove("SPARTAN_RENDER_RES");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::graphics_options::startup_options;
    use std::fs;

    #[test]
    fn nr_requires_its_runtime_and_native_sdr_before_launching() {
        let dir = tempfile::tempdir().unwrap();
        let probe = dir.path().join("spartanport-probe.exe");
        let mut env = IndexMap::from([("SPARTAN_DLSS5_NR".into(), "1".into())]);
        let mut graphics = startup_options(&IndexMap::new(), &[]);
        graphics.render_resolution = "native".into();
        assert!(configure(&mut env, &graphics, &probe, true)
            .unwrap_err()
            .contains("spartanport_dlssnr.dll"));
        fs::write(probe.with_file_name("spartanport_dlssnr.dll"), []).unwrap();
        assert!(configure(&mut env, &graphics, &probe, true)
            .unwrap_err()
            .contains("nvngx_dlssnr.dll"));
        fs::write(probe.with_file_name("nvngx_dlssnr.dll"), []).unwrap();
        graphics.render_resolution = "4K".into();
        assert!(configure(&mut env, &graphics, &probe, true).is_err());
        graphics.render_resolution = "native".into();
        graphics.hdr = "1".into();
        assert!(configure(&mut env, &graphics, &probe, true).is_err());
        graphics.hdr = "0".into();
        assert!(configure(&mut env, &graphics, &probe, false).is_err());
        env.insert("SPARTAN_NO_MSAA".into(), "1".into());
        env.insert("SPARTAN_RENDER_RES".into(), "3840x2160".into());
        configure(&mut env, &graphics, &probe, true).unwrap();
        assert_eq!(env["SPARTAN_NO_MSAA"], "0");
        assert_eq!(env["SPARTAN_GOW3_NR_SCENE"], "1");
        assert_eq!(env["SPARTAN_JITTER"], "0");
        assert!(!env.contains_key("SPARTAN_RENDER_RES"));
        assert!(!env.contains_key("SPARTAN_GOW3_NR_DUMP"));
    }
}
