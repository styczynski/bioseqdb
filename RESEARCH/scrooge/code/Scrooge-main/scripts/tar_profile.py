import tarfile

with tarfile.open('scrooge_profile_results.tar.gz', 'w:gz') as f:
    f.add('profile')
