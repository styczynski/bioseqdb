
#CFLAGS = -O0 -g
CFLAGS = -O4
 
# linker flags, -pg enables use of gprof
#LFLAGS = -pg
LFLAGS =

#NCFLAGS = -O0 -G -Xptxas -v
#NCFLAGS = -O3 -Xptxas -v -lineinfo
#NCFLAGS = -O3 -Xptxas -v
NCFLAGS = -O3 -Xptxas -v -lineinfo
CFLAGS += $(options)

all: darwin

ntcoding.o: ntcoding.h ntcoding.cpp
	g++ $(CFLAGS) -c ntcoding.cpp

fasta.o: fasta.h fasta.cpp 
	g++ $(CFLAGS) -c fasta.cpp

seed_pos_table.o: ntcoding.o seed_pos_table.h seed_pos_table.cpp
	g++ $(CFLAGS) -c seed_pos_table.cpp

darwin.o: darwin.cpp
	g++ -std=c++11 $(CFLAGS) -Wno-multichar -c darwin.cpp

chameleon.o: Chameleon.cpp
	g++ -std=c++11 $(CFLAGS) -Wno-multichar -o chameleon.o -c Chameleon.cpp

config_file.o: ConfigFile.cpp
	g++ -std=c++11 $(CFLAGS) -Wno-multichar -o config_file.o -c ConfigFile.cpp

darwin: ntcoding.o fasta.o seed_pos_table.o chameleon.o config_file.o darwin.o
	#g++ -std=c++11 $(CFLAGS) $(LFLAGS) -Wno-multichar -I/usr/local/include/ fasta.o ntcoding.o seed_pos_table.o chameleon.o config_file.o darwin.o cuda_host.cu gact.cpp align.cpp -o darwin -pthread 
	$(nvccinstance) -std=c++11 $(NCFLAGS) $(LFLAGS) $(gpu_options) -arch=$(GPU_ARCH) -Xcompiler="-pthread -Wno-multichar -fopenmp" -I/usr/local/include/ fasta.o ntcoding.o seed_pos_table.o chameleon.o config_file.o darwin.cpp cuda_host.cu gact.cpp align.cpp -o darwin

clean:
	rm -rf *.o darwin

