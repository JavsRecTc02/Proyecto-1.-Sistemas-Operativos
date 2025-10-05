#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include "shared.h"

static volatile sig_atomic_t keep_running = 1;
void sigint_handler(int s) { (void)s; keep_running = 0; }

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <shm_name> <mode:manual|auto> <key>\n", argv[0]);
        return 1;
    }

    const char *shm_name = argv[1];
    int key = atoi(argv[3]);

    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t*)map;
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    FILE *out = fopen("texto_salida.txt", "a");
    if (!out) { perror("fopen salida"); munmap(map, file_size); close(fd); return 1; }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* registrar nacimiento */
    sem_wait(&hdr->meta_mutex);
    hdr->total_receivers_spawned++;
    hdr->active_receivers++;
    sem_post(&hdr->meta_mutex);

    while (keep_running && !hdr->terminate_flag) {
        if (sem_wait(&hdr->full_count) == -1) {
            if (errno == EINTR) continue;
            break;
        }

        if (hdr->terminate_flag) { /* si finalizer pidió terminar, podemos despertar y salir */
            /* intentar devolver el sem_post por si hay otros */
            sem_post(&hdr->full_count);
            break;
        }

        /* tomar índice de lectura */
        if (sem_wait(&hdr->meta_mutex) == -1) break;
        int idx = hdr->tail;
        hdr->tail = (hdr->tail + 1) % hdr->buffer_size;
        sem_post(&hdr->meta_mutex);

        if (sem_wait(&slot_sems[idx]) == -1) { /* manejar EINTR */ continue; }

        if (!slots[idx].occupied) {
            sem_post(&slot_sems[idx]);
            sem_post(&hdr->full_count);
            continue;
        }

        uint8_t encoded = slots[idx].ascii;
        uint8_t decoded = encoded ^ (uint8_t)key;
        uint64_t seq = slots[idx].seq;

        /* escribir en salida */
        fwrite(&decoded, 1, 1, out);
        fflush(out);

        /* actualizar estadística de transferidos */
        sem_wait(&hdr->meta_mutex);
        hdr->total_transferred++;
        sem_post(&hdr->meta_mutex);

        printf("[RECV] idx=%d seq=%lu char='%c'\n", idx, (unsigned long)seq,
               (decoded >= 32 && decoded <= 126) ? (char)decoded : '?');

        /* marcar libre */
        slots[idx].occupied = 0;
        slots[idx].ascii = 0;
        slots[idx].seq = 0;

        sem_post(&slot_sems[idx]);
        sem_post(&hdr->empty_count);
    }

    /* cleanup: decrementar active_receivers y último finalizer_sem */
    sem_wait(&hdr->meta_mutex);
    if (hdr->active_receivers > 0) hdr->active_receivers--;
    int ae = hdr->active_emitters;
    int ar = hdr->active_receivers;
    if (ae == 0 && ar == 0) {
        sem_post(&hdr->finalizer_sem);
    }
    sem_post(&hdr->meta_mutex);

    fclose(out);
    munmap(map, file_size);
    close(fd);
    printf("Receptor Terminado Exitosamente.\n");
    return 0;
}

