#!/usr/bin/env python3

import pandas as pd
from pathlib import Path
import matplotlib
import matplotlib.pyplot as plt
import tabulate
import seaborn as sns

# from matplotlib.ticker import LogLocator
# from matplotlib.colors import to_rgba

# Limit matplot lib print precision
pd.set_option("display.precision", 2)
pd.set_option("display.float_format", "{:0.2f}".format)


def table():
    print("table")
    DF = pd.read_json("results.json")
    # DF = pd.read_json("results-neon.json")

    DF["canonical"] = DF["name"].apply(lambda x: "canonical " in x)
    DF["name"] = DF["name"].str.replace("canonical ", "")

    print(tabulate.tabulate(DF, headers=DF.columns, tablefmt="orgtbl", floatfmt=".2f"))

    # Incremental table
    df = DF[(DF["experiment"] == "incremental")][["name", "time", "k", "w"]]
    df = df.pivot_table(
        index="name", columns=["w", "k"], values="time", aggfunc="median", sort=False
    )
    print(tabulate.tabulate(df, headers=df.columns, tablefmt="orgtbl", floatfmt=".2f"))
    print(df.to_latex(float_format="%.2f"))

    # External table
    df = DF[DF["experiment"] == "external"]
    df = df.pivot_table(
        index="name",
        columns=["w", "k", "canonical"],
        values="time",
        aggfunc="median",
        sort=False,
    )
    print(tabulate.tabulate(df, headers=df.columns, tablefmt="orgtbl", floatfmt=".2f"))
    print(df.to_latex(float_format="%.2f"))

    # # Sliding min plot
    # import seaborn as sns
    # import matplotlib.pyplot as plt
    # df = DF[DF["experiment"] == "sliding_min"]
    # ax = sns.lineplot(data=df, x="w", y="time", hue="name")
    # # log base 2
    # ax.set_xscale("log", base=2)
    # ax.set_ylim(0, 30)
    # ax.grid(axis="y")
    # plt.show()


# Plots


def plot():
    DF = pd.read_json("results-plot.json")
    DF["canonical"] = DF["name"].apply(lambda x: "canonical " in x)
    DF["name"] = DF["name"].str.replace("canonical ", "")

    print(DF)

    sns.lineplot(
        data=DF,
        x="w",
        y="time",
        hue="name",
        size="k",
        style="canonical",
        markers=True,
        # dashes=False,
        hue_order=["minimizer-iter", "rescan", "simd-minimizers"],
        style_order=[False, True],
        palette={
            "minimizer-iter": "black",
            "rescan": "#a0a0a0",
            "simd-minimizers": "#fcc007",
        },
        estimator="median",
        # Hide confidence interval
        errorbar=None,
    )
    plt.xlabel("$w$")
    # log base 2
    # plt.xscale("log", base=2)
    plt.yscale("log", base=2)
    # Set y label
    plt.ylabel("Time (ns/base)")
    # Set y tick labels
    plt.yticks([1, 2, 4, 8, 16], ["1", "2", "4", "8", "16"])
    plt.yticks([1.5, 3, 6, 12, 24], ["1.5", "3", "6", "12", "24"], minor=True)

    # Show horizontal grid lines
    plt.grid(axis="y", which="major", color="gray")
    plt.grid(axis="y", which="minor", color="lightgray", linestyle="--")

    # Move legend below.
    handles, labels = plt.gca().get_legend_handles_labels()
    handles.pop(0)
    labels.pop(0)
    handles.pop(8)
    labels.pop(8)
    handles.insert(8, handles.pop(9))
    labels[8] = "Canonical"
    labels[9] = "Forward"

    r = matplotlib.patches.Rectangle(
        (0, 0), 1, 1, fill=False, edgecolor="none", visible=False
    )
    handles.insert(0, r)
    labels.insert(0, "Algorithm")
    handles.insert(4, r)
    labels.insert(4, "")
    handles.insert(10, r)
    labels.insert(10, "")
    labels[5] = "$k$"

    # Put legend right of fig
    plt.legend(
        handles,
        labels,
        title="",
        loc="upper left",
        bbox_to_anchor=(1, 1),
        bbox_transform=plt.gca().transAxes,
    )

    # figsize
    plt.gcf().set_size_inches(6, 4)

    plt.savefig("results-plot.png", bbox_inches="tight", dpi=300)
    plt.savefig("results-plot.svg", bbox_inches="tight")
    # plt.show()

    plt.close()


table()
plot()
