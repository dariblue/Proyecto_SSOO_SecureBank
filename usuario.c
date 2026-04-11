#include "banco_comun.h"

/* Variables globales del proceso hijo */
static mqd_t  g_mq_monitor = (mqd_t)-1;
static mqd_t  g_mq_log     = (mqd_t)-1;
static int    g_pipe_rd    = -1;
static int    g_cuenta_id  =  0;
static Config g_cfg;

/* Leer config (solo lectura) */
static void leer_config_usuario(void) {
    FILE *f = fopen("config.txt", "r");
    if (!f) { perror("fopen config.txt"); return; }

    memset(&g_cfg, 0, sizeof(Config));
    char linea[256];

    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '#' || linea[0] == '\n' || strlen(linea) < 3)
            continue;

        if      (strstr(linea, "PROXIMO_ID="))
            sscanf(linea, "PROXIMO_ID=%d",            &g_cfg.proximo_id);
        else if (strstr(linea, "LIM_RET_EUR="))
            sscanf(linea, "LIM_RET_EUR=%f",           &g_cfg.lim_ret_eur);
        else if (strstr(linea, "LIM_RET_USD="))
            sscanf(linea, "LIM_RET_USD=%f",           &g_cfg.lim_ret_usd);
        else if (strstr(linea, "LIM_RET_GBP="))
            sscanf(linea, "LIM_RET_GBP=%f",           &g_cfg.lim_ret_gbp);
        else if (strstr(linea, "LIM_TRF_EUR="))
            sscanf(linea, "LIM_TRF_EUR=%f",           &g_cfg.lim_trf_eur);
        else if (strstr(linea, "LIM_TRF_USD="))
            sscanf(linea, "LIM_TRF_USD=%f",           &g_cfg.lim_trf_usd);
        else if (strstr(linea, "LIM_TRF_GBP="))
            sscanf(linea, "LIM_TRF_GBP=%f",           &g_cfg.lim_trf_gbp);
        else if (strstr(linea, "UMBRAL_RETIROS="))
            sscanf(linea, "UMBRAL_RETIROS=%d",        &g_cfg.umbral_retiros);
        else if (strstr(linea, "UMBRAL_TRANSFERENCIAS="))
            sscanf(linea, "UMBRAL_TRANSFERENCIAS=%d", &g_cfg.umbral_transferencias);
        else if (strstr(linea, "NUM_HILOS="))
            sscanf(linea, "NUM_HILOS=%d",             &g_cfg.num_hilos);
        else if (strstr(linea, "ARCHIVO_CUENTAS="))
            sscanf(linea, "ARCHIVO_CUENTAS=%255s",    g_cfg.archivo_cuentas);
        else if (strstr(linea, "ARCHIVO_LOG="))
            sscanf(linea, "ARCHIVO_LOG=%255s",         g_cfg.archivo_log);
        else if (strstr(linea, "CAMBIO_USD="))
            sscanf(linea, "CAMBIO_USD=%f",            &g_cfg.cambio_usd);
        else if (strstr(linea, "CAMBIO_GBP="))
            sscanf(linea, "CAMBIO_GBP=%f",            &g_cfg.cambio_gbp);
    }

    fclose(f);
}

/* Semáforo: abrir con comprobación */
static sem_t *abrir_sem_cuentas(void) {
    sem_t *s = sem_open(SEM_CUENTAS, 0);
    if (s == SEM_FAILED) { perror("sem_open SEM_CUENTAS"); return NULL; }
    return s;
}

