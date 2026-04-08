import tarfile
import urllib.request
import io

profile_url = 'https://zenodo.org/record/7013734/files/scrooge_profile_results.tar.gz'
with urllib.request.urlopen(profile_url) as f:
    targz_data = f.read()

with tarfile.open(fileobj=io.BytesIO(targz_data), mode='r') as tf:
    tf.extractall()
