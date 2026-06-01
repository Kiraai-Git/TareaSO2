#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>

typedef struct {
    int   n_players;
    int   k_boards;
    int   k_elo;
    int   max_elo_diff;
    int   turn_delay_ms;
    float reenter_probability;
    char  snapshot_path[256];
} Config;

typedef enum {
    WAITING,
    PLAYING,
    DONE
} PlayerState;

typedef struct {
    int    id;
    double elo;
    int    wins;
    int    losses;
    int    ties;
    PlayerState      state;           
    struct timespec  wait_start_time; 
    pthread_cond_t   cond_match_done; 
   
} Player;


typedef struct {
    int         id;
    double      elo;
    int         wins;
    int         losses;
    int         ties;
    PlayerState state;
} PlayerSnapshot;

typedef struct {
    int    fila;
    int    col;
    int    player_id;
} Move;

typedef struct {
    int  id;

    bool is_free;
    int  player1_id;
    int  player2_id;

 
    char  grid[3][3];
    Move  move_history[9];
    int   move_count;

    pthread_cond_t  cond_board_ready; 
    pthread_mutex_t board_mutex;      
} Board;


extern Config                  app_config;
extern volatile sig_atomic_t   sistema_activo;

extern pthread_mutex_t         mutex_sistema;
extern pthread_cond_t          cond_matchmaking;
extern sem_t                   sem_tableros;

extern Player*  jugadores;
extern Board*   tableros;


void* player_routine(void* arg);
void* board_routine(void* arg);

void player_stats(int player_id);
void current_matches(void);
void match_status(int game_id);

#endif 
