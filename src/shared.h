#ifndef SHARED_H
#define SHARED_H

#define _GNU_SOURCE
#include <semaphore.h>
#include <time.h>
#include <stdint.h>

#define MAX_FILENAME 256
#define SHM_DEFAULT_MODE 0660

/* Ranura del buffer */
typedef struct {
    uint8_t ascii;           /* dato codificado (1 byte) */
    uint64_t seq;            /* número de secuencia global (para reconstrucción) */
    struct timespec ts;      /* timestamp de escritura */
    int occupied;            /* 0 = libre, 1 = ocupado */
} buffer_slot_t;

/* Cabecera fija y semáforos globales. */
typedef struct {
    int buffer_size;                 /* Espacio N */
    int head;                        /* índice para escribir (next write) */
    int tail;                        /* índice para leer (next read) */
    uint64_t seq_counter;            /* contador de secuencia global */
    uint64_t total_transferred;      /* datos finales de estadistica */
    int total_emitters_spawned;
    int total_receivers_spawned;
    int active_emitters;
    int active_receivers;
    uint8_t key;                     /* key de 8 bits almacenada por initializer */
    char filename[MAX_FILENAME];     /* archivo de entrada */
    sem_t control_sem;                /* protege counters y head/tail/seq */
    sem_t empty_count;               /* contador de espacios libres (inicial = N) */
    sem_t full_count;                /* contador de espacios ocupados (inicial = 0) */
    sem_t finalizer_sem;             /* para que finalizer espere a que todos terminen */
    int terminate_flag;              /* 0 = normal, 1 = terminar */

} shared_header_t;

/* Calcular punteros dentro del segmento mapeado */
static inline sem_t* get_slot_sems(shared_header_t *hdr) {
    return (sem_t *) ((char*)hdr + sizeof(shared_header_t));
}
static inline buffer_slot_t* get_slots(shared_header_t *hdr) {
    return (buffer_slot_t *) ((char*)hdr + sizeof(shared_header_t) + hdr->buffer_size * sizeof(sem_t));
}

/* Tamaño total a reservar para shm: header + N*sem + N*slot */
static inline size_t compute_shm_size(int n) {
    return sizeof(shared_header_t) + (size_t)n * sizeof(sem_t) + (size_t)n * sizeof(buffer_slot_t);
}

#endif 
