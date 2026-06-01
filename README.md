# TareaSO2

# cmatch — Motor de Emparejamiento Concurrente

Simulación de un torneo de tic-tac-toe con múltiples threads concurrentes, emparejamiento por ELO, tableros compartidos y snapshot persistente.

## Configuración

Antes de compilar/ejecutar el programa debes configurar las variables en el archivo `.env`. Las variables disponibles son:

| Variable              | Tipo   | Descripción                                               |
|-----------------------|--------|-----------------------------------------------------------|
| `N_PLAYERS`           | Entero | Número de threads de jugadores                            |
| `K_BOARDS`            | Entero | Número máximo de partidas simultáneas                     |
| `K_ELO`               | Entero | Factor de ajuste ELO (típicamente 32)                     |
| `MAX_ELO_DIFF`        | Entero | Diferencia máxima de ELO para emparejar                   |
| `TURN_DELAY_MS`       | Entero | Retardo en ms entre jugadas                               |
| `REENTER_PROBABILITY` | Float  | Probabilidad de que un jugador vuelva a la cola (0.0–1.0) |
| `SNAPSHOT_PATH`       | String | Ruta del archivo de snapshot                              |

Ejemplo de `.env`:

```env
N_PLAYERS=10
K_BOARDS=2
K_ELO=32
MAX_ELO_DIFF=500
TURN_DELAY_MS=50
REENTER_PROBABILITY=0.8
SNAPSHOT_PATH=dump.bin
```

## Compilación y Ejecución

Dentro de la carpeta del proyecto, compilar con:

```bash
make
```

Luego ejecutar con:

```bash
./cmatch
```

Para reanudar desde un snapshot anterior basta con dejar el mismo `SNAPSHOT_PATH` en el `.env` y volver a ejecutar.

## Monitor interactivo

Mientras la simulación corre se pueden escribir comandos en stdin:

| Comando    | Descripción                                                    |
|------------|----------------------------------------------------------------|
| `stats <id>`  | Muestra ELO, victorias, derrotas y empates del jugador `id` |
| `matches`     | Lista las partidas actualmente en curso                      |
| `status <id>` | Muestra el tablero y el historial de jugadas del match `id`  |
| `help`        | Muestra la lista de comandos                                 |
| `live`        | Muestra en vivo la partida que se está jugando               |
| `live off`    | Sale del modo en vivo                                        |

### Ejemplos

matches
stats 3
status 0

## Apagado limpio

Presionar **Ctrl+C** envía `SIGINT`. El sistema:

1. No inicia nuevas partidas.
2. Espera que las partidas en curso terminen.
3. Guarda el snapshot en `SNAPSHOT_PATH`.
4. Libera todos los recursos y termina sin threads colgados.

## Diseño de sincronización

| Primitiva | Descripción |
|-----------|-------------|
| `mutex_sistema` | Mutex global que protege el estado de jugadores (ELO, estado, estadísticas), la lista de tableros (`is_free`, `player_ids`) y la cola de matchmaking. |
| `cond_matchmaking` | Condvar sobre `mutex_sistema`; los jugadores duermen aquí sin busy-wait mientras no encuentran oponente. |
| `sem_tableros` | Semáforo inicializado en `K_BOARDS`; garantiza que nunca haya más de K partidas simultáneas sin busy-wait. |
| `cond_match_done` | Por jugador: cada jugador duerme aquí hasta que `board_routine` le señala el fin de su partida. |
| `board_mutex` | Por tablero: protege `grid` y `move_history` frente a lecturas concurrentes del monitor. |

## Advertencia sobre el uso de Herramientas de IA

Se hace constancia que el uso de IA en este codigo fue solo y **únicamente** para el diseño visual de la representación del tablero de Tic-Tac-Toe.
