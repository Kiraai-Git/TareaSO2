#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include "common.h"

/* -- Códigos ANSI -- */
#define ANSI_CLEAR      "\033[2J\033[H"
#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_RED        "\033[31m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_DIM        "\033[2m"
#define ANSI_BG_DARK    "\033[48;5;236m"
#define ANSI_BG_BOARD   "\033[48;5;238m"

/* -- Live view: estado global -- */
static bool live_mode = false;

/* -- Funciones de monitoreo -- */

void player_stats(int player_id) {
    if (player_id < 0 || player_id >= app_config.n_players) {
        printf("player_stats: id %d fuera de rango [0, %d)\n",
               player_id, app_config.n_players);
        return;
    }

    pthread_mutex_lock(&mutex_sistema);
    Player snap = jugadores[player_id];
    pthread_mutex_unlock(&mutex_sistema);

    const char* estado_str[] = { "WAITING", "PLAYING", "DONE" };
    const char* estado_col[] = { ANSI_YELLOW, ANSI_GREEN, ANSI_DIM };

    printf("\n" ANSI_BOLD "=== ESTADISTICAS JUGADOR %d ===" ANSI_RESET "\n", snap.id);
    printf("Estado     : %s%s%s\n", estado_col[snap.state], estado_str[snap.state], ANSI_RESET);
    printf("ELO Actual : " ANSI_CYAN "%.2f" ANSI_RESET "\n", snap.elo);
    printf("Ganadas    : " ANSI_GREEN "%d" ANSI_RESET "\n",   snap.wins);
    printf("Perdidas   : " ANSI_RED "%d" ANSI_RESET "\n",     snap.losses);
    printf("Empatadas  : " ANSI_YELLOW "%d" ANSI_RESET "\n",  snap.ties);
    printf("Total      : %d\n", snap.wins + snap.losses + snap.ties);
    printf(ANSI_BOLD "================================" ANSI_RESET "\n");
}

void current_matches(void) {
    pthread_mutex_lock(&mutex_sistema);

    int activas = 0;
    printf("\n" ANSI_BOLD "=== PARTIDAS EN CURSO ===" ANSI_RESET "\n");
    for (int i = 0; i < app_config.k_boards; i++) {
        if (!tableros[i].is_free) {
            printf("  Match " ANSI_CYAN "%d" ANSI_RESET
                   " | Jugador " ANSI_RED "%d (X)" ANSI_RESET
                   " vs Jugador " ANSI_BLUE "%d (O)" ANSI_RESET "\n",
                   tableros[i].id,
                   tableros[i].player1_id,
                   tableros[i].player2_id);
            activas++;
        }
    }
    if (activas == 0)
        printf("  (ninguna)\n");
    printf("Total activas: " ANSI_GREEN "%d" ANSI_RESET " / %d\n",
           activas, app_config.k_boards);
    printf(ANSI_BOLD "=========================" ANSI_RESET "\n");

    pthread_mutex_unlock(&mutex_sistema);
}

void match_status(int game_id) {
    if (game_id < 0 || game_id >= app_config.k_boards) {
        printf("match_status: id %d fuera de rango [0, %d)\n",
               game_id, app_config.k_boards);
        return;
    }

    pthread_mutex_lock(&mutex_sistema);
    bool is_free = tableros[game_id].is_free;
    int  p1_id   = tableros[game_id].player1_id;
    int  p2_id   = tableros[game_id].player2_id;
    pthread_mutex_unlock(&mutex_sistema);

    char grid_snap[3][3];
    Move moves_snap[9];
    int  move_count_snap;

    pthread_mutex_lock(&tableros[game_id].board_mutex);
    memcpy(grid_snap,  tableros[game_id].grid,         sizeof(grid_snap));
    memcpy(moves_snap, tableros[game_id].move_history, sizeof(moves_snap));
    move_count_snap = tableros[game_id].move_count;
    pthread_mutex_unlock(&tableros[game_id].board_mutex);

    printf("\n" ANSI_BOLD "=== ESTADO DEL MATCH %d ===" ANSI_RESET "\n", game_id);

    if (is_free) {
        printf("Estado: " ANSI_DIM "LIBRE / DISPONIBLE" ANSI_RESET "\n");
        printf(ANSI_BOLD "===========================" ANSI_RESET "\n");
        return;
    }

    printf("Estado     : " ANSI_GREEN "EN CURSO" ANSI_RESET "\n");
    printf("Jugadores  : " ANSI_RED "%d (X)" ANSI_RESET
           " vs " ANSI_BLUE "%d (O)" ANSI_RESET "\n", p1_id, p2_id);
    printf("Jugadas    : %d / 9\n", move_count_snap);

    printf("Tablero:\n");
    for (int r = 0; r < 3; r++) {
        printf("  ");
        for (int c = 0; c < 3; c++) {
            unsigned char v = (unsigned char)grid_snap[r][c];
            if      (v == 1) printf(ANSI_RED  "X" ANSI_RESET);
            else if (v == 2) printf(ANSI_BLUE "O" ANSI_RESET);
            else             printf(ANSI_DIM  "." ANSI_RESET);
            if (c < 2) printf(ANSI_DIM "|" ANSI_RESET);
        }
        printf("\n");
        if (r < 2) printf("  " ANSI_DIM "-+-+-" ANSI_RESET "\n");
    }

    if (move_count_snap > 0) {
        printf("Historial:\n");
        for (int i = 0; i < move_count_snap; i++) {
            char sym = (moves_snap[i].player_id == p1_id) ? 'X' : 'O';
            const char* col = (sym == 'X') ? ANSI_RED : ANSI_BLUE;
            printf("  %2d. Jugador %s%d (%c)%s → [%d,%d]\n",
                   i + 1, col,
                   moves_snap[i].player_id, sym, ANSI_RESET,
                   moves_snap[i].fila, moves_snap[i].col);
        }
    }
    printf(ANSI_BOLD "===========================" ANSI_RESET "\n");
}

