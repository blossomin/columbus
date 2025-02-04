/* Main file to run locally
 * No dependencies on AWS lambda SDK
 */


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
#include <netinet/in.h>
#include <net/if.h>
#include <stdio.h>
#include <ifaddrs.h>


#include "util.h"
#include "RSJparser.tcc"

char const TAG[] = "MEMBUS";

/****** Assumptions around clocks ********
 * 1. Clocks have a precision of microseconds or lower.
 * 2. Context switching/core switching would not affect the monotonicity or steadiness of the clock on millisecond scales
 */

#define SAMPLES_PER_SECOND       1000           /* Sampling rate: This is limited by 1) noise under too much sampling            */
                                                /* and 2) post-processing computation (KS test) done for each sample at every bit*/
#define MAX_BIT_DURATION_SECS    5
#define MUS_IN_ONE_SEC           1000000
#define MAX_PHASES               15
#define PVALUE_THRESHOLD         0.0
// #define PVALUE_THRESHOLD      0.0005
// #define PVALUE_THRESHOLD      0.0000001
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
      printf(lbuffer, __VA_ARGS__);      \
   }                                      \
}
// AWS_LOGSTREAM_INFO(TAG, lbuffer);   \        /* Directs every print statement to AWS Cloudwatch, include it in log_ loop only when debugging */


typedef struct {
   int size;
   int64_t mean;
   int64_t variance;
   int64_t min;
   int64_t max;
} sample_t;

typedef struct {
   int num_phases;
   int ids[MAX_PHASES];
} result_t;

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
   bool within_limit = now.count() < time_pt.count();
   // if (!within_limit) {
   //     // Prints any bad timeoverruns
   //     int64_t ms = (now.count() - time_pt.count()) / 1000;
   //     if (ms > 10) printf("Exceeded time limit by more than 10ms: %lu milliseconds\n", ms);
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


/* Removes outliers beyond 3 standard deviations, returns sample params */
inline sample_t prepare_sample(int64_t* data, int len, bool remove_outliers = true)
{
   if (len <= 0) {
      return {
            .size = 0,
            .mean = 0,
            .variance = 0,
            .min = 0,
            .max = 0
      };
   }

   if (len == 1) {
      return {
            .size = len,
            .mean = data[0],
            .variance = 0,
            .min = data[0],
            .max = data[0]
      };
   }

   /* Not using floating point arithmetic at the expense of precision
   * Should be fine as values are in thousands */
   int64_t sum = 0, varsum = 0, mean, var, max = 0, min = 10000000;
   for (int i = 0; i < len; i++) {
         sum += data[i];
         if (data[i] > max)   max = data[i];
         if (data[i] < min)   min = data[i];
   }
   mean = sum / len;
   for (int i = 0; i < len; i++)   varsum += (data[i] - mean) * (data[i] - mean);
   var = varsum / (len - 1);
   int count = len;

   if (remove_outliers) {
      /* X is an outlier if (X - mean) >= 3 * std, or (X - mean)^2 >= 9 * var - to avoid sqrt */
      int i, j;
      sum = 0;
      varsum = 0;
      for (i = 0, j = len - 1, count = 0; i <= j; count++) {
         int64_t sq_diff = (data[i] - mean) * (data[i] - mean);
         if (sq_diff  <= 9 * var) {
               // Keep it 
               sum += data[i];
               varsum += sq_diff;
               i++;
         }
         else {
               // Replace it with last element and continue
               data[i] = data[j];
               j--;
         }
      }
   }

   return {
      .size = count,
      .mean = sum / count,
      .variance = varsum / (count - 1),
      .min = min,
      .max = max
   };
}


