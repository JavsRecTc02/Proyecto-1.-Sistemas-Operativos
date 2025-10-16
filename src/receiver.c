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

// Estas variables se modifican desde manejadores de señal.
// sig_atomic_t asegura que la lectura/escritura sea segura en señales.
static volatile sig_atomic_t keep_running = 1; // 1 seguir, 0 salir
static volatile sig_atomic_t timer_fired = 0;  // 1 salta SIGALRM

// CTRL+C señal de terminar.
void sigint_handler(int s) { (void)s; keep_running = 0; }
// Señal del timer. No hace trabajo, solo marca para interrumpir fgets.
void sigalrm_handler(int s) { (void)s; timer_fired = 1; }

/*
 * Procesa UNA sola ranura del buffer circular.
 * Precondición importante: el que llama YA consumió 1 unidad de full_count (sem_wait).
 * Retorna 0 si todo bien, -1 si hubo error interrumpible (EINTR) u otro error.
 */

static int process_one_slot(shared_header_t *hdr, sem_t *slot_sems, buffer_slot_t *slots, FILE *out, int key) {
    // Reservar el índice de lectura (tail) de forma atómica.
    // Se entra con control_sem para evitar carreras si hay varios receptores.

    if (sem_wait(&hdr->control_sem) == -1) {
        // el bucle principal decide si continua o sale
        if (errno == EINTR) return -1; 
        perror("sem_wait control_sem");
        return -1;
    }
    int idx = hdr->tail;                            // tomar el indice actual
    hdr->tail = (hdr->tail + 1) % hdr->buffer_size; // avanzar de forma circular
    sem_post(&hdr->control_sem);                    // liberar

    // Bloquear el slot/ranura en concretao para leerla.
    if (sem_wait(&slot_sems[idx]) == -1) {
        if (errno == EINTR) return -1;
        perror("sem_wait slot_sems");
        return -1;
    }

    // Si la ranura no esta marcada como ocupada, se libera y se regresa
    if (!slots[idx].occupied) {
        //condición para liberar y devolver
        sem_post(&slot_sems[idx]);
        sem_post(&hdr->empty_count);
        return 0;
    }

    // Decodificar los caracteres leidos de memoria compartida char XOR key
    uint8_t encoded = slots[idx].ascii;
    uint8_t decoded = encoded ^ (uint8_t)key;
    uint64_t seq = slots[idx].seq;
    struct timespec ts = slots[idx].ts;

    // Escribir el byte decodificado en el archivo de salida
    // Si falla, se informa y se continúa liberando correctamente
    if (fwrite(&decoded, 1, 1, out) != 1) {
        perror("fwrite salida");
    }
    fflush(out); // resultado de forma inmediata

    // Actualizar contadores globales de estadísticas bajo mutex global
    if (sem_wait(&hdr->control_sem) == -1) {
        // Se libera lo que esta bloqueado igualmente
        sem_post(&slot_sems[idx]);
        sem_post(&hdr->empty_count);
        if (errno == EINTR) return -1;
        perror("sem_wait control_sem");
        return -1;
    }
    hdr->total_transferred++; // Actualizar total de bytes transferidos
    sem_post(&hdr->control_sem);

    // Imprimir informacion: index, seq, encoded -> decoded y tiempo
    time_t ssec = ts.tv_sec;
    struct tm tmv;
    localtime_r(&ssec, &tmv);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);

    // Informacion de los caracteres consumidos/leidos desde el buffer
    printf(
        "\x1b[1;32m[RECV]\x1b[0m │ "         
        "\x1b[1;33mIDX\x1b[0m: %2d │ "        
        "\x1b[1;33mSEQ\x1b[0m: %5lu │ "       
        "\x1b[1;35mENC\x1b[0m: %3u │ "        
        "\x1b[1;36mDEC\x1b[0m: %3u │ "        
        "\x1b[1;37mCHAR\x1b[0m: '%c' │ "      
        "\x1b[90mTIME\x1b[0m: %s.%03ld\n",    
        idx,
        (unsigned long)seq,
        (unsigned int)encoded,
        (unsigned int)decoded,
        (decoded >= 32 && decoded <= 126) ? (char)decoded : '?',
        tbuf, (long)(ts.tv_nsec / 1000000));

    // Marcar la ranura como libre solo para el monitor solo visual
    slots[idx].occupied = 0;
    slots[idx].ascii = 0;
    slots[idx].seq = 0;
    // Liberar la ranura y notificar que hay un hueco disponible
    sem_post(&slot_sems[idx]);
    sem_post(&hdr->empty_count);
    return 0;
}

