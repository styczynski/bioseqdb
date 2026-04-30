#!python3
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import sys, os, glob, re, math


results = {}
def AddRes(size, name, eltime):
    if (name.endswith('_16') or name.endswith('_32')):
        name = name[0:-3]
    global results
    results.setdefault(size, {})[name] = eltime


def ReadRes(fn):
    with open(fn, "rt") as f:
        data = f.read()

        
    match = re.search(r"Number of elements = (\d*)", data)
    size = int(match.group(1))

    for match in re.finditer(r"\s*([0-9.]+)\s*ns\s*:\s*(\S+)", data):
        eltime = float(match.group(1))
        name = match.group(2)
        AddRes(size, name, eltime)


for fn in glob.glob("res/*.log"):
    ReadRes(fn)


N = len(results)
cases = list(sorted(results.keys()))

colors = ['#000000', '#666666', 'g', '#33FF0F', 'c', 'r', '#337FFF', 'm', 'y']
dpi = 300
width = 0.08

#names = list(sorted(list(results.values())[0].keys()))
names = [
    "StdSort",
    "InsertionSort",
    "PCSort_Main:any",
    "PCSort_Optimized:any",
    "PCSort_Trans:any",
    "SortingNetwork",
    "PCSort_Main:distinct",
    "PCSort_WideOuter:distinct",
#    "PCSort_WideOuter_U16:distinct",
]
M = len(names)


ax = plt.axes()
for s in range(M):
    name = names[s]
    xpos = [0] * N
    vals = [0] * N
    for i in range(N):
        val = results[cases[i]].get(name, 0)
        val /= results[cases[i]][names[0]]
        xpos[i] = cases[i]
        vals[i] = 1 / val if val > 0 else None

    distinct = 'distinct' in name
    ax.semilogx(xpos, vals, 'x' if distinct else 'o', basex = 2, linestyle='-', color = colors[s])

ax.grid(True, which="major")
ax.grid(True, which="minor", color='0.8', linestyle=':')
ax.get_yaxis().set_minor_formatter(ticker.FuncFormatter(lambda x,p: str(int(x))))
ax.get_yaxis().set_major_formatter(ticker.ScalarFormatter())
ax.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
ax.legend(names)

ax.set_xlabel("Array length (N)")
ax.set_ylabel("Throughput (relative)")
ax.set_title("Performance of sort implementations")
plt.show()
#plt.savefig('res/plot.png', bbox_inches='tight', dpi=dpi)
#plt.gcf().clear()
