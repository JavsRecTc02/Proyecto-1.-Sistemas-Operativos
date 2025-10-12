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
#include <signal.h>
#include <termios.h>
#include <stdint.h>
#include "shared.h"

/* Configuración de espera para el cierre ordenado */
#define MAX_WAIT_SECONDS 15          /* tiempo total máximo antes de forzar salida */
#define WAIT_INTERVAL     5          /* intervalo entre sem_timedwait() (segundos) */

/* --- Manejo de señal asíncrona para permitir SIGTERM --- */
static volatile sig_atomic_t g_exit_signal = 0;
static void sig_handler(int sig) {
    (void)sig;
    g_exit_signal = 1;
}

/* --- Terminal: leer tecla espacio de forma bloqueante sin eco ni canonical --- */
static int set_raw_mode(int fd, struct termios *old)
{
    if (tcgetattr(fd, old) == -1) return -1;
    struct termios raw = *old;

    /* Modo no canónico, sin eco; lectura bloqueante de 1 byte */
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    return tcsetattr(fd, TCSAFLUSH, &raw);
}

static void restore_mode(int fd, const struct termios *old)
{
    (void)tcsetattr(fd, TCSAFLUSH, old);
}

/* Espera bloqueante con tecla ESPACIO en /dev/tty. Si no hay TTY, cae a stdin.
   También sale si llega SIGINT/SIGTERM */
static int wait_for_space_or_signal(void)
{
    int fd = open("/dev/tty", O_RDONLY);
    int use_stdin = 0;
    if (fd < 0) {
        fd = STDIN_FILENO;
        use_stdin = 1;
    }

    struct termios oldt;
    int term_ok = 0;
    if (isatty(fd)) {
        if (set_raw_mode(fd, &oldt) == 0) term_ok = 1;
    }

    /* Mensaje de UI: */
    fprintf(stderr,
        "\n\x1b[1;36m[Finalizador]\x1b[0m Presiona \x1b[1mESPACIO\x1b[0m para finalizar "
        "todos los procesos.\n");

    unsigned char ch = 0;
    ssize_t n;
    while (!g_exit_signal) {
        n = read(fd, &ch, 1);  /* read() bloqueante */
        if (n == 1) {
            if (ch == ' ') break;            /* TECLA espacio */
            if (ch == 'q' || ch == 'Q') break; /* atajo alternativo */
            /* cualquier otra tecla se ignora y seguimos bloqueados */
        } else if (n < 0) {
            if (errno == EINTR) continue; /* interrumpido por señal -> reintentar */
            /* error real de lectura: salimos igualmente a finalizar */
            break;
        }
    }

    if (term_ok) restore_mode(fd, &oldt);
    if (!use_stdin && fd >= 0) close(fd);

    return 0;
}

