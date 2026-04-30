import re, os, glob

def CollectData(N):
    for fn in glob.glob("sort_tmp.*"):
        os.remove(fn)

    with open("sort.cpp", "rt") as f:
        src = f.read()
    ints = 32768 // 4
    total_elems = 1<<25
    src = re.sub(r"const int PACK = (\d*);", r"const int PACK = %d;" % N, src)
    src = re.sub(r"const int MAXN = (.*?);", r"const int MAXN = %d;" % (ints // N), src)
    src = re.sub(r"const int TRIES = (.*?);", r"const int TRIES = %d;" % (total_elems // N), src)
    with open("sort_tmp.cpp", "wt") as f:
        f.write(src)

    with open("c.bat", "rt") as f:
        bat = f.read()
    bat = bat.replace("sort.cpp", "sort_tmp.cpp")
    os.system(bat)

    logname = "res_%03d.log" % (N)
    os.system("sort_tmp >res/" + logname)
    os.system("sort_tmp >res/" + logname)


for fn in glob.glob("res/*"):
    os.remove(fn)

#sizes = [16, 32, 64]
sizes = [16, 32, 64, 128, 256, 512]
for s in sizes:
    CollectData(s)
