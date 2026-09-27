#!/usr/bin/env python3
"""Fetch pinned upstream sources without compiling; instrumentation is source-owned."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

from instrument_sources import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tools',nargs='+',choices=['blastn','mmseqs2','winnowmap','lexicmap'])
    args=parser.parse_args()
    for name in args.tools:
        directory=ROOT/('bench/tool_'+name);lock=json.loads((directory/'source_lock.json').read_text())
        source=directory/'source'
        if source.exists():
            print(name+': existing source retained; no reset or overwrite');continue
        if name=='blastn':
            archive=directory/'source.tar.gz'
            urllib.request.urlretrieve(lock['url'],archive)
            assert hashlib.sha256(archive.read_bytes()).hexdigest()==lock['sha256']
            source.mkdir()
            with tarfile.open(archive) as tar:
                members=tar.getmembers();prefix=members[0].name.split('/')[0]+'/'
                for member in members:
                    if member.name==prefix[:-1]:continue
                    assert member.name.startswith(prefix)
                    member.name=member.name[len(prefix):]
                    assert not Path(member.name).is_absolute() and '..' not in Path(member.name).parts
                    tar.extract(member,source,filter='data')
        else:
            subprocess.run(['git','clone',lock['url'],str(source)],check=True)
            subprocess.run(['git','-C',str(source),'checkout','--detach',lock['revision']],check=True)
            subprocess.run(['git','-C',str(source),'submodule','update','--init','--recursive'],check=True)
        print(name+': pinned source ready')


if __name__=='__main__':main()
