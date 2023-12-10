"""
  Utilities to manage failure and time reporting for arbitrary structured operations.

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
from __future__ import annotations
import time
import math
import uuid
import traceback
from aadg_genomics_class.monitoring.logs import LOGS
from termcolor import colored

from asciitree import LeftAligned
from collections import OrderedDict as OD

def _format_time(start, end, max_time):
    time_diff = end - start
    rel_score = math.floor(max(0, min(time_diff/max_time*100, 100)))
    color = 'light_green'
    if rel_score > 30:
        color = 'light_yellow'
    elif rel_score > 90:
        color = 'light_red'

    elapsed_time_ms = math.floor(time_diff*100000)/100
    elapsed_time_str = f"{elapsed_time_ms} ms"
    if elapsed_time_ms > 1000:
        elapsed_time_str = f"{math.floor(elapsed_time_ms / 100)/10} s"
    return colored(elapsed_time_str, color)

class Task:
    def __init__(self, reporter: TaskReporter, parent: Optional[Task], name: str, is_failure: bool = False, extra_details: str = ""):
        self.reporter = reporter
        self.id = uuid.uuid1()
        self.start = None
        self.end = None
        self.parent = parent
        self.level = parent.level + 1 if parent else 0
        self.name = name
        self.extra_details = extra_details
        self.is_failure = is_failure
    
    def __enter__(self):
        self.reporter._open_task(self)
        return self
    
    def __exit__(self, exception_type, exception_value, exception_traceback):
        self.reporter._close_task(self)

    def task(self, name: str):
        return Task(reporter=self.reporter, parent=self, name=name)

    def fail(self, e: Exception):
        formatted_trace = ["[Exception details] "] + traceback.format_exc().split('\n')[1:-2]
        prefix = " "+("    "*(self.level+1))+"   | "
        formatted_trace = [f"{prefix}{line}" for line in formatted_trace]
        formatted_trace = "\n".join(formatted_trace)
        with Task(reporter=self.reporter, parent=self, name=f"{str(e)}", is_failure=True, extra_details=formatted_trace):
            pass

    def describe_task(self):
        if self.is_failure:
            return f"{colored('Critical failure reported:', 'red')} {colored(self.name, 'red')}\n{colored(self.extra_details, 'light_red')}"
        return f"{colored(self.name, 'light_grey')} ({_format_time(self.start, self.end, self.reporter.max_task_time)})"

class TaskReporter:

    root_tasks: List[uuid.UUID]
    tasks: Dict[uuid.UUID, Task]
    children: Dict[uuid.UUID, List[uuid.UUID]]

    def __init__(self, name: str):
        self.tasks = dict()
        self.children = dict()
        self.root_tasks = []
        self.start = None
        self.end = None
        self.root_name = name
        self.max_task_time = 0

    def __enter__(self):
        self.start = time.time()
        self.tasks = dict()
        self.children = dict()
        self.root_tasks = []
        return self
    
    def __exit__(self, exception_type, exception_value, exception_traceback):
        self.end = time.time()
        
        self.max_task_time = max([task.end - task.start for task in self.tasks.values()])

        LOGS.reporter.info("Printing execution summary")
        task_tree = LeftAligned()
        tree_dict = dict()
        tree_dict[f"{self.root_name} (Total time: {_format_time(self.start, self.end, self.max_task_time)})"] = OD([(self.tasks[root_task_id].describe_task(), self._format_to_ordered_dict(root_task_id)) for root_task_id in self.root_tasks])
        report_str = task_tree(tree_dict)
        print(report_str)
        LOGS.reporter.info("Operation completed.")

    def task(self, name: str):
        return Task(reporter=self, parent=None, name=name)

    def _open_task(self, task: Task):
        self.tasks[task.id] = task
        if task.id not in self.children:
            self.children[task.id] = []
        if task.parent:
            self.children[task.parent.id].append(task.id)
        else:
            LOGS.reporter.info(f"Starting task: {task.name}")
            self.root_tasks.append(task.id)
        task.start = time.time()

    def _close_task(self, task: Task):
        self.tasks[task.id] = task
        if task.id not in self.children:
            self.children[task.id] = []
        if not task.start:
            task.start = time.time()
        task.end = time.time()

    def _format_to_ordered_dict(self, node: uuid.UUID):
        return OD([(self.tasks[child_id].describe_task(), self._format_to_ordered_dict(child_id)) for child_id in self.children[node]])
