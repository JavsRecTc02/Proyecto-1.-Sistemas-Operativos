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
#define MAX_WAIT_SECONDS 15          // tiempo total máximo antes de forzar salida 
#define WAIT_INTERVAL     5          // intervalo entre sem_timedwait() (segundos) 

/* --- Manejo de señal asíncrona para permitir SIGTERM --- */
// Flag para salir de esperas bloqueantes de teclado
static volatile sig_atomic_t g_exit_signal = 0;
static void sig_handler(int sig) {
    (void)sig;
    g_exit_signal = 1;
}

/* --- Terminal: leer tecla espacio de forma bloqueante --- */
static int set_raw_mode(int fd, struct termios *old)
{
    if (tcgetattr(fd, old) == -1) return -1;
    struct termios raw = *old;

    // Modo no canónico y sin eco; VMIN=1 bloquea hasta leer 1 byte; VTIME=0 sin timeout.
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSAFLUSH, &raw);
}

// Restaurar el modo original de la terminal
static void restore_mode(int fd, const struct termios *old)
{
    (void)tcsetattr(fd, TCSAFLUSH, old);
}

/* Espera bloqueante a que el operador presione ESPACIO en la TTY.
   - Si no hay /dev/tty disponible, usa stdin como respaldo.
   - Sale también si llega SIGINT/SIGTERM (g_exit_signal=1).
   - El read() duerme al proceso hasta que haya entrada. */

static int wait_for_space_or_signal(void)
{
    int fd = open("/dev/tty", O_RDONLY);
    int use_stdin = 0;
    if (fd < 0) {
        fd = STDIN_FILENO;       // respaldo: stdin
        use_stdin = 1;
    }

    struct termios oldt;
    int term_ok = 0;
    if (isatty(fd)) {
        if (set_raw_mode(fd, &oldt) == 0) term_ok = 1;
    }

    // Mensaje del Finalizador en consola
    fprintf(stderr,
        "\n\x1b[1;36m[Finalizador]\x1b[0m Presiona \x1b[1mESPACIO\x1b[0m (o 'Q') para finalizar.\n");

    unsigned char ch = 0;
    ssize_t n = read(fd, &ch, 1);   // read bloquea el kernel duerme el proceso

    // Restaurar y cerrar antes de cualquier retorno/recursión
    if (term_ok) restore_mode(fd, &oldt);
    if (!use_stdin && fd >= 0) close(fd);

    // Si llegó una señal y read() fue interrumpido, respetamos la señal para salir
    if (n < 0 && errno == EINTR) {
        // Con SIGINT/SIGTERM handler puso g_exit_signal=1
        return 0;
    }

    // Si leímos una tecla válida → continuar con la finalización
    if (n == 1 && (ch == ' ' || ch == 'q' || ch == 'Q')) {
        return 0;
    }

    // Tecla inválida
    return wait_for_space_or_signal();
}


/* --- Impresión de estadísticas finales y escaneo de slots --- */
// Recorre los slots de forma segura y muestra métricas.
static void print_stats_and_scan(shared_header_t *hdr)
{
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    // Contar ranuras ocupadas leyendo estado de cada slot
    int occupied_count = 0;
    for (int i = 0; i < hdr->buffer_size; ++i) {
        // Tomamos el semáforo del slot para leer su estado de forma consistente
        if (sem_wait(&slot_sems[i]) == -1) {
            // No se bloque por error, si falla se ignora el slot y sigue
            continue;
        }
        if (slots[i].occupied) occupied_count++;
        sem_post(&slot_sems[i]);
    }

    // Imprimir las estadísticas finales 
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


/* ============================== PROCESO DE FINALIZAR ============================== */

int main(int argc, char **argv)
{
    // Recibe solo el nombre del objeto de mem compartida
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <shm_name>\n", argv[0]);
        return 1;
    }

    // Captura de señales para permitir Ctrl+C/SIGTERM como disparador alterno
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const char *shm_name = argv[1];

    // Abrir y mapear la SHM existente
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }
    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    shared_header_t *hdr = (shared_header_t*) map;

    // Esperar señal física del operador, TECLA espacio, q, Q
    wait_for_space_or_signal();

    // Indicar terminación de forma atómica mediante el semáforo de control
    if (sem_wait(&hdr->control_sem) == -1) {
        perror("sem_wait control_sem");
        munmap(map, file_size);
        close(fd);
        return 1;
    }
    hdr->terminate_flag = 1;    // Despertar a los demas procesos para que terminen
    sem_post(&hdr->control_sem);

    // Despertar a procesos potencialmente bloqueados en empty/full
    int posts = hdr->buffer_size + 16; 
    for (int i = 0; i < posts; ++i) {
        sem_post(&hdr->empty_count);
        sem_post(&hdr->full_count);
    }

    // Si ya no hay procesos activos, imprimimos estadísticas y salimos
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

    /* Esperar a que terminen emisores/receptores:
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
            // sem_post realizado por el último proceso en salir
            final_ok = 1;
            break;
        } else {
            if (errno == ETIMEDOUT) {
                // Verificamos si ya no quedan procesos activos
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
                waited += WAIT_INTERVAL; // Acumular el tiempo de espera
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

    // Si el tiempo de espera fue excedido, forzar la detencion de los procesos
    if (!final_ok) {
        fprintf(stderr,
            "\n[Finalizador] Aviso: tiempo de espera excedido (%d s). "
            "Continuando con cierre.\n", MAX_WAIT_SECONDS);
    }

    // Estadísticas finales y limpieza de recursos
    print_stats_and_scan(hdr);

    munmap(map, file_size);
    close(fd);

    // Liberar el segmento de memoria compartida
    if (shm_unlink(shm_name) == 0)
        printf("[Finalizador] Memoria compartida removida del sistema.\n");

    return 0;
}