/* --- Impresión de estadísticas finales y escaneo de slots --- */
static void print_stats_and_scan(shared_header_t *hdr)
{
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    /* Contar ranuras ocupadas leyendo estado de cada slot */
    int occupied_count = 0;
    for (int i = 0; i < hdr->buffer_size; ++i) {
        /* Tomamos el semáforo del slot para leer su estado de forma consistente */
        if (sem_wait(&slot_sems[i]) == -1) {
            /* Si algo raro pasa, no bloqueamos la finalización: ignoramos ese slot */
            continue;
        }
        if (slots[i].occupied) occupied_count++;
        sem_post(&slot_sems[i]);
    }

    /* Impresión elegante de estadísticas finales */
    printf("\n\x1b[1;36m╔══════════════════════════════════════════════════════════════╗\x1b[0m\n");
    printf("\x1b[1;36m║                     ESTADÍSTICAS GLOBALES                    ║\x1b[0m\n");
    printf("\x1b[1;36m╚══════════════════════════════════════════════════════════════╝\x1b[0m\n");

    printf("  \x1b[1;33m• Total caracteres transferidos:\x1b[0m     \x1b[1;32m%lu\x1b[0m\n",
        (unsigned long)hdr->total_transferred);

    printf("  \x1b[1;33m• Caracteres en memoria compartida:\x1b[0m  \x1b[36m%d\x1b[0m / %d\n",
        occupied_count, hdr->buffer_size);

    printf("  \x1b[1;33m• Emisores lanzados:\x1b[0m                 \x1b[32m%d\x1b[0m   "
           "\x1b[1;33mActivos:\x1b[0m \x1b[36m%d\x1b[0m\n",
        hdr->total_emitters_spawned, hdr->active_emitters);

    printf("  \x1b[1;33m• Receptores lanzados:\x1b[0m              \x1b[32m%d\x1b[0m   "
           "\x1b[1;33mActivos:\x1b[0m \x1b[36m%d\x1b[0m\n",
        hdr->total_receivers_spawned, hdr->active_receivers);

    printf("  \x1b[1;33m• Tamaño de Memoria Compartida:\x1b[0m     \x1b[35m%zu bytes\x1b[0m\n",
        compute_shm_size(hdr->buffer_size));

    printf("\x1b[1;36m──────────────────────────────────────────────────────────────\x1b[0m\n");
    printf("  \x1b[90mEjecución finalizada correctamente.\x1b[0m\n");
    printf("\x1b[1;36m══════════════════════════════════════════════════════════════\x1b[0m\n\n");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <shm_name>\n", argv[0]);
        return 1;
    }

    /* Captura de señales para permitir Ctrl+C/SIGTERM como disparador alterno */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const char *shm_name = argv[1];

    /* Abrir y mapear la SHM */
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    shared_header_t *hdr = (shared_header_t*) map;

    /* Esperar señal física del operador: ESPACIO (bloqueante, sin busy-wait) */
    wait_for_space_or_signal();

    /* Indicar terminación de forma atómica mediante el semáforo de control */
    if (sem_wait(&hdr->control_sem) == -1) {
        perror("sem_wait control_sem");
        munmap(map, file_size);
        close(fd);
        return 1;
    }
    hdr->terminate_flag = 1;
    sem_post(&hdr->control_sem);

    /* Despertar a los procesos bloqueados en empty_count/full_count
          (suficientes posts para cubrir esperas pendientes) */
    int posts = hdr->buffer_size + 16; /* margen; evita quedarnos cortos */
    for (int i = 0; i < posts; ++i) {
        sem_post(&hdr->empty_count);
        sem_post(&hdr->full_count);
    }

    /* Si ya no hay procesos activos, imprimir y salir */
    sem_wait(&hdr->control_sem);
    int active_emitters  = hdr->active_emitters;
    int active_receivers = hdr->active_receivers;
    sem_post(&hdr->control_sem);

    if (active_emitters == 0 && active_receivers == 0) {
        print_stats_and_scan(hdr);
        munmap(map, file_size);
        close(fd);
        /* shm_unlink(shm_name); */
        return 0;
    }

    /* Esperar a que terminen los PEs:
          - Ultimo proceso haga sem_post(hdr->finalizer_sem)
          - Verificar contadores bajo control_sem con timeouts */
    struct timespec now;
    time_t waited = 0;
    int final_ok = 0;

    while (waited < MAX_WAIT_SECONDS) {
        clock_gettime(CLOCK_REALTIME, &now);
        struct timespec ts;
        ts.tv_sec  = now.tv_sec + WAIT_INTERVAL;
        ts.tv_nsec = now.tv_nsec;

        int rc = sem_timedwait(&hdr->finalizer_sem, &ts);
        if (rc == 0) {
            /* sem_post realizado por el último proceso en salir */
            final_ok = 1;
            break;
        } else {
            if (errno == ETIMEDOUT) {
                /* Verificamos si ya no quedan procesos activos */
                if (sem_wait(&hdr->control_sem) == -1) {
                    if (errno == EINTR) continue;
                    perror("sem_wait control_sem");
                    break;
                }
                active_emitters  = hdr->active_emitters;
                active_receivers = hdr->active_receivers;
                sem_post(&hdr->control_sem);

                if (active_emitters == 0 && active_receivers == 0) {
                    final_ok = 1;
                    break;
                }
                waited += WAIT_INTERVAL;
                continue;
            } else if (errno == EINTR) {
                /* interrumpido por señal; volvemos a intentar dentro del bucle */
                continue;
            } else {
                perror("sem_timedwait finalizer_sem");
                break;
            }
        }
    }

    if (!final_ok) {
        fprintf(stderr,
            "\n[Finalizador] Aviso: tiempo de espera excedido (%d s). "
            "Continuando con cierre.\n", MAX_WAIT_SECONDS);
    }

    /* 6) Estadísticas finales y limpieza */
    print_stats_and_scan(hdr);

    munmap(map, file_size);
    close(fd);

    /* Liberar el segmento de memoria compartida */
    if (shm_unlink(shm_name) == 0)
        printf("[Finalizador] Memoria compartida removida del sistema.\n");

    return 0;
}
