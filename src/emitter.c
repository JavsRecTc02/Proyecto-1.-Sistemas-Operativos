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

// Variable para el tick de alarma
static void sigalrm_handler(int s) { (void)s; }

/* ---------------------------------------------------------------------------
 * Manejo del modo RAW del terminal para leer carácter a carácter:
 * - Guardamos la config actual para restaurarla
 * - Lectura bloqueante de 1 byte
 * --------------------------------------------------------------------------- */
static struct termios saved_tio;
static int raw_enabled = 0;

// Funcion para capturar entrada caracter a caracter y configurar la terminal, bloqueante o no bloqueante
void set_raw_mode(int enable) {
    struct termios tio;

    // ACTIVAR modo RAW
    if (enable && !raw_enabled) {
        // Guardar el estado actual del terminal para poder restaurarlo
        tcgetattr(STDIN_FILENO, &saved_tio);
        // Crear una copia modificable de la configuración
        tio = saved_tio;
        // Desactivar opciones de procesamiento en terminal
        tio.c_lflag &= ~(ICANON | ECHO);

        // Configurar los parámetros de lectura, de forma bloqueante
        // Lectura bloqueante de 1 caracter 
        tio.c_cc[VMIN]  = 1;
        tio.c_cc[VTIME] = 0;
        // Aplicar los cambios
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        // Marcar que el modo RAW está activo
        raw_enabled = 1;
    }
    // DESACTIVAR modo RAW → restaurar configuración original
    else if (!enable && raw_enabled) {
        // Restaurar la configuración guardada del terminal
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        // Marcar modo normal
        raw_enabled = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Enviar UN byte al receptor desde del buffer en memoria compartida.
 *   - sem_wait(empty_count): bloquear si no hay huecos
 *   - control_sem: reservar índice head y avanzar circularmente
 *   - slot_sems[idx]:  entrar a la ranura, escribir datos
 *   - sem_post(full_count): anunciar que hay un item disponible
 * --------------------------------------------------------------------------- */

int send_byte(shared_header_t *hdr, sem_t *slot_sems, buffer_slot_t *slots, uint8_t byte, uint8_t key) {

    // Esperar hueco libre en buffer, bloqueo por Semaforo si los slots estan llenos
    if (sem_wait(&hdr->empty_count) == -1) {
        if (errno == EINTR) return -1;
        perror("sem_wait empty_count");
        return -1;
    }
    // Reservar indice de escritura de forma atomica  y aumentar seq
    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem"); return -1; }
    int idx = hdr->head;
    hdr->head = (hdr->head + 1) % hdr->buffer_size; // Avance circular
    uint64_t seq = ++hdr->seq_counter;              // Numero de seq
    sem_post(&hdr->control_sem);

    // Entrar en el slot/ranura en concreto sin condicion de carrera
    if (sem_wait(&slot_sems[idx]) == -1) { perror("sem_wait slot"); return -1; }

    // Codificar con XOR y llenar metadatos del slot
    uint8_t encoded = (uint8_t)(byte ^ key);
    slots[idx].ascii = encoded;
    slots[idx].seq = seq;
    clock_gettime(CLOCK_REALTIME, &slots[idx].ts);
    slots[idx].occupied = 1;                        // Para el monitor solamente

    // Informacion del caracter ingresado y enviado
    struct timespec ts = slots[idx].ts;
    time_t ssec = ts.tv_sec;
    struct tm tmv;
    localtime_r(&ssec, &tmv);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);
    // Mostrar informacion de los caracteres ingresados al buffer de mem compartida

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

    // Salir de la ranura y avisar que hay un item disponible para el receptor
    sem_post(&slot_sems[idx]);
    sem_post(&hdr->full_count);
    return 0;
}

/* ----------------------- Flujo del Proceso de Emisor ---------------------- */

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <shm_name> <mode:manual|auto> <key>\n", argv[0]);
        return 1;
    }

    /* ---------------------------------------------------------------
    *  PARÁMETROS DE ENTRADA DEL PROGRAMA
    *  argv[1]: nombre del segmento de memoria compartida
    *  argv[2]: modo de operación
    *  argv[3]: key usada para cifrar/descifrar
    * --------------------------------------------------------------- */

    const char *shm_name = argv[1];
    const char *mode = argv[2];
    int key_arg = atoi(argv[3]);
    uint8_t local_key = (uint8_t)key_arg; // Normaliza la key a 8 bits

    // Abrir el objeto de memoria compartida existente con permisos de r|w.
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) { perror("shm_open"); return 1; }

    // Obtener su tamaño real, para mapear justo esa cantidad
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { perror("lseek"); close(fd); return 1; }

    // Mapearlo con permisos de lectura y escritura, compartido entre procesos
    void *map = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    // Punteros a las estructuras dentro del mapeo.
    shared_header_t *hdr = (shared_header_t *)map;
    // Obtener puntero al arreglo de semáforos por cada slot del buffer 
    sem_t *slot_sems = get_slot_sems(hdr);
    // Obtener puntero al arreglo de estructuras, donde se almacenan los datos transferidos
    buffer_slot_t *slots = get_slots(hdr);

    /* Archivo que mantiene el texto, el texto de entrada .txt */
    FILE *f = fopen(hdr->filename, "a+");
    if (!f) {
        fprintf(stderr, "Error abriendo el archivo %s: %s\n", hdr->filename, strerror(errno));
        munmap(map, file_size); close(fd);
        return 1;
    }

    // Configurar señales, sin interrupciones con Control+C con SIGALRM para ticks
    struct sigaction sa_ign, sa_alrm;
    memset(&sa_ign,  0, sizeof(sa_ign));
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_ign.sa_handler  = SIG_IGN;          
    sigaction(SIGINT,  &sa_ign,  NULL);
    sigaction(SIGTERM, &sa_ign,  NULL);
    sa_alrm.sa_handler = sigalrm_handler;  
    sa_alrm.sa_flags   = 0;                
    sigaction(SIGALRM, &sa_alrm, NULL);

    // Registrar nacimiento del Emisor, actualizar la lista de emisores lanzados
    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem"); }
    hdr->total_emitters_spawned++;
    hdr->active_emitters++;
    sem_post(&hdr->control_sem);
    // En caso de que la key sea cero, se usa la que esta por defecto en el inicializador
    if (key_arg == 0) local_key = hdr->key;

    // Mensaje de Emisor creado correctamente
    printf("[EMISOR] shm=%s mode=%s key=%u file=%s\n", shm_name, mode, (unsigned int)local_key, hdr->filename);
    /* ============================== MODO AUTOMATICO ============================== */
    if (strcmp(mode, "auto") == 0) {
        printf("[EMISOR] Modo AUTO. Ingrese caracteres; se enviarán automáticamente cada 1 s.\n");
        set_raw_mode(1);                 // Modo RAW para leer char a char 
        const unsigned int interval = 1; // Segundos entre envios automaticos
        alarm(interval);                 // Primera alarma
        char line[1024];                 // Buffer local para acumular char en la consola
        size_t len = 0;                  // Registrar la cantidad de char acumulados

        while (!hdr->terminate_flag) {
            char c;
            // Lectura Bloqueante de un byte desde stdin en RAW
            ssize_t r = read(STDIN_FILENO, &c, 1);
            
            if (r == 1) {  // llegó un carácter: guardar y acumular
                if (fseek(f, 0, SEEK_END) == 0) {
                    fwrite(&c, 1, 1, f);
                    fflush(f);
                }
                if (len < sizeof(line) - 1) {
                    line[len++] = c;    // acumular para el envío en el próximo tick
                }
                continue;
            }

            if (r == 0) break; // EOF
            if (r == -1) {
                if (errno == EINTR) {
                    // EINTR aquí viene del SIGALRM (SIGINT/SIGTERM están ignoradas)
                    // => es el "tick": enviar lo acumulado y rearmar la alarma
                    alarm(0);           // Pausar alarma mientras se envía
                    if (len > 0) {
                        for (size_t i = 0; i < len; ++i) {
                            if (hdr->terminate_flag) break;
                            if (send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key) == -1) {
                                // En este diseño no salimos por otras señales, así que continuar
                            }
                        }
                        len = 0;        // limpiar buffer local
                    }
                    alarm(interval);    // reactivar la alarma
                    continue;           // volver al loop
                } else {
                    perror("read stdin");
                    break;
                }
            }
        }
        // Al salir, si quedó algo sin enviar, enviarlo
        if (len > 0) {
            for (size_t i = 0; i < len; ++i) {
                if (hdr->terminate_flag) break;
                send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key);
            }
        }
        set_raw_mode(0);    // Restaurar terminal
        alarm(0);           // Cancelar alarma
    }

    else {

        /* ============================== MODO MANUAL ============================== */
        printf("[EMISOR] Modo MANUAL. Ingrese caracteres y presione ENTER para enviar\n");
        char line[1024];
        const unsigned int interval = 1;   // Usar la alarma para salida rápida si finaliza
        alarm(interval);                   // Primera alarma

        while (!hdr->terminate_flag) {
            // fgets bloquea hasta ENTER; si llega SIGALRM retorna con EINTR (por nuestro handler sin SA_RESTART)
            if (fgets(line, sizeof(line), stdin) == NULL) {
                if (feof(stdin)) break;     // EOF
                if (errno == EINTR) {
                    // Fue la alarma: rearmar y seguir esperando
                    alarm(interval);
                    continue;
                }
                if (ferror(stdin)) { perror("fgets"); break; }
            }

            // Eliminar el salto de linea y calcular longitud útil; línea vacía no se envía
            size_t len = strcspn(line, "\n");
            if (len == 0) {
                alarm(interval);
                continue;
            }

            // Guardar la línea en archivo de salida
            if (fseek(f, 0, SEEK_END) == 0) {
                fwrite(line, 1, len, f);
                fwrite("\n", 1, 1, f);
                fflush(f);
            }

            // Enviar la línea al receptor, carácter a carácter
            alarm(0);  // Desactivar alarma mientras enviamos
            for (size_t i = 0; i < len; ++i) {
                if (hdr->terminate_flag) break;
                if (send_byte(hdr, slot_sems, slots, (uint8_t)line[i], local_key) == -1) {
                    // No salimos por señales; continuar o romper si lo prefieres
                }
            }
            alarm(interval);  // Reactivar alarma
        }

        alarm(0); // Cancelar alarma al salir
    }

    // Para notificar y cerrar decrementar active_emitters / ultimo hace finalizer_sem
    if (sem_wait(&hdr->control_sem) == -1) { perror("sem_wait control_sem (cleanup)"); }
    if (hdr->active_emitters > 0) hdr->active_emitters--;
    int ae = hdr->active_emitters;
    int ar = hdr->active_receivers;
    if (ae == 0 && ar == 0) {
        sem_post(&hdr->finalizer_sem);   // último en salir, notifica al finalizer
    }
    sem_post(&hdr->control_sem);

    // Cerrar recursos locales del emisor
    fclose(f);
    munmap(map, file_size);
    close(fd);
    printf("Emisor Terminado exitosamente.\n");
    return 0;
}
