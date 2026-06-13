import tarfile

with tarfile.open('scrooge_profile_results.tar.gz', mode='r') as tf:
    tf.extractall()
