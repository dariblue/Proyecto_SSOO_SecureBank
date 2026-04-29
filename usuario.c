/* =========================================================================
 * ARCHIVO: usuario.c
 * ROL: Este es el "Cliente" o "Cajero" del SecureBank. 
 * FUNCIONAMIENTO: Se lanza un proceso de estos por cada usuario conectado. 
 *                 Gestiona la interfaz de la terminal, crea hilos (pthreads)
 *                 para hacer las operaciones bancarias sin bloquear el menu,
 *                 y envia mensajes al banco (Padre) y al monitor (Vigilante).
 * ========================================================================= */

#include "banco_comun.h"

/* Variables globales del proceso hijo (se mantienen separadas por cada usuario) */
static mqd_t  g_mq_monitor = (mqd_t)-1; /* Identificador del buzon para el vigilante */
static mqd_t  g_mq_log     = (mqd_t)-1; /* Identificador del buzon para el registro */
static int    g_pipe_rd    = -1;        /* Tubo (pipe) por donde el padre envia "BLOQUEO" */
static int    g_cuenta_id  =  0;        /* ID del usuario actualmente logueado (ej: 1001) */
static Config g_cfg;                    /* Copia local de las reglas del banco */

/* -------------------------------------------------------------------------
 * Funcion: limpiar_telnet
 * Uso: Telnet envia "\r\n" al pulsar Enter, lo cual rompe scanf/fgets.
 *      Esta funcion elimina los caracteres \r y \n sobrantes de un buffer.
 * ------------------------------------------------------------------------- */
static void limpiar_telnet(char *buf) {
    char *p = buf;
    while (*p) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
        p++;
    }
}

/* -------------------------------------------------------------------------
 * Funcion: leer_config_usuario
 * Uso: Lee el archivo "config.txt" para saber cuales son los umbrales
 *      y limites de operaciones en la sesion actual. 
 * ------------------------------------------------------------------------- */
static void leer_config_usuario(void) {
    FILE *f = fopen("config.txt", "r");
    if (!f) { perror("fopen config.txt"); return; }

    memset(&g_cfg, 0, sizeof(Config));
    char linea[256];

    /* Leer linea por linea ignorando comentarios (#) y parseando variables clave */
    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '#' || linea[0] == '\n' || strlen(linea) < 3)
            continue;

        if      (strstr(linea, "PROXIMO_ID=")) /* Busca la firma en la linea (ej. PROXIMO_ID=) */
            sscanf(linea, "PROXIMO_ID=%d",            &g_cfg.proximo_id); /* Extrae el numero puro (%d) inyectandolo a la memoria */
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

/* Forward declarations (Fase II: necesarias porque autenticar_usuario y
 * crear_cuenta_remota se definen antes que leer_cuenta/escribir_cuenta) */
static int leer_cuenta(int id, Cuenta *c);
static int escribir_cuenta(const Cuenta *c);

/* -------------------------------------------------------------------------
 * Funcion: guardar_proximo_id_usuario
 * Uso: Replica de la logica de banco.c para actualizar PROXIMO_ID en
 *      config.txt desde el proceso hijo (necesario para crear cuentas
 *      remotamente en Fase II).
 * ------------------------------------------------------------------------- */
static void guardar_proximo_id_usuario(int nuevo_id) {
    FILE *f = fopen("config.txt", "r");
    if (!f) return;
    char contenido[4096];
    size_t n = fread(contenido, 1, sizeof(contenido) - 1, f);
    contenido[n] = '\0';
    fclose(f);

    char *p = strstr(contenido, "PROXIMO_ID=");
    if (!p) return;
    size_t antes = (size_t)(p - contenido);
    char *fin_linea = strchr(p, '\n');

    char nuevo[4096];
    snprintf(nuevo, sizeof(nuevo), "%.*sPROXIMO_ID=%d%s",
             (int)antes, contenido, nuevo_id, fin_linea ? fin_linea : "");

    f = fopen("config.txt", "w");
    if (!f) return;
    fputs(nuevo, f);
    fclose(f);
}