/* -- Live view -- */

/*
 * print_board_box - imprime un tablero individual dentro del live view.
 * Recibe copias locales para no sostener mutexes durante la impresión.
 */
static void print_board_box(int id, bool is_free, int p1, int p2,
                             char grid[3][3], int moves)
{
    if (is_free) {
        printf("  " ANSI_BG_DARK " Tablero %-2d  " ANSI_RESET
               "  " ANSI_DIM "[ LIBRE ]" ANSI_RESET "\n\n", id);
        /* Espacio para alinear con tableros ocupados */
        printf("              . | . | .\n");
        printf("              -+-+-\n");
        printf("              . | . | .\n");
        printf("              -+-+-\n");
        printf("              . | . | .\n\n");
        return;
    }

    printf("  " ANSI_BG_BOARD ANSI_BOLD " Tablero %-2d  " ANSI_RESET
           "  " ANSI_RED "%d(X)" ANSI_RESET " vs " ANSI_BLUE "%d(O)" ANSI_RESET
           "  jugada %d/9\n\n", id, p1, p2, moves);

    for (int r = 0; r < 3; r++) {
        printf("             ");
        for (int c = 0; c < 3; c++) {
            unsigned char v = (unsigned char)grid[r][c];
            if      (v == 1) printf(" " ANSI_BOLD ANSI_RED  "X" ANSI_RESET " ");
            else if (v == 2) printf(" " ANSI_BOLD ANSI_BLUE "O" ANSI_RESET " ");
            else             printf(" " ANSI_DIM  "·" ANSI_RESET " ");
            if (c < 2) printf(ANSI_DIM "|" ANSI_RESET);
        }
        printf("\n");
        if (r < 2) printf("             " ANSI_DIM "---+---+---" ANSI_RESET "\n");
    }
    printf("\n");
}

/*
 * print_live_view - refresca la pantalla completa con el estado actual.
 * Estrategia de bloqueo: snapshot rápido bajo mutex, luego imprime libre.
 */
