#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <stdatomic.h>

/* ========================================================================== */
/* CONFIGURATION VARIABLES                           */
/* ========================================================================== */
#define TARGET_URL          "127.0.0.1"  // Target URL or Hostname
#define CONCURRENT_USERS    10            // Number of concurrent users (threads)
#define TEST_DURATION_SECS  15            // Test duration in seconds
/* ========================================================================== */

// Global atomic counter for total requests sent across all threads
atomic_long total_requests_sent = 0;
atomic_int active_threads = 0;

// Structure to pass arguments to threads
typedef struct {
    struct sockaddr_in dest_addr;
    int duration_seconds;
    int thread_id;
} thread_args_t;

// Standard Internet Checksum function
unsigned short checksum(void *b, int len) {
    unsigned short *buf = b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2)
        sum += *buf++;
    if (len == 1)
        sum += *(unsigned char *)buf;
    
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

// Monitor thread to handle live console updating
void *monitor_worker(void *arg) {
    int duration = *(int *)arg;
    time_t start_time = time(NULL);
    int elapsed = 0;

    printf("\n--- Test Started ---\n");
    while (elapsed < duration) {
        int remaining = duration - elapsed;
        
        // \r resets the cursor to the beginning of the line to update it live
        printf("\r[LIVE] Active Users: %d/%d | Remaining Time: %ds | Total Requests Sent: %ld", 
               atomic_load(&active_threads), CONCURRENT_USERS, remaining, atomic_load(&total_requests_sent));
        fflush(stdout);
        
        sleep(1);
        elapsed = (int)(time(NULL) - start_time);
    }
    
    // Final clear up of the live line
    printf("\r[LIVE] Active Users: 0/%d | Remaining Time: 0s | Total Requests Sent: %ld\n", 
           CONCURRENT_USERS, atomic_load(&total_requests_sent));
    return NULL;
}

// Thread function that continuously pings
void *ping_worker(void *arguments) {
    thread_args_t *args = (thread_args_t *)arguments;
    
    // Create raw socket for ICMP
    int sock_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock_fd < 0) {
        perror("\nSocket creation failed (Are you running as root/sudo?)");
        atomic_fetch_sub(&active_threads, 1);
        pthread_exit(NULL);
    }

    // Set up packet structure
    struct icmp icmp_hdr;
    char packet[sizeof(struct icmp)];
    
    memset(&icmp_hdr, 0, sizeof(icmp_hdr));
    icmp_hdr.icmp_type = ICMP_ECHO; 
    icmp_hdr.icmp_code = 0;
    icmp_hdr.icmp_id = getpid() + args->thread_id; 
    
    time_t start_time = time(NULL);
    int sequence = 0;

    // Loop continuously for the specified duration
    while (time(NULL) - start_time < args->duration_seconds) {
        icmp_hdr.icmp_seq = sequence++;
        icmp_hdr.icmp_cksum = 0; 
        
        memcpy(packet, &icmp_hdr, sizeof(icmp_hdr));
        icmp_hdr.icmp_cksum = checksum(packet, sizeof(packet));
        memcpy(packet, &icmp_hdr, sizeof(icmp_hdr));

        // Send the ICMP packet
        if (sendto(sock_fd, packet, sizeof(packet), 0, 
                   (struct sockaddr *)&args->dest_addr, sizeof(args->dest_addr)) >= 0) {
            // Safely increment global request counter across threads
            atomic_fetch_add(&total_requests_sent, 1);
        }

        // 10ms delay to keep the socket stable and avoid instant network choke
        usleep(10000); 
    }

    close(sock_fd);
    atomic_fetch_sub(&active_threads, 1);
    pthread_exit(NULL);
}

int main() {
    // Resolve URL to IP
    struct hostent *host = gethostbyname(TARGET_URL);
    if (host == NULL) {
        herror("Failed to resolve target URL");
        return 1;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    memcpy(&dest_addr.sin_addr, host->h_addr_list[0], host->h_length);

    // Initial Details Output
    printf("==================================================\n");
    printf("                 PING STRESS TEST                 \n");
    printf("==================================================\n");
    printf("Target URL      : %s\n", TARGET_URL);
    printf("Target IP       : %s\n", inet_ntoa(dest_addr.sin_addr));
    printf("Concurrent Users: %d\n", CONCURRENT_USERS);
    printf("Target Duration : %d seconds\n", TEST_DURATION_SECS);
    printf("==================================================\n");

    pthread_t *threads = malloc(CONCURRENT_USERS * sizeof(pthread_t));
    thread_args_t *thread_args = malloc(CONCURRENT_USERS * sizeof(thread_args_t));
    
    pthread_t monitor_thread;
    int duration = TEST_DURATION_SECS;

    // Start all ping threads
    for (int i = 0; i < CONCURRENT_USERS; i++) {
        thread_args[i].dest_addr = dest_addr;
        thread_args[i].duration_seconds = TEST_DURATION_SECS;
        thread_args[i].thread_id = i + 1;

        atomic_fetch_add(&active_threads, 1);
        if (pthread_create(&threads[i], NULL, ping_worker, (void *)&thread_args[i]) != 0) {
            perror("Failed to create user thread");
            return 1;
        }
    }

    // Start the live dashboard monitoring thread
    if (pthread_create(&monitor_thread, NULL, monitor_worker, &duration) != 0) {
        perror("Failed to create monitor thread");
        return 1;
    }

    // Join threads
    for (int i = 0; i < CONCURRENT_USERS; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_join(monitor_thread, NULL);

    // Final Summary Output
    printf("\n==================================================\n");
    printf("                  FINAL SUMMARY                   \n");
    printf("==================================================\n");
    printf("Status          : Completed Successfully\n");
    printf("Target URL      : %s (%s)\n", TARGET_URL, inet_ntoa(dest_addr.sin_addr));
    printf("Total Users Run : %d\n", CONCURRENT_USERS);
    printf("Total Duration  : %d seconds\n", TEST_DURATION_SECS);
    printf("Total Requests  : %ld packets sent\n", atomic_load(&total_requests_sent));
    printf("Avg Request Rate: %.2f packets/sec\n", (double)atomic_load(&total_requests_sent) / TEST_DURATION_SECS);
    printf("==================================================\n");

    // Clean up
    free(threads);
    free(thread_args);

    return 0;
}
