#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "common.h"

/* Ajustes Globales */

Config               app_config;
volatile sig_atomic_t sistema_activo = 1;

pthread_mutex_t  mutex_sistema;
pthread_cond_t   cond_matchmaking;
sem_t            sem_tableros;

Player* jugadores = NULL;
Board*  tableros  = NULL;

/* Apagado seguro */

static void handle_sigint(int sig) {
    (void)sig;
    sistema_activo = 0;
}

/* aquí te lee toda la config */

static void load_config(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        perror("Error al abrir el archivo de configuracion");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == '#') continue;

        char key[128], value[128];
        if (sscanf(line, "%127[^=]=%127s", key, value) != 2) continue;

        /* Eliminar espacios/tabs/CR del final del key */
        char* end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
            *end-- = '\0';

        if      (strcmp(key, "N_PLAYERS")          == 0) app_config.n_players          = atoi(value);
        else if (strcmp(key, "K_BOARDS")            == 0) app_config.k_boards            = atoi(value);
        else if (strcmp(key, "K_ELO")               == 0) app_config.k_elo               = atoi(value);
        else if (strcmp(key, "MAX_ELO_DIFF")         == 0) app_config.max_elo_diff         = atoi(value);
        else if (strcmp(key, "TURN_DELAY_MS")        == 0) app_config.turn_delay_ms        = atoi(value);
        else if (strcmp(key, "REENTER_PROBABILITY")  == 0) app_config.reenter_probability  = atof(value);
        else if (strcmp(key, "SNAPSHOT_PATH")        == 0) {
            strncpy(app_config.snapshot_path, value, sizeof(app_config.snapshot_path) - 1);
            app_config.snapshot_path[sizeof(app_config.snapshot_path) - 1] = '\0';
        }
    }
    fclose(f);

    /* unas validaciones  */
    if (app_config.n_players <= 0) { fprintf(stderr, "N_PLAYERS debe ser > 0\n"); exit(EXIT_FAILURE); }
    if (app_config.k_boards  <= 0) { fprintf(stderr, "K_BOARDS debe ser > 0\n");  exit(EXIT_FAILURE); }
    if (app_config.k_boards  >  app_config.n_players / 2) {
        fprintf(stderr, "Advertencia: K_BOARDS > N_PLAYERS/2; "
                        "podria haber tableros que nunca se usen.\n");
    }

    printf("Configuracion cargada: %d jugadores, %d tableros, ELO diff max %d\n",
           app_config.n_players, app_config.k_boards, app_config.max_elo_diff);
}

/* tuneo del sistema */

/* Aquí dejé esta explicación pq está bastante wena

 * inicializar_sistema — aloja memoria e inicializa jugadores y tableros.
 *
 * Si existe un snapshot, restaura los datos de los jugadores (ELO,
 * estadísticas) y re-inicializa todas las primitivas de sincronización,
 * que NO son serializables a disco.
 */
static void inicializar_sistema(void) {
    jugadores = malloc((size_t)app_config.n_players * sizeof(Player));
    tableros  = malloc((size_t)app_config.k_boards  * sizeof(Board));
    if (!jugadores || !tableros) { perror("malloc"); exit(EXIT_FAILURE); }

    FILE* snap = fopen(app_config.snapshot_path, "rb");
    if (snap) {
        /* Restaurar desde snapshot*/
        printf("Snapshot encontrado en '%s'. Restaurando estado...\n",
               app_config.snapshot_path);

        for (int i = 0; i < app_config.n_players; i++) {
            PlayerSnapshot ps;
            if (fread(&ps, sizeof(PlayerSnapshot), 1, snap) == 1) {
                jugadores[i].id     = ps.id;
                jugadores[i].elo    = ps.elo;
                jugadores[i].wins   = ps.wins;
                jugadores[i].losses = ps.losses;
                jugadores[i].ties   = ps.ties;
                /* si el jugador estaba DONE, no lo reintroducimos */
                jugadores[i].state  = (ps.state == DONE) ? DONE : WAITING;
            } else {
                /* snapshot incompleto: inicializar el resto desde cero */
                jugadores[i].id     = i;
                jugadores[i].elo    = 1200.0;
                jugadores[i].wins   = jugadores[i].losses = jugadores[i].ties = 0;
                jugadores[i].state  = WAITING;
            }
            clock_gettime(CLOCK_MONOTONIC, &jugadores[i].wait_start_time);
            /* CRÍTICO: re-inicializar la condvar */
            pthread_cond_init(&jugadores[i].cond_match_done, NULL);
        }
        fclose(snap);
        printf("Estado restaurado correctamente.\n");
    } else {
        /*  Inicialización normal  */
        for (int i = 0; i < app_config.n_players; i++) {
            jugadores[i].id     = i;
            jugadores[i].elo    = 1200.0;
            jugadores[i].wins   = jugadores[i].losses = jugadores[i].ties = 0;
            jugadores[i].state  = WAITING;
            clock_gettime(CLOCK_MONOTONIC, &jugadores[i].wait_start_time);
            pthread_cond_init(&jugadores[i].cond_match_done, NULL);
        }
    }

    for (int i = 0; i < app_config.k_boards; i++) {
        tableros[i].id         = i;
        tableros[i].is_free    = true;
        tableros[i].move_count = 0;
        memset(tableros[i].grid, 0, sizeof(tableros[i].grid));
        pthread_cond_init(&tableros[i].cond_board_ready, NULL);
        pthread_mutex_init(&tableros[i].board_mutex, NULL);
    }
}