static void print_live_view(void) {
    /* -- Snapshot global bajo mutex_sistema -- */
    pthread_mutex_lock(&mutex_sistema);

    int waiting = 0, playing = 0, done = 0;
    for (int i = 0; i < app_config.n_players; i++) {
        switch (jugadores[i].state) {
            case WAITING: waiting++; break;
            case PLAYING: playing++; break;
            case DONE:    done++;    break;
        }
    }

    /* Top-5 jugadores por ELO */
    typedef struct { int id; double elo; int w, l, t; } EloEntry;
    EloEntry top[5];
    int top_n = (app_config.n_players < 5) ? app_config.n_players : 5;
    for (int i = 0; i < top_n; i++) {
        top[i] = (EloEntry){
            jugadores[i].id, jugadores[i].elo,
            jugadores[i].wins, jugadores[i].losses, jugadores[i].ties
        };
    }
    /* Bubble sort simple (top_n ≤ 5)  */
    for (int i = 0; i < top_n - 1; i++)
        for (int j = i + 1; j < top_n; j++)
            if (top[j].elo > top[i].elo) {
                EloEntry tmp = top[i]; top[i] = top[j]; top[j] = tmp;
            }

    /* Snapshot de tablers */
    typedef struct {
        int  id;
        bool is_free;
        int  p1, p2, moves;
        char grid[3][3];
    } BoardSnap;
    BoardSnap bsnaps[app_config.k_boards];
    for (int i = 0; i < app_config.k_boards; i++) {
        bsnaps[i].id      = tableros[i].id;
        bsnaps[i].is_free = tableros[i].is_free;
        bsnaps[i].p1      = tableros[i].player1_id;
        bsnaps[i].p2      = tableros[i].player2_id;
    }
    pthread_mutex_unlock(&mutex_sistema);

    /* Snapshot de grids bajo board_mutex */
    for (int i = 0; i < app_config.k_boards; i++) {
        pthread_mutex_lock(&tableros[i].board_mutex);
        memcpy(bsnaps[i].grid, tableros[i].grid, sizeof(bsnaps[i].grid));
        bsnaps[i].moves = tableros[i].move_count;
        pthread_mutex_unlock(&tableros[i].board_mutex);
    }

    /* -- Renderizado (sin ningún mutex tomado) -- */
    printf(ANSI_CLEAR);

    /* Encabezado */
    printf(ANSI_BOLD ANSI_CYAN
           "╔══════════════════════════════════════════════════════╗\n"
           "║           cmatch  —  Live View                       ║\n"
           "╚══════════════════════════════════════════════════════╝\n"
           ANSI_RESET);

    /* Estado global */
    printf("\n  Jugadores: "
           ANSI_YELLOW "%-3d esperando" ANSI_RESET "  "
           ANSI_GREEN  "%-3d jugando"   ANSI_RESET "  "
           ANSI_DIM    "%-3d terminados" ANSI_RESET "\n\n",
           waiting, playing, done);

    /* Tableros */
    printf(ANSI_BOLD "  ── Tableros ──────────────────────────────────────────\n"
           ANSI_RESET);
    for (int i = 0; i < app_config.k_boards; i++)
        print_board_box(bsnaps[i].id, bsnaps[i].is_free,
                        bsnaps[i].p1, bsnaps[i].p2,
                        bsnaps[i].grid, bsnaps[i].moves);

    /* Top ELO */
    printf(ANSI_BOLD "  ── Top ELO ────────────────────────────────────────────\n"
           ANSI_RESET);
    for (int i = 0; i < top_n; i++) {
        printf("  #%d  Jugador " ANSI_CYAN "%-3d" ANSI_RESET
               "  ELO " ANSI_YELLOW "%.1f" ANSI_RESET
               "  " ANSI_GREEN "W:%-3d" ANSI_RESET
               " " ANSI_RED "L:%-3d" ANSI_RESET
               " T:%-3d\n",
               i + 1, top[i].id, top[i].elo,
               top[i].w, top[i].l, top[i].t);
    }

    printf("\n" ANSI_DIM
           "  [live activo — escribe 'live off' para salir, 'help' para comandos]\n"
           ANSI_RESET);
    fflush(stdout);
}

/* -- Thread de monitoreo interactivo -- */

void* monitor_thread(void* arg) {
    (void)arg;

    printf("[monitor] Listo. Comandos: stats <id> | matches | status <id> | live | help\n");
    fflush(stdout);

    while (sistema_activo) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        /* En live mode refresca cada 500 ms; si no, espera 200 ms */
        struct timeval tv = {
            .tv_sec  = 0,
            .tv_usec = live_mode ? 500000 : 200000
        };
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

        if (ret < 0) break;

        if (ret == 0) {
            /* Timeout: si estamos en live mode, refrescar */
            if (live_mode) print_live_view();
            continue;
        }

        char line[128];
        if (!fgets(line, sizeof(line), stdin)) break;

        char cmd[64] = {0};
        int  id      = -1;
        int  parsed  = sscanf(line, "%63s %d", cmd, &id);
        if (parsed < 1) continue;

        /* Comando: live / live off */
        if (strcmp(cmd, "live") == 0) {
            if (parsed == 1) {
                /* toggle */
                live_mode = !live_mode;
            } else {
                /* live / live off necesitan el segundo token como string */
                char sub[16] = {0};
                sscanf(line, "%*s %15s", sub);
                if (strcmp(sub, "off") == 0) live_mode = false;
                else                          live_mode = true;
            }
            if (live_mode) print_live_view();
            else printf("[monitor] Live view desactivado.\n");
            fflush(stdout);
            continue;
        }

        /* En live mode, salir temporalmente con cualquier otro comando */
        if (live_mode) {
            live_mode = false;
            printf(ANSI_CLEAR);
        }

        if      (strcmp(cmd, "stats")   == 0 && parsed == 2) player_stats(id);
        else if (strcmp(cmd, "matches") == 0)                 current_matches();
        else if (strcmp(cmd, "status")  == 0 && parsed == 2) match_status(id);
        else if (strcmp(cmd, "help")    == 0) {
            printf(ANSI_BOLD "Comandos disponibles:\n" ANSI_RESET);
            printf("  stats  <id>   ver ELO y record de un jugador\n");
            printf("  matches       ver partidas en curso\n");
            printf("  status <id>   ver tablero y jugadas de un match\n");
            printf("  live          activar/desactivar vista en vivo\n");
            printf("  live off      desactivar vista en vivo\n");
        }
        else {
            printf("[monitor] Comando no reconocido. Escribe 'help' para ver opciones.\n");
        }
        fflush(stdout);
    }

    return NULL;
}
