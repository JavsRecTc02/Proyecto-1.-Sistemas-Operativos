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
#include <semaphore.h>
#include "shared.h"

static volatile sig_atomic_t keep_running = 1;
void sigint_handler(int s) { (void)s; keep_running = 0; }

/* Procesa una única ranura, ya se consumió un full_count por el caller */
static int process_one_slot(shared_header_t *hdr, sem_t *slot_sems, buffer_slot_t *slots, FILE *out, int key) {
    /* indice de lectura de forma atómica */
    if (sem_wait(&hdr->control_sem) == -1) {
        /* el bucle principal decidirá si continua o sale */
        if (errno == EINTR) return -1; 
        perror("sem_wait control_sem");
        return -1;
    }
    int idx = hdr->tail;
    hdr->tail = (hdr->tail + 1) % hdr->buffer_size;
    sem_post(&hdr->control_sem);

    /* bloquear ranura y leer */
    if (sem_wait(&slot_sems[idx]) == -1) {
        if (errno == EINTR) return -1;
        perror("sem_wait slot_sems");
        return -1;
    }

    if (!slots[idx].occupied) {
        /* condición para liberar y devolver */
        sem_post(&slot_sems[idx]);
        sem_post(&hdr->empty_count);
        return 0;
    }

    /* Decodificar los caracteres leidos de memoria compartida char XOR key*/
    uint8_t encoded = slots[idx].ascii;
    uint8_t decoded = encoded ^ (uint8_t)key;
    uint64_t seq = slots[idx].seq;
    struct timespec ts = slots[idx].ts;

    /* escribir en archivo de salida */
    if (fwrite(&decoded, 1, 1, out) != 1) {
        perror("fwrite salida");
        /* Limpiar la ranura */
    }
    fflush(out);

    /* actualizar estadísticas para control de mem */
    if (sem_wait(&hdr->control_sem) == -1) {
        sem_post(&slot_sems[idx]);
        sem_post(&hdr->empty_count);
        if (errno == EINTR) return -1;
        perror("sem_wait control_sem");
        return -1;
    }
    hdr->total_transferred++;
    sem_post(&hdr->control_sem);

    /* imprimir info: index, seq, encoded -> decoded y tiempo */
    time_t ssec = ts.tv_sec;
    struct tm tmv;
    localtime_r(&ssec, &tmv);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);

    printf("\x1b[1;34m[RECV]\x1b[0m idx=%2d seq=%5lu enc=%3u (0x%02X) -> dec=%3u (0x%02X) char='%c' time=%s.%03ld\n",
           idx, (unsigned long)seq,
           (unsigned int)encoded, (unsigned int)encoded,
           (unsigned int)decoded, (unsigned int)decoded,
           (decoded >= 32 && decoded <= 126) ? (char)decoded : '?',
           tbuf, (long)(ts.tv_nsec / 1000000));

    /* marcar libre la ranura */
    slots[idx].occupied = 0;
    slots[idx].ascii = 0;
    slots[idx].seq = 0;

    sem_post(&slot_sems[idx]);
    sem_post(&hdr->empty_count);
    return 0;
}

/* Flujo del Proceso de Receptor*/

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <shm_name> <mode:manual|auto> <key>\n", argv[0]);
        return 1;
    }

    const char *shm_name = argv[1];
    const char *mode = argv[2];
    int key = atoi(argv[3]);

    /* abrir la memoria compartida */
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t*) map;
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    /* archivo de salida */
    FILE *out = fopen("texto_salida.txt", "a");
    if (!out) { perror("fopen salida"); munmap(map, file_size); close(fd); return 1; }

    /* señales */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* registrar nacimiento atomico */
    if (sem_wait(&hdr->control_sem) == -1) {
        if (errno == EINTR) goto cleanup;
        perror("sem_wait control_sem (start)");
        goto cleanup;
    }
    hdr->total_receivers_spawned++;
    hdr->active_receivers++;
    sem_post(&hdr->control_sem);

    printf("[RECEPTOR] Conectado a %s modo=%s key=%d buffer_size=%d\n",
           shm_name, mode, key, hdr->buffer_size);

    if (strcmp(mode, "auto") == 0) {
        /* MODO AUTOMÁTICO: bloquea en sem_wait(full_count) cuando no hay datos */
        while (keep_running && !hdr->terminate_flag) {
            if (sem_wait(&hdr->full_count) == -1) {
                /* interrumpido por señal: reintentar o salir */
                if (errno == EINTR) continue; 
                perror("sem_wait full_count");
                break;
            }

            /* Si finalizer pidió terminar, devolver y salir */
            if (hdr->terminate_flag) {
                sem_post(&hdr->full_count);
                break;
            }

            if (process_one_slot(hdr, slot_sems, slots, out, key) == -1) {
                /* si fue EINTR intentamos seguir; si es error entonces sale */
                if (errno == EINTR) continue;
                break;
            }
        }
    }
    else if (strcmp(mode, "manual") == 0) {
        /* MODO MANUAL: bloquear en fgets hasta ENTER; luego pedir 1 dato (sem_wait) y procesarlo.*/
        char line[512];
        printf("[RECEPTOR] Modo MANUAL. Presione ENTER para leer 1 carácter.\n");

        while (keep_running && !hdr->terminate_flag) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                if (feof(stdin)) break;
                if (errno == EINTR) continue; /* interrumpido por señal */
                if (ferror(stdin)) { perror("fgets"); break; }
            }

            /* pedir 1 dato (se bloquea hasta que haya) */
            if (sem_wait(&hdr->full_count) == -1) {
                if (errno == EINTR) continue;
                perror("sem_wait full_count");
                break;
            }

            if (hdr->terminate_flag) {
                sem_post(&hdr->full_count);
                break;
            }

            if (process_one_slot(hdr, slot_sems, slots, out, key) == -1) {
                if (errno == EINTR) continue;
                break;
            }
        }
    }
    else {
        fprintf(stderr, "Modo no reconocido: use 'auto' o 'manual'\n");
    }

    /* cleanup: decrementar active_receivers y notificar finalizer si somos últimos */
    if (sem_wait(&hdr->control_sem) == -1) {
        if (errno != EINTR) perror("sem_wait control_sem (cleanup)");
    } else {
        if (hdr->active_receivers > 0) hdr->active_receivers--;
        int ae = hdr->active_emitters;
        int ar = hdr->active_receivers;
        if (ae == 0 && ar == 0) sem_post(&hdr->finalizer_sem);
        sem_post(&hdr->control_sem);
    }

cleanup:
    fclose(out);
    munmap(map, file_size);
    close(fd);
    printf("Receptor Terminado Exitosamente.\n");
    return 0;
}
