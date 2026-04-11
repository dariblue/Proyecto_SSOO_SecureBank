#include "banco_comun.h"

/* Parámetros de detección */
static int   g_umbral_retiros        = 3;
static int   g_umbral_transferencias = 5;
static volatile sig_atomic_t g_salir = 0;

static mqd_t g_mq_monitor = (mqd_t)-1;
static mqd_t g_mq_alerta  = (mqd_t)-1;

/* Señal de terminación  */
static void manejador_sigterm(int s) { (void)s; g_salir = 1; }

/* Leer config */
static void leer_config_monitor(void) {
    FILE *f = fopen("config.txt", "r");
    if (!f) { perror("[MONITOR] fopen config.txt"); return; }

    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '#' || linea[0] == '\n' || strlen(linea) < 3)
            continue;
        if (strstr(linea, "UMBRAL_RETIROS="))
            sscanf(linea, "UMBRAL_RETIROS=%d", &g_umbral_retiros);
        else if (strstr(linea, "UMBRAL_TRANSFERENCIAS="))
            sscanf(linea, "UMBRAL_TRANSFERENCIAS=%d", &g_umbral_transferencias);
    }
    fclose(f);

    printf("[MONITOR] Umbrales: retiros=%d, transferencias=%d\n",
           g_umbral_retiros, g_umbral_transferencias);
}

/*  Enviar alerta al padre  */
static void enviar_alerta(int cuenta_id, const char *tipo_alerta) {
    DatosAlerta da;
    memset(&da, 0, sizeof(da));
    da.cuenta_id = cuenta_id;
    snprintf(da.mensaje, sizeof(da.mensaje), "%s", tipo_alerta);

    if (mq_send(g_mq_alerta, (const char *)&da, sizeof(da), 0) < 0)
        perror("[MONITOR] mq_send alerta");
    else
        printf("[MONITOR] ALERTA enviada: %s para cuenta %d\n",
               tipo_alerta, cuenta_id);
}


#define MAX_CUENTAS_TRACK 256
#define MAX_TRAN_TRACK    64

typedef struct {
    int   cuenta_id;
    int   retiros_consecutivos;
    float ultimo_retiro;
} TrackRetiros;

typedef struct {
    int cuenta_origen;
    int cuenta_destino;
    int contador;
} TrackTransferencias;

static TrackRetiros        g_retiros[MAX_CUENTAS_TRACK];
static int                 g_n_retiros = 0;
static TrackTransferencias g_transf[MAX_TRAN_TRACK];
static int                 g_n_transf = 0;

/*  Analizar mensaje  */
static void analizar(const DatosMonitor *dm) {

    /*  Retiros consecutivos */
    if (dm->tipo_op == OP_RETIRO) {
        /* Buscar o crear entrada de tracking para esta cuenta */
        int idx = -1;
        for (int i = 0; i < g_n_retiros; i++)
            if (g_retiros[i].cuenta_id == dm->cuenta_origen) { idx = i; break; }
        if (idx < 0 && g_n_retiros < MAX_CUENTAS_TRACK) {
            idx = g_n_retiros++;
            g_retiros[idx].cuenta_id = dm->cuenta_origen;
            g_retiros[idx].retiros_consecutivos = 0;
            g_retiros[idx].ultimo_retiro = 0;
        }
        if (idx >= 0) {
            g_retiros[idx].retiros_consecutivos++;
            g_retiros[idx].ultimo_retiro = dm->cantidad;
            printf("[MONITOR] Cuenta %d: retiro #%d consecutivo (%.2f)\n",
                   dm->cuenta_origen, g_retiros[idx].retiros_consecutivos, dm->cantidad);
            if (g_retiros[idx].retiros_consecutivos >= g_umbral_retiros) {
                enviar_alerta(dm->cuenta_origen, ALERTA_RETIROS);
                g_retiros[idx].retiros_consecutivos = 0;
            }
        }
    } else {
        for (int i = 0; i < g_n_retiros; i++)
            if (g_retiros[i].cuenta_id == dm->cuenta_origen) {
                g_retiros[i].retiros_consecutivos = 0; break;
            }
    }

    /*  Transferencias repetitivas */
    if (dm->tipo_op == OP_TRANSFERENCIA) {
        int idx = -1;
        for (int i = 0; i < g_n_transf; i++)
            if (g_transf[i].cuenta_origen  == dm->cuenta_origen &&
                g_transf[i].cuenta_destino == dm->cuenta_destino) { idx = i; break; }
        if (idx < 0 && g_n_transf < MAX_TRAN_TRACK) {
            idx = g_n_transf++;
            g_transf[idx].cuenta_origen  = dm->cuenta_origen;
            g_transf[idx].cuenta_destino = dm->cuenta_destino;
            g_transf[idx].contador       = 0;
        }
        if (idx >= 0) {
            g_transf[idx].contador++;
            if (g_transf[idx].contador >= g_umbral_transferencias) {
                enviar_alerta(dm->cuenta_origen, ALERTA_TRANSFERENCIAS);
                g_transf[idx].contador = 0;
            }
        }
    }
}


int main(void) {
    signal(SIGTERM, manejador_sigterm);
    signal(SIGINT,  SIG_IGN);

    leer_config_monitor();

    /* Abrir las colas ya creadas por banco.c */
    g_mq_monitor = mq_open(MQ_MONITOR, O_RDONLY);
    g_mq_alerta  = mq_open(MQ_ALERTA,  O_WRONLY);
    if (g_mq_monitor==(mqd_t)-1 || g_mq_alerta==(mqd_t)-1) {
        perror("[MONITOR] mq_open");
        return 1;
    }

    /* poll() sobre el descriptor de MQ_MONITOR: bloqueo eficiente */
    struct pollfd pfd;
    pfd.fd     = (int)g_mq_monitor;
    pfd.events = POLLIN;

    while (!g_salir) {
        int ret = poll(&pfd, 1, 500);  /* timeout 500ms para comprobar g_salir */
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[MONITOR] poll");
            break;
        }
        if (ret == 0) continue;  /* timeout, comprobar g_salir */

        if (pfd.revents & POLLIN) {
            DatosMonitor dm;
            ssize_t n = mq_receive(g_mq_monitor, (char *)&dm, sizeof(dm), NULL);
            if (n > 0) {
                printf("[MONITOR] Recibido: op=%d cuenta=%d monto=%.2f\n",
                       dm.tipo_op, dm.cuenta_origen, dm.cantidad);
                analizar(&dm);
            }
        }
    }

    mq_close(g_mq_monitor);
    mq_close(g_mq_alerta);
    return 0;
}