/* Buffers to save samples of latencies for post-experiment analysis */
bool save_samples;
int64_t samples[MAX_BIT_DURATION_SECS*SAMPLES_PER_SECOND];
int64_t base_readings[MAX_BIT_DURATION_SECS*SAMPLES_PER_SECOND];
int base_readings_len = 0;
int64_t bit1_readings[MAX_BIT_DURATION_SECS*SAMPLES_PER_SECOND];
int bit1_readings_len = 0;
double bit1_pvalue;
int64_t bit0_readings[MAX_BIT_DURATION_SECS*SAMPLES_PER_SECOND];
int bit0_readings_len = 0;
double bit0_pvalue;

sample_t base_sample;
sample_t last_sample;

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
   int interval_mus = bit_duration_secs * MUS_IN_ONE_SEC;
   double sampling_rate_mus = SAMPLES_PER_SECOND * 1.0 / MUS_IN_ONE_SEC;

   /* Release a bit early to avoid overruns (and allow for post-processing) */
   release_time_mus -= ten_ms;

   int64_t start, end, mean, count = 0;
   // printf("%ld, %ld,\n", next.count(), release_time_mus.count());      /** COMMENT OUT IN REAL RUNS **/
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
      base_sample = prepare_sample(samples, count, false);
      memcpy(base_readings, samples, count * sizeof(samples[0]));
      base_readings_len = count;

      /* Sort the base sample so we don't have to do it every time */
      timSort(base_readings, base_readings_len);
      return 0;   //not used
   }

   *ksvalue = kstest_mean(base_readings, base_readings_len, true, samples, count, false);

   last_sample = prepare_sample(samples, count, false);
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
            printf("ERROR! Cannot find the cacheline size on this machine\n");
            return NULL;
      }
   }
   printf("Cache line size: %ld B\n", cacheline_sz);

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
      printf("ERROR! Could not find a cacheline boundary in the array.\n");
      return NULL;
   }
   else {
      addr = (uint64_t*)((uint8_t*)(arr+i-1) + 4);
      printf("Found an address that falls on two cache lines: %p\n", (void*) addr);
   }

   // printf("Membus latencies with sliding address:\n");
   // for (int j = -8; j < 16; j++) {
   //    uint64_t* cacheline = (uint64_t*)((uint8_t*)(arr+i-1) + j);
   //    rdtsc();
   //    __atomic_fetch_add(cacheline, 1, __ATOMIC_SEQ_CST);
   //    rdtsc1();
   //    uint64_t start = ( ((int64_t)cycles_high << 32) | cycles_low );
   //    uint64_t end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
   //    printf("%d,%lu\n", j, (end - start));
   // }

   return (uint64_t*)addr;
}

/* Execute the info exchange protocol where all participating lambdas on a same machine
* learn the id of one (max-id) lambda in each phase. Runs till all lambdas know each 
* other or for a specified number of phases
* If repeat_phases is true, protocol repeats the first phase i.e., in every phase all 
* lambdas try to agree on the same max lambda id  */
result_t* run_membus_protocol(int my_id, microseconds start_time_mus, int max_phases, int max_bits_in_id, int bit_duration_secs, uint64_t* cacheline_addr, bool repeat_phases)
{
   double pvalue;
   microseconds bit_duration = microseconds(bit_duration_secs * MUS_IN_ONE_SEC);
   microseconds five_ms = microseconds(5000);
   microseconds ten_ms = microseconds(10000);
   microseconds phase_duration = bit_duration * max_bits_in_id;
   result_t* result = (result_t*) malloc(sizeof(result_t));
   result->num_phases = 0;

   // Sync with other lambdas a few milliseconds early
   if (poll_wait(start_time_mus - ten_ms)){
      printf("ERROR! Already past the intitial sync point, bad run for current lambda.\n");
      return result;
   }

   // Calibrate baseline latencies (when no contention)
   microseconds next_time_mus = start_time_mus + bit_duration;
   // if (my_id % 2)   next_time_mus += five_ms;
   read_bit(cacheline_addr, next_time_mus - ten_ms, bit_duration_secs, true, my_id, 0, 0, &pvalue);

   // Start protocol phases
   bool advertised = false;

   printf("[Lambda-%3d] Phase, Position, Bit, Sent, Read, Lat Size, Lat Mean, Lat Std, Lat Max, Lat Min, Base Size, Base Mean, Base Std, KSValue\n", my_id);

   // for (int phase = 0; phase < max_phases; phase++) {
   for (int phase = 0; true; phase++) {
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
            printf("[Lambda-%3d] %3d %9d %4d %5d %5d %9d %9lu %8lu %8lu %8lu %10d %10lu %9lu %2.15f\n", 
               my_id, phase, bit_pos, my_bit, advertising && my_bit, bit_read, 
               last_sample.size,
               advertising && my_bit ? 0 : last_sample.mean, 
               advertising && my_bit ? 0 : (long) sqrt(last_sample.variance), 
               advertising && my_bit ? 0 : last_sample.max, 
               advertising && my_bit ? 0 : last_sample.min, 
               base_sample.size, base_sample.mean, (long) sqrt(base_sample.variance), pvalue);              /** COMMENT OUT IN REAL RUNS **/
      }

      printf("[Lambda-%d] Phase %d, Id read: %d\n", my_id, phase, id_read);      /** COMMENT OUT IN REAL RUNS **/
      std::cout << std::flush;

      // if (id_read == 0)               // End of protocol
      //       break;

      result->ids[result->num_phases] = id_read;
      result->num_phases++;

      if (!repeat_phases && id_read == my_id)           // My part is done, I will just listen from now on.
            advertised = true;
   }

   return result;
}

