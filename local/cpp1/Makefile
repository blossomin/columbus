CC=g++
CFLAGS=-I.
DEPS = 
OBJ = 

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: thrasher sampler frequencer

thrasher: local.cpp
	$(CC) -o $@ $^ $(CFLAGS) -DTHRASHER
	
sampler: local.cpp
	$(CC) -o $@ $^ $(CFLAGS) -DSAMPLER

frequencer: local.cpp
	$(CC) -o $@ $^ $(CFLAGS) 

.PHONY: clean

clean:
	rm -r thrasher sampler
