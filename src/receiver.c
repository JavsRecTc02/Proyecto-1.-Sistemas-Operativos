#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include "shared.h"

static volatile sig_atomic_t keep_running = 1;
void sigint_handler(int s) { (void)s; keep_running = 0; }

static int safe_sem_wait(sem_t *s) {
    while (1) {
        if (sem_wait(s) == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

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

    /* realizar el archivo de salida */
    FILE *out = fopen("texto_salida.txt", "a");
    if (!out) { perror("fopen salida"); munmap(map, file_size); close(fd); return 1; }

    /* señales */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* registrar nacimiento atomico */
    if (safe_sem_wait(&hdr->control_sem) == -1) {
        perror("sem_wait control_sem (start)");
        goto cleanup;
    }
    hdr->total_receivers_spawned++;
    hdr->active_receivers++;
    sem_post(&hdr->control_sem);

    printf("[RECEPTOR] Conectado a %s modo=%s key=%d\n", shm_name, mode, key);

    if (strcmp(mode, "auto") == 0) {
        /* modo automático procesar cada dato en cuanto esté disponible */
        while (keep_running && !hdr->terminate_flag) {
            /* bloquear hasta que haya al menos 1 dato */
            if (safe_sem_wait(&hdr->full_count) == -1) {
                if (errno == EINTR) continue;
                break;
            }

            /* si finalizer pidió terminar, devolvemos y salimos */
            if (hdr->terminate_flag) {
                sem_post(&hdr->full_count); /* permitir a otros wakeups si fue posteado en exceso */
                break;
            }

            /* obtener índice de lectura de forma atómica */
            if (safe_sem_wait(&hdr->control_sem) == -1) break;
            int idx = hdr->tail;
            hdr->tail = (hdr->tail + 1) % hdr->buffer_size;
            sem_post(&hdr->control_sem);

            /* bloquear ranura y leer */
            if (safe_sem_wait(&slot_sems[idx]) == -1) {
                /* si falla volvemos a postear full_count para no perder el dato */
                sem_post(&hdr->full_count);
                continue;
            }

            if (!slots[idx].occupied) {
                /* podría suceder si finalizer nos despertó; restaurar y continuar */
                sem_post(&slot_sems[idx]);
                sem_post(&hdr->full_count);
                continue;
            }

            uint8_t encoded = slots[idx].ascii;
            uint8_t decoded = encoded ^ (uint8_t)key;
            uint64_t seq = slots[idx].seq;
            struct timespec ts = slots[idx].ts;

            /* escribir en salida (archivo y stdout) */
            fwrite(&decoded, 1, 1, out);
            fflush(out);

            /* actualizar estadísticas */
            if (safe_sem_wait(&hdr->control_sem) == -1) { sem_post(&slot_sems[idx]); sem_post(&hdr->empty_count); break; }
            hdr->total_transferred++;
            sem_post(&hdr->control_sem);

            /* Informacion del caracter obtenido */
            time_t ssec = ts.tv_sec;
            struct tm tmv;
            localtime_r(&ssec, &tmv);
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);
            printf("\x1b[1;34m[RECV]\x1b[0m (AUTO) idx=%2d seq=%5lu char='%c' time=%s.%03ld\n",
                   idx, (unsigned long)seq,
                   (decoded >= 32 && decoded <= 126) ? (char)decoded : '?',
                   tbuf, ts.tv_nsec);

            /* marcar libre la ranura */
            slots[idx].occupied = 0;
            slots[idx].ascii = 0;
            slots[idx].seq = 0;

            sem_post(&slot_sems[idx]);
            sem_post(&hdr->empty_count);
        }
    }
    /*Modo Manual*/
    else if (strcmp(mode, "manual") == 0) {
        char line[4];
        printf("[RECEIVER] Modo MANUAL. Presione ENTER para leer 1 carácter.\n");
        while (keep_running && !hdr->terminate_flag) {
            /* esperar ENTER: bloqueante en stdin (sin busy-wait) */
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            int sel = select(STDIN_FILENO + 1, &fds, NULL, NULL, NULL); /* bloquea */
            if (sel <= 0) {
                if (sel == -1 && errno == EINTR) continue;
                break;
            }
            /* consumir la línea (usuario puede teclear algo antes de ENTER) */
            if (!fgets(line, sizeof(line), stdin)) {
                if (feof(stdin)) break;
                if (errno == EINTR) continue;
            }

            /* ahora pedir 1 dato: sem_wait(&full_count) -> bloquea hasta que haya */
            if (safe_sem_wait(&hdr->full_count) == -1) {
                if (errno == EINTR) continue;
                break;
            }
            if (hdr->terminate_flag) { sem_post(&hdr->full_count); break; }

            /* leer exactamente 1 slot (igual que en auto) */
            if (safe_sem_wait(&hdr->control_sem) == -1) { sem_post(&hdr->full_count); break; }
            int idx = hdr->tail;
            hdr->tail = (hdr->tail + 1) % hdr->buffer_size;
            sem_post(&hdr->control_sem);

            if (safe_sem_wait(&slot_sems[idx]) == -1) {
                sem_post(&hdr->full_count);
                continue;
            }

            if (!slots[idx].occupied) {
                sem_post(&slot_sems[idx]);
                sem_post(&hdr->full_count);
                continue;
            }

            uint8_t encoded = slots[idx].ascii;
            uint8_t decoded = encoded ^ (uint8_t)key;
            uint64_t seq = slots[idx].seq;
            struct timespec ts = slots[idx].ts;

            fwrite(&decoded, 1, 1, out);
            fflush(out);

            if (safe_sem_wait(&hdr->control_sem) == -1) { sem_post(&slot_sems[idx]); sem_post(&hdr->empty_count); break; }
            hdr->total_transferred++;
            sem_post(&hdr->control_sem);

            time_t ssec = ts.tv_sec;
            struct tm tmv;
            localtime_r(&ssec, &tmv);
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);
            printf("\x1b[1;34m[RECV]\x1b[0m (MANUAL) idx=%2d seq=%5lu char='%c' time=%s.%09ld\n",
                   idx, (unsigned long)seq,
                   (decoded >= 32 && decoded <= 126) ? (char)decoded : '?',
                   tbuf, ts.tv_nsec);

            slots[idx].occupied = 0;
            slots[idx].ascii = 0;
            slots[idx].seq = 0;

            sem_post(&slot_sems[idx]);
            sem_post(&hdr->empty_count);
        }
    }
    else {
        fprintf(stderr, "Modo no reconocido: use 'auto' o 'manual'\n");
    }

    /* Hacer Cleanup, decrementar active_receivers y, si somos el último, avisar finalizer */
    if (safe_sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem (cleanup)"); goto cleanup; }
    if (hdr->active_receivers > 0) hdr->active_receivers--;
    int ae = hdr->active_emitters;
    int ar = hdr->active_receivers;
    if (ae == 0 && ar == 0) {
        sem_post(&hdr->finalizer_sem);
    }
    sem_post(&hdr->control_sem);

cleanup:
    fclose(out);
    munmap(map, file_size);
    close(fd);
    printf("Receptor Terminado Exitosamente.\n");
    return 0;
}