/* Get comma-seperated MAC addresses of all interfaces */
void get_mac_addrs(char* mac)
{
   struct ifreq ifr;
   int s;
   if ((s = socket(AF_INET, SOCK_STREAM,0)) < 0) {
      perror("socket");
      return;
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
            perror("ioctl");
            return;
         }
         
         unsigned char *hwaddr = (unsigned char *)ifr.ifr_hwaddr.sa_data;
         sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", hwaddr[0], hwaddr[1], hwaddr[2],
                     hwaddr[3], hwaddr[4], hwaddr[5]);
      }

      tmp = tmp->ifa_next;
   }

   freeifaddrs(addrs);
   close(s);
}


/* Get IP address associated with the socket interface 
 * Courtesy of https://stackoverflow.com/questions/49335001/get-local-ip-address-in-c
 */
std::string get_ipaddr(void) {
    int sock = socket(PF_INET, SOCK_DGRAM, 0);
    sockaddr_in loopback;
    if (sock == -1) {
        printf("ERROR! Could not get socket for IP address\n");
        return std::string("");
    }

    std::memset(&loopback, 0, sizeof(loopback));
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = INADDR_LOOPBACK;   // using loopback ip address
    loopback.sin_port = htons(9);                 // using debug port

    if (connect(sock, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback)) == -1) {
        close(sock);
        printf("ERROR! Could not connect to socket for IP addr\n");
        return std::string("");
    }

    socklen_t addrlen = sizeof(loopback);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&loopback), &addrlen) == -1) {
        close(sock);
        printf("ERROR! Could not getsockname for IP addr\n");
        return std::string("");
    }

    close(sock);

    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &loopback.sin_addr, buf, INET_ADDRSTRLEN) == 0x0) {
        printf("ERROR! Could not inet_ntop for IP addr\n");
        std::string("");
    } else {
        return std::string(buf);
    }
}

/* Performs a CPU-bound operation and gets time taken for the operation. (A measure of how much CPU the process is getting...) */
uint64_t get_cpu_cycles_for_task(){
   int TIMES = 1e6;
   uint64_t x = 0, start, end;
   rdtsc();
   for(int i = 0; i < TIMES; i++)  x += i;
   rdtsc1();

   start = ( ((int64_t)cycles_high << 32) | cycles_low );
   end = ( ((int64_t)cycles_high1 << 32) | cycles_low1 );
   lprintf("Summing %d times to %lu took %lu cycles\n", TIMES, x, (end-start));
   return (end - start);
}


