
from typing import List, Dict , Union
from dataclasses import dataclass
from collections import UserString

import datetime

@dataclass
class FastqRead:
    name: str = ""
    seq: str = ""
    quality: str = ""


    def __len__(self):
        return len(self.seq)


    @staticmethod
    def _read_line(f, first=False):
        line = f.readline()
        if line: 
            return line

        else: 
            if first: 
                raise ExperimentStopReading()
            else:
                raise FastqReadDeserializationError("could not deserialize read: hit end of the file")


    @classmethod
    def from_file(cls, fileobj):
        name = cls._read_line(fileobj, first=True)[1:-1]
        seq = cls._read_line(fileobj)[:-1]
        optional = cls._read_line(fileobj).strip()
        quality = cls._read_line(fileobj).strip()

        return cls(name, seq, quality)



class FastqReadDeserializationError(Exception):
    def __init__(self, message=None):
        self.message = message

    def __str__(self):
        if self.message:
            return message
        else:
            return ""


class ExperimentStopReading(Exception):
    def __init__(self):
        pass

    def __str__(self):
        return "ExperimentStopReading"




class FastqExperiment:
    def __init__(self, reads: Union[Dict[str, FastqRead], None] = None):
        raise Exception("Not implemented")
        if reads:
            self.reads = reads
        else:
            self.reads = dict()


    @classmethod
    def from_file(cls, fileobj):
        reads = dict()
        while True:
                try: 

                    read = FastqRead.from_file(fileobj)

                except ExperimentStopReading:
                    break


                reads[read.name] = read

        return cls(reads)

    def __len__(self):
        return len(self.reads)
    def __iter__(self):
        for key, value in self.reads.items():
            yield (key, value)  


    def __setitem__(self, key:str, item:FastqRead):
        self.reads[key] = item

    def __getitem__(self, key:str):
        return self.reads[key]  
    


@dataclass
class Suffix:
    text: str
    position: int


class AlignmentResult(dict):
    def __init__(self,start=None, stop=None, *args, **kwargs):
        self.start = start
        self.stop = stop
        super(AlignmentResult, self).__init__(*args, **kwargs)


    def __setitem__(self, key, item):
        self.__dict__[key] = item

    def __getitem__(self, key):
        return self.__dict__[key]

    def __repr__(self):
        return repr(self.__dict__)

    def __len__(self):
        return len(self.__dict__)

    def __delitem__(self, key):
        del self.__dict__[key]

    def clear(self):
        return self.__dict__.clear()

    def copy(self):
        return self.__dict__.copy()

    def has_key(self, k):
        return k in self.__dict__

    def update(self, *args, **kwargs):
        return self.__dict__.update(*args, **kwargs)

    def keys(self):
        return self.__dict__.keys()

    def values(self):
        return self.__dict__.values()

    def items(self):
        return self.__dict__.items()

    def pop(self, *args):
        return self.__dict__.pop(*args)

    def __cmp__(self, dict_):
        return self.__cmp__(self.__dict__, dict_)

    def __contains__(self, item):
        return item in self.__dict__

    def __iter__(self):
        return iter(self.__dict__)


class Genome(UserString):

    @classmethod
    def from_file(cls, fileobj):
        return cls(fileobj.readline().strip())




