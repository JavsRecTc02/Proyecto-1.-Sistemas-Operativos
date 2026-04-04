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
#include <stdint.h>
#include "shared.h"

/* ====== Config ====== */
#define REFRESH_INTERVAL_SEC 1   /* intervalo de refresco si no hay eventos (bloqueante con timeout) */

/* ====== Estado señales ====== */
static volatile sig_atomic_t keep_running = 1;
static void sig_handler(int s) { (void)s; keep_running = 0; }

/* ====== Util: timestamp HH:MM:SS.mmm ====== */
static void fmt_time_ms(const struct timespec *ts, char *buf, size_t buflen) {
    struct tm tm_info;
    localtime_r(&ts->tv_sec, &tm_info);
    char tonly[16];
    strftime(tonly, sizeof(tonly), "%H:%M:%S", &tm_info);
    long ms = (long)(ts->tv_nsec / 1000000L);
    snprintf(buf, buflen, "%s.%03ld", tonly, ms);
}

/* ====== Dibujar cabecera ====== */
static void draw_header(shared_header_t *hdr, int active_emitters, int active_receivers) {
    /* Limpiar pantalla y situar cursor */
    printf("\033[2J\033[H");

    printf("\x1b[1;36m╔══════════════════════════════════════════════════════════════╗\x1b[0m\n");
    printf("\x1b[1;36m║                        MONITOR DE BUFFER                      ║\x1b[0m\n");
    printf("\x1b[1;36m╚══════════════════════════════════════════════════════════════╝\x1b[0m\n");

    printf("  \x1b[1;33m• Buffer slots:\x1b[0m \x1b[36m%d\x1b[0m   ", hdr->buffer_size);
    printf("\x1b[1;33m• Head:\x1b[0m \x1b[36m%d\x1b[0m   ", hdr->head);
    printf("\x1b[1;33m• Tail:\x1b[0m \x1b[36m%d\x1b[0m   ", hdr->tail);
    printf("\x1b[1;33m• Total transferidos:\x1b[0m \x1b[32m%lu\x1b[0m\n",
           (unsigned long)hdr->total_transferred);

    printf("  \x1b[1;33m• Emisores activos:\x1b[0m \x1b[32m%d\x1b[0m   ", active_emitters);
    printf("\x1b[1;33m• Receptores activos:\x1b[0m \x1b[32m%d\x1b[0m   ", active_receivers);
    printf("\x1b[1;33m• Key (XOR 8-bit):\x1b[0m \x1b[35m%d\x1b[0m\n", hdr->key);

    printf("\x1b[1;36m──────────────────────────────────────────────────────────────\x1b[0m\n");
}

/* ====== Dibujar tabla de slots ====== */
static void draw_table(shared_header_t *hdr) {
    sem_t *slot_sems     = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    printf("  \x1b[1m%-4s %-7s %-6s %-8s %-12s %-14s\x1b[0m\n",
           "IDX", "ASCII", "CHAR", "ESTADO", "SEQ", "TIMESTAMP");
    printf("  \x1b[90m%-4s %-7s %-6s %-8s %-12s %-14s\x1b[0m\n",
           "────", "───────", "──────", "────────", "────────────", "──────────────");

    for (int i = 0; i < hdr->buffer_size; ++i) {
        /* leer slot con su semáforo para consistencia */
        if (sem_wait(&slot_sems[i]) == -1) {
            /* ignorar slot si hay error inesperado */
            continue;
        }

        buffer_slot_t s = slots[i];
        sem_post(&slot_sems[i]);

        /* CHAR imprimible o '.' */
        char ch = (s.ascii >= 32 && s.ascii <= 126) ? (char)s.ascii : '.';

        /* Estado con colores */
        const char *state = s.occupied ? "\x1b[1;31mOCUPADO\x1b[0m" : "\x1b[1;32mLIBRE\x1b[0m";

        /* timestamp bonito HH:MM:SS.mmm (si no se ha escrito nunca, deja en gris) */
        char tbuf[32];
        if (s.ts.tv_sec == 0 && s.ts.tv_nsec == 0) {
            snprintf(tbuf, sizeof(tbuf), "\x1b[90m--:--:--.---\x1b[0m");
        } else {
            char local[24];
            fmt_time_ms(&s.ts, local, sizeof(local));
            snprintf(tbuf, sizeof(tbuf), "%s", local);
        }

        /* Procedencia: si tienes campo en slot (p.ej. producer_id), muéstralo; aquí inferimos por head/tail no intrusivo */
        /* Mostramos también la secuencia global (útil para seguimiento) */
        printf("  \x1b[36m%3d\x1b[0m   %-7u %-6c %-8s %-12lu %-14s\n",
               i,
               (unsigned)s.ascii,
               ch,
               state,
               (unsigned long)s.seq,
               tbuf);
    }
}

/* ====== Main ====== */
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <shm_name>\n", argv[0]);
        return 1;
    }

    /* señales para salir con Ctrl+C/TERM */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const char *shm_name = argv[1];

    /* abrir y mapear SHM */
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t *)map;

    printf("\x1b[1;32m[Monitor conectado]\x1b[0m Observando buffer circular (%d slots)\n", hdr->buffer_size);
    printf("Refresco monitor cada %ds.\n", REFRESH_INTERVAL_SEC);

    /* Bucle de monitor: SIN busy-wait
       - usamos sem_timedwait() sobre finalizer_sem con timeout corto.
       - No consumimos señales de productores/consumidores (no interferimos con empty/full).
       - En cada timeout (o si todos terminan), refrescamos la vista. */
    while (keep_running) {
        /* Leer contadores bajo control_sem (consistente) */
        if (sem_wait(&hdr->control_sem) == -1) {
            if (errno == EINTR) continue;
            perror("sem_wait control_sem");
            break;
        }
        int active_emitters  = hdr->active_emitters;
        int active_receivers = hdr->active_receivers;
        int terminate_flag   = hdr->terminate_flag;
        sem_post(&hdr->control_sem);

        draw_header(hdr, active_emitters, active_receivers);
        draw_table(hdr);
        fflush(stdout);

        if (terminate_flag || (active_emitters == 0 && active_receivers == 0)) {
            /* Si el sistema ya está terminando, no esperamos más. */
            break;
        }

        /* Espera bloqueante: usamos finalizer_sem como “evento global”
           - Si el último proceso postea, despertamos antes del timeout.
           - Si no, hacemos timeout y simplemente refrescamos.
           Nota: esto NO interfiere con empty/full. */
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        struct timespec ts;
        ts.tv_sec  = now.tv_sec + REFRESH_INTERVAL_SEC;
        ts.tv_nsec = now.tv_nsec;

        int rc = sem_timedwait(&hdr->finalizer_sem, &ts);
        if (rc == 0) {
            /* alguien posteó (probablemente el último en salir); volvemos a iterar y saldremos */
            continue;
        } else {
            if (errno == ETIMEDOUT) {
                /* simple refresco por timeout */
                continue;
            } else if (errno == EINTR) {
                /* interrumpido por señal: respetamos keep_running en la siguiente vuelta */
                continue;
            } else {
                perror("sem_timedwait finalizer_sem");
                break;
            }
        }
    }

    munmap(map, file_size);
    close(fd);
    printf("\n\x1b[1;32m[Monitor]\x1b[0m Finalizado correctamente.\n");
    return 0;
}
