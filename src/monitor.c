#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <time.h>
#include "shared.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <shm_name>\n", argv[0]);
        return 1;
    }

    const char *shm_name = argv[1];
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    void *map = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t *)map;
    buffer_slot_t *slots = get_slots(hdr);

    printf("[Monitor conectado] Observando buffer circular (%d slots)\n", hdr->buffer_size);
    printf("Presione Ctrl+C para salir\n");

    while (!hdr->terminate_flag) {
        // Espera actividad (ya sea un full o empty que indique movimiento)
        sem_wait(&hdr->control_sem);
        int active_emitters = hdr->active_emitters;
        int active_receivers = hdr->active_receivers;
        sem_post(&hdr->control_sem);

        // Mostrar estado general
        printf("\033[2J\033[H"); // limpiar pantalla (ANSI escape)
        printf("==== ESTADO ACTUAL DEL BUFFER ====\n");
        printf("Emisores activos: %d | Receptores activos: %d | Buffer size: %d\n", 
                active_emitters, active_receivers, hdr->buffer_size);
        printf("Head: %d | Tail: %d | Total transferidos: %lu\n", 
                hdr->head, hdr->tail, (unsigned long)hdr->total_transferred);
        printf("----------------------------------\n");
        printf("Idx | ASCII | Char | Ocupado | Timestamp\n");
        printf("----------------------------------\n");

        for (int i = 0; i < hdr->buffer_size; i++) {
            buffer_slot_t s = slots[i];
            char ch = (s.ascii >= 32 && s.ascii < 127) ? (char)s.ascii : '.';
            printf("%3d | %5u |  %c   | %7d | %ld.%09ld\n", 
                i, s.ascii, ch, s.occupied, (long)s.ts.tv_sec, s.ts.tv_nsec);
        }

        fflush(stdout);
        sleep(1);  // Refrescar monitorio cada segundo
    }

    munmap(map, file_size);
    close(fd);
    printf("\n[Monitor] Finalizado correctamente.\n");
    return 0;
}
