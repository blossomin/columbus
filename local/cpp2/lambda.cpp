#include <cstdio> 
#include <cstdint>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <ctime>

#include <chrono>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>

/****** Assumptions around clocks ********
 * 1. clocks have a precision of microseconds or lower.
 * 2. Context switching/core switching would not affect the monotonicity or steadiness of the clock on millisecond scales
 * 3. 
 */

#define MAX_BITS_IN_ID      10              // Max lambdas = 2^10
#define BASELINE_SAMPLES    500
#define BASELINE_INTERVAL   1000000         // 1 second to calibrate
#define SAMPLES_PER_BIT     500
#define BIT_INTERVAL_MUS    1000000         // 1 second for communicating each bit

using Clock = std::chrono::high_resolution_clock;
using microseconds = std::chrono::microseconds;
using std::chrono::duration;
using std::chrono::duration_cast;

/* Rdtsc blocks for time measurements */
unsigned cycles_low, cycles_high, cycles_low1, cycles_high1;

static __inline__ unsigned long long rdtsc(void)
{
    __asm__ __volatile__ ("RDTSC\n\t"
            "mov %%edx, %0\n\t"
            "mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::
            "%rax", "rbx", "rcx", "rdx");
}

static __inline__ unsigned long long rdtsc1(void)
{
    __asm__ __volatile__ ("RDTSC\n\t"
            "mov %%edx, %0\n\t"
            "mov %%eax, %1\n\t": "=r" (cycles_high1), "=r" (cycles_low1)::
            "%rax", "rbx", "rcx", "rdx");
}

double next_poisson_time(double rate)
{
    return -logf(1.0f - ((double) random()) / (double) (RAND_MAX)) / rate;
}

/* Prepare randomized seed. Get if from /dev/urandom if possible
 * Repurposed from https://stackoverflow.com/questions/2640717/c-generate-a-good-random-seed-for-psudo-random-number-generators*/
unsigned int good_seed(int id)
{
    unsigned int random_seed, random_seed_a, random_seed_b; 
    std::ifstream file("/dev/urandom", std::ios::in|std::ios::binary);
    if (file.is_open())
    {
        char * memblock;
        int size = sizeof(int);
        memblock = new char [size];
        file.read (memblock, size);
        file.close();
        random_seed_a = *reinterpret_cast<int*>(memblock);
        delete[] memblock;
        printf("Found urandom file!\n");
    }// end if
    else
    {
        random_seed_a = 0;
    }
    random_seed_b = std::time(0);
    random_seed = random_seed_a xor random_seed_b;
    random_seed = random_seed xor (getpid() << 16);
    random_seed = random_seed xor (id  << 16);
    return random_seed;
} 


/* Check if program is not past specified time yet */
inline bool within_time(microseconds time_pt) {
    microseconds now = duration_cast<microseconds>(Clock::now().time_since_epoch());
    return now.count() < time_pt.count();
}

/* Stalls the program until a specified point in time */
inline int poll_wait(microseconds release_time)
{
    // If already past the release time, return but in error
    if (!within_time(release_time))
        return 1;

    while(true) {
        microseconds now = duration_cast<microseconds>(Clock::now().time_since_epoch());
        if (now.count() >= release_time.count())
            return 0;
    }
}

