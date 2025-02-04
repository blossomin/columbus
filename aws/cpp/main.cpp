/* Main handler for the lambda implementing neighbor discovery and 
 * covert channel communication on AWS
 * 
 * Author: Anil Yelam
 */


#include <aws/core/Aws.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/ConsoleLogSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/lambda-runtime/runtime.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iomanip>
#include <chrono> 

#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <ifaddrs.h>

#include "util.h"
#include "RSJparser.tcc"

using namespace aws::lambda_runtime;
char const TAG[] = "MEMBUS";

/****** Assumptions around clocks ********
 * 1. Clocks have a precision of microseconds or lower.
 * 2. Context switching/core switching would not affect the monotonicity or steadiness of the clock on millisecond scales
 */

#define SAMPLES_PER_SECOND       1000           /* Sampling rate: This is limited by 1) noise under too much sampling            */
                                                /* and 2) post-processing computation (KS test) done for each sample at every bit*/
#define MAX_BIT_DURATION_SECS    5
#define MUS_PER_SEC              1000000
#define MAX_PHASES               15
#define DEFAULT_KS_MEAN_CUTOFF   3.0            /* KS Statistic more than this would indicate enough contention to infer 1-bit   */

using Clock = std::chrono::high_resolution_clock;
using microseconds = std::chrono::microseconds;
using seconds = std::chrono::seconds;
using std::chrono::duration;
using std::chrono::duration_cast;

/* Defined in timsort.cpp (Bad practice using extern!) */
extern void timSort(int64_t arr[], int n);
/* Defined in kstest.cpp */
extern double kstest_mean(int64_t* sample1, int size1, bool is_sorted1, int64_t* sample2, int size2, bool is_sorted2);

/* Save all lambdas invoked in this container. */
std::vector<std::string> lambdas;

/* Logging */
bool log_ = true;
std::vector<std::string> logs;
char lbuffer[1000];
#define lprintf(...) {                    \
   if (log_) {                            \
      sprintf(lbuffer, __VA_ARGS__);      \
      logs.push_back(lbuffer);            \
   }                                      \
}
// AWS_LOGSTREAM_INFO(TAG, lbuffer);   \        /* Directs every print statement to AWS Cloudwatch, TODO: include it in log_ conditional only when debugging */

typedef struct {
   int num_phases;
   int ids[MAX_PHASES];
} result_t;


/* A fast but good enough pseudo-random number generator. Good enough for what? */
/* Courtesy of https://stackoverflow.com/questions/1640258/need-a-fast-random-generator-for-c */
uint64_t rand_xorshf96(void) {          //period 2^96-1
    static uint64_t x=123456789, y=362436069, z=521288629;
    uint64_t t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;
    return z;
}


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
      lprintf("Found urandom file!\n");
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

/*hex string to bool array */
const char* hex_char_to_bin(char c)
{
    // TODO handle default / error
    switch(toupper(c))
    {
        case '0': return "0000";
        case '1': return "0001";
        case '2': return "0010";
        case '3': return "0011";
        case '4': return "0100";
        case '5': return "0101";
        case '6': return "0110";
        case '7': return "0111";
        case '8': return "1000";
        case '9': return "1001";
        case 'A': return "1010";
        case 'B': return "1011";
        case 'C': return "1100";
        case 'D': return "1101";
        case 'E': return "1110";
        case 'F': return "1111";
    }
}

std::string hex_str_to_bin_str(const std::string& hex)
{
    // TODO use a loop from <algorithm> or smth
    std::string bin;
    for(unsigned i = 0; i != hex.length(); ++i)
       bin += hex_char_to_bin(hex[i]);
    return bin;
}


