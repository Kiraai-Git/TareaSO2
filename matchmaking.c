#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "common.h"

/* ── Utilidades internas ────────────────────────────────────────────────── */

static double calcular_expectativa(double elo_a, double elo_b) {
    return 1.0 / (1.0 + pow(10.0, (elo_b - elo_a) / 400.0));
}

/*
 * buscar_oponente - localiza al candidato más antiguo en cola dentro del
 * rango de ELO permitido.
 * PRECONDICIÓN: mutex_sistema debe estar tomado por el llamador.
 */
static int buscar_oponente(const Player* me) {
    int             seleccionado_id = -1;
    /* oldest_wait guarda el timestamp más antiguo encontrado hasta ahora
       (menor valor = lleva más tiempo esperando = tiene prioridad).      */
    struct timespec oldest_wait     = {0, 0};

    for (int i = 0; i < app_config.n_players; i++) {
        const Player* c = &jugadores[i];
        if (c->id == me->id || c->state != WAITING) continue;

        if (fabs(me->elo - c->elo) <= app_config.max_elo_diff) {
            if (seleccionado_id == -1 ||
                c->wait_start_time.tv_sec < oldest_wait.tv_sec ||
                (c->wait_start_time.tv_sec  == oldest_wait.tv_sec &&
                 c->wait_start_time.tv_nsec <  oldest_wait.tv_nsec)) {
                seleccionado_id = c->id;
                oldest_wait     = c->wait_start_time;
            }
        }
    }
    return seleccionado_id;
}

/* Devuelve el símbolo ganador (1 o 2) o 0 si la partida no terminó. */
static int evaluar_tablero(char g[3][3]) {
    for (int i = 0; i < 3; i++) {
        if (g[i][0] && g[i][0] == g[i][1] && g[i][1] == g[i][2]) return g[i][0];
        if (g[0][i] && g[0][i] == g[1][i] && g[1][i] == g[2][i]) return g[0][i];
    }
    if (g[0][0] && g[0][0] == g[1][1] && g[1][1] == g[2][2]) return g[0][0];
    if (g[0][2] && g[0][2] == g[1][1] && g[1][1] == g[2][0]) return g[0][2];
    return 0;
}

/* -- Thread de jugador -- */

/*
 * CICLO DE VIDA:
 *   1. MATCHMAKING   - espera oponente sin busy-wait (pthread_cond_wait)
 *   2. ADQUISICIÓN   - el iniciador adquiere un tablero libre (semáforo)
 *   3. ESPERA        - ambos duermen hasta que board_routine señale el fin
 *   4. DECISIÓN      - reingresa con probabilidad REENTER_PROBABILITY o termina
 *
 * Invariante de mutex: al inicio y al final de cada fase, mutex_sistema
 * está TOMADO.      La única excepción es el bloque del semáforo, que ocurre
 * con mutex LIBERADO para no bloquear al resto del sistema .
 */
void* player_routine(void* arg) {
    Player* me = (Player*)arg;

    /* Entramos con el mutex libre; lo tomamos una sola vez aquí y lo
       mantenemos entre fases para evitar ventanas de condición de carrera. */
    pthread_mutex_lock(&mutex_sistema);
    me->state = WAITING;
    clock_gettime(CLOCK_MONOTONIC, &me->wait_start_time);

    while (1) {

        /* -- MATCHMAKING -- */
        int  rival_id    = -1;
        bool is_initiator = false;

        while (sistema_activo) {
            if (me->state == PLAYING) break;

            rival_id = buscar_oponente(me);
            if (rival_id != -1) {
                is_initiator              = true;
                me->state                 = PLAYING;
                jugadores[rival_id].state = PLAYING;
                /* Notificar a todos que hay un jugador menos en cola */
                pthread_cond_broadcast(&cond_matchmaking);
                break;
            }

            /* Sin oponente entonces libera mutex y duerme sin consumir CPU */
            pthread_cond_wait(&cond_matchmaking, &mutex_sistema);
        }

        /* Sistema apagado y aún no tenemos partida asignada entonces salimos */
        if (!sistema_activo && me->state != PLAYING) {
            me->state = DONE;
            break; /* sale con mutex_sistema TOMADO */
        }

        /* Liberar el mutex antes de operaciones potencialmente bloqueantes */
        pthread_mutex_unlock(&mutex_sistema);

        /* -- ADQUISICIÓN DE TABLERO (sólo iniciador) -- */
        if (is_initiator) {

            /* sem_wait bloquea si todos los tableros están ocupados.
               El hilo principal publica N semáforos extra al apagarse,
               garantizando que ningún hilo quede colgado aquí.           */
            sem_wait(&sem_tableros);

            pthread_mutex_lock(&mutex_sistema);

            if (!sistema_activo) {
                /* Apagado mientras esperábamos: cancelar la partida y
                   despertar al rival que está bloqueado en cond_match_done */
                me->state                 = DONE;
                jugadores[rival_id].state = DONE;
                pthread_cond_signal(&jugadores[rival_id].cond_match_done);
                sem_post(&sem_tableros); /* devolver el tablero que no usamos */
                break; /* sale con mutex_sistema TOMADO */
            }

            /* Localizar el tablero libre que el semáforo garantiza */
            int tid = -1;
            for (int i = 0; i < app_config.k_boards; i++) {
                if (tableros[i].is_free) {
                    tid = i;

                    /* Reiniciar datos de juego bajo board_mutex */
                    pthread_mutex_lock(&tableros[i].board_mutex);
                    tableros[i].is_free      = false;
                    tableros[i].player1_id   = me->id;
                    tableros[i].player2_id   = rival_id;
                    tableros[i].move_count   = 0;
                    memset(tableros[i].grid, 0, sizeof(tableros[i].grid));
                    pthread_mutex_unlock(&tableros[i].board_mutex);

                    /* Despertar al board thread mientras tenemos mutex_sistema
                       (evita pérdida de señal) */
                    pthread_cond_signal(&tableros[tid].cond_board_ready);
                    break;
                }
            }
            /* tid != -1 está garantizado por el semáforo */
            (void)tid;
            pthread_mutex_unlock(&mutex_sistema);
        }

        /* -- ESPERAR FIN DE PARTIDA (sin busy-wait) -- */
        pthread_mutex_lock(&mutex_sistema);

        /* board_routine cambia state a WAITING y señala cond_match_done
           con mutex_sistema tomado, por lo que no hay pérdida de señal. */
        while (me->state == PLAYING)
            pthread_cond_wait(&me->cond_match_done, &mutex_sistema);

        /* -- DECISIÓN DE REINGRESO -- */
        if (!sistema_activo) {
            me->state = DONE;
            break; /* sale con mutex_sistema TOMADO */
        }

        float r = (float)rand() / (float)RAND_MAX;
        if (r > app_config.reenter_probability) {
            me->state = DONE;
            break; /* sale con mutex_sistema TOMADO */
        }

        /* Reingresar: actualizar tiempo de espera y notificar a posibles
           rivales que estaban buscando oponente    */
        me->state = WAITING;
        clock_gettime(CLOCK_MONOTONIC, &me->wait_start_time);
        pthread_cond_broadcast(&cond_matchmaking);
        /* mutex_sistema sigue TOMADO entonces vuelve al inicio del while */
    }

    /* Punto de salida único: mutex_sistema siempre está TOMADO aquí */
    pthread_mutex_unlock(&mutex_sistema);
    return NULL;
}

