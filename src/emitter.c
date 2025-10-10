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

/* Estructura de los datos enviados hacia el receptor */
int send_byte(shared_header_t *hdr, sem_t *slot_sems, buffer_slot_t *slots, uint8_t byte, uint8_t key) {

    /* Bloqueo por Semaforo si los slots estan llenos*/
    if (sem_wait(&hdr->empty_count) == -1) {
        if (errno == EINTR) return -1;
        perror("sem_wait empty_count");
        return -1;
    }

    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem"); return -1; }
    int idx = hdr->head;
    hdr->head = (hdr->head + 1) % hdr->buffer_size;
    uint64_t seq = ++hdr->seq_counter;
    sem_post(&hdr->control_sem);

    if (sem_wait(&slot_sems[idx]) == -1) { perror("sem_wait slot"); return -1; }

    /* Encriptacion de los caracteres ingresados con Byte XOR key */
    uint8_t encoded = (uint8_t)(byte ^ key);
    slots[idx].ascii = encoded;
    slots[idx].seq = seq;
    clock_gettime(CLOCK_REALTIME, &slots[idx].ts);
    slots[idx].occupied = 1;

    /* Informacion del caracter ingresado */
    struct timespec ts = slots[idx].ts;
    time_t ssec = ts.tv_sec;
    struct tm tmv;
    localtime_r(&ssec, &tmv);

    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);

    /* mostrar milisegundos (3 decimales) */
    printf("\x1b[1;32m[EMIS]\x1b[0m idx=%2d seq=%5lu encoded=%3u (orig='%c') time=%s.%03ld\n",
        idx, (unsigned long)seq,
        (unsigned int)encoded,
        (byte >= 32 && byte <= 126) ? (char)byte : '?',
        tbuf, ts.tv_nsec / 1000000);

    sem_post(&slot_sems[idx]);
    sem_post(&hdr->full_count);
    return 0;
}

/* Ejecucion / Flujo del programa */
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
        fprintf(stderr, "Error abriendo el archivo %s: %s\n", hdr->filename, strerror(errno));
        munmap(map, file_size); close(fd);
        return 1;
    }

    /* señales: SIN SA_RESTART para que read/fgets sean interrumpibles por alarm/ctrl-c */
    struct sigaction sa_int;
    struct sigaction sa_alrm;
    memset(&sa_int, 0, sizeof(sa_int));
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_int.sa_handler = sigint_handler;
    sa_int.sa_flags = 0; /* sin SA_RESTART */
    sa_alrm.sa_handler = sigalrm_handler;
    sa_alrm.sa_flags = 0; /* sin SA_RESTART */
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);
    sigaction(SIGALRM, &sa_alrm, NULL);

    /* registrar : total_emitters_spawned++ y active_emitters++ */
    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem"); }
    hdr->total_emitters_spawned++;
    hdr->active_emitters++;
    sem_post(&hdr->control_sem);

    if (key_arg == 0) local_key = hdr->key;

    printf("[EMISOR] shm=%s mode=%s key=%u file=%s\n", shm_name, mode, (unsigned int)local_key, hdr->filename);

    /* Emisor en modo automatico */
    if (strcmp(mode, "auto") == 0) {
        printf("[EMISOR] Modo AUTO. Ingrese caracteres; se enviarán automáticamente cada 1 s.\n");
        /* modo raw para capturar single-char input sin esperar ENTER */
        set_raw_mode(1);
        const unsigned int interval = 1;
        alarm(interval);

        long last_pos = 0;
        while (keep_running && !hdr->terminate_flag) {
            char c;
            ssize_t r = read(STDIN_FILENO, &c, 1);
            if (r == 1) {
                /* si el usuario escribió, lo guardamos en el archivo y se hace append */
                if (fseek(f, 0, SEEK_END) == 0) {
                    fwrite(&c, 1, 1, f);
                    fflush(f);
                }
                continue; /* seguir esperando con read bloqueante */
            } else if (r == 0) {
                break;

            } else {
                /* r == -1 */
                if (errno == EINTR) {
                    /* señal interrumpe read; puede ser SIGALRM o SIGINT */
                    if (got_sigint) break;
                    if (timer_fired) {
                        /* Procesar nuevo contenido del archivo en disco */
                        timer_fired = 0;
                        if (fseek(f, last_pos, SEEK_SET) == -1) {
                            /* si no se puede posicionar, intentar continuar */
                        } else {
                            int ch;
                            while ((ch = fgetc(f)) != EOF) {
                                if (!keep_running || hdr->terminate_flag) break;
                                /* Se bloquea si el buffer está lleno con sem_wait */
                                if (send_byte(hdr, slot_sems, slots, (uint8_t)ch, local_key) == -1) {
                                    if (got_sigint) break;
                                }
                            }
                            last_pos = ftell(f);
                        }
                        /* Rearmar la alarma */
                        alarm(interval);
                        continue;
                    }
                    /* Continual con otra signal y repetir el loop */
                    continue;
                } else {
                    perror("read stdin");
                    break;
                }
            }
        }
        set_raw_mode(0);
    }
    else {
         /* Emisor en modo manual */
        printf("[EMISOR] Modo MANUAL. Ingrese caracteres y presione ENTER para enviar\n");
        char line[1024];

        while (keep_running && !hdr->terminate_flag) {
            /* fgets bloquea en kernel hasta que el usuario presione ENTER. */
            if (fgets(line, sizeof(line), stdin) == NULL) {
                if (feof(stdin)) break;
                if (errno == EINTR) {
                    /* fue interrumpido por señal, comprobar flags y continuar */
                    if (got_sigint) break;
                    continue;
                }
                /* otro error */
                if (ferror(stdin)) { perror("fgets"); break; }
            }

            size_t len = strcspn(line, "\n");
            if (len == 0) continue;

            /* Append al archivo con coherencia con modo auto */
            if (fseek(f, 0, SEEK_END) == 0) {
                fwrite(line, 1, len, f);
                fwrite("\n", 1, 1, f);
                fflush(f);
            }

            for (size_t i = 0; i < len; ++i) {
                if (!keep_running || hdr->terminate_flag) break;
                if (send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key) == -1) {
                    if (got_sigint) break;
                }
            }
        }
    }

    /* Parac cerrar decrementar active_emitters / ultimo hace finalizer_sem */
    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem (cleanup)"); }
    if (hdr->active_emitters > 0) hdr->active_emitters--;
    int ae = hdr->active_emitters;
    int ar = hdr->active_receivers;
    if (ae == 0 && ar == 0) {
        sem_post(&hdr->finalizer_sem);
    }
    sem_post(&hdr->control_sem);

    fclose(f);
    munmap(map, file_size);
    close(fd);
    printf("Emisor Terminado exitosamente.\n");
    return 0;
}