/* Check if program is not past specified time yet */
inline bool within_time(microseconds time_pt) {
   microseconds now = duration_cast<microseconds>(Clock::now().time_since_epoch());
   bool within_limit = now.count() < time_pt.count();
   // if (!within_limit) {
   //     // Prints any bad timeoverruns
   //     int64_t ms = (now.count() - time_pt.count()) / 1000;
   //     if (ms > 10) lprintf("Exceeded time limit by more than 10ms: %lu milliseconds\n", ms);
   // }
   return within_limit;
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

/* Finds an address on heap that falls on consecutive cache lines */
uint64_t* get_cache_line_straddled_address()
{
   uint64_t *arr;
   int i, size;
   uint64_t *addr;

   /* Figure out last-level cache line size of this system */
   long cacheline_sz = sysconf(_SC_LEVEL3_CACHE_LINESIZE);
   if (!cacheline_sz) {
      // If L3 does not exist, try L2.
      cacheline_sz = sysconf(_SC_LEVEL2_CACHE_LINESIZE);
      if (!cacheline_sz) {
            lprintf("ERROR! Cannot find the cacheline size on this machine\n");
            return NULL;
      }
   }
   lprintf("Cache line size: %ld B\n", cacheline_sz);

   /* Allocate an array that spans multiple cache lines */
   size = 10 * cacheline_sz / sizeof(uint64_t);
   arr = (uint64_t*) malloc(size * sizeof(uint64_t));
   for (i = 0; i < size; i++ ) arr[i] = 1; 

   /* Find the second cacheline boundary */
   bool first = true;
   for (i = 1; i < size; i++) {
      long address = (long)(arr + i);
      if (address % cacheline_sz == 0) {
         if (!first)
            break;
         first = false;
      }
   }

   if (i == size) {
      lprintf("ERROR! Could not find a cacheline boundary in the array.\n");
      return NULL;
   }
   else {
      addr = (uint64_t*)((uint8_t*)(arr+i-1) + 4);
      lprintf("Found an address that falls on two cache lines: %p\n", (void*) addr);
   }

   // lprintf("Membus latencies with sliding address:\n");
   // for (int j = -8; j < 16; j++) {
   //    uint64_t* cacheline = (uint64_t*)((uint8_t*)(arr+i-1) + j);
   //    rdtsc();
   //    __atomic_fetch_add(cacheline, 1, __ATOMIC_SEQ_CST);
   //    rdtsc1();
   //    uint64_t start = ( ((int64_t)cycles_high << 32) | cycles_low );
   //    uint64_t end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
   //    lprintf("%d,%lu\n", j, (end - start));
   // }

   return (uint64_t*)addr;
}

/************************** NEIGHBOR DISCOVERY PROTOCOL IMPLEMENTATION ******************************************************/

/* Buffers to save andsamples of latencies for post-experiment analysis */
#define MAX_SAMPLES (MAX_BIT_DURATION_SECS*SAMPLES_PER_SECOND)
bool save_samples;
int64_t samples[MAX_SAMPLES];
int64_t base_readings[MAX_SAMPLES];
int base_readings_len = 0;
int64_t bit1_readings[MAX_SAMPLES];
int bit1_readings_len = 0;
double bit1_pvalue;
int64_t bit0_readings[MAX_SAMPLES];
int bit0_readings_len = 0;
double bit0_pvalue;

/* Samples membus lock latencies periodically to infer contention. If calibrate is set, uses these readings as baseline. */
int read_bit(uint64_t* addr, microseconds release_time_mus, int bit_duration_secs, 
   bool calibrate, int id, int phase, int round, double* ksvalue)
{
   int i;
   microseconds one_ms = microseconds(1000);
   microseconds ten_ms = microseconds(10000);
   microseconds next = duration_cast<microseconds>(Clock::now().time_since_epoch());
   // int num_samples = calibrate ? BASELINE_SAMPLES : SAMPLES_PER_BIT;
   // int interval_mus = calibrate ? BASELINE_INTERVAL : BIT_INTERVAL_MUS;
   int num_samples = bit_duration_secs * SAMPLES_PER_SECOND;
   int interval_mus = bit_duration_secs * MUS_PER_SEC;
   double sampling_rate_mus = SAMPLES_PER_SECOND * 1.0 / MUS_PER_SEC;

   /* Release a bit early to avoid overruns (and allow for post-processing) */
   release_time_mus -= ten_ms;

   int64_t start, end, mean, count = 0;
   // lprintf("%ld, %ld,\n", next.count(), release_time_mus.count());      /** COMMENT OUT IN REAL RUNS **/
   for (i = 0; i < num_samples && within_time(release_time_mus); i++)
   {   
      // Get a sample
      rdtsc();
      __atomic_fetch_add(addr, 1, __ATOMIC_SEQ_CST);
      rdtsc1();

      start = ( ((int64_t)cycles_high << 32) | cycles_low );
      end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
      samples[i] = (end - start);
      count++;
         
      next += microseconds((int)next_poisson_time(sampling_rate_mus));
      poll_wait(next);
   }

   if (calibrate) {
      memcpy(base_readings, samples, count * sizeof(samples[0]));
      base_readings_len = count;

      /* Sort the base sample so we don't have to do it every time */
      timSort(base_readings, base_readings_len);
      return 0;   //not used
   }

   *ksvalue = kstest_mean(base_readings, base_readings_len, true, samples, count, false);
   if(*ksvalue >= DEFAULT_KS_MEAN_CUTOFF) {
      if (save_samples && bit1_readings_len == 0){
         memcpy(bit1_readings, samples, count * sizeof(samples[0]));
         bit1_readings_len = count;
         bit1_pvalue = *ksvalue;
      } 
   }
   else {
      if (save_samples && bit0_readings_len == 0){
         memcpy(bit0_readings, samples, count * sizeof(samples[0]));
         bit0_readings_len = count;
         bit0_pvalue = *ksvalue;
      } 
   }

   return *ksvalue >= DEFAULT_KS_MEAN_CUTOFF;        /* Need to figure out the threshold that works for current platform */
}

/* Causes membus locking contention until a certain time */
void write_bit(uint64_t* addr, microseconds release_time_mus)
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

/* Execute the info exchange protocol where all participating lambdas on a same machine
* learn the id of one (max-id) lambda in each phase. Runs till all lambdas know each 
* other or for a specified number of phases
* If repeat_phases is true, protocol repeats the first phase i.e., in every phase all 
* lambdas try to agree on the same max lambda id  */
result_t* run_membus_protocol(int my_id, microseconds start_time_mus, int max_phases, int max_bits_in_id, int bit_duration_secs, uint64_t* cacheline_addr, bool repeat_phases, double* time_secs)
{
   double pvalue;

   std::clock_t protocol_start, protocol_end;
   microseconds bit_duration = microseconds(bit_duration_secs * MUS_PER_SEC);
   microseconds five_ms = microseconds(5000);
   microseconds ten_ms = microseconds(10000);
   microseconds phase_duration = bit_duration * max_bits_in_id;
   result_t* result = (result_t*) malloc(sizeof(result_t));
   result->num_phases = 0;

   // Sync with other lambdas a few milliseconds early
   if (poll_wait(start_time_mus - ten_ms)){
      lprintf("ERROR! Already past the intitial sync point, bad run for current lambda.\n");
      return result;
   }

   /* Record start timestamp */
   microseconds begin = duration_cast<microseconds>(Clock::now().time_since_epoch());

   /* Calibrate baseline latencies (when no contention) */
   microseconds next_time_mus = start_time_mus + bit_duration;
   read_bit(cacheline_addr, next_time_mus - ten_ms, bit_duration_secs, true, my_id, 0, 0, &pvalue);

   /* Start protocol phases */
   bool advertised = false;
   lprintf("[Lambda-%3d] Phase, Position, Bit, Sent, Read, Lat Size, Lat Mean, Lat Std, Lat Max, Lat Min, Base Size, Base Mean, Base Std, KSValue\n", my_id);

   for (int phase = 0; phase < max_phases; phase++) {
      bool advertising = !advertised;
      int id_read = 0;

      for (int bit_pos = max_bits_in_id - 1; bit_pos >= 0; bit_pos--) {
            bool my_bit = my_id & (1 << bit_pos);
            int bit_read;

            poll_wait(next_time_mus);
            next_time_mus += bit_duration;
            // if (my_id % 2)   next_time_mus += five_ms;

            if (advertising && my_bit) {
               write_bit(cacheline_addr, next_time_mus - ten_ms);      // Write until 10ms before next interval
               bit_read = 1;                                           // When writing a bit, assume that bit read is one.
            }
            else {
               bit_read = read_bit(cacheline_addr, next_time_mus - ten_ms, bit_duration_secs, false, my_id, phase, bit_pos, &pvalue);
            }

            /* Stop advertising if my bit is 0 and bit read is 1 i.e., someone else has higher id than mine */
            if (advertising && !my_bit && bit_read)
               advertising = false;

            id_read = (2 * id_read) + bit_read;     // We get bits in most to least significant order

            /* CAUTION: Below print statement is used in log analysis, changing format may break post-experiment analysis scripts */
            // lprintf("[Lambda-%3d] %3d %9d %4d %5d %5d %9d %9lu %8lu %8lu %8lu %10d %10lu %9lu %2.15f\n", 
            //    my_id, phase, bit_pos, my_bit, advertising && my_bit, bit_read, 
            //    last_sample.size,
            //    advertising && my_bit ? 0 : last_sample.mean, 
            //    advertising && my_bit ? 0 : (long) sqrt(last_sample.variance), 
            //    advertising && my_bit ? 0 : last_sample.max, 
            //    advertising && my_bit ? 0 : last_sample.min, 
            //    base_sample.size, base_sample.mean, (long) sqrt(base_sample.variance), pvalue);              /** COMMENT OUT IN REAL RUNS **/
      }

      lprintf("[Lambda-%d] Phase %d, Id read: %d\n", my_id, phase, id_read);      /** COMMENT OUT IN REAL RUNS **/

      if (id_read == 0)               // End of protocol
            break;

      result->ids[result->num_phases] = id_read;
      result->num_phases++;

      if (!repeat_phases && id_read == my_id)           // My part is done, I will just listen from now on.
            advertised = true;
   }

   /* Record end timestamp */
   microseconds end = duration_cast<microseconds>(Clock::now().time_since_epoch());
   *time_secs = (end - begin).count() * 1.0 / MUS_PER_SEC;
   lprintf("The protocol ran for %.2lf seconds.\n", *time_secs);

   return result;
}


/************************** COVERT CHANNEL IMPLEMENTATION ******************************************************/
/* Define everything related to data transmission over the covert channel here */

#define DEFAULT_CHANNEL_UPTIME_SECS                   5           // Transfer data for 5 secs                
#define CHANNEL_BIT_INTERVAL_MUS                      1000        // Spend 5ms per bit
#define ATOMIC_OPS_BATCH_SIZE                         10          // do 10 ops before checking timeout
#define MEM_ACCESS_LATENCY_WITH_LOCKING_THRESHOLD     200         // affect on regular memory accesses by membus locking
#define MEM_ACCESS_LATENCY_WITH_LOCKING_MAX           2000        // anything above this number is a silly outlier caused due to context switching, etc

/* This threshold separates the 0 and 1 bit on the receiver. The mean of base latencies without sender contention have been seen to range from 9250 to 10000
 * Ref: plots/samples_stats_02-02-22-43.pdf, plots/samples_stats_02-02-22-53.pdf  */
#define ATOMIC_OPS_LATENCY_WITH_LOCKING_THRESHOLD     10500
#define ATOMIC_OPS_LATENCY_WITH_LOCKING_MAX           20000       // anything above this number is a silly outlier caused due to context switching, etc


/* Takes a large sized buffer, performs a number of random accesses 
   and reports the time (randomized to increase the possiblity of a cache miss).
   NOTE: It seems like the GCC optimization options are important to properly measure time
 */
#pragma GCC push_options
#pragma GCC optimize ("O0")
uint64_t perform_random_access(void* buffer, size_t buf_size, int num_accesses) {         // TODO: Inline it?
   uint64_t src = 100, rand, start, end;
   rdtsc();
   for(int i = 0; i < num_accesses; i++) {
      rand = rand_xorshf96() % buf_size;
      memcpy(buffer + rand, &src, sizeof(uint64_t));
   }
   rdtsc1();

   start = ( ((int64_t)cycles_high << 32) | cycles_low );
   end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
   return (end - start);
}
#pragma GCC pop_options

/* Takes a large sized buffer, performs a number of random accesses 
   and reports the time (randomized to increase the possiblity of a cache miss).
   NOTE: It seems like the GCC optimization options are important to properly measure time
 */
inline uint64_t perform_exotic_ops(uint64_t* cacheline_addr, int num_accesses) {
   uint64_t start, end;
   rdtsc();
   for (int i = 0; i < ATOMIC_OPS_BATCH_SIZE; i++)
      __atomic_fetch_add(cacheline_addr, 1, __ATOMIC_SEQ_CST);
   rdtsc1();

   start = ( ((int64_t)cycles_high << 32) | cycles_low );
   end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
   return (end - start);
}

/* Send a data segment; returns number of erasures detected  */
int send_data(std::vector<bool> data, int nbits, int bit_interval_mus, microseconds start_time_mus, uint64_t* cacheline_addr, void* big_buffer, size_t big_buf_size, int threshold) {
   microseconds bit_start_mus, bit_end_mus;
   int num_erasures, erasures[nbits];
   uint64_t start, end, access_cycles, access_count, access_avg, access_thresh, cycles;

   /* Wait till the startpoint */
   microseconds one_ms = microseconds(1000);
   if (poll_wait(start_time_mus - one_ms)){
      lprintf("ERROR! Already past the start point for covert channel data.\n");
      return 1;
   }

   num_erasures = 0;
   lprintf("Sending bits.. bit interval=%d mus, threshold=%d\n", bit_interval_mus, threshold);
   for(int bit_idx = 0; bit_idx < nbits; bit_idx++) { 
      bit_start_mus = start_time_mus + microseconds(bit_idx * bit_interval_mus);
      bit_end_mus = bit_start_mus + microseconds(bit_interval_mus);

      /* Checking time takes order of micro-seconds, so do it sparesely to not affect contention-causing.
      * assuming each locking op costs few microseconds, check time every few hundred microseconds
      * Release a bit early to avoid overruns */
      microseconds ten_mus = microseconds(10);
      bit_end_mus -= ten_mus;
      access_cycles = 0;
      access_count = 0;
      bit0_readings_len = 0;     /* FIXME: I'm reusing NDP sample buffers to save channel samples as well, fix it! */
      base_readings_len = 0;
      while (within_time(bit_end_mus))
      {  
         /* if 1 bit, lock the mem bus using atomic ops; else, perform regular memory accesses */
         if (data[bit_idx]){
            cycles = perform_exotic_ops(cacheline_addr, ATOMIC_OPS_BATCH_SIZE);
            if (cycles / ATOMIC_OPS_BATCH_SIZE > ATOMIC_OPS_LATENCY_WITH_LOCKING_MAX)  continue;
            access_cycles += cycles;
            if(bit0_readings_len < MAX_SAMPLES)    bit0_readings[bit0_readings_len++] = cycles / ATOMIC_OPS_BATCH_SIZE;
         }
         else{
            /* TODO: Don't do this for now; suspecting that this may affect atomic op latencies and doesn't really help with the protocol */
            // cycles = perform_random_access(big_buffer, big_buf_size, ATOMIC_OPS_BATCH_SIZE);
            // if (cycles / ATOMIC_OPS_BATCH_SIZE > MEM_ACCESS_LATENCY_WITH_LOCKING_MAX)  continue;
            // access_cycles += cycles;
            // if(base_readings_len < MAX_SAMPLES)    base_readings[base_readings_len++] = cycles / ATOMIC_OPS_BATCH_SIZE;
         } 
         access_count += ATOMIC_OPS_BATCH_SIZE;
      }

      if (!data[bit_idx])
         continue;

      if (access_count == 0) {
         /* We are past the time for this bit, perhaps the sender was descheduled */
         erasures[num_erasures++] = bit_idx;
         continue;
      }
      access_avg = access_cycles / access_count;
      // printf("Send bit %d, %d - latency: %lu, count: %d\n", bit_idx, data[bit_idx] ? 1 : 0, access_avg, access_count);

      // if access less then threshold, its an erasure
      if (access_avg < threshold) {
         /* Receiver was not listening during this time */
         erasures[num_erasures++] = bit_idx;
      }
   }

   // lprintf("Sender sent: ");
   // for (int i = 0; i < nbits; i++)  lprintf("%d ", data[i] ? 1 : 0);
   // lprintf("\nSender - erasures:%d\n", num_erasures);
   return num_erasures;
}


/* Receive a data segment; returns number of erasures detected */
int receive_data(std::vector<bool>* data, int nbits, int bit_interval_mus, microseconds start_time_mus, uint64_t* cacheline_addr, int threshold) {
   microseconds bit_start_mus, bit_end_mus;
   int num_erasures, erasures[nbits];
   uint64_t start, end, access_cycles, access_count, access_avg, cycles;

   /* Wait till the startpoint */
   microseconds one_ms = microseconds(1000);
   if (poll_wait(start_time_mus - one_ms)){
      printf("ERROR! Already past the start point for covert channel data.\n");
      return 1;
   }

   num_erasures = 0;
   lprintf("Receiving bits.. bit interval=%d mus, threshold=%d\n", bit_interval_mus, threshold);
   for(int bit_idx = 0; bit_idx < nbits; bit_idx++) { 
      bit_start_mus = start_time_mus + microseconds(bit_idx * bit_interval_mus);
      bit_end_mus = bit_start_mus + microseconds(bit_interval_mus);

      /* Checking time takes order of micro-seconds, so do it sparesely to not affect contention-causing.
      * assuming each locking op costs few microseconds, check time every few hundred microseconds
      * Release a bit early to avoid overruns */
      microseconds ten_mus = microseconds(10);
      bit_end_mus -= ten_mus;
      access_cycles = 0;
      access_count = 0;
      bit1_readings_len = 0;     /* FIXME: I'm reusing NDP sample buffers to save channel samples as well, fix it! */
      while (within_time(bit_end_mus))
      {  
         /* receiver just performs exotic ops */
         cycles = perform_exotic_ops(cacheline_addr, ATOMIC_OPS_BATCH_SIZE);
         access_cycles += cycles;
         if(bit1_readings_len < MAX_SAMPLES)    bit1_readings[bit1_readings_len++] = cycles / ATOMIC_OPS_BATCH_SIZE;
         access_count += ATOMIC_OPS_BATCH_SIZE;
      }

      if (access_count == 0) {
         /* We are past the time for this bit, perhaps the receiver was descheduled */
         erasures[num_erasures++] = bit_idx;
         data->push_back(false);
         continue;
      }
      access_avg = access_cycles / access_count;
      // printf("Recv bit %d - latency: %lu, count: %d\n", bit_idx, access_avg, access_count);
      data->push_back(access_avg >= threshold);
   }

   // lprintf("Recver rcvd: ");
   // for (int i = 0; i < nbits; i++)  lprintf("%d ", result[i] ? 1 : 0);
   // lprintf("\nRecver - erasures:%d\n", num_erasures);
   return num_erasures;
}

/************************** SYSTEM INFORMATION  ******************************************************/

/* Get comma-seperated MAC addresses of all interfaces */
std::string get_mac_addrs()
{
   char mac[50] = "";
   struct ifreq ifr;
   int s;
   if ((s = socket(AF_INET, SOCK_STREAM,0)) < 0) {
      lprintf("ERROR! Could not get socket for MAC address\n");
      return std::string("");
   }

   struct ifaddrs *addrs,*tmp;
   getifaddrs(&addrs);
   tmp = addrs;
   while (tmp)
   {
      if (tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_PACKET)
      {
         strcpy(ifr.ifr_name, tmp->ifa_name);
         if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
            lprintf("ERROR! Could not ioctl for MAC address\n");
            return std::string("");
         }
         
         unsigned char *hwaddr = (unsigned char *)ifr.ifr_hwaddr.sa_data;
         sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", hwaddr[0], hwaddr[1], hwaddr[2],
                     hwaddr[3], hwaddr[4], hwaddr[5]);
      }

      tmp = tmp->ifa_next;
   }

   freeifaddrs(addrs);
   close(s);
   return std::string(mac);
}

