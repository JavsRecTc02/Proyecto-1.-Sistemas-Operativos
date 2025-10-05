#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/select.h>
#include "shared.h"

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t timer_fired = 0;
static volatile sig_atomic_t got_sigint = 0;

void sigint_handler(int s) { (void)s; got_sigint = 1; keep_running = 0; }
void sigalrm_handler(int s) { (void)s; timer_fired = 1; }

/* mantener estado previo del terminal para restaurarlo */
static struct termios saved_tio;
static int raw_enabled = 0;

void set_raw_mode(int enable) {
    struct termios tio;
    if (enable && !raw_enabled) {
        tcgetattr(STDIN_FILENO, &saved_tio);
        tio = saved_tio;
        tio.c_lflag &= ~(ICANON | ECHO);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        raw_enabled = 1;
    } else if (!enable && raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        raw_enabled = 0;
    }
}

int send_byte(shared_header_t *hdr, sem_t *slot_sems, buffer_slot_t *slots, uint8_t byte, uint8_t key) {
    if (sem_wait(&hdr->empty_count) == -1) {
        if (errno == EINTR) return -1;
        perror("sem_wait empty_count");
        return -1;
    }

    if (sem_wait(&hdr->meta_mutex) == -1) { perror("sem_wait meta"); return -1; }
    int idx = hdr->head;
    hdr->head = (hdr->head + 1) % hdr->buffer_size;
    uint64_t seq = ++hdr->seq_counter;
    sem_post(&hdr->meta_mutex);

    if (sem_wait(&slot_sems[idx]) == -1) { perror("sem_wait slot"); return -1; }

    slots[idx].ascii = (uint8_t)(byte ^ key);
    slots[idx].seq = seq;
    clock_gettime(CLOCK_REALTIME, &slots[idx].ts);
    slots[idx].occupied = 1;

    struct timespec ts = slots[idx].ts;
    printf("\x1b[1;32m[EMIT]\x1b[0m idx=%d seq=%lu encoded=%u (orig=%c) time=%ld.%09ld\n",
           idx, (unsigned long)seq, (unsigned int)slots[idx].ascii,
           (byte >= 32 && byte <= 126) ? (char)byte : '?',
           (long)ts.tv_sec, ts.tv_nsec);

    sem_post(&slot_sems[idx]);
    sem_post(&hdr->full_count);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <shm_name> <mode:manual|auto> <key>\n", argv[0]);
        return 1;
    }

    const char *shm_name = argv[1];
    const char *mode = argv[2];
    int key_arg = atoi(argv[3]);
    uint8_t local_key = (uint8_t)key_arg;

    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t*)map;
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    /* archivo que mantiene el texto (coherencia con inicializer) */
    FILE *f = fopen(hdr->filename, "a+");
    if (!f) {
        fprintf(stderr, "Error abriendo %s: %s\n", hdr->filename, strerror(errno));
        munmap(map, file_size); close(fd);
        return 1;
    }

    /* señales */
    struct sigaction sa_int = {0}, sa_alrm = {0};
    sa_int.sa_handler = sigint_handler;
    sa_alrm.sa_handler = sigalrm_handler;
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);
    sigaction(SIGALRM, &sa_alrm, NULL);

    /* registrar : total_emitters_spawned++ y active_emitters++ */
    sem_wait(&hdr->meta_mutex);
    hdr->total_emitters_spawned++;
    hdr->active_emitters++;
    sem_post(&hdr->meta_mutex);

    if (key_arg == 0) local_key = hdr->key;

    printf("[EMITTER] shm=%s mode=%s key=%u file=%s\n", shm_name, mode, (unsigned int)local_key, hdr->filename);

    if (strcmp(mode, "auto") == 0) {
        printf("[AUTO] Escriba caracteres; se enviarán automáticamente cada 1 s.\n");
        set_raw_mode(1);
        const unsigned int interval = 1;
        alarm(interval);

        long last_pos = 0;
        while (keep_running && !hdr->terminate_flag) {
            /* usar select para detectar input */
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 }; /* 0.5s */
            int sel = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
            if (sel > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
                char c;
                ssize_t r = read(STDIN_FILENO, &c, 1);
                if (r == 1) {
                    /* append al archivo en disco para que otros emisores lo vean en auto */
                    fseek(f, 0, SEEK_END);
                    fwrite(&c, 1, 1, f);
                    fflush(f);
                }
            }

            if (timer_fired) {
                timer_fired = 0;
                /* leer lo nuevo del archivo */
                fseek(f, last_pos, SEEK_SET);
                int ch;
                while ((ch = fgetc(f)) != EOF) {
                    if (!keep_running || hdr->terminate_flag) break;
                    send_byte(hdr, slot_sems, slots, (uint8_t)ch, local_key);
                }
                last_pos = ftell(f);
                alarm(interval);
            }
        }
        set_raw_mode(0);
    }
    else { /* manual */
        printf("[MANUAL] Escriba texto y presione ENTER. (Ctrl+C para salir)\n");
        /* Usar select con bloqueos para permitir terminar por terminate_flag */
        char line[1024];
        while (keep_running && !hdr->terminate_flag) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            /* esperar indefinidamente hasta input o señal; select bloquea internamente */
            int sel = select(STDIN_FILENO + 1, &fds, NULL, NULL, NULL);
            if (sel > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
                if (fgets(line, sizeof(line), stdin) == NULL) {
                    if (feof(stdin)) break;
                    if (errno == EINTR) continue;
                    break;
                }
                size_t len = strcspn(line, "\n");
                /* append al archivo */
                if (fseek(f, 0, SEEK_END) == 0) {
                    fwrite(line, 1, len, f);
                    fwrite("\n", 1, 1, f);
                    fflush(f);
                }
                for (size_t i = 0; i < len; ++i) {
                    if (!keep_running || hdr->terminate_flag) break;
                    send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key);
                }
            }
        }
    }

    /* Parac cerrar decrementar active_emitters / ultimo hace finalizer_sem */
    sem_wait(&hdr->meta_mutex);
    if (hdr->active_emitters > 0) hdr->active_emitters--;
    int ae = hdr->active_emitters;
    int ar = hdr->active_receivers;
    if (ae == 0 && ar == 0) {
        sem_post(&hdr->finalizer_sem);
    }
    sem_post(&hdr->meta_mutex);

    fclose(f);
    munmap(map, file_size);
    close(fd);
    printf("Emisor Terminado exitosamente.\n");
    return 0;
}