/* -------------------------------------------------------------------------
 * Funcion: crear_cuenta_remota
 * Uso: Fase II — Permite crear una cuenta nueva desde el proceso hijo
 *      (via Telnet). Replica la sincronizacion exacta de banco.c:
 *      SEM_CONFIG para el ID y SEM_CUENTAS para la escritura en disco.
 * ------------------------------------------------------------------------- */
static int crear_cuenta_remota(const char *titular) {
    Cuenta nueva;
    memset(&nueva, 0, sizeof(nueva));
    strncpy(nueva.titular, titular, MAX_TITULARES - 1);
    nueva.saldo_eur = nueva.saldo_usd = nueva.saldo_gbp = 0.0f;

    /* 1. Reservar PROXIMO_ID bajo SEM_CONFIG */
    sem_t *sc = sem_open(SEM_CONFIG, 0);
    if (sc == SEM_FAILED) { perror("sem_open SEM_CONFIG"); return -1; }

    sem_wait(sc);

    /* Leer PROXIMO_ID actual directamente de config.txt */
    int proximo_id = -1;
    FILE *f = fopen("config.txt", "r");
    if (f) {
        char linea[256];
        while (fgets(linea, sizeof(linea), f)) {
            if (strstr(linea, "PROXIMO_ID=")) {
                sscanf(linea, "PROXIMO_ID=%d", &proximo_id);
                break;
            }
        }
        fclose(f);
    }

    if (proximo_id < ID_INICIAL) {
        sem_post(sc); sem_close(sc);
        return -1;
    }

    nueva.numero_cuenta = proximo_id;
    guardar_proximo_id_usuario(proximo_id + 1);

    sem_post(sc);
    sem_close(sc);

    /* 2. Escribir la nueva cuenta bajo SEM_CUENTAS */
    sem_t *sa = sem_open(SEM_CUENTAS, 0);
    if (sa == SEM_FAILED) { perror("sem_open SEM_CUENTAS"); return -1; }

    sem_wait(sa);
    f = fopen(g_cfg.archivo_cuentas, "rb+");
    if (!f) f = fopen(g_cfg.archivo_cuentas, "wb+");
    if (f) {
        long offset = (long)(nueva.numero_cuenta - ID_INICIAL) * (long)sizeof(Cuenta);
        fseek(f, offset, SEEK_SET);
        fwrite(&nueva, sizeof(Cuenta), 1, f);
        fflush(f);
        fclose(f);
    }
    sem_post(sa);
    sem_close(sa);

    printf("Cuenta %d creada para '%s'\n", nueva.numero_cuenta, nueva.titular);
    fflush(stdout);
    return nueva.numero_cuenta;
}

/* -------------------------------------------------------------------------
 * Funcion: autenticar_usuario
 * Uso: Fase II — Pide el numero de cuenta al usuario conectado por Telnet.
 *      Permite login con cuenta existente o crear cuenta nueva (opcion 0).
 *      Retorna el cuenta_id validado o -1 si el usuario cierra la conexion.
 * ------------------------------------------------------------------------- */
static int autenticar_usuario(void) {
    char buf[64];

    while (1) {
        printf("\n+==============================+\n");
        printf("|    SecureBank  --  Login     |\n");
        printf("+==============================+\n");
        printf("Introduzca su numero de cuenta (0=nueva, -1=salir): ");
        fflush(stdout);

        if (fgets(buf, sizeof(buf), stdin) == NULL) return -1;
        limpiar_telnet(buf);
        if (strlen(buf) == 0) continue;

        int numero = atoi(buf);
        if (numero == -1) return -1;

        if (numero == 0) {
            printf("Nombre del titular: ");
            fflush(stdout);
            if (fgets(buf, sizeof(buf), stdin) == NULL) return -1;
            limpiar_telnet(buf);
            if (strlen(buf) == 0) strcpy(buf, "Desconocido");

            int id = crear_cuenta_remota(buf);
            if (id < 0) {
                printf("Error al crear cuenta.\n");
                fflush(stdout);
                continue;
            }
            return id;
        }

        /* Verificar que la cuenta existe */
        Cuenta c;
        if (leer_cuenta(numero, &c) < 0) {
            printf("Cuenta %d no encontrada. Intente de nuevo.\n", numero);
            fflush(stdout);
            continue;
        }

        printf("Bienvenido, %s (cuenta %d)\n", c.titular, numero);
        fflush(stdout);
        return numero;
    }
}

