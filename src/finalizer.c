#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <semaphore.h>
#include "shared.h"

/* tiempo total máximo antes de forzar la salida */
#define MAX_WAIT_SECONDS 15
/* intervalo de espera por sem_timedwait (segundos) */
#define WAIT_INTERVAL 5

static void print_stats_and_scan(shared_header_t *hdr) {
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    /* Contar ranuras ocupadas leyendo estado de cada slot */
    int occupied_count = 0;
    for (int i = 0; i < hdr->buffer_size; ++i) {
        /* intentamos sem_wait pero si falla (EINTR) lo reintentamos */
        if (sem_wait(&slot_sems[i]) == -1) {
            /* si hay un error serio, ignoramos ese slot */
            continue;
        }
        if (slots[i].occupied) occupied_count++;
        sem_post(&slot_sems[i]);
    }

    /* Impresión elegante */
    printf("\n\033[1;34m=== ESTADÍSTICAS GLOBALES ===\033[0m\n");
    printf("Total chars transferidos : %lu\n", (unsigned long)hdr->total_transferred);
    printf("Chars actuales en memoria compartida : %d (buffer_size=%d)\n", occupied_count, hdr->buffer_size);
    printf("Total Emisores lanzados : %d   Emisores activos : %d\n", hdr->total_emitters_spawned, hdr->active_emitters);
    printf("Total Receptores lanzados: %d   Receptores activos: %d\n", hdr->total_receivers_spawned, hdr->active_receivers);
    printf("Espacio de Memoria compartida (bytes): %zu\n", compute_shm_size(hdr->buffer_size));
    printf("\033[1;34m=============================\033[0m\n\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <shm_name>\n", argv[0]);
        return 1;
    }

    const char *shm_name = argv[1];
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t*) map;

    /* Indicar terminación de forma atómica */
    if (sem_wait(&hdr->meta_mutex) == -1) {
        perror("sem_wait meta_mutex");
        munmap(map, file_size);
        close(fd);
        return 1;
    }
    hdr->terminate_flag = 1;
    sem_post(&hdr->meta_mutex);

    /* Despertar a quién esté bloqueado en empty_count/full_count */
    /* postear suficiente cantidad para liberar procesos bloqueados */
    int posts = hdr->buffer_size + 10; /* margen adicional */
    for (int i = 0; i < posts; ++i) {
        sem_post(&hdr->empty_count);
        sem_post(&hdr->full_count);
    }

    /* Si no hay procesos activos -> imprimir y salir */
    sem_wait(&hdr->meta_mutex);
    int active_emitters = hdr->active_emitters;
    int active_receivers = hdr->active_receivers;
    sem_post(&hdr->meta_mutex);

    if (active_emitters == 0 && active_receivers == 0) {
        print_stats_and_scan(hdr);
        munmap(map, file_size);
        close(fd);
        /* opcional: shm_unlink(shm_name); */
        return 0;
    }

    /* Esperar al último proceso de forma segura usando sem_timedwait en un bucle.*/
    struct timespec now;
    time_t waited = 0;
    int final_ok = 0;

    while (waited < MAX_WAIT_SECONDS) {
        /* Construir tiempo absoluto para sem_timedwait */
        clock_gettime(CLOCK_REALTIME, &now);
        struct timespec ts;
        ts.tv_sec = now.tv_sec + WAIT_INTERVAL;
        ts.tv_nsec = now.tv_nsec;

        int rc = sem_timedwait(&hdr->finalizer_sem, &ts);
        if (rc == 0) {
            /* finalizer_sem -> último proceso terminó */
            final_ok = 1;
            break;
        } else {
            if (errno == ETIMEDOUT) {
                /* timeout: re-check active counts */
                sem_wait(&hdr->meta_mutex);
                active_emitters = hdr->active_emitters;
                active_receivers = hdr->active_receivers;
                sem_post(&hdr->meta_mutex);

                if (active_emitters == 0 && active_receivers == 0) {
                    final_ok = 1;
                    break;
                }
                /* seguir esperando, acumulamos tiempo */
                waited += WAIT_INTERVAL;
                continue;
            } else if (errno == EINTR) {
                /* señal interrumpió sem_timedwait; volver a intentar */
                continue;
            } else {
                perror("sem_timedwait finalizer_sem");
                break;
            }
        }
    }

    if (!final_ok) {
        fprintf(stderr, "\n[finalizer] Aviso: tiempo de espera excedido (%d s). Procederé a imprimir estadísticas parciales.\n", MAX_WAIT_SECONDS);
    }

    /* imprimir estadísticas y conteo de ranuras ocupadas */
    print_stats_and_scan(hdr);

    /* limpieza */
    munmap(map, file_size);
    close(fd);
    if (shm_unlink(shm_name) == 0) printf("[finalizer] Shared memory unlinked.\n");

    return 0;
}