/* snapshot  *

/*
 * guardar_snapshot — serializa sólo los datos de los jugadores.
 * Se llama DESPUÉS de que todos los board_threads han terminado,
 * garantizando que ningún jugador queda en estado PLAYING.
 * Las primitivas de sincronización NO se serializan (no son portables).
 */
static void guardar_snapshot(void) {
    FILE* f = fopen(app_config.snapshot_path, "wb");
    if (!f) {
        perror("Error al guardar snapshot");
        return;
    }
    for (int i = 0; i < app_config.n_players; i++) {
        PlayerSnapshot ps = {
            .id     = jugadores[i].id,
            .elo    = jugadores[i].elo,
            .wins   = jugadores[i].wins,
            .losses = jugadores[i].losses,
            .ties   = jugadores[i].ties,
            .state  = jugadores[i].state,
        };
        fwrite(&ps, sizeof(PlayerSnapshot), 1, f);
    }
    fclose(f);
    printf("Snapshot guardado en '%s'.\n", app_config.snapshot_path);
}

/*  el main */

/* Declaración de monitor_thread (implementado en monitor.c) */
extern void* monitor_thread(void* arg);

int main(void) {
    srand((unsigned)time(NULL));

    load_config(".env");

    /* Instalar manejador de SIGINT */
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    /* Inicializar primitivas globales */
    pthread_mutex_init(&mutex_sistema,  NULL);
    pthread_cond_init(&cond_matchmaking, NULL);
    sem_init(&sem_tableros, 0, (unsigned)app_config.k_boards);

    inicializar_sistema();

    /* Crear threads */
    pthread_t* t_players = malloc((size_t)app_config.n_players * sizeof(pthread_t));
    pthread_t* t_boards  = malloc((size_t)app_config.k_boards  * sizeof(pthread_t));
    pthread_t  t_monitor;
    if (!t_players || !t_boards) { perror("malloc threads"); exit(EXIT_FAILURE); }

    /* Lanzar board threads primero (deben estar listos antes que los players) */
    for (int i = 0; i < app_config.k_boards; i++)
        pthread_create(&t_boards[i], NULL, board_routine, &tableros[i]);

    /* Lanzar player threads */
    for (int i = 0; i < app_config.n_players; i++) {
        /* No crear thread para jugadores que terminaron en el snapshot */
        if (jugadores[i].state == DONE) {
            t_players[i] = 0;
            continue;
        }
        pthread_create(&t_players[i], NULL, player_routine, &jugadores[i]);
    }

    /* Lanzar thread de monitoreo interactivo */
    pthread_create(&t_monitor, NULL, monitor_thread, NULL);

    /* Bucle principal: esperar SIGINT */
    printf("Simulacion en marcha. Ctrl+C para detener.\n");
    while (sistema_activo)
        sleep(1);

    printf("\n[main] SIGINT recibido. Apagando...\n");

    /* ── Apagado limpio (graceful shutdown) ─────────────────────────────
     *
     * Orden correcto:
     *  1. sistema_activo ya es 0 (lo puso el manejador).
     *  2. Despertar a todos los threads bloqueados en condvars.
     *  3. Publicar N semáforos para desbloquear cualquier sem_wait.
     *  4. Esperar que los board threads terminen sus partidas activas.
     *  5. Esperar que los player threads salgan.
     *  6. Guardar snapshot (con todos los juegos ya terminados).
     * ─────────────────────────────────────────────────────────────────── */

    pthread_mutex_lock(&mutex_sistema);
    pthread_cond_broadcast(&cond_matchmaking);
    for (int i = 0; i < app_config.k_boards; i++)
        pthread_cond_signal(&tableros[i].cond_board_ready);
    /* Despertar jugadores bloqueados esperando fin de su partida */
    for (int i = 0; i < app_config.n_players; i++)
        pthread_cond_signal(&jugadores[i].cond_match_done);
    pthread_mutex_unlock(&mutex_sistema);

    /* Suficientes semáforos para desbloquear cualquier sem_wait */
    for (int i = 0; i < app_config.n_players; i++)
        sem_post(&sem_tableros);

    /* Esperar board threads: ellos terminan las partidas activas */
    for (int i = 0; i < app_config.k_boards; i++)
        pthread_join(t_boards[i], NULL);

    /* Esperar player threads */
    for (int i = 0; i < app_config.n_players; i++) {
        if (t_players[i] != 0)
            pthread_join(t_players[i], NULL);
    }

    /* El thread de monitor sale solo (select con timeout 200ms) */
    pthread_join(t_monitor, NULL);

    /* Guardar estado DESPUÉS de que todos los juegos terminaron */
    guardar_snapshot();

    /* Liberar recursos */
    for (int i = 0; i < app_config.n_players; i++)
        pthread_cond_destroy(&jugadores[i].cond_match_done);
    for (int i = 0; i < app_config.k_boards; i++) {
        pthread_cond_destroy(&tableros[i].cond_board_ready);
        pthread_mutex_destroy(&tableros[i].board_mutex);
    }
    pthread_mutex_destroy(&mutex_sistema);
    pthread_cond_destroy(&cond_matchmaking);
    sem_destroy(&sem_tableros);

    free(t_players);
    free(t_boards);
    free(jugadores);
    free(tableros);

    printf("Simulacion finalizada de forma segura.\n");
    return 0;
}
