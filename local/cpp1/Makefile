CC=g++
CFLAGS=-I.
DEPS = 
OBJ = 

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: thrasher sampler

# g++ -o thrasher local.cpp -I. -DTHRASHER
thrasher: local.cpp
	$(CC) -o $@ $^ $(CFLAGS) -DTHRASHER
	
# g++ -o sampler local.cpp -I. -DSAMPLER
sampler: local.cpp
	$(CC) -o $@ $^ $(CFLAGS) -DSAMPLER

.PHONY: clean

clean:
	rm -r thrasher sampler