/* Get IP address associated with the socket interface 
 * Courtesy of https://stackoverflow.com/questions/49335001/get-local-ip-address-in-c
 */
std::string get_ipaddr(void) {
   int sock = socket(PF_INET, SOCK_DGRAM, 0);
   sockaddr_in loopback;
   if (sock == -1) {
      lprintf("ERROR! Could not get socket for IP address\n");
      return std::string("");
   }

   std::memset(&loopback, 0, sizeof(loopback));
   loopback.sin_family = AF_INET;
   loopback.sin_addr.s_addr = INADDR_LOOPBACK;   // using loopback ip address
   loopback.sin_port = htons(9);                 // using debug port

   if (connect(sock, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback)) == -1) {
      close(sock);
      lprintf("ERROR! Could not connect to socket for IP addr\n");
      return std::string("");
   }

   socklen_t addrlen = sizeof(loopback);
   if (getsockname(sock, reinterpret_cast<sockaddr*>(&loopback), &addrlen) == -1) {
      close(sock);
      lprintf("ERROR! Could not getsockname for IP addr\n");
      return std::string("");
   }

   close(sock);

   char buf[INET_ADDRSTRLEN];
   if (inet_ntop(AF_INET, &loopback.sin_addr, buf, INET_ADDRSTRLEN) == 0x0) {
      lprintf("ERROR! Could not inet_ntop for IP addr\n");
      std::string("");
   } else {
      return std::string(buf);
   }
}