/* Samples membus lock latencies periodically to infer contention. If calibrate is set, uses those reading as baseline. */
uint64_t base_mean_latency = 0;
uint64_t last_mean;
uint64_t samples[SAMPLES_PER_BIT];
int read_bit(uint32_t* addr, microseconds release_time_mus, bool calibrate, int id, int phase, int round)
{
    int i;
    microseconds one_ms = microseconds(1000);
    microseconds ten_ms = microseconds(10000);
    microseconds next = duration_cast<microseconds>(Clock::now().time_since_epoch());
    int num_samples = calibrate ? BASELINE_SAMPLES : SAMPLES_PER_BIT;
    int interval_mus = calibrate ? BASELINE_INTERVAL : BIT_INTERVAL_MUS;
    double sampling_rate_mus = num_samples * 1.0 / interval_mus;

    /* Checking time takes order of micro-seconds, so do it sparesely to not affect contention-causing.
     * assuming each locking op costs few microseconds, check time every few hundred microseconds
     * Release a bit early to avoid overruns */
    release_time_mus -= ten_ms;
    uint64_t start, end, sum = 0, count = 0, mean;

    // printf("%ld, %ld,\n", next.count(), release_time_mus.count());      /** COMMENT OUT IN REAL RUNS **/
    for (i = 0; i < SAMPLES_PER_BIT && within_time(release_time_mus); i++)
    {   
        // Get a sample
        rdtsc();
        __atomic_fetch_add(addr, 1, __ATOMIC_SEQ_CST);
        rdtsc1();

        start = ( ((uint64_t)cycles_high << 32) | cycles_low );
        end = ( ((uint64_t)cycles_high1 << 32) | cycles_low1 );
        //std::cout << (end - start) << std::endl;
        sum += (end - start);
        samples[i] = (end - start);
        count++;
          
        next += microseconds((int)next_poisson_time(sampling_rate_mus));
        poll_wait(next);
    }

    mean = sum / (count > 0 ? count : 1);
    last_mean = mean;
    if (calibrate) {
        base_mean_latency = mean;
        printf("Base mean latency: %lu\n", base_mean_latency);
    }

    bool write_to_file = true;                                   /** COMMENT OUT IN REAL RUNS **/       
    if (write_to_file) {
        char name[100];
        sprintf(name, calibrate ? "data/results_base_%d_%d_%d" : "data/results_%d_%d_%d", id, phase, round);
        FILE *fp = fopen(name, "w");
            fprintf(fp, "Cycles\n");
        for (i = 0; i < count; i++) {
            fprintf(fp, "%lu\n", samples[i]);
        }
        fclose(fp);
    }

    // printf("mean: %lu, base: %lu, samples: %lu\n", mean, base_mean_latency, count);
    return ( 100 * mean > 130 * base_mean_latency);         // For now, call it 1 if mean observed is 30% more than base.
}

/* Causes membus locking contention until a certain time */
void write_bit(uint32_t* addr, microseconds release_time_mus)
{
    int i;
    microseconds ten_ms = microseconds(10000);

    /* Checking time takes order of micro-seconds, so do it sparesely to not affect contention-causing.
     * assuming each locking op costs few microseconds, check time every few hundred microseconds
     * Release a bit early to avoid overruns */
    release_time_mus -= ten_ms;
    while (within_time(release_time_mus))
    {   
        /* atomic sum of cacheline boundary */
        for (i = 0; i < 1000; i++)
            __atomic_fetch_add(addr, 1, __ATOMIC_SEQ_CST);
    }
}

/* Finds an address on heap that falls on consecutive cache lines */
uint32_t* get_cache_line_straddled_address()
{
    int *arr;
    int i, size;
    uint32_t *addr;

    /* Figure out last-level cache line size of this system */
    long cacheline_sz = sysconf(_SC_LEVEL3_CACHE_LINESIZE);
    if (!cacheline_sz) {
        // If L3 does not exist, try L2.
        cacheline_sz = sysconf(_SC_LEVEL2_CACHE_LINESIZE);
        if (!cacheline_sz) {
            printf("ERROR! Cannot find the cacheline size on this machine\n");
            return NULL;
        }
    }
    printf("Cache line size: %ld B\n", cacheline_sz);

    /* Allocate an array that spans multiple cache lines */
    size = 10 * cacheline_sz / sizeof(int);
    arr = (int*) malloc(size * sizeof(int));
    for (i = 0; i < size; i++ ) arr[i] = 1; 

    /* Find the first cacheline boundary */
    for (i = 1; i < size; i++) {
        long address = (long)(arr + i);
        if (address % cacheline_sz == 0)  break;
    }

    if (i == size) {
        printf("ERROR! Could not find a cacheline boundary in the array.\n");
        return NULL;
    }
    else {
        addr = (uint32_t*)((uint8_t*)(arr+i-1) + 2);
        printf("Found an address that falls on two cache lines: %p\n", (void*) addr);
    }

    return addr;
}

/* Execute the info exchange protocol where all participating lambdas on a same machine
 * learn the id of one (max-id) lambda in each phase. Runs till all lambdas know each 
 * other or for a specified number of phases */
