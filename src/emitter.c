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

/* Funcion para capturar entrada caracter a caracter y configurar la terminal, bloqueante o no bloqueante*/
void set_raw_mode(int enable) {
    struct termios tio;

    /* ACTIVAR modo RAW */
    if (enable && !raw_enabled) {
        /* Guardar el estado actual del terminal para poder restaurarlo */
        tcgetattr(STDIN_FILENO, &saved_tio);
        /* Crear una copia modificable de la configuración */
        tio = saved_tio;
        /*Desactivar opciones de procesamiento en terminal*/
        tio.c_lflag &= ~(ICANON | ECHO);

        /*Configurar los parámetros de lectura, de forma bloqueante */
        tio.c_cc[VMIN]  = 1;
        tio.c_cc[VTIME] = 0;
        /* Aplicar los cambios */
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        /* Marcar que el modo RAW está activo */
        raw_enabled = 1;
    }
    /* DESACTIVAR modo RAW → restaurar configuración original */
    else if (!enable && raw_enabled) {
        /* Restaurar la configuración guardada del terminal */
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        /* Marcar modo normal */
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

    /* mostrar informacion de los caracteres ingresados al buffer de mem compartida */

    printf(
        "\x1b[1;32m[EMIS]\x1b[0m │ "
        "\x1b[36mIDX\x1b[0m: %2d │ "
        "\x1b[36mSEQ\x1b[0m: %5lu │ "
        "\x1b[34mENC\x1b[0m: %3u │ "
        "\x1b[33mORIG\x1b[0m: '%c' │ "
        "\x1b[90mTIME\x1b[0m: %s.%03ld\n",
        idx,
        (unsigned long)seq,
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

    /* ---------------------------------------------------------------
    *  PARÁMETROS DE ENTRADA DEL PROGRAMA
    *  argv[1] → nombre del segmento de memoria compartida
    *  argv[2] → modo de operación
    *  argv[3] → key usada para cifrar/descifrar
    * --------------------------------------------------------------- */

    const char *shm_name = argv[1];
    const char *mode = argv[2];
    int key_arg = atoi(argv[3]);
    /* Guardar key como entero de 8 bits */
    uint8_t local_key = (uint8_t)key_arg;

    /* ---------------------------------------------------------------
    *  ABRIR LA MEMORIA COMPARTIDA EXISTENTE
    *  Con permisos de lectura y escritura 
    * ----------------------------------------------------------------*/
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    /* ---------------------------------------------------------------
    *  OBTENER EL TAMAÑO DEL ARCHIVO DE MEMORIA COMPARTIDA
    *  Usa lseek() para mover el puntero al final (SEEK_END)
    *  y así obtener su tamaño total en bytes.
    * --------------------------------------------------------------- */
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    /* ---------------------------------------------------------------
    *  MAPEAR LA MEMORIA COMPARTIDA EN EL ESPACIO DEL PROCESO
    *  PROT_READ | PROT_WRITE → permisos de lectura y escritura.
    *  MAP_SHARED → los cambios se reflejan para todos los PEs
    * --------------------------------------------------------------- */
    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }


    /* Encabezado principal con metadatos del sistema compartido */
    shared_header_t *hdr = (shared_header_t *)map;

    /* Obtener puntero al arreglo de semáforos por cada slot del buffer */
    sem_t *slot_sems = get_slot_sems(hdr);

    /* Obtener puntero al arreglo de estructuras, donde se almacenan 
    los datos transferidos */
    buffer_slot_t *slots = get_slots(hdr);

    /* archivo que mantiene el texto (coherencia con inicializer) */
    FILE *f = fopen(hdr->filename, "a+");
    if (!f) {
        fprintf(stderr, "Error abriendo el archivo %s: %s\n", hdr->filename, strerror(errno));
        munmap(map, file_size); close(fd);
        return 1;
    }

    struct sigaction sa_int;
    struct sigaction sa_alrm;
    memset(&sa_int, 0, sizeof(sa_int));
    memset(&sa_alrm, 0, sizeof(sa_alrm));

    /* sin SA_RESTART para que fgets/read puedan ser interrumpidas */
    sa_int.sa_handler = sigint_handler;
    sa_int.sa_flags = 0; 
    sa_alrm.sa_handler = sigalrm_handler;
    sa_alrm.sa_flags = 0;

    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);
    sigaction(SIGALRM, &sa_alrm, NULL);

    /* Registrar, actualizar la lista de emisores lanzados */
    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem"); }
    hdr->total_emitters_spawned++;
    hdr->active_emitters++;
    sem_post(&hdr->control_sem);

    if (key_arg == 0) local_key = hdr->key;

    printf("[EMISOR] shm=%s mode=%s key=%u file=%s\n", shm_name, mode, (unsigned int)local_key, hdr->filename);

    /* Emisor en modo automatico */
    if (strcmp(mode, "auto") == 0) {
        printf("[EMISOR] Modo AUTO. Ingrese caracteres; se enviarán automáticamente cada 1 s.\n");
        set_raw_mode(1);
        /* Variables para la alarma/ timer y buff local para escribir en consola*/
        const unsigned int interval = 1;
        timer_fired = 0;
        alarm(interval);
        char line[1024];
        size_t len = 0;

        while (keep_running && !hdr->terminate_flag) {
            char c;
            ssize_t r = read(STDIN_FILENO, &c, 1);  /* bloqueante */

            /* r = 1 Carácter leído entonce guardar y acumular*/
            if (r == 1) {
                /* guardar en archivo */
                if (fseek(f, 0, SEEK_END) == 0) {
                    fwrite(&c, 1, 1, f);
                    fflush(f);
                }
                /* acumular caracter desde consola en buffer local */
                if (len < sizeof(line) - 1) {
                    line[len++] = c;
                }
                continue;
            }
            /* r = 0 Fin de entrada de caracteres y salir del bucle */
            if (r == 0) break;

            /* r = -1 Interrupción por señal de alarma / timer */
            if (r == -1) {
                if (errno == EINTR) {
                    /* interrupción por señal */
                    if (got_sigint) break;
                    if (hdr->terminate_flag || !keep_running) break;

                    /* Si fue la alarma → enviar todo lo acumulado */
                    if (timer_fired) {
                        timer_fired = 0;
                        /* Detener mientras se enviar los char */
                        alarm(0);  

                        if (len > 0) {
                            for (size_t i = 0; i < len; ++i) {
                                if (!keep_running || hdr->terminate_flag) break;
                                if (send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key) == -1) {
                                    if (got_sigint) break;
                                }
                            }
                            /* limpiar linea de escritura en consola */
                            len = 0;
                        }
                        /* Reactivar timer */
                        alarm(interval);
                    }continue;
                } else {
                    perror("read stdin");
                    break;
                }
            }
        }
        /* Enviar el resto de char antes de salir */
        if (len > 0) {
            for (size_t i = 0; i < len; ++i) {
                if (!keep_running || hdr->terminate_flag) break;
                send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key);
            }
        }
        set_raw_mode(0);
        alarm(0);
    }

    else {
        /* Emisor en modo manual */
        printf("[EMISOR] Modo MANUAL. Ingrese caracteres y presione ENTER para enviar\n");
        char line[1024];
        const unsigned int interval = 1;
        timer_fired = 0;
        alarm(interval);

        while (keep_running && !hdr->terminate_flag) {
            /* fgets bloquea hasta ENTER, pero podrá ser interrumpido por SIGALRM */
            if (fgets(line, sizeof(line), stdin) == NULL) {
                if (feof(stdin)) break;
                if (errno == EINTR) {
                    /* solo interrumpimos fgets si termina el proceso */
                    if (got_sigint) break;
                    if (hdr->terminate_flag || !keep_running) break;
                    /* Rearmar la alarma si fue timer */
                    if (timer_fired) {
                        timer_fired = 0;
                        alarm(interval);
                    }
                    continue;
                }
                if (ferror(stdin)) { perror("fgets"); break; }
            }
            size_t len = strcspn(line, "\n");
            if (len == 0) {
                alarm(interval);
                continue;
            }
            /* Escribir al archivo de salida */
            if (fseek(f, 0, SEEK_END) == 0) {
                fwrite(line, 1, len, f);
                fwrite("\n", 1, 1, f);
                fflush(f);
            }

            /* Enviar los caracteres uno a uno */
            /* Desactivar alarma de timer para no cortar los char */
            alarm(0);
            for (size_t i = 0; i < len; ++i) {
                if (!keep_running || hdr->terminate_flag) break;
                if (send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key) == -1) {
                    if (got_sigint) break;
                }
            }
            /* Reactivar el timer después de enviar los char */
            timer_fired = 0;
            alarm(interval);
        }
        alarm(0);
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