/* ----------------------- Flujo del Proceso de Receptor ---------------------- */

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <shm_name> <mode:manual|auto> <key>\n", argv[0]);
        return 1;
    }

    // Argumentos que solicita el recepto
    const char *shm_name = argv[1];
    const char *mode = argv[2];
    int key = atoi(argv[3]);

    // Abrir la memoria compartida existente 
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    // Medir el size real del objeto para mapear esa cantidad exactamente
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    // Mapear el segmento en espacio de direcciones con lectura/escritura compartida
    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    // Calcular punteros a las estructuras dentro del mapeo
    shared_header_t *hdr = (shared_header_t*) map;
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    // Archivo de salida donde se escribe lo recibido, en modo append
    FILE *out = fopen("texto_salida.txt", "a");
    if (!out) { perror("fopen salida"); munmap(map, file_size); close(fd); return 1; }

    // Configurar señales
    // - SIGINT/SIGTERM piden salida ordenada (keep_running = 0)
    // - SIGALRM sirve para despertar fgets en modo manual y revisar terminate_flag
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;                    // sin SA_RESTART, fgets/read pueden interrumpirse
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_alrm;
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_alrm.sa_handler = sigalrm_handler;
    sa_alrm.sa_flags = 0;               // sin SA_RESTART: alarm interrumpe fgets
    sigaction(SIGALRM, &sa_alrm, NULL);

    // Registrar nacimiento del receptor de forma atomica, con metricas y contadores
    if (sem_wait(&hdr->control_sem) == -1) {
        if (errno == EINTR) goto cleanup;
        perror("sem_wait control_sem");
        goto cleanup;
    }
    hdr->total_receivers_spawned++; // Actualizar total de receptores creados
    hdr->active_receivers++;        // Actualizar receptores vivos en el momento
    sem_post(&hdr->control_sem);
    // Mensaje de receptor creado exitosamente
    printf("[RECEPTOR] Conectado a %s modo=%s key=%d buffer_size=%d\n",
           shm_name, mode, key, hdr->buffer_size);

    /* ============================== MODO AUTOMATICO ============================== */
    if (strcmp(mode, "auto") == 0) {
        // - Se bloquea en full_count cuando no hay datos
        // - Sale si keep_running=0 o si finalize_flag pide terminar
        while (keep_running && !hdr->terminate_flag) {
            if (sem_wait(&hdr->full_count) == -1) {
                // Interrumpido por señal, volver al loop para reevaluar
                if (errno == EINTR) continue; 
                perror("sem_wait full_count");
                break;
            }
            // Si el finalizador pidió terminar, devolvemos el “token” y salimos.
            if (hdr->terminate_flag) {
                sem_post(&hdr->full_count);
                break;
            }
            // Procesar una ranura/slot, ya se consume el full_count
            if (process_one_slot(hdr, slot_sems, slots, out, key) == -1) {
                // si fue EINTR intentamos seguir; si es error entonces sale
                if (errno == EINTR) continue;
                break;
            }
        }
    }
     /* ============================== MODO MANUAL ============================== */
    else if (strcmp(mode, "manual") == 0) {
        // - Se presiona ENTER para consumir 1 carácter
        // - alarm(1) para interrumpir periódicamente fgets con EINTR,
        //   revisar terminate_flag y permitir salida rápida sin bloquear
        char line[512];
        printf("[RECEPTOR] Modo MANUAL. Presione ENTER para leer 1 carácter.\n");

        //Alarma para interrumpir fgets y chequear terminate_flag
        const unsigned int interval = 1;    // Segundos para despertar fgets
        timer_fired = 0;
        alarm(interval);                    // Iniciar el timer

        while (keep_running && !hdr->terminate_flag) {
            // Espera la entrada del usuario, read bloqueante
            // Si llega SIGALRM/SIGINT, fgets devuelve NULL con errno=EINTR
            if (fgets(line, sizeof(line), stdin) == NULL) {
                if (feof(stdin)) break;                               //EOF en stdin
                if (errno == EINTR) {
                    //Interrumpido por señal SIGALRM o SIGINT, si no hay nada que hacer sale
                    if (!keep_running || hdr->terminate_flag) break;  
                    // Si fue la alarma, rearmar para el próximo despertar.
                    if (timer_fired) {
                        timer_fired = 0;
                        alarm(interval);
                    }
                    continue;
                }
                if (ferror(stdin)) { perror("fgets"); break; }
            }
            // Si el suario presionó ENTER, consumir 1 carácter.
            if (sem_wait(&hdr->full_count) == -1) {
                if (errno == EINTR) {
                    // Señal durante la espera de datos, revisar y continuar
                    if (!keep_running || hdr->terminate_flag) break;
                    if (timer_fired) {
                        timer_fired = 0;
                        alarm(interval);
                    }
                    continue;
                }
                perror("sem_wait full_count");
                break;
            }
            // Si en este punto se pidió terminar, devolvemos y salimos.
            if (hdr->terminate_flag) {
                sem_post(&hdr->full_count);
                break;
            }
            // Procesar una ranura, un caracter
            if (process_one_slot(hdr, slot_sems, slots, out, key) == -1) {
                if (errno == EINTR) {
                    if (!keep_running || hdr->terminate_flag) break;
                    if (timer_fired) {
                        timer_fired = 0;
                        alarm(interval);
                    }
                    continue;
                }
                break;
            }
            // Rearmar la alarma para que, si el usuario no presiona ENTER,
            // podamos despertar fgets y revisar flags.
            alarm(interval);
        }
        alarm(0); // Cancelar la alarma al salir
    }

    else {
        fprintf(stderr, "Modo no reconocido: use 'auto' o 'manual'\n");
    }

    // Al salir decrementar receptores activos y notificar finalizer si somos últimos
    if (sem_wait(&hdr->control_sem) == -1) {
        if (errno != EINTR) perror("sem_wait control_sem");
    } else {
        if (hdr->active_receivers > 0) hdr->active_receivers--;
        int ae = hdr->active_emitters;
        int ar = hdr->active_receivers;
        if (ae == 0 && ar == 0) sem_post(&hdr->finalizer_sem); // En caso de ser los ultimos
        sem_post(&hdr->control_sem);
    }

cleanup:
    // Limpierza de recursos del proceso receptor
    fclose(out);
    munmap(map, file_size);
    close(fd);
    printf("Receptor Terminado Exitosamente.\n");
    return 0;
}
