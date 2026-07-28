use crate::InfoArgs;
use crate::publish;
use anyhow::Result;

pub fn info(args: InfoArgs) -> Result<(), anyhow::Error> {
    let config = args.project.load_config(true)?;
    if args.show_install_page_url {
        let repository_config = config.repositories.find_by_name_or_default(args.repository.as_deref())?.clone();
        let page_url = publish::install_page_url(&repository_config).unwrap_or_default();
        println!("{}", page_url);
    } else if args.list_distros {
        let distros: Vec<&str> = config.builds.iter().map(|b| b.distro.as_str()).collect();
        match args.format.as_str() {
            "json" => println!("{}", serde_json::to_string(&distros)?),
            _ => distros.iter().for_each(|d| println!("{}", d)),
        }
    }

    Ok(())
}