/*
E2C940E3C540D7C9C4C5D540D8E4C540C3D6D5E2C5D9E5C5E240C5D340C5E2D8E4C5D3C5E3D640C4C540C5E2E3C540D7D9D6C7D9C1D4C140C9D5E3C1C3E3D66B40D5D640C8C1C7C1E240C3C1E2D66B40D4D6C4C9C6C9C3C140E2C9C5D4D7D9C540C5D340E3C5E7E3D640C5D540C5C2C3C4C9C340E2E4D4C1D5C4D640F140C1D340C4C5C3C9D4D640C3D1D9C1C3E3C5D940C5D540C5C2C3C4C9C34BE2C940E3C540C4C9C3C5D540D8E4C540D7D6D940D8E4C540D3D640C8C1E240C3C1D4C2C9C1C4D640C4C940D8E4C540D5D640D3D640C8C1E240C3C1D4C2C9C1C4D64BD5D640C8C1C7C1E240D5C9D5C7D2D540C3D6C4C9C6C9C3C1C4D640C1D340C3D6C4C9C7D640D6C3E4D3E3D640C4C5D5E3D9D640C4C5D340C2D3D6D8E4C540C3D6C4C9C6C9C3C1C4D640C5D540C5C2C3C4C9C3
*/
/*  Leer cuenta calculando el offset directo */
static int leer_cuenta(int id, Cuenta *c) {
    if (id < ID_INICIAL) return -1;
    long offset = (long)(id - ID_INICIAL) * (long)sizeof(Cuenta);

    FILE *f = fopen(g_cfg.archivo_cuentas, "rb");
    if (!f) return -1;

    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    int ok = (fread(c, sizeof(Cuenta), 1, f) == 1) ? 0 : -1;
    fclose(f);

    /* Validar que realmente hemos leído la cuenta esperada */
    if (ok == 0 && c->numero_cuenta != id) {
        return -1;
    }

    return ok;
}

/* Escribir cuenta usando offset directo */
static int escribir_cuenta(const Cuenta *c) {
    if (c->numero_cuenta < ID_INICIAL) return -1;
    long offset = (long)(c->numero_cuenta - ID_INICIAL) * (long)sizeof(Cuenta);

    FILE *f = fopen(g_cfg.archivo_cuentas, "rb+");
    if (!f) return -1;

    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    int ok = (fwrite(c, sizeof(Cuenta), 1, f) == 1) ? 0 : -1;
    fflush(f);
    fclose(f);
    return ok;
}

/*
E2C940E3C540D7C9C4C5D540D8E4C540C3D6D5E2C5D9E5C5E240C5D340C5E2D8E4C5D3C5E3D640C4C540C5E2E3C540D7D9D6C7D9C1D4C140C9D5E3C1C3E3D66B40D5D640C8C1C7C1E240C3C1E2D66B40D4D6C4C9C6C9C3C140E2C9C5D4D7D9C540C5D340E3C5E7E3D640C5D540C5C2C3C4C9C340E2E4D4C1D5C4D640F140C1D340C4C5C3C9D4D640C3D1D9C1C3E3C5D940C5D540C5C2C3C4C9C34BE2C940E3C540C4C9C3C5D540D8E4C540D7D6D940D8E4C540D3D640C8C1E240C3C1D4C2C9C1C4D640C4C940D8E4C540D5D640D3D640C8C1E240C3C1D4C2C9C1C4D64BD5D640C8C1C7C1E240D5C9D5C7D2D540C3D6C4C9C6C9C3C1C4D640C1D340C3D6C4C9C7D640D6C3E4D3E3D640C4C5D5E3D9D640C4C5D340C2D3D6D8E4C540C3D6C4C9C6C9C3C1C4D640C5D540C5C2C3C4C9C3
*/


/* Enviar mensajes a las colas POSIX */
static void enviar_monitor(int cuenta_origen, int cuenta_destino,
                           int tipo_op, float cantidad, int divisa) {
    DatosMonitor dm;
    memset(&dm, 0, sizeof(dm));
    dm.cuenta_origen  = cuenta_origen;
    dm.cuenta_destino = cuenta_destino;
    dm.tipo_op        = tipo_op;
    dm.cantidad       = cantidad;
    dm.divisa         = divisa;
    timestamp_ahora(dm.timestamp, sizeof(dm.timestamp));
    mq_send(g_mq_monitor, (const char *)&dm, sizeof(dm), 0);
}