/* -- Thread de tablero -- */

/*
 * board_routine gestiona UNA partida de tic-tac-toe por iteración.
 * Las partidas en curso siempre se completan (incluso ante SIGINT),
 * conforme al enunciado: "las partidas en curso deben terminar normalmente".
 */
void* board_routine(void* arg) {
    Board* b = (Board*)arg;

    while (1) {

        /* Esperar asignación de partida: cond_board_ready + mutex_sistema */
        pthread_mutex_lock(&mutex_sistema);
        while (b->is_free && sistema_activo)
            pthread_cond_wait(&b->cond_board_ready, &mutex_sistema);

        if (b->is_free) {
            /* sistema_activo == 0 y no hay partida pendiente entonces salir */
            pthread_mutex_unlock(&mutex_sistema);
            break;
        }
        pthread_mutex_unlock(&mutex_sistema);

        /* -- SIMULACIÓN DEL JUEGO -- */
        int turno   = 1;  /* 1: jugador 1 (X),  2: jugador 2 (O) */
        int ganador = 0;

        /* Sin chequeo de sistema_activo aquí: la partida termina siempre */
        while (b->move_count < 9 && ganador == 0) {

            /* Pausa configurable entre jugadas */
            struct timespec ts = {
                .tv_sec  =  app_config.turn_delay_ms / 1000,
                .tv_nsec = (app_config.turn_delay_ms % 1000) * 1000000L
            };
            nanosleep(&ts, NULL);

            /* board_mutex protege grid y move_history frente a monitor */
            pthread_mutex_lock(&b->board_mutex);

            int f, c;
            do {
                f = rand() % 3;
                c = rand() % 3;
            } while (b->grid[f][c] != 0);

            b->grid[f][c] = (char)turno;
            int jugador_actual = (turno == 1) ? b->player1_id : b->player2_id;
            b->move_history[b->move_count] = (Move){
                .fila      = f,
                .col       = c,
                .player_id = jugador_actual
            };
            b->move_count++;
            ganador = evaluar_tablero(b->grid);
            turno   = (turno == 1) ? 2 : 1;

            pthread_mutex_unlock(&b->board_mutex);
        }

        /* -- ACTUALIZACIÓN DE ELO Y ESTADÍSTICAS -- */
        /*
         * ELO protegido exclusivamente por mutex_sistema (sin elo_mutex
         * individual) .  Todas las lecturas/escrituras de elo ocurren con
         * este mutex tomado, por lo que no hay condición de carrera.
         */
        pthread_mutex_lock(&mutex_sistema);

        Player* p1 = &jugadores[b->player1_id];
        Player* p2 = &jugadores[b->player2_id];

        double sa = 0.5, sb = 0.5;
        if      (ganador == 1) { sa = 1.0; sb = 0.0; p1->wins++;  p2->losses++; }
        else if (ganador == 2) { sa = 0.0; sb = 1.0; p2->wins++;  p1->losses++; }
        else                   {                      p1->ties++;  p2->ties++;   }

        double ea = calcular_expectativa(p1->elo, p2->elo);
        double eb = calcular_expectativa(p2->elo, p1->elo);
        p1->elo += app_config.k_elo * (sa - ea);
        p2->elo += app_config.k_elo * (sb - eb);

        /* Liberar jugadores y notificarlos (con mutex tomado entonces significa que no hay pérdida de señal) */
        p1->state = WAITING;
        p2->state = WAITING;
        pthread_cond_signal(&p1->cond_match_done);
        pthread_cond_signal(&p2->cond_match_done);

        /* Notificar a threads de jugadores que buscan oponente */
        pthread_cond_broadcast(&cond_matchmaking);

        /* Liberar el tablero bajo mutex_sistema (is_free protegido por él) */
        b->is_free = true;

        pthread_mutex_unlock(&mutex_sistema);

        /* Publicar el semáforo DESPUÉS de liberar el mutex: permite que el
           próximo iniciador tome el tablero con is_free ya en true.        */
        sem_post(&sem_tableros);
    }

    return NULL;
}
