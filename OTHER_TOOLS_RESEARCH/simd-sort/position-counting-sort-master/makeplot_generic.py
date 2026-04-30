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
    "PCSort_WideOuter_U16:distinct",
]
M = len(names)


ax = plt.axes()
bars = [None] * M
for s in range(M):
    name = names[s]
    xpos = [i + (s - 0.5*M) * width for i in range(N)]
    vals = [0] * N
    for i in range(N):
        val = results[cases[i]].get(name, 0)
        val /= results[cases[i]][names[0]]
        vals[i] = 1.0 / val if val > 0 else 0

    distinct = 'distinct' in name
    bars[s] = ax.bar(xpos, vals, width, color=colors[s], hatch = 'x' if distinct else None)

ax.set_xticks([0,1,2])
ax.set_xticklabels(('N = 16', 'N = 32', 'N = 64'))
ax.legend([bars[s][0] for s in range(M)], names)

for s in range(M):
    for i in range(N):
        rect = bars[s][i]
        height = rect.get_height()
        value = results[cases[i]].get(names[s], 0)
        if value > 0:
            ax.text(rect.get_x() + rect.get_width()/2., height + 0.05, '_1_\n%0.0lf' % value, ha='center', va='bottom', size='smaller')

ax.set_xlabel("Array length (N)")
ax.set_ylabel("Throughput (relative)")
ax.set_title("Performance of sort implementations")
plt.show()
#plt.savefig('res/plot.png', bbox_inches='tight', dpi=dpi)
#plt.gcf().clear()