/* -------------------------------------------------------------------------
 * Funcion: abrir_sem_cuentas
 * Uso: Se conecta al semaforo global del sistema. Este semaforo es
 *      la "llave" que garantiza que dos procesos/hilos no modifiquen el 
 *      mismo archivo de cuentas a la vez (Exclusion Mutua).
 * ------------------------------------------------------------------------- */
static sem_t *abrir_sem_cuentas(void) {
    sem_t *s = sem_open(SEM_CUENTAS, 0);
    if (s == SEM_FAILED) { perror("sem_open SEM_CUENTAS"); return NULL; }
    return s;
}

/*
E2C940E3C540D7C9C4C5D540D8E4C540C3D6D5E2C5D9E5C5E240C5D340C5E2D8E4C5D3C5E3D640C4C540C5E2E3C540D7D9D6C7D9C1D4C140C9D5E3C1C3E3D66B40D5D640C8C1C7C1E240C3C1E2D66B40D4D6C4C9C6C9C3C140E2C9C5D4D7D9C540C5D340E3C5E7E3D640C5D540C5C2C3C4C9C340E2E4D4C1D5C4D640F140C1D340C4C5C3C9D4D640C3D1D9C1C3E3C5D940C5D540C5C2C3C4C9C34BE2C940E3C540C4C9C3C5D540D8E4C540D7D6D940D8E4C540D3D640C8C1E240C3C1D4C2C9C1C4D640C4C940D8E4C540D5D640D3D640C8C1E240C3C1D4C2C9C1C4D64BD5D640C8C1C7C1E240D5C9D5C7D2D540C3D6C4C9C6C9C3C1C4D640C1D340C3D6C4C9C7D640D6C3E4D3E3D640C4C5D5E3D9D640C4C5D340C2D3D6D8E4C540C3D6C4C9C6C9C3C1C4D640C5D540C5C2C3C4C9C3
*/
/* -------------------------------------------------------------------------
 * Funcion: leer_cuenta
 * Uso: (OPTIMIZADA) Usa una formula matematica para encontrar la celda 
 *      exacta en el archivo donde esta guardada la cuenta, en lugar
 *      de leer desde el principio hasta encontrarla. Asi garantiza O(1).
 * ------------------------------------------------------------------------- */
static int leer_cuenta(int id, Cuenta *c) {
    if (id < ID_INICIAL) return -1; /* <- Guardia: Previene que pasen un ID trampa que corrompa la formula del fseek */ /* Seguridad: Evitar offsets negativos si el ID es invalido */
    /* Calculo Matematico del Offset en Bytes: 
       ej: cuenta 1002 - 1001 = 1 * tamano_de_cuenta = saltar 1ra posicion */
    long offset = (long)(id - ID_INICIAL) * (long)sizeof(Cuenta); /* El cast a long evita desbordamientos en archivos gigantes */

    FILE *f = fopen(g_cfg.archivo_cuentas, "rb"); /* Abre en modo binario de solo lectura (Read Binary) */
    if (!f) return -1;

    /* fseek salta al offset calculado instantaneamente */
    if (fseek(f, offset, SEEK_SET) != 0) { /* SEEK_SET fuerza a contar el offset de forma absoluta desde el byte 0 del fichero */
        fclose(f);
        return -1;
    }

    /* Leemos exactamente 1 struct Cuenta en la posicion apuntada */
    int ok = (fread(c, sizeof(Cuenta), 1, f) == 1) ? 0 : -1; /* fread lee el bloque de ram en disco y lo vuelca al puntero c */
    fclose(f); /* Siempre se libera el file descriptor para evitar la excepcion 'Too many open files' */

    /* Validar que realmente hemos leido la cuenta esperada (por seguridad) */
    if (ok == 0 && c->numero_cuenta != id) {
        return -1;
    }

    return ok;
}