static void enviar_log(int tipo_op, float cantidad, int divisa, int estado) {
    DatosLog dl;
    memset(&dl, 0, sizeof(dl));
    dl.cuenta_id = g_cuenta_id;
    dl.pid_hijo  = getpid();
    dl.tipo_op   = tipo_op;
    dl.cantidad  = cantidad;
    dl.divisa    = divisa;
    dl.estado    = estado;
    timestamp_ahora(dl.timestamp, sizeof(dl.timestamp));
    mq_send(g_mq_log, (const char *)&dl, sizeof(dl), 0);
}

/*
E2C940E3C540D7C9C4C5D540D8E4C540C3D6D5E2C5D9E5C5E240C5D340C5E2D8E4C5D3C5E3D640C4C540C5E2E3C540D7D9D6C7D9C1D4C140C9D5E3C1C3E3D66B40D5D640C8C1C7C1E240C3C1E2D66B40D4D6C4C9C6C9C3C140E2C9C5D4D7D9C540C5D340E3C5E7E3D640C5D540C5C2C3C4C9C340E2E4D4C1D5C4D640F140C1D340C4C5C3C9D4D640C3D1D9C1C3E3C5D940C5D540C5C2C3C4C9C34BE2C940E3C540C4C9C3C5D540D8E4C540D7D6D940D8E4C540D3D640C8C1E240C3C1D4C2C9C1C4D640C4C940D8E4C540D5D640D3D640C8C1E240C3C1D4C2C9C1C4D64BD5D640C8C1C7C1E240D5C9D5C7D2D540C3D6C4C9C6C9C3C1C4D640C1D340C3D6C4C9C7D640D6C3E4D3E3D640C4C5D5E3D9D640C4C5D340C2D3D6D8E4C540C3D6C4C9C6C9C3C1C4D640C5D540C5C2C3C4C9C3
*/
/* ════════════════════════════════════════════════════════════
 * Threads de operación bancaria
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    int   tipo_op;
    int   cuenta_id;
    int   cuenta_destino;
    float cantidad;
    int   divisa_origen;
    int   divisa_destino;
} DatosOperacion;

/* Helpers para seleccionar saldo y límite según divisa */
static float *saldo_ptr(Cuenta *c, int divisa) {
    switch (divisa) {
        case DIV_EUR: return &c->saldo_eur;
        case DIV_USD: return &c->saldo_usd;
        case DIV_GBP: return &c->saldo_gbp;
        default:      return NULL;
    }
}

static float limite_retiro(int divisa) {
    switch (divisa) {
        case DIV_EUR: return g_cfg.lim_ret_eur;
        case DIV_USD: return g_cfg.lim_ret_usd;
        case DIV_GBP: return g_cfg.lim_ret_gbp;
        default:      return 0;
    }
}

static float limite_transferencia(int divisa) {
    switch (divisa) {
        case DIV_EUR: return g_cfg.lim_trf_eur;
        case DIV_USD: return g_cfg.lim_trf_usd;
        case DIV_GBP: return g_cfg.lim_trf_gbp;
        default:      return 0;
    }
}

