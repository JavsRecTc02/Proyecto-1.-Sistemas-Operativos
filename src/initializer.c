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


int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Uso: %s <shm_name> <buffer_size> <key (0-255)> <input_file>\n", argv[0]);
        return 1;
    }

    const char *shm_name = argv[1];
    int buf_size = atoi(argv[2]);
    int key = atoi(argv[3]);
    const char *infile = argv[4];

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

    /* calcular header + N sem_t + N slots */
    size_t shm_size = compute_shm_size(buf_size);

    /* crear archivo de entrada si no existe */
    struct stat st;
    if (stat(infile, &st) == -1) {
        /* intentar crear archivo vacío con permisos 0644 */
        FILE *tf = fopen(infile, "w");
        if (!tf) {
            fprintf(stderr, "No se pudo crear input_file '%s': %s\n", infile, strerror(errno));
            return 1;
        }
        fclose(tf);
        printf("[initializer] Archivo de entrada '%s' creado (vacío).\n", infile);
    }

    /* Memoria Compartida */
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, SHM_DEFAULT_MODE);
    if (fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "Error: shared memory '%s' ya existe. Elimínela o use otro nombre.\n", shm_name);
        } else {
            perror("shm_open");
        }
        return 1;
    }

    if (ftruncate(fd, (off_t)shm_size) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(shm_name);
        return 1;
    }

    void *map = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(shm_name);
        return 1;
    }

    /* Asegurar memoria limpia */
    memset(map, 0, shm_size);

    shared_header_t *hdr = (shared_header_t*) map;

    /* Inicializar head*/
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

    /* inicializar semáforos globales (pshared = 1) */
    if (sem_init(&hdr->control_sem, 1, 1) == -1) {
        perror("sem_init control_sem");
        goto cleanup_error;
    }
    if (sem_init(&hdr->empty_count, 1, buf_size) == -1) {
        perror("sem_init empty_count");
        goto cleanup_error;
    }
    if (sem_init(&hdr->full_count, 1, 0) == -1) {
        perror("sem_init full_count");
        goto cleanup_error;
    }
    if (sem_init(&hdr->finalizer_sem, 1, 0) == -1) {
        perror("sem_init finalizer_sem");
        goto cleanup_error;
    }

    /* inicializar semáforos por ranura y ranuras */
    sem_t *slot_sems = get_slot_sems(hdr);
    buffer_slot_t *slots = get_slots(hdr);

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

    /* Informacion del proceso de Inicio */
    printf("Memoria compartida '%s' creada correctamente.\n", shm_name);
    printf("  Buffer slots    : %d\n", buf_size);
    printf("  Key (XOR 8-bit) : %d\n", key);
    printf("  Input file      : %s\n", hdr->filename);
    printf("  Tamaño shm (bytes): %zu\n", shm_size);
    printf("Inicialización completa. El proceso 'initializer' finaliza ahora.\n");

    /* Limpieza */
    munmap(map, shm_size);
    close(fd);
    return 0;

cleanup_error:
    /* Limpiar semaforos y unlink memoria compartida*/
    munmap(map, shm_size);
    close(fd);
    shm_unlink(shm_name);
    return 1;
}