/* Performs a CPU-bound operation and gets time taken for the operation. (A measure of how much CPU the process is getting...)
 * Turn off GCC optimizations for this piece of code as to avoid any funky changes to the CPU operation.
 */
#pragma GCC push_options
#pragma GCC optimize ("O0")
double get_cpu_cycles_per_operation() {
   int TIMES = 1e8;
   uint64_t x = 0, start, end;
   microseconds begin_time = duration_cast<microseconds>(Clock::now().time_since_epoch());

   rdtsc();
   for(int i = 0; i < TIMES; i++)  x += i;
   rdtsc1();

   start = ( ((int64_t)cycles_high << 32) | cycles_low );
   end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
       
   microseconds end_time = duration_cast<microseconds>(Clock::now().time_since_epoch());
   lprintf("Calculating CPU CPI took %.2lf seconds\n", (end_time - begin_time).count() * 1.0 / MUS_PER_SEC);

   return (end - start) * 1.0 / TIMES;
}
#pragma GCC pop_options

/* Get current date/time, format is YYYY-MM-DD.HH:mm:ss */
const std::string current_datetime() {
   time_t     now = time(0);
   struct tm  tstruct;
   char       buf[80];
   tstruct = *localtime(&now);
   strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
   return buf;
}

/* Write a string to S3 bucket */
bool write_to_s3(Aws::S3::S3Client const& client, std::string const& bucket, std::string const& key, std::string data) {
   Aws::S3::Model::PutObjectRequest request;
   request.SetBucket(bucket);
   request.SetKey(key);

   const std::shared_ptr<Aws::IOStream> input_data = Aws::MakeShared<Aws::StringStream>("");
   *input_data << data.c_str();
   request.SetBody(input_data);

   // Write to S3 and return result
   try {
      Aws::S3::Model::PutObjectOutcome outcome = client.PutObject(request);
      if (outcome.IsSuccess()) {   
         lprintf("Added object %s to s3 bucket %s\n", key.c_str(), bucket.c_str());
         return true;
      }
      else {
         lprintf("Error adding object %s to s3 bucket %s: %s\n", key.c_str(), bucket.c_str(), 
            outcome.GetError().GetMessage().c_str());
         return false;
      }
   }
   catch (std::exception e){
      lprintf("Exception saving to S3: %s", e.what());
      return false;
   }

   return true;
}

