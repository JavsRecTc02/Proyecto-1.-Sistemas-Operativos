#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "shared.h"
#include <inttypes.h>


int main(int argc, char **argv) {
    // Esperamos exactamente 4 parámetros de usuario.
    // 1) nombre del SHM iniciado con /
    // 2) tamaño del buffer
    // 3) clave XOR (0-255)
    // 4) archivo de entrada, se crea vacio si no existe
    if (argc != 5) {
        fprintf(stderr, "Uso: %s <shm_name> <buffer_size> <key (0-255)> <input_file>\n", argv[0]);
        return 1;
    }

    /* -------------------- Lectura y validación de parámetros -------------------- */
    const char *shm_name = argv[1];
    int buf_size = atoi(argv[2]);
    int key = atoi(argv[3]);
    const char *infile = argv[4];

    // Verificacion de cada uno de los elementos de entrada
    if (shm_name[0] != '/') {
        fprintf(stderr, "Error: shm_name debe comenzar con '/'. Ej: /my_shm\n");
        return 1;
    }
    if (buf_size <= 0) {
        fprintf(stderr, "Error: buffer_size debe ser > 0\n");
        return 1;
    }
    if (key < 0 || key > 255) {
        fprintf(stderr, "Error: key debe estar en un rango de [0-255] \n");
        return 1;
    }
    if (strlen(infile) >= MAX_FILENAME) {
        fprintf(stderr, "Error: ruta de input_file demasiado larga (max %d)\n", MAX_FILENAME-1);
        return 1;
    }

    // Tamaño total del segmento SHM = header + N*sem_t + N*buffer_slot_t
    size_t shm_size = compute_shm_size(buf_size);

    // Si el archivo de entrada no existe, se crea vacío para mantener coherencia
    struct stat st;
    if (stat(infile, &st) == -1) {
        FILE *tf = fopen(infile, "w");
        if (!tf) {
            fprintf(stderr, "No se pudo crear input_file '%s': %s\n", infile, strerror(errno));
            return 1;
        }
        fclose(tf);
        printf("[initializer] Archivo de entrada '%s' creado (vacío).\n", infile);
    }

    /* ------------------------- Crear memoria compartida ------------------------- */
    // O_CREAT | O_EXCL, falla si ya existe una con el mismo nombre
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, SHM_DEFAULT_MODE);
    if (fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "Error: shared memory '%s' ya existe. Elimínela o use otro nombre.\n", shm_name);
        } else {
            perror("shm_open");
        }
        return 1;
    }

    // Ajustar el tamaño del objeto SHM exactamente a shm_size bytes
    if (ftruncate(fd, (off_t)shm_size) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(shm_name);
        return 1;
    }

    // Mapear el segmento en el espacio de direcciones de este proceso
    void *map = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(shm_name);
        return 1;
    }

    // Limpiar a cero todo el bloque mapeado (header, semáforos y slots)
    // Deja un estado conocido y quitar basura inicial.
    memset(map, 0, shm_size);

    // Puntero al header dentro del mapeo
    shared_header_t *hdr = (shared_header_t*) map;

    /* --------------------- Inicialización de campos del header --------------------- */
    hdr->buffer_size = buf_size;
    hdr->head = 0;
    hdr->tail = 0;
    hdr->seq_counter = 0;
    hdr->total_transferred = 0;
    hdr->total_emitters_spawned = 0;
    hdr->total_receivers_spawned = 0;
    hdr->active_emitters = 0;
    hdr->active_receivers = 0;
    hdr->key = (uint8_t) key;
    strncpy(hdr->filename, infile, MAX_FILENAME-1);
    hdr->filename[MAX_FILENAME-1] = '\0';
    hdr->terminate_flag = 0;

    /* ---------------------------- Semáforos globales ---------------------------- */
    // pshared=1, cuando es visibles entre procesos vía SHM

    // control_sem para cambios en el header (head/tail/contadores)
    if (sem_init(&hdr->control_sem, 1, 1) == -1) {
        perror("sem_init control_sem");
        goto cleanup_error;
    }
    // empty_count, saber cuántos huecos libres hay en el buffer; inicia en N
    if (sem_init(&hdr->empty_count, 1, buf_size) == -1) {
        perror("sem_init empty_count");
        goto cleanup_error;
    }
    // full_count, saber cuántos items listos hay; inicia en 0 (buffer vacío)
    if (sem_init(&hdr->full_count, 1, 0) == -1) {
        perror("sem_init full_count");
        goto cleanup_error;
    }
    // finalizer_sem, se usa para despertar al finalizador cuando ya no quedan procesos activos
    if (sem_init(&hdr->finalizer_sem, 1, 0) == -1) {
        perror("sem_init finalizer_sem");
        goto cleanup_error;
    }

    /* --------------------- Semáforos por ranura y slots iniciales --------------------- */
    // Obtener las regiones dentro del mapeo, con arreglo de semáforos por-slot y arreglo de slots
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

    // Por cada slot de la mem circular
    // - semáforo tipo mutex inicial en 1 (permite acceso exclusivo al slot)
    // - estado del slot occupied=0 y datos en cero
    for (int i = 0; i < buf_size; ++i) {
        if (sem_init(&slot_sems[i], 1, 1) == -1) {
            perror("sem_init slot_sems[i]");
            goto cleanup_error;
        }
        slots[i].occupied = 0;
        slots[i].ascii = 0;
        slots[i].seq = 0;
        slots[i].ts.tv_sec = 0;
        slots[i].ts.tv_nsec = 0;
    }

    // Calcula tamaños reales y punteros ya mapeados
    size_t sz_hdr  = sizeof(shared_header_t);
    size_t sz_sem  = sizeof(sem_t);
    size_t sz_slot = sizeof(buffer_slot_t);

    // Validar con compute_shm_size(N), que ftruncate() fue exacto en la mem compartida
    size_t shm_size_expected = compute_shm_size(buf_size);

    /* ------------------------------- Informacion del Inicializador ------------------------------------- */
    printf("\n\x1b[1;36m╔══════════════════════════════════════════════════════════════╗\x1b[0m\n");
    printf("\x1b[1;36m║                INFORMACIÓN DE INICIALIZACIÓN                 ║\x1b[0m\n");
    printf("\x1b[1;36m╚══════════════════════════════════════════════════════════════╝\x1b[0m\n");

    printf("  \x1b[1;33m• Memoria compartida:\x1b[0m   \x1b[1;32m'%s'\x1b[0m\n", shm_name);
    printf("  \x1b[1;33m• Buffer slots:\x1b[0m         \x1b[36m%d\x1b[0m\n", buf_size);
    printf("  \x1b[1;33m• Clave XOR (8-bit):\x1b[0m    \x1b[35m%d\x1b[0m\n", key);
    printf("  \x1b[1;33m• Archivo de entrada:\x1b[0m   \x1b[37m%s\x1b[0m\n", hdr->filename);
    printf("  \x1b[1;33m• Tamaño SHM (bytes):\x1b[0m  \x1b[36m%zu\x1b[0m\n", shm_size);

    // Detalles de layout con tamaños y totales
    size_t total_simple = sz_hdr + (size_t)buf_size * sz_sem + (size_t)buf_size * sz_slot;

    printf("\x1b[1;36m──────────────── DETALLES DE LAYOUT ────────────────\x1b[0m\n");
    printf("  \x1b[90mHeader (shared_header_t)\x1b[0m : %zu\n", sz_hdr);
    printf("  \x1b[90mSemáforos por ranura (sem_t)\x1b[0m  : %zu  (× %d = %zu)\n",
        sz_sem, buf_size, (size_t)buf_size * sz_sem);
    printf("  \x1b[90mRanuras de buffer (buffer_slot_t)\x1b[0m : %zu  (× %d = %zu)\n",
        sz_slot, buf_size, (size_t)buf_size * sz_slot);
    printf("  \x1b[90mTotal esperado\x1b[0m  : \x1b[36m%zu\x1b[0m\n", total_simple);

    // Tamaño reservado según compute_shm_size(N) en el Header
    printf("  \x1b[90mTamaño reservado\x1b[0m : \x1b[36m%zu\x1b[0m\n", shm_size_expected);


    // Punteros para offset, del header, slot_sem, slots
    printf("  \x1b[90mPtr header\x1b[0m              : %p\n", (void*)hdr);
    printf("  \x1b[90mPtr slot_sems\x1b[0m           : %p (N x sem_t)\n", (void*)slot_sems);
    printf("  \x1b[90mPtr slots\x1b[0m               : %p (N x buffer_slot_t)\n", (void*)slots);

    // Comparar el size obtenido con el esperado
    if (shm_size != shm_size_expected) {
        printf("  \x1b[1;31m¡Aviso!\x1b[0m tamaño real del segmento = %zu, pero compute_shm_size(N) = %zu.\n",
            shm_size, shm_size_expected);
    }
    // Proceso de Inicializador termino
    printf("\x1b[1;36m──────────────────────────────────────────────────────────────\x1b[0m\n");
    printf("  \x1b[90mInicialización completada correctamente.\x1b[0m\n");
    printf("  \x1b[32mMemoria compartida lista para Emisores y Receptores.\x1b[0m\n");
    printf("\x1b[1;36m══════════════════════════════════════════════════════════════\x1b[0m\n\n");

    /* ------------------------------- Limpieza local ------------------------------- */
    // El initializer solo crea y deja todo listo, despues se desmapea y cierra su FD.
    munmap(map, shm_size);
    munmap(map, shm_size);
    close(fd);
    return 0;

cleanup_error:
    // Limpiar semaforos y unlink memoria compartida en caso de error
    munmap(map, shm_size);
    close(fd);
    shm_unlink(shm_name);
    return 1;
}