/* Depósito  */
static void *thread_deposito(void *arg) {
    DatosOperacion *op = (DatosOperacion *)arg;
    sem_t *sem = abrir_sem_cuentas();
    if (!sem) { free(op); return NULL; }

    sem_wait(sem);

    Cuenta c;
    if (leer_cuenta(op->cuenta_id, &c) < 0) {
        printf("  [ERROR] Cuenta %d no encontrada.\n", op->cuenta_id);
        enviar_log(OP_DEPOSITO, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    float *s = saldo_ptr(&c, op->divisa_origen);
    if (!s) {
        printf("  [ERROR] Divisa inválida.\n");
        enviar_log(OP_DEPOSITO, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    *s += op->cantidad;
    escribir_cuenta(&c);

    sem_post(sem);
    sem_close(sem);

    printf("  Depósito OK: +%.2f %s -> saldo %s = %.2f\n",
           op->cantidad, nombre_divisa(op->divisa_origen),
           nombre_divisa(op->divisa_origen), *s);

    enviar_monitor(op->cuenta_id, 0, OP_DEPOSITO, op->cantidad, op->divisa_origen);
    enviar_log(OP_DEPOSITO, op->cantidad, op->divisa_origen, 1);

    free(op);
    return NULL;
}

/* Retiro */
static void *thread_retiro(void *arg) {
    DatosOperacion *op = (DatosOperacion *)arg;
    sem_t *sem = abrir_sem_cuentas();
    if (!sem) { free(op); return NULL; }

    /* Comprobar límite de retiro */
    float lim = limite_retiro(op->divisa_origen);
    if (lim > 0 && op->cantidad > lim) {
        printf("  [ERROR] Retiro %.2f %s excede el límite (%.0f).\n",
               op->cantidad, nombre_divisa(op->divisa_origen), lim);
        enviar_log(OP_RETIRO, op->cantidad, op->divisa_origen, 0);
        sem_close(sem); free(op);
        return NULL;
    }

    sem_wait(sem);

    Cuenta c;
    if (leer_cuenta(op->cuenta_id, &c) < 0) {
        printf("  [ERROR] Cuenta %d no encontrada.\n", op->cuenta_id);
        enviar_log(OP_RETIRO, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    float *s = saldo_ptr(&c, op->divisa_origen);
    if (!s || *s < op->cantidad) {
        printf("  [ERROR] Saldo insuficiente en %s (disponible: %.2f).\n",
               nombre_divisa(op->divisa_origen), s ? *s : 0);
        enviar_log(OP_RETIRO, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    *s -= op->cantidad;
    escribir_cuenta(&c);

    sem_post(sem);
    sem_close(sem);

    printf("  Retiro OK: -%.2f %s -> saldo %s = %.2f\n",
           op->cantidad, nombre_divisa(op->divisa_origen),
           nombre_divisa(op->divisa_origen), *s);

    enviar_monitor(op->cuenta_id, 0, OP_RETIRO, op->cantidad, op->divisa_origen);
    enviar_log(OP_RETIRO, op->cantidad, op->divisa_origen, 1);

    free(op);
    return NULL;
}

/*
E2C940E3C540D7C9C4C5D540D8E4C540C3D6D5E2C5D9E5C5E240C5D340C5E2D8E4C5D3C5E3D640C4C540C5E2E3C540D7D9D6C7D9C1D4C140C9D5E3C1C3E3D66B40D5D640C8C1C7C1E240C3C1E2D66B40D4D6C4C9C6C9C3C140E2C9C5D4D7D9C540C5D340E3C5E7E3D640C5D540C5C2C3C4C9C340E2E4D4C1D5C4D640F140C1D340C4C5C3C9D4D640C3D1D9C1C3E3C5D940C5D540C5C2C3C4C9C34BE2C940E3C540C4C9C3C5D540D8E4C540D7D6D940D8E4C540D3D640C8C1E240C3C1D4C2C9C1C4D640C4C940D8E4C540D5D640D3D640C8C1E240C3C1D4C2C9C1C4D64BD5D640C8C1C7C1E240D5C9D5C7D2D540C3D6C4C9C6C9C3C1C4D640C1D340C3D6C4C9C7D640D6C3E4D3E3D640C4C5D5E3D9D640C4C5D340C2D3D6D8E4C540C3D6C4C9C6C9C3C1C4D640C5D540C5C2C3C4C9C3
*/
/* Transferencia */
static void *thread_transferencia(void *arg) {
    DatosOperacion *op = (DatosOperacion *)arg;
    sem_t *sem = abrir_sem_cuentas();
    if (!sem) { free(op); return NULL; }

    /* Comprobar límite de transferencia */
    float lim = limite_transferencia(op->divisa_origen);
    if (lim > 0 && op->cantidad > lim) {
        printf("  [ERROR] Transferencia %.2f %s excede el límite (%.0f).\n",
               op->cantidad, nombre_divisa(op->divisa_origen), lim);
        enviar_log(OP_TRANSFERENCIA, op->cantidad, op->divisa_origen, 0);
        sem_close(sem); free(op);
        return NULL;
    }

    sem_wait(sem);

    /* Leer cuenta origen */
    Cuenta origen;
    if (leer_cuenta(op->cuenta_id, &origen) < 0) {
        printf("  [ERROR] Cuenta origen %d no encontrada.\n", op->cuenta_id);
        enviar_log(OP_TRANSFERENCIA, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    /* Verificar saldo */
    float *s_orig = saldo_ptr(&origen, op->divisa_origen);
    if (!s_orig || *s_orig < op->cantidad) {
        printf("  [ERROR] Saldo insuficiente en %s (disponible: %.2f).\n",
               nombre_divisa(op->divisa_origen), s_orig ? *s_orig : 0);
        enviar_log(OP_TRANSFERENCIA, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    /* Leer cuenta destino */
    Cuenta destino;
    if (leer_cuenta(op->cuenta_destino, &destino) < 0) {
        printf("  [ERROR] Cuenta destino %d no encontrada.\n", op->cuenta_destino);
        enviar_log(OP_TRANSFERENCIA, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    /* Mover fondos */
    float *s_dest = saldo_ptr(&destino, op->divisa_origen);
    *s_orig -= op->cantidad;
    *s_dest += op->cantidad;

    escribir_cuenta(&origen);
    escribir_cuenta(&destino);

    sem_post(sem);
    sem_close(sem);

    printf("  Transferencia OK: %.2f %s de cuenta %d -> cuenta %d\n",
           op->cantidad, nombre_divisa(op->divisa_origen),
           op->cuenta_id, op->cuenta_destino);

    enviar_monitor(op->cuenta_id, op->cuenta_destino,
                   OP_TRANSFERENCIA, op->cantidad, op->divisa_origen);
    enviar_log(OP_TRANSFERENCIA, op->cantidad, op->divisa_origen, 1);

    free(op);
    return NULL;
}

/* Mover divisas */
static void *thread_mover_divisa(void *arg) {
    DatosOperacion *op = (DatosOperacion *)arg;
    sem_t *sem = abrir_sem_cuentas();
    if (!sem) { free(op); return NULL; }

    if (op->divisa_origen == op->divisa_destino) {
        printf("  [ERROR] Divisa origen y destino son iguales.\n");
        sem_close(sem); free(op);
        return NULL;
    }

    sem_wait(sem);

    Cuenta c;
    if (leer_cuenta(op->cuenta_id, &c) < 0) {
        printf("  [ERROR] Cuenta %d no encontrada.\n", op->cuenta_id);
        enviar_log(OP_MOVER_DIVISA, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    float *s_orig = saldo_ptr(&c, op->divisa_origen);
    float *s_dest = saldo_ptr(&c, op->divisa_destino);
    if (!s_orig || !s_dest || *s_orig < op->cantidad) {
        printf("  [ERROR] Saldo insuficiente en %s (disponible: %.2f).\n",
               nombre_divisa(op->divisa_origen), s_orig ? *s_orig : 0);
        enviar_log(OP_MOVER_DIVISA, op->cantidad, op->divisa_origen, 0);
        sem_post(sem); sem_close(sem); free(op);
        return NULL;
    }

    /* Conversión via EUR como base:
     * origen -> EUR -> destino
     * tasa_divisa = cuántas unidades de divisa por 1 EUR */
    float tasa_orig = 1.0f, tasa_dest = 1.0f;
    if (op->divisa_origen == DIV_USD) tasa_orig = g_cfg.cambio_usd;
    if (op->divisa_origen == DIV_GBP) tasa_orig = g_cfg.cambio_gbp;
    if (op->divisa_destino == DIV_USD) tasa_dest = g_cfg.cambio_usd;
    if (op->divisa_destino == DIV_GBP) tasa_dest = g_cfg.cambio_gbp;

    float en_eur = op->cantidad / tasa_orig;    /* a EUR */
    float convertido = en_eur * tasa_dest;       /* a destino */

    *s_orig -= op->cantidad;
    *s_dest += convertido;
    escribir_cuenta(&c);

    sem_post(sem);
    sem_close(sem);

    printf("  Conversión OK: %.2f %s -> %.2f %s\n",
           op->cantidad, nombre_divisa(op->divisa_origen),
           convertido, nombre_divisa(op->divisa_destino));

    enviar_monitor(op->cuenta_id, 0, OP_MOVER_DIVISA, op->cantidad, op->divisa_origen);
    enviar_log(OP_MOVER_DIVISA, op->cantidad, op->divisa_origen, 1);

    free(op);
    return NULL;
}

/* Lanzar thread */
static void lanzar_operacion(void *(*fn)(void*), DatosOperacion *d) {
    pthread_t t;
    pthread_create(&t, NULL, fn, d);
    pthread_join(t, NULL);
}

/* Consultar saldos */
static void consultar_saldos(void) {
    sem_t *sem = abrir_sem_cuentas();
    if (!sem) return;

    sem_wait(sem);

    Cuenta c;
    int ok = leer_cuenta(g_cuenta_id, &c);

    sem_post(sem);
    sem_close(sem);

    if (ok < 0) {
        printf("  [ERROR] No se pudo leer la cuenta %d.\n", g_cuenta_id);
        return;
    }

    printf("\n  ┌─────────────────────────────┐\n");
    printf("  │  Saldos cuenta %-13d│\n", g_cuenta_id);
    printf("  ├─────────────────────────────┤\n");
    printf("  │  EUR: %20.2f │\n", c.saldo_eur);
    printf("  │  USD: %20.2f │\n", c.saldo_usd);
    printf("  │  GBP: %20.2f │\n", c.saldo_gbp);
    printf("  └─────────────────────────────┘\n");
}

/* Pedir divisa */
static int pedir_divisa(const char *prompt) {
    int d = -1;
    while (d < 0 || d > 2) {
        printf("%s (0=EUR, 1=USD, 2=GBP): ", prompt);
        fflush(stdout);
        if (scanf("%d", &d) != 1) {
            int c; while ((c=getchar())!='\n'&&c!=EOF);
            d = -1;
        }
    }
    return d;
}

/* Procesar opción del menú */
static void procesar_opcion(int opcion) {
    DatosOperacion *d = NULL;
    switch (opcion) {
    case 1:
        d = calloc(1, sizeof(DatosOperacion));
        d->tipo_op       = OP_DEPOSITO;
        d->cuenta_id     = g_cuenta_id;
        d->divisa_origen = pedir_divisa("Divisa");
        printf("Cantidad a depositar: "); fflush(stdout);
        scanf("%f", &d->cantidad);
        lanzar_operacion(thread_deposito, d);
        break;
    case 2:
        d = calloc(1, sizeof(DatosOperacion));
        d->tipo_op       = OP_RETIRO;
        d->cuenta_id     = g_cuenta_id;
        d->divisa_origen = pedir_divisa("Divisa");
        printf("Cantidad a retirar: "); fflush(stdout);
        scanf("%f", &d->cantidad);
        lanzar_operacion(thread_retiro, d);
        break;
    case 3:
        d = calloc(1, sizeof(DatosOperacion));
        d->tipo_op       = OP_TRANSFERENCIA;
        d->cuenta_id     = g_cuenta_id;
        printf("Cuenta destino: "); fflush(stdout);
        scanf("%d", &d->cuenta_destino);
        d->divisa_origen = pedir_divisa("Divisa");
        printf("Cantidad: "); fflush(stdout);
        scanf("%f", &d->cantidad);
        lanzar_operacion(thread_transferencia, d);
        break;
    case 4:
        consultar_saldos();
        break;
    case 5:
        d = calloc(1, sizeof(DatosOperacion));
        d->tipo_op        = OP_MOVER_DIVISA;
        d->cuenta_id      = g_cuenta_id;
        d->divisa_origen  = pedir_divisa("Divisa origen");
        d->divisa_destino = pedir_divisa("Divisa destino");
        printf("Cantidad a convertir: "); fflush(stdout);
        scanf("%f", &d->cantidad);
        lanzar_operacion(thread_mover_divisa, d);
        break;
    case 6:
        printf("Saliendo...\n");
        mq_close(g_mq_monitor);
        mq_close(g_mq_log);
        exit(0);
    default:
        printf("Opcion no valida.\n");
    }
}

/* Mostrar menú */
static void mostrar_menu(void) {
    printf("\n+==========================+\n");
    printf("|  Cuenta %-17d|\n", g_cuenta_id);
    printf("|==========================|\n");
    printf("| 1. Deposito              |\n");
    printf("| 2. Retiro                |\n");
    printf("| 3. Transferencia         |\n");
    printf("| 4. Consultar saldos      |\n");
    printf("| 5. Mover divisas         |\n");
    printf("| 6. Salir                 |\n");
    printf("+==========================+\n");
    printf("Opcion: ");
    fflush(stdout);
}

/*
E2C940E3C540D7C9C4C5D540D8E4C540C3D6D5E2C5D9E5C5E240C5D340C5E2D8E4C5D3C5E3D640C4C540C5E2E3C540D7D9D6C7D9C1D4C140C9D5E3C1C3E3D66B40D5D640C8C1C7C1E240C3C1E2D66B40D4D6C4C9C6C9C3C140E2C9C5D4D7D9C540C5D340E3C5E7E3D640C5D540C5C2C3C4C9C340E2E4D4C1D5C4D640F140C1D340C4C5C3C9D4D640C3D1D9C1C3E3C5D940C5D540C5C2C3C4C9C34BE2C940E3C540C4C9C3C5D540D8E4C540D7D6D940D8E4C540D3D640C8C1E240C3C1D4C2C9C1C4D640C4C940D8E4C540D5D640D3D640C8C1E240C3C1D4C2C9C1C4D64BD5D640C8C1C7C1E240D5C9D5C7D2D540C3D6C4C9C6C9C3C1C4D640C1D340C3D6C4C9C7D640D6C3E4D3E3D640C4C5D5E3D9D640C4C5D340C2D3D6D8E4C540C3D6C4C9C6C9C3C1C4D640C5D540C5C2C3C4C9C3
*/
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: usuario <cuenta_id> <pipe_rd>\n");
        return 1;
    }
    g_cuenta_id = atoi(argv[1]);
    g_pipe_rd   = atoi(argv[2]);

    leer_config_usuario();

    /* Abrir las colas POSIX ya creadas por banco.c */
    g_mq_monitor = mq_open(MQ_MONITOR, O_WRONLY);
    g_mq_log     = mq_open(MQ_LOG,     O_WRONLY);
    if (g_mq_monitor==(mqd_t)-1 || g_mq_log==(mqd_t)-1) {
        perror("mq_open en usuario");
        return 1;
    }

    sem_t *test = sem_open(SEM_CUENTAS, 0);
    if (test == SEM_FAILED) {
        fprintf(stderr, "[Cuenta %d] ERROR: semaforo %s no disponible: %s\n",
                g_cuenta_id, SEM_CUENTAS, strerror(errno));
        return 1;
    }
    sem_close(test);

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
    fds[1].fd = g_pipe_rd;    fds[1].events = POLLIN;

    mostrar_menu();

    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) { if (errno==EINTR) continue; break; }

        if (fds[1].revents & POLLIN) {
            char buf[256];
            ssize_t n = read(g_pipe_rd, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                printf("\n[ALERTA DEL BANCO]: %s\n", buf);
                fflush(stdout);
                if (strstr(buf, "BLOQUEO")) {
                    mq_close(g_mq_monitor);
                    mq_close(g_mq_log);
                    close(g_pipe_rd);
                    exit(0);
                }
            }
        }

        if (fds[0].revents & POLLIN) {
            int opcion;
            if (scanf("%d", &opcion) == 1) {
                procesar_opcion(opcion);
            } else {
                int c; while ((c=getchar())!='\n'&&c!=EOF);
            }
            mostrar_menu();
        }
    }

    mq_close(g_mq_monitor);
    mq_close(g_mq_log);
    close(g_pipe_rd);
    return 0;
}