/************************** MAIN ENTRY ******************************************************/

invocation_response my_handler(invocation_request const& request, const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& credentialsProvider, 
   const Aws::Client::ClientConfiguration& config)
{
   std::string start_time = current_datetime();
   int id, max_phases, max_bits, bit_duration_secs;
   long start_time_secs;
   bool success = true, sysinfo, return_data, setup_channel, repeat_phases;
   std::string error, s3bucket, s3key, guid, chdata;
   result_t* result = NULL;
   double protocol_time = 0;
   int erasures, num_bits, sender_id, receiver_id, rate_bps, access_threshold, chdatalen;
   bool channel_created = false;
   std::vector<bool> data;

   AWS_LOGSTREAM_INFO(TAG, "Start");

   /* Clear any global arrays before using. I don't know how but these arrays persist 
    * information across lambda invocations. AMAZING, isn't it?
    * We could use this to detect if a lambda underwent a warm start or a cold start */
   logs.clear();

   /* Parse request body for arguments */
   try {     
      RSJresource body(request.payload);
      if (body["body"].exists()) {
         // See if body is nested in payload
         std::string escaped_body = body["body"].as<std::string>("");
         body = RSJresource(util::unescape_json(escaped_body));
      }
      id = body["id"].as<int>(0);
      start_time_secs = body["stime"].as<int>(0);
      log_ = body["log"].as<bool>(false);                   // include logs in response  
      save_samples = body["samples"].as<bool>(false);       // include a sample of latencies in response
      max_phases = body["phases"].as<int>(1);               // run 1 phase by default
      repeat_phases = body["repeat_phases"].as<int>(true);  // repeat phases by default (i.e., always run the "first iteration" of the protocol advertising the max id)
      max_bits = body["maxbits"].as<int>(8);                // Assume maximum of 8 bits in ID by default
      bit_duration_secs = body["bitduration"].as<int>(1);   // takes 1 second for communicating each bit by default. phases*maxbits*bitduration gives total time
      return_data = body["return_data"].as<bool>(false);    // return data in API response. Stored to S3 by default.
      s3bucket = body["s3bucket"].as<std::string>("");      // s3 bucket
      s3key = body["s3key"].as<std::string>("");            // s3 key
      guid = body["guid"].as<std::string>("");              // globally unique id for this lambda (across experiments)
      setup_channel = body["channel"].as<bool>(false);      // setup covert channel and measure its bandwidth after identifying neighbors 
      rate_bps = body["rate"].as<int>(1000000/CHANNEL_BIT_INTERVAL_MUS);      // covert channel rate. default is 1000 bps
      access_threshold = body["threshold"].as<int>(ATOMIC_OPS_LATENCY_WITH_LOCKING_THRESHOLD);      // threshold at which we call a received bit a 1 or 0
      chdata = body["data"].as<std::string>("");            // custom data to send on the covert channel
      chdatalen = body["datalen"].as<int>(0);               // length of custom data (IN BITS) to send on the covert channel
   }
   catch(std::exception& e){
      success = false;
      error = "INVALID_BODY";
      lprintf("Could not parse request body\n");
   }

   lprintf("Starting lambda %d (GUID: %s) at %s", id, guid.c_str(), start_time.c_str());
   lambdas.push_back(guid);
 
   #if __cplusplus==201402L
   lprintf("C++14\n");
   #elif __cplusplus==201103L
   lprintf("C++11\n");
   #else
   lprintf("C++\n");
   #endif

   // Test availability of s3 bucket
   if (success && !s3bucket.empty()) { 
      Aws::S3::S3Client client(credentialsProvider, config);
      write_to_s3(client, s3bucket, "temp", "Some data..");
   }

   if (success && id <= 0 || id >= (1<<max_bits)) {
      success = false;
      error = "INVALID_ID";
      lprintf("Id is not provided or invalid (should be in [1, %d)\n", 1<<max_bits);
   }

   if (success && (bit_duration_secs < 1  || bit_duration_secs > MAX_BIT_DURATION_SECS)) {
      success = false;
      error = "INVALID_BIT_DURATION";
      lprintf("Bit duration is invalid (should be in [1, %d]\n", MAX_BIT_DURATION_SECS);
   }
     
   if (setup_channel && (repeat_phases || max_phases < 2)) {
      success = false;
      error = "INVALID_CHANNEL_PARAMS";
      lprintf("To set up a covert channel, need to run for atleast two `distinct` phases "
               "(i.e., max_phases >= 2 and repeat_phases=false)\n");
   }
    
   // if (setup_channel && (rate_bps <= 0 || 1000000 % rate_bps != 0)) {
   if (setup_channel && (rate_bps <= 0)) {
      success = false;
      error = "INVALID_CHANNEL_RATE";
      lprintf("Channel rate must be a positive number and a factor of 10^6. Rate provided: %d bps\n", rate_bps);
   }

   if (setup_channel && chdata.size() != chdatalen) {
      lprintf("Provided channel data length %d does not match the actual length of the data %lu", chdatalen, chdata.size());
      error = "INVALID_CHANNEL_DATA";
      success = false;
   }

   seconds now = duration_cast<seconds>(Clock::now().time_since_epoch());
   if (success && start_time_secs <= now.count()) {    // Unix time in secs
      success = false;
      error = "INVALID_STIME";
      lprintf("Start time is not provided or is in the past");
   }

   /* Run membus protcol */
   if (success) {
      /* Using a good seed that is different enough for each lambda is critical as 
      * randomness is used in sampling intervals. If these intervals are not random, 
      * lambdas sample the membus at the same time resulting in membus contention 
      * even if none of the lambdas are actually thrashing 
      * NOTE: Purely time-based seed will backfire for applications that start together */
      unsigned int seed = std::time(nullptr) ^ (getpid()<<16 ^ (id << 16));
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
         lprintf("Need atleast microsecond precision on wallclock time");
         error = "CLOCK_PRECISION_LOW";
         success = false;
      }
      lprintf("clock precision level: %d\n", prec);

      /* Get cacheline address */
      uint64_t* addr = get_cache_line_straddled_address();
      if (addr == NULL){
         lprintf("Cannot find cacheline straddled address");
         error = "NO_CACHELINE_ADDR";
         success = false;
      }

      if (success) {
         /* Run id exchange protocol */
         try {
            AWS_LOGSTREAM_INFO(TAG, "Running");
            microseconds start_time_mus = duration_cast<microseconds>(seconds(start_time_secs));
            result = run_membus_protocol(id, start_time_mus, max_phases, max_bits, bit_duration_secs, addr, repeat_phases, &protocol_time);
         }
         catch (std::exception e){
            lprintf("Exception in membus protocol execution: %s", e.what());
            error = "MEMBUS_CRASH";
            success = false;
         }

         /*Set up a really simple covert channel and measure bandwidth */
         if (success && setup_channel && result != NULL) {
            bool sender = false, receiver = false;
            if (result->ids[0] == id)  sender = true;
            if (result->num_phases > 1 && result->ids[1] == id)  receiver = true;
            if (result->num_phases < 2)   sender = receiver = false;    // no neighbors to talk to, ignore
            if (sender && receiver)       sender = receiver = false;    // this shouldn't occur, but just in case

            if (sender || receiver) {
               channel_created = true;
               sender_id = result->ids[0];
               receiver_id = result->ids[1];
               lprintf("Setting up channel between lambdas %d and %d!\n", sender_id, receiver_id);   

               num_bits = chdatalen == 0 ? DEFAULT_CHANNEL_UPTIME_SECS * rate_bps : chdatalen;
               base_readings_len = bit0_readings_len = bit1_readings_len = 0;       // FIXME: HACK to get some latency samples
               int channel_start_time = start_time_secs + (max_phases * max_bits * bit_duration_secs) + 5;
               microseconds start_time_mus = duration_cast<microseconds>(seconds(channel_start_time));

               if (sender){
                  /* Allocate a big buffer for regular memory accesses */
                  const size_t DUMMY_BUF_SIZE = pow(2,29);		// 1GB
                  void* dummy_buffer = NULL; //malloc(DUMMY_BUF_SIZE + sizeof(uint64_t));   
                  // memset(dummy_buffer, 1, DUMMY_BUF_SIZE);     // this is necessary to actually allocate memory
                  lprintf("Lambda %d: I'm a sender!\n", id);

                  /* Use custom data if provided; else generate random data to send */
                  if (chdata.empty()) {
                     int percent_ones = std::rand() % 10;
                     for(int i = 0; i < num_bits; i++)   data.push_back((std::rand() % 10) <= percent_ones);    // generate random data to send (with random number of ones)
                  }
                  else {
                     // Custom data is provided ias bit string, covert to vector bool
                     lprintf("Received custom data to send of length %lu\n", chdata.size())
                     for (char const &c: chdata)   data.push_back(c == '1');
                  }

                  erasures = send_data(data, num_bits, 1000000 / rate_bps, start_time_mus, addr, dummy_buffer, DUMMY_BUF_SIZE, access_threshold);
               }
               if (receiver){     
                  lprintf("Lambda %d: I'm a receiver!\n", id); 
                  erasures = receive_data(&data, num_bits, 1000000 / rate_bps, start_time_mus, addr, access_threshold);
               }
            }
            else {
               lprintf("No neighbors to talk to or not selected as channel participant\n");
            }
         }
      }
   }

   /* Prepare response body as JSON*/
   RSJresource body("{}");
   // body["request"] = util::escape_json(request.payload);     // Enable this for request payload debugging
   body["Id"] = id;
   body["Start Time"] =  start_time;
   body["End Time"] =   current_datetime();
   body["Request ID"] = request.request_id;

   /* Save results (return all fields whether applicable or not) */
   body["Success"] = success;
   body["Error"] = error;
   body["Protocol Time"] = (int)protocol_time;
   body["Phases"] =  result != NULL ? result->num_phases : 0;
   for (int i = 0; i < max_phases; i++) {
      body["Phase " + std::to_string(i+1)] = (result != NULL && i < result->num_phases) ? result->ids[i] : -1;
   }

   /* Save samples if specified */
   if (success && (save_samples || channel_created)) {
      std::string arr;
      std::stringstream ss1, ss2;

      if (base_readings_len > 0) {
         arr = "";
         for (int i = 0; i < base_readings_len; i++) {
            arr += std::to_string(base_readings[i]) + ',';
         }
         body["Base Sample"] = RSJresource(arr, true);
      }
      
      if (bit1_readings_len > 0) {
         arr = "";
         for (int i = 0; i < bit1_readings_len; i++) {
            arr += std::to_string(bit1_readings[i]) + ',';
         }
         body["Bit-1 Sample"] = RSJresource(arr, true);
      }
      if (save_samples) {
         ss1 << std::setprecision(15) << bit1_pvalue;
         body["Bit-1 Pvalue"] = ss1.str();
      }
      
      if (bit0_readings_len > 0) {
         arr = "";
         for (int i = 0; i < bit0_readings_len; i++) {
            arr += std::to_string(bit0_readings[i]) + ',';
         }
         body["Bit-0 Sample"] = RSJresource(arr, true);
      }
      if (save_samples) {
         ss2 << std::setprecision(15) << bit0_pvalue;
         body["Bit-0 Pvalue"] = ss2.str();   //ss.str to preserve precision
      }
   }

   /* Save covert channel info */
   if (setup_channel && channel_created) {
      body["Channel"] = RSJresource("{}");
      body["Channel"]["Role"] = sender_id == id ? "Sender" : "Receiver";
      body["Channel"]["Sender Id"] = sender_id;
      body["Channel"]["Receiver Id"] = receiver_id;
      body["Channel"]["Bits"] = num_bits;
      body["Channel"]["Rate"] = rate_bps;
      body["Channel"]["Erasures"] = erasures;
      
      /* Encode sent/received data */
      std::string arr = "";
      for (int i = 0; i < num_bits; i++) {
         arr += data[i] ? "1" : "0";
      }
      body["Channel"]["Data"] = RSJresource(arr, true);
   }

   /* Save some system info */
   /* Get MAC addresses and add to the buffer */ 
   body["MAC Address"] = get_mac_addrs();
   body["IP Address"] = get_ipaddr();
   /* Get boot id */
   std::ifstream ifs("/proc/sys/kernel/random/boot_id");
   std::string boot_id ( (std::istreambuf_iterator<char>(ifs) ), (std::istreambuf_iterator<char>()) );
   body["Boot ID"] = boot_id;
   body["CPU CPI"] = get_cpu_cycles_per_operation();

   /* Save all the lambas that previously used the current container */
   std::string arr;
   for (int i = 0; i < lambdas.size()-1 ; i++) {
      if (i == lambdas.size()-2)    arr += lambdas[i];
      else                          arr += lambdas[i] + ',';
   }
   body["Predecessors"] = RSJresource(arr, true);
   body["GUID"] = guid;

   /* Save logs to response */
   if (log_) {
      std::string logarr;
      for (std::vector<std::string>::const_iterator it = begin (logs); it != end (logs); ++it) {
         logarr += (*it) + '\t';
      }
      body["Logs"] = RSJresource(logarr, true);
   }

   /* Save response to s3 */
   if (!s3bucket.empty() && !s3key.empty()){
      /* WARNING: Always initialize S3 client object close to its usage. It expires after a while (100 seconds?) */
      lprintf("Writing result to S3 at s3://%s/%s", s3bucket.c_str(), s3key.c_str());
      Aws::S3::S3Client client(credentialsProvider, config);
      write_to_s3(client, s3bucket, s3key, body.as_str());
   }

   // WARNING: Do not log using lprintf after this point, corrupts logs array that is being written into S3
   AWS_LOGSTREAM_INFO(TAG, "Wrote result to S3");
   
   /* Prepare response with statuscode, headers and body
   * In the format required by Lambda Proxy Integration: 
   * https://aws.amazon.com/premiumsupport/knowledge-center/malformed-502-api-gateway/ */  
   RSJresource response("{}");
   response["statusCode"] = 200;
   response["headers"] = RSJresource("{}");
   response["body"] = RSJresource("");
   if (return_data) {
      std::string escaped_body = util::escape_json(body.as_str());
      response["body"] = RSJresource(escaped_body, true);  // don't parse escaped body as json object
   }

   /* send response */
   return invocation_response::success(response.as_str(), "application/json");
}


/* AWS Console Logger. Logs at CloudWatch > Log Groups > Lambda Name on the AWS console */
std::function<std::shared_ptr<Aws::Utils::Logging::LogSystemInterface>()> GetConsoleLoggerFactory()
{
    return [] {
        return Aws::MakeShared<Aws::Utils::Logging::ConsoleLogSystem>(
            "console_logger", Aws::Utils::Logging::LogLevel::Info);        // Use LogLevel::Trace for more verbose log.
    };
}

int main()  
{
    using namespace Aws;
    SDKOptions options;
    options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Trace;
    options.loggingOptions.logger_create_fn = GetConsoleLoggerFactory();
    InitAPI(options);
    {
        Client::ClientConfiguration config;
        config.region = Aws::Environment::GetEnv("AWS_REGION");
        config.caFile = "/etc/pki/tls/certs/ca-bundle.crt";

        auto credentialsProvider = Aws::MakeShared<Aws::Auth::EnvironmentAWSCredentialsProvider>(TAG);
        auto handler_fn = [&credentialsProvider, &config](aws::lambda_runtime::invocation_request const& req) {
            return my_handler(req, credentialsProvider, config);
        };
        run_handler(handler_fn);
    }
    ShutdownAPI(options);
    return 0;
}