/* Get current date/time, format is YYYY-MM-DD.HH:mm:ss */
const std::string current_datetime() {
   time_t     now = time(0);
   struct tm  tstruct;
   char       buf[80];
   tstruct = *localtime(&now);
   strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
   return buf;
}


/* Main entry point */
int main()
{
   std::string start_time = current_datetime();
   int id, max_phases, max_bits, bit_duration_secs;
   long start_time_secs;
   bool success = true, sysinfo, return_data;
   std::string error, s3bucket, s3key, guid;
   result_t* res = NULL;

   /* Clear any global arrays before using. I don't know how but these arrays persist 
    * information across lambda invocations. AMAZING, isn't it?
    * We could use this to detect if a lambda underwent a warm start or a cold start */
   logs.clear();
   std::cout << get_ipaddr() << std::endl;
   std::cout << get_cpu_cycles_for_task() << std::endl;
   return 0;

   // std::string payload = "";
   // /* Parse request body for arguments */
   // try {     
   //    RSJresource body(payload);
   //    if (body["body"].exists()) {
   //       // See if body is nested in payload
   //       std::string escaped_body = body["body"].as<std::string>("");
   //       body = RSJresource(util::unescape_json(escaped_body));
   //    }
   //    id = body["id"].as<int>(0);
   //    start_time_secs = body["stime"].as<int>(0);
   //    log_ = body["log"].as<bool>(false);                   // include logs in response  
   //    save_samples = body["samples"].as<bool>(false);       // include a sample of latencies in response
   //    max_phases = body["phases"].as<int>(1);               // run 1 phase by default
   //    max_bits = body["maxbits"].as<int>(8);                // Assume maximum of 8 bits in ID by default
   //    bit_duration_secs = body["bitduration"].as<int>(1);   // takes 1 second for communicating each bit by default. phases*maxbits*bitduration gives total time
   //    return_data = body["return_data"].as<bool>(false);    // return data in API response. Stored to S3 by default.
   //    s3bucket = body["s3bucket"].as<std::string>("");      // return data in API response. Stored to S3 by default.
   //    s3key = body["s3key"].as<std::string>("");            // return data in API response. Stored to S3 by default.
   //    guid = body["guid"].as<std::string>("");              // globally unique id for this lambda (across experiments)
   // }
   // catch(std::exception& e){
   //    success = false;
   //    error = "INVALID_BODY";
   //    printf("Could not parse request body\n");
   // }

   /* SET DEFAULTS*/
   id = 0;
   start_time_secs = 0;
   log_ = true;                   // include logs in response  
   save_samples = true;       // include a sample of latencies in response
   max_phases = 0;               // run 1 phase by default
   max_bits = 10;                // Assume maximum of 8 bits in ID by default
   bit_duration_secs = 1;   // takes 1 second for communicating each bit by default. phases*maxbits*bitduration gives total time

   printf("Starting lambda %d (GUID: %s) LOCALLY at %s\n", id, guid.c_str(), start_time.c_str());
   lambdas.push_back(guid);

   if (success && id < 0 || id >= (1<<max_bits)) {
      success = false;
      error = "INVALID_ID";
      printf("Id is not provided or invalid (should be in [1, %d)\n", 1<<max_bits);
   }

   if (success && (bit_duration_secs < 1  || bit_duration_secs > MAX_BIT_DURATION_SECS)) {
      success = false;
      error = "INVALID_BIT_DURATION";
      printf("Bit duration is invalid (should be in [1, %d]\n", MAX_BIT_DURATION_SECS);
   }
   
   seconds now = duration_cast<seconds>(Clock::now().time_since_epoch());
   start_time_secs = now.count() + 5;
   if (success && start_time_secs <= now.count()) {    // Unix time in secs
      success = false;
      error = "INVALID_STIME";
      printf("Start time is not provided or is in the past\n");
   }

   /* Run membus protcol */
   if (success) {
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
         error = "CLOCK_PRECISION_LOW";
         success = false;
      }
      printf("clock precision level: %d\n", prec);

      /* Get cacheline address */
      uint64_t* addr = get_cache_line_straddled_address();
      if (addr == NULL){
         printf("Cannot find cacheline straddled address");
         error = "NO_CACHELINE_ADDR";
         success = false;
      }

      if (success) {
         /* Run id exchange protocol */
         try {
            microseconds start_time_mus = duration_cast<microseconds>(std::chrono::seconds(start_time_secs));
            res = run_membus_protocol(id, start_time_mus, max_phases, max_bits, bit_duration_secs, addr, true);
         }
         catch (std::exception e){
            printf("Exception in membus protocol execution: %s", e.what());
            error = "MEMBUS_CRASH";
            success = false;
         }
      }
   }

   // /* Prepare response body as JSON*/
   // RSJresource body("{}");
   // // body["request"] = util::escape_json(request.payload);     // Enable this for request payload debugging
   // body["Id"] = id;
   // body["Start Time"] =  start_time;
   // body["End Time"] =   current_datetime();
   // body["Request ID"] = "";

   // /* Save results (return all fields whether applicable or not) */
   // body["Success"] = success;
   // body["Error"] = error;
   // body["Phases"] =  res != NULL ? res->num_phases : 0;
   // for (int i = 0; i < max_phases; i++) {
   //    body["Phase " + std::to_string(i+1)] = (res != NULL && i < res->num_phases) ? res->ids[i] : -1;
   // }

   // /* Save samples if specified */
   // if (success && save_samples) {
   //    std::string arr;
   //    std::stringstream ss1, ss2;

   //    for (int i = 0; i < base_readings_len; i++) {
   //       arr += std::to_string(base_readings[i]) + ',';
   //    }
   //    body["Base Sample"] = RSJresource(arr, true);
      
   //    arr = "";
   //    for (int i = 0; i < bit1_readings_len; i++) {
   //       arr += std::to_string(bit1_readings[i]) + ',';
   //    }
   //    body["Bit-1 Sample"] = RSJresource(arr, true);
   //    ss1 << std::setprecision(15) << bit1_pvalue;
   //    body["Bit-1 Pvalue"] = ss1.str();
      
   //    arr = "";
   //    for (int i = 0; i < bit0_readings_len; i++) {
   //       arr += std::to_string(bit0_readings[i]) + ',';
   //    }
   //    body["Bit-0 Sample"] = RSJresource(arr, true);
   //    ss2 << std::setprecision(15) << bit0_pvalue;
   //    body["Bit-0 Pvalue"] = ss2.str();   //ss.str to preserve precision
   // }

   // /* Save some system info */
   // /* Get MAC addresses and add to the buffer */ 
   // char mac[50] = "";
   // get_mac_addrs(mac);
   // body["MAC Address"] = std::string(mac);
   // /* Get boot id */
   // std::ifstream ifs("/proc/sys/kernel/random/boot_id");
   // std::string boot_id ( (std::istreambuf_iterator<char>(ifs) ), (std::istreambuf_iterator<char>()) );
   // body["Boot ID"] = boot_id;

   // /* Save all the lambas that previously used the current container */
   // std::string arr;
   // for (int i = 0; i < lambdas.size()-1 ; i++) {
   //    if (i == lambdas.size()-2)    arr += lambdas[i];
   //    else                          arr += lambdas[i] + ',';
   // }
   // body["Predecessors"] = RSJresource(arr, true);
   // body["GUID"] = guid;

   // /* Save logs to response */
   // if (log_) {
   //    std::string logarr;
   //    for (std::vector<std::string>::const_iterator it = begin (logs); it != end (logs); ++it) {
   //       logarr += (*it) + '\t';
   //    }
   //    body["Logs"] = RSJresource(logarr, true);
   // }

   // std::cout << body.as_str() << std::endl;   
   return 0;
}