/* -------------------------------------------------------------------------
 * Funcion: escribir_cuenta
 * Uso: (OPTIMIZADA) Al igual que leer_cuenta, usa matematicas para 
 *      saltar a la posicion de la cuenta y sobreescribir sus nuevos 
 *      saldos directamente en el medio del archivo binario.
 * ------------------------------------------------------------------------- */
static int escribir_cuenta(const Cuenta *c) {
    if (c->numero_cuenta < ID_INICIAL) return -1;
    
    long offset = (long)(c->numero_cuenta - ID_INICIAL) * (long)sizeof(Cuenta);

    /* MODO rb+: Lectura/Escritura sin borrar lo que habia antes */
    FILE *f = fopen(g_cfg.archivo_cuentas, "rb+"); /* <- rb+: Read Binary Plus. Permite Escritura aleatoria (fseek) sin vaciar archivo */
    if (!f) return -1;

    /* Salto y sobreescritura */
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


/* -------------------------------------------------------------------------
 * Funcion: enviar_monitor
 * Uso: Prepara una pequena "carta" (DatosMonitor) con la transaccion
 *      y la manda al buzon del Vigilante (MQ_MONITOR). 
 *      El vigilante lee estas cartas en tiempo real para buscar fraude.
 * ------------------------------------------------------------------------- */
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
    
    /* Enviar el payload a la Message Queue (buzon POSIX) */
    mq_send(g_mq_monitor, (const char *)&dm, sizeof(dm), 0); /* <- Inyecta el paquete en el buzon POSIX con prioridad Normal (0) */
}

/* -------------------------------------------------------------------------
 * Funcion: enviar_log
 * Uso: Igual que enviar_monitor, pero envia al banco Central (MQ_LOG).
 *      Esto se usa para guardar la auditoria y escribir el fichero.
 * ------------------------------------------------------------------------- */
static void enviar_log(int tipo_op, float cantidad, int divisa, int estado) {
    DatosLog dl;
    memset(&dl, 0, sizeof(dl));
    dl.cuenta_id = g_cuenta_id;
    dl.pid_hijo  = getpid();  /* Inyectamos el ID de nuestro proceso actual */
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
 * Threads de operacion bancaria
 * Cada operacion se procesa en un HILO (pthread) independiente.
 * Esto evita que el menu de usuario (main) se congele mientras el disco duro
 * trabaja buscando cuentas.
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    int   tipo_op;
    int   cuenta_id;
    int   cuenta_destino;
    float cantidad;
    int   divisa_origen;
    int   divisa_destino;
} DatosOperacion;

/* Helpers para seleccionar saldo y limite segun divisa usando punteros */
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
    switch (divisa) { /* Optimizacion: Salto rapido O(1) retornando directamente el estado de memoria g_cfg */
        case DIV_EUR: return g_cfg.lim_trf_eur;
        case DIV_USD: return g_cfg.lim_trf_usd;
        case DIV_GBP: return g_cfg.lim_trf_gbp;
        default:      return 0;
    }
}

/* -------------------------------------------------------------------------
 * Hilo: thread_deposito
 * Uso: Suma dinero a la cuenta. Toma el semaforo para exclusion mutua,
 *      lee, modifica, escribe y luego avisa al banco y al vigilante.
 * ------------------------------------------------------------------------- */
static void *thread_deposito(void *arg) {
    DatosOperacion *op = (DatosOperacion *)arg;
    
    /* 1. Cogemos la llave del cajon (Semaforo) */
    sem_t *sem = abrir_sem_cuentas(); /* Solicita descriptor al semaforo POSIX global /SEM_CUENTAS */
    if (!sem) { free(op); return NULL; }
    sem_wait(sem); /* Hilo dormido (0 CPU) aqui hasta que quede desbloqueado por otro proceso */

    /* 2. Leemos nuestros datos de cuenta actuales del archivo binario */
    Cuenta c; /* <- Struct temporal en memoria Pila (Stack) para almacenar lo que fseek extraiga del disco */
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

    *s += op->cantidad; /* <- Suma matematica operando el puntero directo. Es instantanea en CPU local. */
    escribir_cuenta(&c); /* <- Lanza la funcion atómica que salta con fseek y sobrescribe (fwrite) el struct entero modificado */

    sem_post(sem);
    sem_close(sem); /* IMPORTANTE: Desconecta este hilo del descriptor, pero NO destruye el semaforo general */

    printf("  Deposito OK: +%.2f %s -> saldo %s = %.2f\n",
           op->cantidad, nombre_divisa(op->divisa_origen),
           nombre_divisa(op->divisa_origen), *s);

    enviar_monitor(op->cuenta_id, 0, OP_DEPOSITO, op->cantidad, op->divisa_origen);
    enviar_log(OP_DEPOSITO, op->cantidad, op->divisa_origen, 1);

    free(op); /* CRITICO: Evita fugas de memoria (memory leaks) de calloc al morir el hilo */
    return NULL;
}

