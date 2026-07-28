use crate::PortalArgs;
use crate::distros::Distros;
use crate::shell::Command;
use anyhow::Result;
use std::collections::HashMap;
use std::path::Path;

pub fn run(portal_args: PortalArgs) -> Result<(), anyhow::Error> {
    let mut args = vec!["run".to_string(), "-it".to_string(), "--rm".to_string(), "--entrypoint".to_string(), "/bin/bash".to_string()];

    std::fs::create_dir_all(&portal_args.build_dir)?;

    let mountpoint_basename = Path::new(&portal_args.build_dir)
        .file_name()
        .ok_or_else(|| anyhow::anyhow!("build_dir has no basename"))?
        .to_str()
        .ok_or_else(|| anyhow::anyhow!("build_dir is not valid UTF-8"))?;

    let mut mounts: HashMap<String, String> = HashMap::new();
    mounts.insert(portal_args.build_dir.to_string_lossy().to_string(), format!("/{mountpoint_basename}").to_string());
    let mount_args: Vec<String> = mounts.iter().flat_map(|(from, to)| ["--mount".to_string(), format!("type=bind,source={from},target={to}")]).collect();
    args.extend(mount_args);

    let distro = Distros::get().by_id(&portal_args.distro);
    args.push(distro.image.clone());

    Command::container(args).run_interactive()
}
