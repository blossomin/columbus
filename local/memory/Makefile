CC=gcc
CFLAGS=-I.
DEPS = 
OBJ = 

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: ram_access

# g++ -o thrasher local.cpp -I. -DTHRASHER
ram_access: ram_access_time.c
	$(CC) -o $@ $^ $(CFLAGS) 
	
# g++ -o sampler local.cpp -I. -DSAMPLER
# sampler: local.cpp
# 	$(CC) -o $@ $^ $(CFLAGS) -DSAMPLER

.PHONY: clean

clean:
	rm -r ram_access