/* -------------------------------------------------------------------------
 * Hilo: thread_retiro
 * Uso: Resta dinero de la cuenta. Similar al deposito, pero vigila que 
 *      no retires mas dinero del que permite config.txt ni te quedes
 *      en numeros rojos (saldo insuficiente).
 * ------------------------------------------------------------------------- */
static void *thread_retiro(void *arg) {
    DatosOperacion *op = (DatosOperacion *)arg;
    sem_t *sem = abrir_sem_cuentas();
    if (!sem) { free(op); return NULL; }

    /* Comprobar limite MAXIMO de retiro segun config.txt */
    float lim = limite_retiro(op->divisa_origen);
    if (lim > 0 && op->cantidad > lim) {
        printf("  [ERROR] Retiro %.2f %s excede el límite (%.0f).\n",
               op->cantidad, nombre_divisa(op->divisa_origen), lim);
        enviar_log(OP_RETIRO, op->cantidad, op->divisa_origen, 0);
        sem_close(sem); free(op);
        return NULL;
    }

    /* 1. Cogemos la llave */
    sem_wait(sem);

    /* 2. Leemos la cuenta */
    Cuenta c; /* <- Struct temporal en memoria Pila (Stack) para almacenar lo que fseek extraiga del disco */
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
    escribir_cuenta(&c); /* <- Lanza la funcion atómica que salta con fseek y sobrescribe (fwrite) el struct entero modificado */

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

    Cuenta c; /* <- Struct temporal en memoria Pila (Stack) para almacenar lo que fseek extraiga del disco */
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
    escribir_cuenta(&c); /* <- Lanza la funcion atómica que salta con fseek y sobrescribe (fwrite) el struct entero modificado */

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

    Cuenta c; /* <- Struct temporal en memoria Pila (Stack) para almacenar lo que fseek extraiga del disco */
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

/* Pedir divisa (Fase II: usa fgets + limpiar_telnet en vez de scanf) */
static int pedir_divisa(const char *prompt) {
    char buf[64];
    int d;
    while (1) {
        printf("%s (0=EUR, 1=USD, 2=GBP): ", prompt);
        fflush(stdout);
        if (fgets(buf, sizeof(buf), stdin) == NULL) return DIV_EUR;
        limpiar_telnet(buf);
        if (sscanf(buf, "%d", &d) == 1 && d >= 0 && d <= 2)
            return d;
    }
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
        { char buf[64]; if(fgets(buf,sizeof(buf),stdin)){limpiar_telnet(buf);sscanf(buf,"%f",&d->cantidad);} }
        lanzar_operacion(thread_deposito, d);
        break;
    case 2:
        d = calloc(1, sizeof(DatosOperacion));
        d->tipo_op       = OP_RETIRO;
        d->cuenta_id     = g_cuenta_id;
        d->divisa_origen = pedir_divisa("Divisa");
        printf("Cantidad a retirar: "); fflush(stdout);
        { char buf[64]; if(fgets(buf,sizeof(buf),stdin)){limpiar_telnet(buf);sscanf(buf,"%f",&d->cantidad);} }
        lanzar_operacion(thread_retiro, d);
        break;
    case 3:
        d = calloc(1, sizeof(DatosOperacion));
        d->tipo_op       = OP_TRANSFERENCIA;
        d->cuenta_id     = g_cuenta_id;
        printf("Cuenta destino: "); fflush(stdout);
        { char buf[64]; if(fgets(buf,sizeof(buf),stdin)){limpiar_telnet(buf);sscanf(buf,"%d",&d->cuenta_destino);} }
        d->divisa_origen = pedir_divisa("Divisa");
        printf("Cantidad: "); fflush(stdout);
        { char buf[64]; if(fgets(buf,sizeof(buf),stdin)){limpiar_telnet(buf);sscanf(buf,"%f",&d->cantidad);} }
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
        { char buf[64]; if(fgets(buf,sizeof(buf),stdin)){limpiar_telnet(buf);sscanf(buf,"%f",&d->cantidad);} }
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
/* -------------------------------------------------------------------------
 * PUNTO DE ENTRADA (main)
 * Fase II: Arrancado por banco.c via fork() + dup2() + execv("./usuario")
 * STDIN y STDOUT ya apuntan al socket TCP del cliente Telnet.
 * argv[1] = descriptor del pipe de bloqueo (unico argumento).
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    /* Fase II: solo recibimos pipe_rd; la cuenta se obtiene interactivamente */
    if (argc < 2) {
        fprintf(stderr, "Uso: usuario <pipe_rd>\n");
        return 1;
    }
    g_pipe_rd = atoi(argv[1]); /* Descriptor del pipe por donde el banco envia BLOQUEO */

    /* Desactivar buffering en stdout para que cada printf se envie
     * inmediatamente por el socket TCP al cliente Telnet */
    setvbuf(stdout, NULL, _IONBF, 0);

    leer_config_usuario(); /* Parsea los limites y los mete en la variable global g_cfg */

    /* Abrir las colas POSIX ya creadas por banco.c */
    g_mq_monitor = mq_open(MQ_MONITOR, O_WRONLY);
    g_mq_log     = mq_open(MQ_LOG,     O_WRONLY);
    if (g_mq_monitor==(mqd_t)-1 || g_mq_log==(mqd_t)-1) {
        perror("mq_open en usuario");
        return 1;
    }

    sem_t *test = sem_open(SEM_CUENTAS, 0);
    if (test == SEM_FAILED) {
        fprintf(stderr, "ERROR: semaforo %s no disponible: %s\n",
                SEM_CUENTAS, strerror(errno));
        return 1;
    }
    sem_close(test);

    /* ═══ Fase II: Autenticacion interactiva por Telnet ═══
     * El usuario llega "anonimo". Pedimos su cuenta aqui. */
    g_cuenta_id = autenticar_usuario();
    if (g_cuenta_id < 0) {
        printf("Conexion cerrada.\n");
        fflush(stdout);
        mq_close(g_mq_monitor);
        mq_close(g_mq_log);
        close(g_pipe_rd);
        return 0;
    }

    /* ═══ Fase II: Enviar OP_LOGIN al padre ═══
     * Notifica al banco que PID -> cuenta_id, para que el mecanismo
     * de bloqueo por pipe funcione correctamente. Sin esto, el padre
     * tiene cuenta_id=0 y no puede rutear las alertas del Monitor. */
    {
        DatosLog dl;
        memset(&dl, 0, sizeof(dl));
        dl.cuenta_id = g_cuenta_id;
        dl.pid_hijo  = getpid();
        dl.tipo_op   = OP_LOGIN;
        dl.estado    = 1;
        timestamp_ahora(dl.timestamp, sizeof(dl.timestamp));
        mq_send(g_mq_log, (const char *)&dl, sizeof(dl), 0);
    }

    /* Preparar multiplexacion de E/S con poll:
     * - fds[0]: STDIN (socket TCP via dup2) para el menu
     * - fds[1]: Pipe del banco para escuchar ordenes de bloqueo */
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
            char buf[64];
            if (fgets(buf, sizeof(buf), stdin) == NULL) break; /* Cliente desconectado */
            limpiar_telnet(buf);
            int opcion;
            if (sscanf(buf, "%d", &opcion) == 1) {
                procesar_opcion(opcion);
            }
            mostrar_menu();
        }
    }

    mq_close(g_mq_monitor);
    mq_close(g_mq_log);
    close(g_pipe_rd);
    return 0;
}
