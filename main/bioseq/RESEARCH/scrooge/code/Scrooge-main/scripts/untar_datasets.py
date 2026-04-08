import tarfile

with tarfile.open('scrooge_datasets.tar.gz', mode='r') as tf:
    tf.extractall()