int run_membus_protocol(int my_id, microseconds start_time_mus, int max_phases, uint32_t* cacheline_addr)
{
    microseconds bit_duration = microseconds(BIT_INTERVAL_MUS);
    microseconds five_ms = microseconds(5000);
    microseconds ten_ms = microseconds(10000);
    microseconds phase_duration = bit_duration * MAX_BITS_IN_ID;

    // Sync with other lamdas a few milliseconds early
    if (poll_wait(start_time_mus - ten_ms)){
        printf("ERROR! Already past the intitial sync point, bad run for current lambda.\n");
        return 1;
    }

    // Calibrate baseline latencies (when no contention)
    microseconds next_time_mus = start_time_mus + microseconds(BASELINE_INTERVAL);
    if (my_id % 2)   next_time_mus += five_ms;
    read_bit(cacheline_addr, next_time_mus - ten_ms, true, my_id, 0, 0);

    // Start protocol phases
    bool advertised = false;
    printf("[Lambda: %3d] Phase, Position, Bit, Sent, Read, Mean Lat, Base Lat\n", my_id);
    for (int phase = 0; phase < max_phases; phase++) {
        bool advertising = !advertised;
        int id_read = 0;

        for (int bit_pos = MAX_BITS_IN_ID - 1; bit_pos >= 0; bit_pos--) {
            bool my_bit = my_id & (1 << bit_pos);
            int bit_read;

            poll_wait(next_time_mus);
            next_time_mus += bit_duration;
            if (my_id % 2)   next_time_mus += five_ms;

            if (advertising && my_bit) {
                write_bit(cacheline_addr, next_time_mus - ten_ms);      // Write until 10ms before next interval
                bit_read = 1;                                           // When writing a bit, assume that bit read is one.
            }
            else {
                bit_read = read_bit(cacheline_addr, next_time_mus - ten_ms, false, my_id, phase, bit_pos);
            }

            /* Stop advertising if my bit is 0 and bit read is 1 i.e., someone else has higher id than mine */
            if (advertising && !my_bit && bit_read)
                advertising = false;

            id_read = (2 * id_read) + bit_read;     // We get bits in most to least significant order
            printf("[Lambda: %3d] %3d %9d %4d %5d %5d %9lu %9lu \n", 
                my_id, phase, bit_pos, my_bit, advertising && my_bit, bit_read, 
                advertising && my_bit ? 0 : last_mean, base_mean_latency);                          /** COMMENT OUT IN REAL RUNS **/
        }

        if (id_read == 0)               // End of protocol
            break;

        if (id_read == my_id)           // My part is done, I will just listen from now on.
            advertised = true;
        
        printf("[Lambda-%d] Phase %d, Id read: %d\n", my_id, phase, id_read);      /** COMMENT OUT IN REAL RUNS **/
    }

    return 0;
}

int main(int argc, char** argv)
{  
    std::string role;
    int id = 0;
    long start_time_secs;
    bool parsed_id = false, parsed_time = false;

    /* Parse Lambda Id */
    if (argc >= 2) {
        std::istringstream iss(argv[1]);
        if (iss >> id && id > 0 && id < (1<<MAX_BITS_IN_ID)) {
            printf("Starting lambda: %d\n", id);
            parsed_id = true;
        }
    }
    if (!parsed_id) {
        printf("ERROR! Provide proper id (%d) in [1, %d)\n", id, 1<<MAX_BITS_IN_ID);
        return 1;
    }

    /* Parse protocol start time (Secs since epoch) */
    if (argc >= 3) {       
        std::istringstream iss(argv[2]);
        if (iss >> start_time_secs && start_time_secs >= 1500000000) {
            printf("Protocol start time (secs since epoch): %lu\n", start_time_secs);
            parsed_time = true;
        }
    }
    if (!parsed_time) {
        printf("ERROR! Provide proper arg 2: time in seconds since epoch\n");
        return 1;
    }

    /* Using a good seed that is different enough for each lambda is critical as 
    * randomness is used in sampling intervals. If these intervals are not random, 
    * lambdas sample the membus at the same time resulting in membus contention 
    * even if none of the lambdas are actually thrashing 
    * NOTE: Purely time-based seed will backfire for applications that start together */
    unsigned int seed = std::time(nullptr) ^ (getpid()<<16 ^ (id << 16));
    // unsigned int seed = good_seed(id);
    std::srand(seed);

    /* Check clock precision on the system is at least micro-seconds (TODO: Does this give real precision?) */
    int prec;
    constexpr auto num = Clock::period::num;
    constexpr auto den = Clock::period::den;
    if (den <= num)                 prec = 0;   // seconds or more
    if (den/num >= 1000)            prec = 1;   // milliseconds or less
    if (den/num >= 1000000)         prec = 2;   // microseconds or less
    if (den/num >= 1000000000)      prec = 3;   // nanoseconds (or less?)
    if (prec < 2) {
        printf("Need atleast microsecond precision on wallclock time");
        return 1;
    }
    printf("clock precision level: %d\n", prec);

    /* Get cacheline address */
    uint32_t* addr = get_cache_line_straddled_address();
    if (addr == NULL)
        return 1;

    /* Run id exchange protocol */
    microseconds start_time_mus = duration_cast<microseconds>(std::chrono::seconds(start_time_secs));
    run_membus_protocol(id, start_time_mus, 4, addr);

    return 0;
}