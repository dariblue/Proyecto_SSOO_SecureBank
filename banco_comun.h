#ifndef BANCO_COMUN_H
#define BANCO_COMUN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <mqueue.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Constantes  */
#define ID_INICIAL      1001
#define MAX_TITULARES   50
#define MAX_PATH        256
#define MAX_TS          20
#define MAX_ALERT       256

/* Tipos de operación */
#define OP_DEPOSITO      1
#define OP_RETIRO        2
#define OP_TRANSFERENCIA 3
#define OP_MOVER_DIVISA  4
#define OP_LOGIN         5

/* Divisas */
#define DIV_EUR 0
#define DIV_USD 1
#define DIV_GBP 2

/* Nombres de las tres colas POSIX (una por tipo de mensaje) */
#define MQ_MONITOR  "/securebank_monitor"   /* hijo  → monitor  */
#define MQ_LOG      "/securebank_log"        /* hijo  → padre    */
#define MQ_ALERTA   "/securebank_alerta"     /* monitor → padre  */

/* Capacidad máxima de mensajes en cada cola.
 * El límite del sistema suele ser 10 (cat /proc/sys/fs/mqueue/msg_max) */
#define MQ_MAXMSG   10

/* Tipos de alerta */
#define ALERTA_RETIROS        "RETIROS_EXCESIVOS"
#define ALERTA_TRANSFERENCIAS "TRANSFERENCIAS_REPETITIVAS"

/* Semáforos con nombre */
#define SEM_CUENTAS "/sem_cuentas"
#define SEM_CONFIG  "/sem_config"

/* Estructuras de datos */

typedef struct {
    int   numero_cuenta;
    char  titular[MAX_TITULARES];
    float saldo_eur;
    float saldo_usd;
    float saldo_gbp;
} Cuenta;

typedef struct {
    int   cuenta_origen;
    int   cuenta_destino;
    int   tipo_op;
    float cantidad;
    int   divisa;
    char  timestamp[MAX_TS];
} DatosMonitor;

typedef struct {
    int    cuenta_id;
    pid_t  pid_hijo;
    int    tipo_op;
    float  cantidad;
    int    divisa;
    int    estado;
    char   timestamp[MAX_TS];
} DatosLog;

typedef struct {
    int  cuenta_id;
    char mensaje[MAX_ALERT];
} DatosAlerta;

/* Configuración global */
typedef struct {
    int   proximo_id;
    int   puerto;
    float lim_ret_eur, lim_ret_usd, lim_ret_gbp;
    float lim_trf_eur, lim_trf_usd, lim_trf_gbp;
    int   umbral_retiros;
    int   umbral_transferencias;
    int   num_hilos;
    char  archivo_cuentas[MAX_PATH];
    char  archivo_log[MAX_PATH];
    float cambio_usd;
    float cambio_gbp;
} Config;

/* Utilidades  */
static inline void timestamp_ahora(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", tm);
}

static inline const char *nombre_divisa(int d) {
    switch (d) {
        case DIV_EUR: return "EUR";
        case DIV_USD: return "USD";
        case DIV_GBP: return "GBP";
        default:      return "???";
    }
}

#endif /* BANCO_COMUN_H */
