import tarfile

with tarfile.open('scrooge_datasets.tar.gz', 'w:gz') as f:
    f.add('datasets')
